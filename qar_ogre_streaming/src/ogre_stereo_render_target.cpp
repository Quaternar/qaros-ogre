// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"

// Ogre's own headers are not /W4-clean; isolate them behind a warning push/pop.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreCamera.h>
#include <OgreColourValue.h>
#include <OgreGpuProgramParams.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreHighLevelGpuProgram.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreRenderTexture.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreTechnique.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_streaming
{
namespace
{

const char * const kDepthScheme = "qar_linear_depth";
const char * const kDepthVpName = "qar/LinearDepthVP";
const char * const kDepthFpName = "qar/LinearDepthFP";
const char * const kDepthMatName = "qar/LinearDepthMat";

// GLSL 1.50 (GL3+). Writes metric linear eye-space distance (metres) into the
// single R channel of a PF_FLOAT32_R target. The camera looks down its local -Z,
// so -(worldView * vertex).z is the positive distance in front of the camera.
const char * const kDepthVertexSrc =
  "#version 150\n"
  "in vec4 vertex;\n"
  "uniform mat4 worldViewProj;\n"
  "uniform mat4 worldView;\n"
  "out float eyeDepth;\n"
  "void main()\n"
  "{\n"
  "    gl_Position = worldViewProj * vertex;\n"
  "    eyeDepth = -(worldView * vertex).z;\n"
  "}\n";

/// Which encoding the depth texture carries. QAR_DEPTH_ENCODING selects it.
///
/// The default writes METRIC linear eye-space distance in metres. The QarOS
/// Visualizer, however, expects the raw window-space z-buffer value in [0,1] and
/// inverts it itself, so metric data renders wrongly there, and neither
/// QarRenderFrameShow.depth_scale nor rendered_near_far compensates. See
/// docs/ARCHITECTURE.md#depth-encoding.
///
///   metric (default) : eyeDepth in metres, optionally * QAR_DEPTH_SHADER_SCALE
///   ndc              : gl_FragCoord.z — the standard non-linear window depth
bool depthEncodingIsNdc()
{
  static const bool ndc = [] {
      const char * e = std::getenv("QAR_DEPTH_ENCODING");
      return e != nullptr && std::string(e) == "ndc";
    }();
  return ndc;
}

std::string makeDepthFragmentSrc()
{
  std::ostringstream os;
  os << "#version 150\n"
     << "in float eyeDepth;\n"
     << "out vec4 fragColour;\n"
     << "void main()\n"
     << "{\n";
  if (depthEncodingIsNdc()) {
    // Window-space depth in [0,1]: exactly what a depth buffer holds, derived from the
    // same custom projection we render with, so near/far invert it consistently.
    os << "    fragColour = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);\n";
  } else {
    float scale = 1.0f;
    if (const char * s = std::getenv("QAR_DEPTH_SHADER_SCALE")) {
      try {
        const float v = std::stof(std::string(s));
        if (v > 0.0f) { scale = v; }
      } catch (...) {
        // malformed: keep metric metres
      }
    }
    os << "    fragColour = vec4(eyeDepth * " << std::scientific << scale
       << ", 0.0, 0.0, 1.0);\n";
  }
  os << "}\n";
  return os.str();
}

/// Material-scheme listener: for the "qar_linear_depth" scheme, substitute the
/// linear-depth technique for EVERY renderable's material (regardless of that
/// material's own techniques), so a plain second render of the scene through this
/// scheme yields metric eye-space depth for the whole RViz scene.
class LinearDepthSchemeListener : public Ogre::MaterialManager::Listener
{
public:
  explicit LinearDepthSchemeListener(Ogre::Technique * depth_technique)
  : depth_technique_(depth_technique)
  {
  }

  Ogre::Technique * handleSchemeNotFound(
    unsigned short /*scheme_index*/,
    const Ogre::String & scheme_name,
    Ogre::Material * /*original_material*/,
    unsigned short /*lod_index*/,
    const Ogre::Renderable * /*renderable*/) override
  {
    if (scheme_name == kDepthScheme) {
      return depth_technique_;
    }
    return nullptr;
  }

private:
  Ogre::Technique * depth_technique_;
};

/// Create the linear-depth GPU programs, material and scheme listener exactly
/// once for the whole process. Must be called on the render thread (Ogre must be
/// initialized). Returns false with a message on failure.
bool ensureLinearDepthScheme(std::string & error_out)
{
  static bool initialized = false;
  static LinearDepthSchemeListener * listener = nullptr;
  if (initialized) {
    return true;
  }

  try {
    auto & hgpm = Ogre::HighLevelGpuProgramManager::getSingleton();
    // Literal value of Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME.
  // Using the literal (not the imported static data member) avoids a delay-load-
  // blocking DATA import so the headless unit-test exe can /DELAYLOAD OgreMain.dll.
  const Ogre::String group = "General";

    Ogre::HighLevelGpuProgramPtr vp =
      hgpm.createProgram(kDepthVpName, group, "glsl", Ogre::GPT_VERTEX_PROGRAM);
    vp->setSource(kDepthVertexSrc);
    vp->load();

    const std::string depth_fragment_src = makeDepthFragmentSrc();
    Ogre::HighLevelGpuProgramPtr fp =
      hgpm.createProgram(kDepthFpName, group, "glsl", Ogre::GPT_FRAGMENT_PROGRAM);
    fp->setSource(depth_fragment_src);
    fp->load();

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(kDepthMatName, group);
    // A freshly created material has no techniques/passes yet — build one.
    Ogre::Technique * tech = mat->createTechnique();
    Ogre::Pass * pass = tech->createPass();
    pass->setVertexProgram(kDepthVpName);
    pass->setFragmentProgram(kDepthFpName);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(true);
    pass->setDepthWriteEnabled(true);

    Ogre::GpuProgramParametersSharedPtr vparams = pass->getVertexProgramParameters();
    vparams->setNamedAutoConstant(
      "worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
    vparams->setNamedAutoConstant(
      "worldView", Ogre::GpuProgramParameters::ACT_WORLDVIEW_MATRIX);

    listener = new LinearDepthSchemeListener(tech);
    Ogre::MaterialManager::getSingleton().addListener(listener);

    initialized = true;
    return true;
  } catch (const Ogre::Exception & e) {
    error_out = std::string("linear-depth scheme setup failed: ") + e.getFullDescription();
    return false;
  }
}

Ogre::PixelFormat mapColorFormat(QarPixelFormat f)
{
  switch (f) {
    case QAR_PIXEL_FORMAT_B8G8R8A8:
      return Ogre::PF_B8G8R8A8;
    case QAR_PIXEL_FORMAT_R8G8B8A8:
    default:
      return Ogre::PF_R8G8B8A8;
  }
}

}  // namespace

OgreStereoRenderTarget::OgreStereoRenderTarget(
  Ogre::SceneManager * scene_manager, std::string name)
: scene_manager_(scene_manager), name_(std::move(name))
{
  if (scene_manager_ == nullptr) {
    throw std::invalid_argument("OgreStereoRenderTarget: scene_manager must not be null");
  }
  if (name_.empty()) {
    throw std::invalid_argument("OgreStereoRenderTarget: name must not be empty");
  }
}

OgreStereoRenderTarget::~OgreStereoRenderTarget()
{
  destroy();
}

Result OgreStereoRenderTarget::create(
  uint32_t width,
  uint32_t height,
  QarPixelFormat color_format,
  QarPixelFormat depth_format,
  bool with_depth)
{
  if (width == 0 || height == 0) {
    return Result::error(Status::InvalidArgument, "create: width/height must be non-zero");
  }
  if (created_) {
    return Result::error(Status::InvalidState, "create: already created (call resize/destroy)");
  }

  width_ = width;
  height_ = height;
  color_format_ = color_format;
  depth_format_ = depth_format;
  has_depth_ = with_depth;

  // Literal value of Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME.
  // Using the literal (not the imported static data member) avoids a delay-load-
  // blocking DATA import so the headless unit-test exe can /DELAYLOAD OgreMain.dll.
  const Ogre::String group = "General";

  try {
    // --- Camera (free camera; local transform == world transform) -----------
    camera_ = scene_manager_->createCamera(name_ + "/cam");
    camera_->setAutoAspectRatio(false);
    camera_->setAspectRatio(static_cast<Ogre::Real>(width_) / static_cast<Ogre::Real>(height_));
    camera_->setNearClipDistance(0.05f);
    camera_->setFarClipDistance(1000.0f);

    // --- Color render texture + viewport ------------------------------------
    Ogre::TexturePtr color_tex = Ogre::TextureManager::getSingleton().createManual(
      name_ + "/color", group, Ogre::TEX_TYPE_2D, width_, height_, 0,
      mapColorFormat(color_format_), Ogre::TU_RENDERTARGET);
    color_texture_ = color_tex.get();
    color_target_ = color_texture_->getBuffer()->getRenderTarget();
    color_target_->setAutoUpdated(false);
    viewport_ = color_target_->addViewport(camera_);
    viewport_->setClearEveryFrame(true);
    viewport_->setBackgroundColour(Ogre::ColourValue(0.1f, 0.1f, 0.1f, 1.0f));
    viewport_->setOverlaysEnabled(false);
    viewport_->setShadowsEnabled(false);

    // --- Depth render texture + viewport ------------------------------------
    if (has_depth_) {
      std::string scheme_error;
      if (!ensureLinearDepthScheme(scheme_error)) {
        destroy();
        return Result::error(Status::InvalidState, scheme_error);
      }
      Ogre::TexturePtr depth_tex = Ogre::TextureManager::getSingleton().createManual(
        name_ + "/depth", group, Ogre::TEX_TYPE_2D, width_, height_, 0,
        Ogre::PF_FLOAT32_R, Ogre::TU_RENDERTARGET);
      depth_texture_ = depth_tex.get();
      depth_target_ = depth_texture_->getBuffer()->getRenderTarget();
      depth_target_->setAutoUpdated(false);
      depth_viewport_ = depth_target_->addViewport(camera_);
      depth_viewport_->setClearEveryFrame(true);
      // "No geometry" must clear to whatever the chosen encoding calls infinitely far:
      //   metric : 0 — the documented sentinel, also used as a mask on readback;
      //   ndc    : 1 — the far plane, i.e. what a real depth buffer clears to. Clearing
      //            an NDC target to 0 would instead mark every empty pixel as being at
      //            the NEAR plane, painting the whole background as nearest.
      const float bg = depthEncodingIsNdc() ? 1.0f : 0.0f;
      depth_viewport_->setBackgroundColour(Ogre::ColourValue(bg, bg, bg, 1.0f));
      depth_viewport_->setOverlaysEnabled(false);
      depth_viewport_->setShadowsEnabled(false);
      depth_viewport_->setMaterialScheme(kDepthScheme);
    }

    created_ = true;
    return Result::success();
  } catch (const Ogre::Exception & e) {
    destroy();
    return Result::error(
      Status::InvalidState,
      std::string("OgreStereoRenderTarget::create failed: ") + e.getFullDescription());
  }
}

Result OgreStereoRenderTarget::resize(uint32_t width, uint32_t height)
{
  if (width == 0 || height == 0) {
    return Result::error(Status::InvalidArgument, "resize: width/height must be non-zero");
  }
  if (!created_) {
    return Result::error(Status::InvalidState, "resize: target not created");
  }
  if (width == width_ && height == height_) {
    return Result::success();
  }
  const QarPixelFormat color_fmt = color_format_;
  const QarPixelFormat depth_fmt = depth_format_;
  const bool with_depth = has_depth_;
  destroy();
  return create(width, height, color_fmt, depth_fmt, with_depth);
}

void OgreStereoRenderTarget::cloneCameraFrom(const Ogre::Camera * src)
{
  if (!created_ || camera_ == nullptr || src == nullptr) {
    return;
  }
  camera_->setProjectionType(src->getProjectionType());
  camera_->setNearClipDistance(src->getNearClipDistance());
  camera_->setFarClipDistance(src->getFarClipDistance());
  camera_->setAspectRatio(static_cast<Ogre::Real>(width_) / static_cast<Ogre::Real>(height_));
  if (src->getProjectionType() == Ogre::PT_ORTHOGRAPHIC) {
    camera_->setOrthoWindowHeight(src->getOrthoWindowHeight());
  } else {
    camera_->setFOVy(src->getFOVy());
  }
  // Copy the source's WORLD pose onto our parentless camera (local == world).
  camera_->setPosition(src->getDerivedPosition());
  camera_->setOrientation(src->getDerivedOrientation());
}

Result OgreStereoRenderTarget::render()
{
  if (!created_ || color_target_ == nullptr) {
    return Result::error(Status::InvalidState, "render: target not created");
  }
  // update(false): render into the FBO but do NOT swap buffers (offscreen).
  color_target_->update(false);
  if (has_depth_ && depth_target_ != nullptr) {
    depth_target_->update(false);
  }
  return Result::success();
}

Result OgreStereoRenderTarget::readColorRgba(std::vector<uint8_t> & out) const
{
  if (!created_ || color_texture_ == nullptr) {
    return Result::error(Status::InvalidState, "readColorRgba: not created");
  }
  try {
    out.resize(static_cast<size_t>(width_) * height_ * 4u);
    // PF_BYTE_RGBA is canonical R,G,B,A byte order on all endiannesses.
    Ogre::PixelBox dst(width_, height_, 1, Ogre::PF_BYTE_RGBA, out.data());
    color_texture_->getBuffer()->blitToMemory(dst);
    return Result::success();
  } catch (const Ogre::Exception & e) {
    return Result::error(
      Status::InvalidState, std::string("readColorRgba failed: ") + e.getFullDescription());
  }
}

Result OgreStereoRenderTarget::readDepthMetric(std::vector<float> & out) const
{
  if (!created_ || depth_texture_ == nullptr) {
    return Result::error(Status::InvalidState, "readDepthMetric: no depth target");
  }
  try {
    out.resize(static_cast<size_t>(width_) * height_);
    Ogre::PixelBox dst(width_, height_, 1, Ogre::PF_FLOAT32_R, out.data());
    depth_texture_->getBuffer()->blitToMemory(dst);
    return Result::success();
  } catch (const Ogre::Exception & e) {
    return Result::error(
      Status::InvalidState, std::string("readDepthMetric failed: ") + e.getFullDescription());
  }
}

uint32_t OgreStereoRenderTarget::colorGlTextureId() const
{
  // Native GL texture object id via the bridge (getCustomAttribute("GLID"),
  // OpenGL-guarded). Returns 0 if not created / not GL-backed.
  if (color_texture_ == nullptr) {
    return 0u;
  }
  OgreOpenGlTextureBridge bridge;
  GlVideoTexture out{};
  return bridge.extract_gl_texture(color_texture_, out).ok() ? out.texture_id : 0u;
}

uint32_t OgreStereoRenderTarget::depthGlTextureId() const
{
  if (depth_texture_ == nullptr) {
    return 0u;
  }
  OgreOpenGlTextureBridge bridge;
  GlVideoTexture out{};
  return bridge.extract_gl_texture(depth_texture_, out).ok() ? out.texture_id : 0u;
}

void OgreStereoRenderTarget::destroy()
{
  // Detach viewports first (so no render target still references the camera),
  // then destroy the camera, then remove the textures (which owns/destroys the
  // render targets). Guarded so it is safe to call repeatedly / partially built.
  if (color_target_ != nullptr) {
    color_target_->removeAllViewports();
  }
  if (depth_target_ != nullptr) {
    depth_target_->removeAllViewports();
  }
  viewport_ = nullptr;
  depth_viewport_ = nullptr;
  color_target_ = nullptr;
  depth_target_ = nullptr;

  if (camera_ != nullptr && scene_manager_ != nullptr) {
    try {
      scene_manager_->destroyCamera(camera_);
    } catch (const Ogre::Exception &) {
      // Already gone (e.g. scene torn down): ignore.
    }
    camera_ = nullptr;
  }

  if (color_texture_ != nullptr) {
    try {
      Ogre::TextureManager::getSingleton().remove(color_texture_->getHandle());
    } catch (const Ogre::Exception &) {
    }
    color_texture_ = nullptr;
  }
  if (depth_texture_ != nullptr) {
    try {
      Ogre::TextureManager::getSingleton().remove(depth_texture_->getHandle());
    } catch (const Ogre::Exception &) {
    }
    depth_texture_ = nullptr;
  }

  created_ = false;
}

}  // namespace qar_ogre_streaming
