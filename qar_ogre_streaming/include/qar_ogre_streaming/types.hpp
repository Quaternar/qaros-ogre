// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__TYPES_HPP_
#define QAR_OGRE_STREAMING__TYPES_HPP_

/// @file types.hpp
/// @brief Shared value types for qar_ogre_streaming.
///
/// This header reuses the real Qar C types (via the vendored qar_streaming.h)
/// and adds a small set of library-level POD types that describe stereo frame
/// requests and OpenGL stereo frame submissions. It has ZERO dependency on
/// rviz_common, rviz_rendering, Qt, ament or ROS — only the vendored Qar header,
/// Ogre (forward-declared here) and the C++ standard library.

// The wrapper performs the single, warning-suppressed include of the vendored
// qar_streaming.h, which defines the REAL OpenGL producer types
// (QarVideoTextureOpenGl, QarVideoFrameOpenGl, ... — note the "OpenGl" casing). The
// library uses the real ABI struct at the QAR boundary and the small
// library-internal descriptors below for its own richer per-texture bookkeeping (a
// `target` GL enum + a GL fence handle the real ABI struct does not carry).
#include "qar_ar_core/qar_streaming_c.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Ogre is an allowed dependency of this library, but the public headers only
// need forward declarations: they hold Ogre resources by pointer only, so a
// consumer that does not touch Ogre never includes an Ogre header.
namespace Ogre
{
class SceneManager;
class Camera;
class RenderTexture;
class Viewport;
class Texture;
}  // namespace Ogre

namespace qar_ogre_streaming
{

/// @brief Coarse result code returned by library operations.
///
/// An operation that cannot do its work returns an error status with an
/// explanatory message rather than pretending to succeed.
enum class Status
{
  Ok = 0,
  NotImplemented,      ///< Reserved: the operation is not implemented by this component.
  InvalidArgument,     ///< A caller-supplied argument was null/out-of-range.
  InvalidState,        ///< Operation not valid in the current object state.
  AlreadyExists,       ///< e.g. a stream for this peer already exists.
  NotFound,            ///< e.g. no stream for the requested peer.
  RenderThreadRequired ///< Called off the render thread (see IRenderThreadExecutor).
};

/// @brief Human-readable name for a @ref Status value (for logs/diagnostics).
const char * to_string(Status status);

/// @brief A @ref Status paired with an explanatory message.
struct Result
{
  Status status = Status::Ok;
  std::string message;

  bool ok() const { return status == Status::Ok; }
  explicit operator bool() const { return ok(); }

  static Result success() { return {Status::Ok, {}}; }
  static Result error(Status s, std::string msg) { return {s, std::move(msg)}; }
};

/// @brief Library-internal descriptor for one OpenGL texture handed toward QAR.
///
/// This is qar_ogre_streaming's OWN type, NOT the QAR ABI struct. The real
/// `QarVideoTextureOpenGl` carries only `{QarTextureSize size;
/// uint32_t texture;}`; we keep an extra `target` GL enum (GL_TEXTURE_2D vs
/// GL_TEXTURE_2D_ARRAY) for the layout seam. `OgreOpenGlTextureBridge` /
/// `QarCApiRenderSenderAdapter` marshal this into the real struct at the QAR
/// boundary (dropping `target`, which is implicit per-layout).
struct GlVideoTexture
{
  QarTextureSize size{};
  uint32_t texture_id = 0;  ///< GLuint transported as uint32_t.
  uint32_t target = 0;      ///< GL_TEXTURE_2D (0x0DE1) or GL_TEXTURE_2D_ARRAY.
};

/// @brief Library-internal GL fence descriptor for a submitted stereo frame.
///
/// Also OUR type: the SDK's `QarVideoFrameOpenGl` has NO synchronization field at
/// all. The producer still tracks its GL fence here for the in-process
/// shared-context handoff; how QarOS consumes a GL fence is not specified by the
/// API.
struct GlFrameSync
{
  void * fence_sync = nullptr;   ///< GLsync as an opaque handle; NULL if unused.
  bool qar_waits_on_fence = false;
};

/// @brief Ordering functor so a QarPeerId can key an ordered container.
struct PeerIdLess
{
  bool operator()(const QarPeerId & a, const QarPeerId & b) const noexcept;
};

/// @brief Value equality for QarPeerId.
bool peer_id_equal(const QarPeerId & a, const QarPeerId & b) noexcept;

/// @brief Lowercase hex rendering of a QarPeerId (for logs/diagnostics).
std::string peer_id_to_hex(const QarPeerId & id);

/// @brief Pose + projection inputs for a single eye of a stereo view.
///
/// Uses the real Qar math types so values round-trip losslessly to/from the Qar
/// frame-info and per-view-override structures.
struct EyeView
{
  QarPose pose{};        ///< View pose in the Qar coordinate convention.
  QarFov fov{};          ///< Asymmetric field of view (OpenXR-style angles).
  QarNearFar near_far{}; ///< Near/far clip planes.
};

/// @brief Per-frame information a Qar sender hands back at begin_frame().
///
/// Mirrors what a real QarRenderFrameInfo query yields: an index, a predicted
/// display time, and per-eye pose/FOV. Produced by
/// IQarRenderSenderAdapter::begin_frame and consumed by the scheduler to place
/// the stereo cameras.
struct QarFrameInfo
{
  uint64_t frame_index = 0;
  QarTimePoint predicted_display_time{};
  EyeView left_eye{};
  EyeView right_eye{};
  bool should_render = false;  ///< false => sender does not want a frame now.
};

/// @brief A rendered stereo OpenGL frame ready to submit to a Qar sender.
///
/// The supported layout is QAR_FRAME_LAYOUT_SEPARATED_TEXTURES
/// (texture[0]=left color, texture[1]=right color). Depth
/// textures are optional and carried alongside when produced. This structure is
/// the qar_ogre_streaming-side view that a real adapter marshals into a
/// QarVideoFrameOpenGl (OgreOpenGlTextureBridge::fill_opengl_frame).
///
/// ## Separated-texture layout
/// The @ref views array makes the per-view semantics explicit and matches the
/// real Qar @c QarVideoFrameLayout::views: each entry names a logical view's
/// data type (color/depth) + eye (left/right) and the index of its backing
/// texture in the canonical ordering below:
///   texture_index 0 = left color, 1 = right color, 2 = left depth, 3 = right depth
/// so `views[0] = {COLOR, LEFT, 0}`, `views[1] = {COLOR, RIGHT, 1}` and, when
/// depth is present, `views[2] = {DEPTH, LEFT, 2}`, `views[3] = {DEPTH, RIGHT, 3}`.
/// The left_color/right_color/left_depth/right_depth members carry those same
/// backing textures for convenient positional access.
struct QarOpenGlStereoFrame
{
  QarPeerId peer_id{};
  uint64_t frame_index = 0;
  QarFrameLayout layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;

  GlVideoTexture left_color{};
  GlVideoTexture right_color{};

  bool has_depth = false;
  GlVideoTexture left_depth{};
  GlVideoTexture right_depth{};

  /// @brief Per-view descriptors (data type + eye label + backing texture index),
  /// mirroring the real Qar layout so eye labels + layout are explicit and
  /// assertable. Populated by assemble_separated_stereo_frame().
  QarRenderFrameView views[QAR_MAX_FRAME_VIEWS]{};
  std::size_t views_count = 0;

  GlFrameSync synchronization{};
};

/// @brief Plain RGB triple (keeps Ogre::ColourValue out of the public header).
struct Rgb
{
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
};

/// @brief Static configuration of one peer's stereo stream.
struct StereoStreamConfig
{
  QarPeerId peer_id{};
  uint32_t width = 1024;
  uint32_t height = 1024;
  uint32_t max_fps = 72;
  QarPixelFormat color_format = QAR_PIXEL_FORMAT_R8G8B8A8;
  QarPixelFormat depth_format = QAR_PIXEL_FORMAT_D32_FLOAT;
  QarFrameLayout layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;

  /// @brief Validate ranges; returns an error Result describing the first
  /// problem found (zero size, zero fps, non-separated layout).
  Result validate() const;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__TYPES_HPP_
