// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_OPENGL_TEXTURE_BRIDGE_HPP_
#define QAR_OGRE_STREAMING__OGRE_OPENGL_TEXTURE_BRIDGE_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "qar_ogre_streaming/gl_fence_hook.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief How the bridge hands a rendered Ogre texture to the Qar OpenGL frame.
enum class BridgeMode
{
  /// Zero-copy: pass the native GL texture id + a GL fence (the C1/C9 production
  /// path — in-process, main thread, shared context per C2).
  ZeroCopyGl = 0,
  /// Debug-only fallback (contractor C1): read texels back to CPU with Ogre's
  /// blitToMemory. Never the streaming path; kept for diagnostics / non-GL bring-up.
  CpuReadbackFallback
};

/// @brief Bridges Ogre render textures to Qar OpenGL frame structures
/// Highest-risk component of the pipeline.
///
/// ## What M7 proves (Gate B)
/// Inside the live RViz GL3+ context, on the render/main thread (C2), an Ogre
/// RenderTexture's backing GL texture object can be handed to a Qar-style OpenGL
/// frame **zero-copy** — as a native `GLuint` + a `GLsync` fence — with no CPU
/// readback. See `OgreGlTextureSpike` for the in-process validation harness.
///
/// ## Native id extraction (the real 1.12 accessor)
/// The task anticipated `Ogre::GL3PlusTexture::getGLID()` reached via `dynamic_cast`
/// to the GL3Plus texture type. In Ogre 1.12 the sanctioned public accessor is the
/// **virtual** `Ogre::Texture::getCustomAttribute("GLID", &id)` (documented in
/// `OgreTexture.h`: "GLID — The OpenGL texture object id"), which dispatches through
/// OgreMain's vtable straight to `GL3PlusTexture`/`GLTextureCommon::getGLID()`.
/// We use that instead of `dynamic_cast` on purpose: it needs only OgreMain (no
/// `RenderSystem_GL3Plus` import lib, no cross-DLL RTTI, no gl3w header), which keeps
/// the reusable library link-clean and the headless unit-test exe loadable without a
/// GL driver. Extraction is still guarded by a runtime render-system-name check that
/// the active system is GL3+, so "GLID" is never queried on a non-GL backend. See
/// docs/api_gaps.md (M7).
class OgreOpenGlTextureBridge
{
public:
  explicit OgreOpenGlTextureBridge(BridgeMode mode = BridgeMode::ZeroCopyGl)
  : mode_(mode)
  {
  }

  BridgeMode mode() const { return mode_; }

  /// @brief Name of the active Ogre render system (e.g. "OpenGL Rendering Subsystem"
  /// or "OpenGL 3+ Rendering Subsystem"), or "<none>" if Ogre/RS is not initialized.
  /// For diagnostics / the spike log. OgreMain-only.
  static std::string active_render_system_name();

  /// @brief Extract the native GL texture id + target + size from an Ogre texture.
  /// Ogre-only (no GL calls): guards the active render system is GL3+, then reads
  /// the "GLID" custom attribute. @p out.target is GL_TEXTURE_2D for a 2D texture.
  /// @return Ok on success; InvalidArgument if @p texture is null; InvalidState if
  ///         the active render system is not GL3+ or the id came back 0.
  Result extract_gl_texture(const Ogre::Texture * texture, GlVideoTexture & out) const;

  /// @brief Validate an extracted id against the live GL context: `glIsTexture`
  /// must be true and the queried level-0 dimensions must equal @p tex.size.
  /// Must run on the render/GL thread. @p detail receives a human-readable summary.
  /// @return Ok if valid; InvalidState otherwise (message explains which check
  ///         failed). Debug/spike path — the per-frame submit path does not call it.
  Result validate_gl_texture(const GlVideoTexture & tex, std::string & detail) const;

  /// @brief Insert a GL fence after the current draw commands and return it as an
  /// opaque `GLsync` handle in @p out (per §13 GlFrameSync). Must run on the
  /// render/GL thread. Sets @p out.qar_waits_on_fence = true on success.
  /// @return Ok if a fence was created; InvalidState if GL/fence support is missing.
  Result insert_fence(GlFrameSync & out) const;

  /// @brief Marshal our convenience @ref QarOpenGlStereoFrame (native ids + fence)
  /// into the real Qar C `QarVideoFrameOpenGl` (flat textures[] + views[] + sync).
  /// This is the exact structure a real `qar_render_sender_frame_opengl` consumes.
  /// Pure (no Ogre, no GL) — headless unit-testable.
  /// @return Ok on success; InvalidArgument if the frame overflows QAR_MAX_* limits.
  static Result fill_opengl_frame(const QarOpenGlStereoFrame & src, QarVideoFrameOpenGl & out);

  /// @brief CPU readback fallback (BridgeMode::CpuReadbackFallback; debug only).
  /// Blits @p texture to CPU RGBA8 via Ogre's blitToMemory into @p out_bytes and
  /// points @p out at that buffer (so @p out is valid only while @p out_bytes lives).
  /// @return Ok on success; InvalidArgument if @p texture is null.
  Result cpu_readback(
    const Ogre::Texture * texture, std::vector<uint8_t> & out_bytes,
    QarVideoTextureCpu & out) const;

private:
  BridgeMode mode_;
};

/// @brief Concrete GL fence seam installed on the scheduler (§12.2 step 7 / M6
/// IGlFenceHook). For each just-rendered stream, inserts a `glFenceSync` and stores
/// the resulting `GLsync` on the stream (which owns its lifetime; see
/// OgreQarStereoStream::set_pending_fence). Runs on the render thread inside the
/// batch render lock.
class GlFenceHook : public IGlFenceHook
{
public:
  void insert_fence(OgreQarStereoStream & stream) override;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_OPENGL_TEXTURE_BRIDGE_HPP_
