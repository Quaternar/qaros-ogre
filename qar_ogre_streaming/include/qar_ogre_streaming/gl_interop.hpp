// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__GL_INTEROP_HPP_
#define QAR_OGRE_STREAMING__GL_INTEROP_HPP_

#include <cstdint>
#include <vector>

/// @file gl_interop.hpp
/// @brief A tiny, self-contained OpenGL entry-point wrapper used by the native
/// texture handoff (OgreOpenGlTextureBridge).
///
/// ## Why this exists / why no GL header is included here
/// The GL3Plus render system that `rviz_ogre_vendor` ships carries its own GL
/// loader (`OGRE/RenderSystems/GL3Plus/GL/gl3w.h` + `GL/glcorearb.h`). We do NOT
/// include those here for two reasons:
///   1. `<GL/gl3w.h>` `#define`s every `glXxx` name to an entry in gl3w's private,
///      per-DLL function-pointer table (populated only *inside*
///      `RenderSystem_GL3Plus.dll`). Referencing them from our own translation
///      unit would hit an unresolved/uninitialised table.
///   2. Keeping GL types out of this header means it is safe to include from any
///      translation unit — including ones that DO pull in Ogre's GL3Plus headers —
///      without symbol/macro collisions, and it keeps the reusable library free of
///      a hard link-time GL dependency (the entry points are resolved at runtime
///      from the already-loaded `opengl32.dll`, exactly the way gl3w itself does).
///
/// All functions here MUST be called on the thread that owns the live GL context
/// (in RViz: the Qt render/main thread, inside DisplayContext::lockRender()). The
/// model is in-process, main thread, shared GL context.
///
/// The interface uses only `uint32_t` / `void*` / `std::vector<uint8_t>` so that
/// no GL header ever leaks to callers.
namespace qar_ogre_streaming
{
namespace gl_interop
{

/// @brief Resolve the core GL 1.1 readback entry points from `opengl32.dll`
/// (lazy, idempotent, thread-safe). @return true once they are available. Safe to
/// call with no current context (the 1.1 functions are plain exports); the fence
/// entry points are resolved separately (see @ref fence_insert) because those need
/// a current context via `wglGetProcAddress`.
bool ensure_loaded();

/// @brief The GL enum value for a 2D texture target (GL_TEXTURE_2D = 0x0DE1).
uint32_t gl_texture_2d();

/// @brief `glIsTexture(id) == GL_TRUE`. @return false if not loaded / not a texture.
bool is_texture(uint32_t id);

/// @brief Bind @p id to GL_TEXTURE_2D and read back level-0 width/height via
/// `glGetTexLevelParameteriv`. Restores the previous GL_TEXTURE_2D binding.
/// @return true on success.
bool texture_2d_dimensions(uint32_t id, uint32_t & width, uint32_t & height);

/// @brief Read level-0 of texture @p id as RGBA8 (row-major, byte order R,G,B,A)
/// via a raw `glGetTexImage` — a path that does NOT go through Ogre. Binds @p id to
/// GL_TEXTURE_2D, sets GL_PACK_ALIGNMENT=1, reads, and restores both. @p out_rgba is
/// resized to width*height*4. @return true on success (and GL_NO_ERROR afterwards).
bool read_texture_2d_rgba8(
  uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t> & out_rgba);

/// @brief `glGetError()` — drains and returns one error code (0 == GL_NO_ERROR).
uint32_t get_error();

/// @brief Result of a client-side wait on a GL fence.
enum class FenceWaitStatus
{
  NoFence,            ///< No fence / fence support unavailable.
  AlreadySignaled,    ///< GL_ALREADY_SIGNALED — GPU had already finished.
  ConditionSatisfied, ///< GL_CONDITION_SATISFIED — completed during the wait.
  TimeoutExpired,     ///< GL_TIMEOUT_EXPIRED — not done within the timeout.
  WaitFailed          ///< GL_WAIT_FAILED — invalid sync / GL error.
};

/// @brief Human-readable GL token name for a wait status (for log lines).
const char * to_string(FenceWaitStatus status);

/// @brief `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)` after the current draw
/// commands. Lazily resolves the ARB_sync entry points (needs a current context).
/// @return the `GLsync` as an opaque `void*`, or nullptr if unsupported / failed.
void * fence_insert();

/// @brief `glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns)`.
/// @return the mapped @ref FenceWaitStatus (NoFence if @p sync is null / no support).
FenceWaitStatus fence_client_wait(void * sync, uint64_t timeout_ns);

/// @brief `glDeleteSync(sync)`. No-op for nullptr. Must run on the GL thread.
void fence_delete(void * sync);

}  // namespace gl_interop
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__GL_INTEROP_HPP_
