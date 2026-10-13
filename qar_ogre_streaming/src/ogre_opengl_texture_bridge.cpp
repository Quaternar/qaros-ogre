// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"

#include <cstring>

#include "qar_ogre_streaming/gl_interop.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"

// Ogre's headers are not /W4-clean; isolate them behind a warning push/pop. Only
// OgreMain headers are needed here — no GL3Plus / gl3w header (see the bridge
// header's "Native id extraction" note): the GL texture id is read through the
// virtual Ogre::Texture::getCustomAttribute("GLID"), dispatched via OgreMain.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreHardwarePixelBuffer.h>
#include <OgrePixelFormat.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreTexture.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_streaming
{
namespace
{

/// True if the active Ogre render system is an OpenGL backend. Guards the "GLID"
/// query so we never ask a non-GL backend (D3D/Metal/null) for a GL texture name.
/// Uses only the OgreMain Root/RenderSystem interface (no GL3Plus link dependency).
///
/// RViz2 Humble on Windows runs Ogre's LEGACY GL render system, NOT GL3Plus. Both
/// derive their texture from `Ogre::GLTextureCommon`, so getCustomAttribute("GLID")
/// returns the native id on either, and any "OpenGL" render system is accepted. See
/// docs/ARCHITECTURE.md#native-gl-texture-id-extraction.
bool active_render_system_is_opengl()
{
  Ogre::Root * root = Ogre::Root::getSingletonPtr();
  if (root == nullptr) {
    return false;
  }
  const Ogre::RenderSystem * rs = root->getRenderSystem();
  if (rs == nullptr) {
    return false;
  }
  // Matches both "OpenGL Rendering Subsystem" (legacy) and "OpenGL 3+ Rendering
  // Subsystem" (GL3Plus).
  return rs->getName().find("OpenGL") != Ogre::String::npos;
}

/// Best-effort Ogre pixel format -> QAR pixel format (for the standalone extract
/// path; the stream path carries the authoritative QAR format from the render
/// target's colorTextureSize()/depthTextureSize()).
QarPixelFormat ogre_to_qar_format(Ogre::PixelFormat pf)
{
  switch (pf) {
    case Ogre::PF_B8G8R8A8:
      return QAR_PIXEL_FORMAT_B8G8R8A8;
    case Ogre::PF_FLOAT32_R:
      return QAR_PIXEL_FORMAT_R32_FLOAT;
    case Ogre::PF_R8G8B8A8:
    default:
      return QAR_PIXEL_FORMAT_R8G8B8A8;
  }
}

}  // namespace

std::string OgreOpenGlTextureBridge::active_render_system_name()
{
  Ogre::Root * root = Ogre::Root::getSingletonPtr();
  if (root == nullptr) {
    return "<none>";
  }
  const Ogre::RenderSystem * rs = root->getRenderSystem();
  return rs != nullptr ? std::string(rs->getName().c_str()) : std::string("<none>");
}

Result OgreOpenGlTextureBridge::extract_gl_texture(
  const Ogre::Texture * texture, GlVideoTexture & out) const
{
  out = GlVideoTexture{};
  if (texture == nullptr) {
    return Result::error(Status::InvalidArgument, "extract_gl_texture: texture is null");
  }
  if (!active_render_system_is_opengl()) {
    return Result::error(
      Status::InvalidState,
      "extract_gl_texture: active render system is not an OpenGL backend "
      "(native GL id extraction requires OpenGL)");
  }

  // Const-correctness: getCustomAttribute is a non-const Ogre API, but reading the
  // GL name does not mutate the texture. The texture is owned by us (the render
  // target); casting away const here is safe and local.
  Ogre::Texture * tex = const_cast<Ogre::Texture *>(texture);

  // The documented Ogre 1.12 accessor for the native GL texture object id.
  unsigned int gl_id = 0;
  tex->getCustomAttribute("GLID", &gl_id);
  if (gl_id == 0u) {
    return Result::error(
      Status::InvalidState,
      "extract_gl_texture: GLID came back 0 (texture not GL-backed / not yet loaded)");
  }

  // We only ever create TEX_TYPE_2D render targets; the Qar OpenGL path is
  // GL_TEXTURE_2D-only for the separated-textures layout (the ABI texture struct
  // has no target field).
  if (tex->getTextureType() != Ogre::TEX_TYPE_2D) {
    return Result::error(
      Status::InvalidState, "extract_gl_texture: only TEX_TYPE_2D is supported");
  }

  out.texture_id = static_cast<uint32_t>(gl_id);
  out.target = gl_interop::gl_texture_2d();  // GL_TEXTURE_2D
  out.size.format = ogre_to_qar_format(tex->getFormat());
  out.size.width = tex->getWidth();
  out.size.height = tex->getHeight();
  out.size.array_layers = 1u;
  return Result::success();
}

Result OgreOpenGlTextureBridge::validate_gl_texture(
  const GlVideoTexture & tex, std::string & detail) const
{
  if (!gl_interop::ensure_loaded()) {
    detail = "GL entry points unavailable (no current GL context?)";
    return Result::error(Status::InvalidState, detail);
  }
  if (tex.texture_id == 0u) {
    detail = "texture_id is 0";
    return Result::error(Status::InvalidState, detail);
  }
  const bool is_tex = gl_interop::is_texture(tex.texture_id);
  uint32_t qw = 0;
  uint32_t qh = 0;
  const bool got_dims = gl_interop::texture_2d_dimensions(tex.texture_id, qw, qh);
  const bool dims_match = got_dims && qw == tex.size.width && qh == tex.size.height;

  detail = "glIsTexture=" + std::string(is_tex ? "1" : "0") +
    " queried=" + std::to_string(qw) + "x" + std::to_string(qh) +
    " expected=" + std::to_string(tex.size.width) + "x" + std::to_string(tex.size.height) +
    " dims_match=" + std::string(dims_match ? "1" : "0");

  if (is_tex && dims_match) {
    return Result::success();
  }
  return Result::error(Status::InvalidState, "validate_gl_texture: " + detail);
}

Result OgreOpenGlTextureBridge::insert_fence(GlFrameSync & out) const
{
  out = GlFrameSync{};
  void * sync = gl_interop::fence_insert();
  if (sync == nullptr) {
    return Result::error(
      Status::InvalidState,
      "insert_fence: glFenceSync failed / ARB_sync unavailable (no current GL context?)");
  }
  out.fence_sync = sync;
  out.qar_waits_on_fence = true;
  return Result::success();
}

Result OgreOpenGlTextureBridge::fill_opengl_frame(
  const QarOpenGlStereoFrame & src, QarVideoFrameOpenGl & out)
{
  out = QarVideoFrameOpenGl{};

  // Marshal our internal GlVideoTexture (size + GL id + target) into the real QAR
  // ABI texture struct QarVideoTextureOpenGl {size, texture} — the real struct has
  // no `target` (implicit GL_TEXTURE_2D per the separated layout).
  auto to_abi = [](const GlVideoTexture & t) {
    QarVideoTextureOpenGl a{};
    a.size = t.size;
    a.texture = t.texture_id;
    return a;
  };

  // Backing textures in the canonical separated-textures order:
  //   0 = left color, 1 = right color, [2 = left depth, 3 = right depth].
  std::size_t tcount = 0;
  out.textures[tcount++] = to_abi(src.left_color);
  out.textures[tcount++] = to_abi(src.right_color);
  if (src.has_depth) {
    out.textures[tcount++] = to_abi(src.left_depth);
    out.textures[tcount++] = to_abi(src.right_depth);
  }
  if (tcount > QAR_MAX_FRAME_TEXTURES) {
    return Result::error(Status::InvalidArgument, "fill_opengl_frame: too many textures");
  }
  out.textures_count = tcount;

  // Views: one logical view per (data_type, eye), each naming its backing texture.
  if (src.views_count > QAR_MAX_FRAME_VIEWS) {
    return Result::error(Status::InvalidArgument, "fill_opengl_frame: too many views");
  }
  std::size_t vcount = 0;
  for (std::size_t i = 0; i < src.views_count; ++i) {
    const QarRenderFrameView & rv = src.views[i];
    if (rv.texture_index >= tcount) {
      return Result::error(Status::InvalidArgument, "fill_opengl_frame: view texture_index OOB");
    }
    const QarTextureSize & sz = out.textures[rv.texture_index].size;
    QarVideoFrameView & v = out.texture_views[vcount++];
    v.start_x = 0u;
    v.start_y = 0u;
    v.end_x = sz.width;
    v.end_y = sz.height;
    v.texture_index = rv.texture_index;
    v.array_layer_index = 0u;
    v.texture_format = sz.format;
    v.data_type = rv.data_type;
    v.eye = rv.eye;
  }
  out.texture_views_count = vcount;

  // The SDK's QarVideoFrameOpenGl has NO synchronization field, so the GL fence
  // (src.synchronization) is NOT part of the QAR ABI submit here. The producer
  // keeps the fence in its own GlFrameSync for the in-process handoff; how QarOS
  // consumes a GL fence is not specified by the API.
  return Result::success();
}

Result OgreOpenGlTextureBridge::cpu_readback(
  const Ogre::Texture * texture, std::vector<uint8_t> & out_bytes, QarVideoTextureCpu & out) const
{
  out = QarVideoTextureCpu{};
  if (texture == nullptr) {
    return Result::error(Status::InvalidArgument, "cpu_readback: texture is null");
  }
  Ogre::Texture * tex = const_cast<Ogre::Texture *>(texture);
  const uint32_t w = tex->getWidth();
  const uint32_t h = tex->getHeight();
  try {
    out_bytes.assign(static_cast<size_t>(w) * h * 4u, 0u);
    Ogre::PixelBox dst(w, h, 1, Ogre::PF_BYTE_RGBA, out_bytes.data());
    tex->getBuffer()->blitToMemory(dst);
  } catch (const Ogre::Exception & e) {
    return Result::error(
      Status::InvalidState, std::string("cpu_readback failed: ") + e.getFullDescription());
  }
  out.size.format = ogre_to_qar_format(tex->getFormat());
  out.size.width = w;
  out.size.height = h;
  out.size.array_layers = 1u;
  out.pitch = w * 4u;
  out.texture_data = out_bytes.data();
  out.texture_data_size = out_bytes.size();
  return Result::success();
}

// ---------------------------------------------------------------------------
// GlFenceHook — installed on the scheduler; runs inside the batch render lock.
// ---------------------------------------------------------------------------
void GlFenceHook::insert_fence(OgreQarStereoStream & stream)
{
  // One fence per stream per batch: covers both eyes' just-issued draw commands
  // (they were rendered back-to-back immediately before this phase). The stream
  // takes ownership of the GLsync and manages its lifetime (delete-previous-on-next
  // / delete-on-destroy). See OgreQarStereoStream::set_pending_fence.
  void * sync = gl_interop::fence_insert();
  stream.set_pending_fence(sync);
}

}  // namespace qar_ogre_streaming
