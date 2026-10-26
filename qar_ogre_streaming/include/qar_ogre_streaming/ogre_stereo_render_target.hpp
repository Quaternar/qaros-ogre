// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_STEREO_RENDER_TARGET_HPP_
#define QAR_OGRE_STREAMING__OGRE_STEREO_RENDER_TARGET_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief The QAR pixel format actually produced for the depth render target.
///
/// Ogre 1.12 GL3+ cannot expose a metric, sampleable hardware depth texture
/// through its public API, so depth is produced as a shader-written single-channel
/// 32-bit float texture holding metric linear eye-space distance in metres
/// (QAR_DEPTH_ENCODING=ndc switches it to the window-space z value). Regardless of
/// what depth format the caller requests, the produced/transmitted format is
/// therefore always @c QAR_PIXEL_FORMAT_R32_FLOAT (tagged with the depth view
/// semantic elsewhere).
///
/// This is a pure function (no Ogre dependency) so the decision is unit
/// testable without a GL context.
QarPixelFormat depth_output_qar_format(QarPixelFormat requested) noexcept;

/// @brief Owns and manages one Ogre color render texture plus a metric depth
/// render texture, each with an attached camera + viewport.
///
/// This is the single-eye building block: a stereo stream owns two of these
/// (left/right eye); the plugin's offscreen mode uses one to mirror the live RViz
/// view (cloneCameraFrom). It centralizes safe creation/resize/destruction of the
/// Ogre resources and the CPU readback used for the debug PNG capture path.
///
/// ## Threading
/// Every method that touches Ogre (create/resize/destroy/render/readback) MUST be
/// called on the thread that owns the OpenGL context — in RViz that is the Qt
/// render thread, guarded by DisplayContext::lockRender()/unlockRender(). This
/// class performs no locking of its own; the caller (the plugin's render path, or
/// the standalone sample) owns that discipline.
///
/// ## Depth encoding
/// The depth target is an Ogre @c PF_FLOAT32_R texture. The scene is rendered a
/// second time through the "qar_linear_depth" material scheme, whose fragment
/// shader writes @c -viewSpaceZ (metric linear eye-space distance in metres) by
/// default, or the window-space z value when QAR_DEPTH_ENCODING=ndc. The
/// scheme + shaders are created once, lazily, on first create().
class OgreStereoRenderTarget
{
public:
  /// @param scene_manager  Non-null Ogre scene to create the camera in.
  /// @param name           Unique base name for the Ogre resources.
  /// @throws std::invalid_argument if @p scene_manager is null or @p name empty.
  OgreStereoRenderTarget(Ogre::SceneManager * scene_manager, std::string name);
  ~OgreStereoRenderTarget();

  OgreStereoRenderTarget(const OgreStereoRenderTarget &) = delete;
  OgreStereoRenderTarget & operator=(const OgreStereoRenderTarget &) = delete;

  /// @brief Create the color (and, if @p with_depth, metric-depth) render
  /// texture(s) at the given size and attach a camera + viewport to each.
  ///
  /// @param color_format  QAR color format, mapped to an Ogre color format
  ///                      (R8G8B8A8/B8G8R8A8 supported, else falls back to RGBA8).
  /// @param depth_format  Requested depth format; the produced depth texture is
  ///                      always a single-channel @c PF_FLOAT32_R. Ignored beyond
  ///                      being reported via depth_output_qar_format().
  /// @param with_depth    Also create the metric depth target.
  /// @return Ok on success; InvalidArgument on zero size; InvalidState if already
  ///         created; a descriptive error if an Ogre resource could not be made.
  Result create(
    uint32_t width,
    uint32_t height,
    QarPixelFormat color_format,
    QarPixelFormat depth_format,
    bool with_depth);

  /// @brief Resize the render texture(s) (destroy + recreate at the new size,
  /// preserving formats/depth choice). @return Ok on success.
  Result resize(uint32_t width, uint32_t height);

  /// @brief Copy the given camera's world pose + projection onto this target's
  /// offscreen camera, using this target's aspect ratio. Used each tick to mirror
  /// the live RViz view. No-op if not created or @p src is null.
  void cloneCameraFrom(const Ogre::Camera * src);

  /// @brief Render the color target (and depth target, if present) once. Calls
  /// Ogre::RenderTarget::update(false). Must run on the GL/render thread inside
  /// lockRender()/unlockRender(). @return Ok if rendered; InvalidState if not
  /// created.
  Result render();

  /// @brief Blit the color target to a CPU RGBA8 buffer (row-major, 4 bytes/px,
  /// byte order R,G,B,A). @param out resized to width*height*4. @return Ok on
  /// success; InvalidState if not created.
  Result readColorRgba(std::vector<uint8_t> & out) const;

  /// @brief Blit the depth target to a CPU float buffer of metric eye-space
  /// distances in metres (one float per pixel, row-major). @param out resized to
  /// width*height. @return Ok on success; InvalidState if no depth target.
  Result readDepthMetric(std::vector<float> & out) const;

  /// @brief Release all owned Ogre resources. Safe to call repeatedly and when
  /// nothing was created (idempotent; guards double-free).
  void destroy();

  bool is_created() const { return created_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool has_depth() const { return has_depth_; }
  QarPixelFormat color_format() const { return color_format_; }
  /// @brief The depth format actually produced (always R32F).
  QarPixelFormat depth_format() const { return depth_output_qar_format(depth_format_); }
  const std::string & name() const { return name_; }

  /// @brief Owned camera (null until created).
  Ogre::Camera * camera() const { return camera_; }
  /// @brief Owned color render target (null until created).
  Ogre::RenderTexture * color_target() const { return color_target_; }
  /// @brief Owned metric depth render target (null if depth not requested).
  Ogre::RenderTexture * depth_target() const { return depth_target_; }

  /// @brief QAR texture descriptor for the color target (format/size), suitable
  /// for a GlVideoTexture. Reports the produced color format.
  QarTextureSize colorTextureSize() const
  {
    return QarTextureSize{color_format_, width_, height_, 1u};
  }
  /// @brief QAR texture descriptor for the depth target (always R32F).
  QarTextureSize depthTextureSize() const
  {
    return QarTextureSize{depth_output_qar_format(depth_format_), width_, height_, 1u};
  }

  /// @brief The NATIVE OpenGL texture object id (GLuint) for the color texture,
  /// transported as the GL texture id in the submitted zero-copy frame. It is
  /// extracted via OgreOpenGlTextureBridge (Ogre::Texture::getCustomAttribute(
  /// "GLID"), guarded to an OpenGL render system). Returns 0 if not created or if
  /// the active render system is not OpenGL. Cheap (no GL call — reads the cached id).
  uint32_t colorGlTextureId() const;
  /// @brief As colorGlTextureId(), for the metric depth texture (0 if no depth).
  uint32_t depthGlTextureId() const;

private:
  Ogre::SceneManager * scene_manager_;
  std::string name_;

  Ogre::Camera * camera_ = nullptr;
  Ogre::Texture * color_texture_ = nullptr;
  Ogre::Texture * depth_texture_ = nullptr;
  Ogre::RenderTexture * color_target_ = nullptr;
  Ogre::RenderTexture * depth_target_ = nullptr;
  Ogre::Viewport * viewport_ = nullptr;
  Ogre::Viewport * depth_viewport_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  QarPixelFormat color_format_ = QAR_PIXEL_FORMAT_R8G8B8A8;
  QarPixelFormat depth_format_ = QAR_PIXEL_FORMAT_D32_FLOAT;
  bool has_depth_ = false;
  bool created_ = false;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_STEREO_RENDER_TARGET_HPP_
