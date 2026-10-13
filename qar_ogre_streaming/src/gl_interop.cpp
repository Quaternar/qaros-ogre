// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/gl_interop.hpp"

#include <mutex>

// ---------------------------------------------------------------------------
// Platform-specific GL entry-point loading.
//
// This is the ONE translation unit that talks to raw OpenGL. It declares the
// minimal subset of GL types / constants / function-pointer signatures it needs
// (mirroring the shipped GL3Plus GL/glcorearb.h) and resolves the entry points at
// runtime from the already-loaded GL driver, exactly the way gl3w does:
//   * GL 1.1 functions      -> GetProcAddress(opengl32.dll, name)
//   * GL >=1.2 / ARB_sync    -> wglGetProcAddress(name) (needs a current context)
//
// Resolving at runtime (rather than linking opengl32 / the GL3Plus render system)
// keeps the reusable static library free of a load-time GL dependency, so the
// headless unit-test executable — which never calls any of these — still loads
// with no GL driver interaction. See gl_interop.hpp for the rationale.
//
// Only the Windows loader exists. A Linux/EGL port swaps the loader body for
// glXGetProcAddress / eglGetProcAddress; nothing above this file changes.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  if defined(_MSC_VER)
#    pragma warning(push, 0)
#  endif
#  include <windows.h>
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
#endif

namespace qar_ogre_streaming
{
namespace gl_interop
{
namespace
{

// --- Minimal GL type + constant subset (matches GL/glcorearb.h) ------------
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLuint64 = unsigned long long;
using GLsync = struct __GLsync *;

#if defined(_WIN32)
#  define QAR_GLAPI __stdcall
#else
#  define QAR_GLAPI
#endif

constexpr GLenum GL_NO_ERROR_ = 0x0000;
constexpr GLboolean GL_TRUE_ = 1;
constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_TEXTURE_BINDING_2D_ = 0x8069;
constexpr GLenum GL_TEXTURE_WIDTH_ = 0x1000;
constexpr GLenum GL_TEXTURE_HEIGHT_ = 0x1001;
constexpr GLenum GL_RGBA_ = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_PACK_ALIGNMENT_ = 0x0D05;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE_ = 0x9117;
constexpr GLenum GL_ALREADY_SIGNALED_ = 0x911A;
constexpr GLenum GL_TIMEOUT_EXPIRED_ = 0x911B;
constexpr GLenum GL_CONDITION_SATISFIED_ = 0x911C;
constexpr GLenum GL_WAIT_FAILED_ = 0x911D;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT_ = 0x00000001;

// --- Function-pointer signatures -------------------------------------------
using PFN_glGetError = GLenum(QAR_GLAPI *)(void);
using PFN_glGetIntegerv = void(QAR_GLAPI *)(GLenum, GLint *);
using PFN_glPixelStorei = void(QAR_GLAPI *)(GLenum, GLint);
using PFN_glBindTexture = void(QAR_GLAPI *)(GLenum, GLuint);
using PFN_glIsTexture = GLboolean(QAR_GLAPI *)(GLuint);
using PFN_glGetTexLevelParameteriv = void(QAR_GLAPI *)(GLenum, GLint, GLenum, GLint *);
using PFN_glGetTexImage = void(QAR_GLAPI *)(GLenum, GLint, GLenum, GLenum, void *);
using PFN_glFenceSync = GLsync(QAR_GLAPI *)(GLenum, GLbitfield);
using PFN_glClientWaitSync = GLenum(QAR_GLAPI *)(GLsync, GLbitfield, GLuint64);
using PFN_glDeleteSync = void(QAR_GLAPI *)(GLsync);

struct GlApi
{
  PFN_glGetError GetError = nullptr;
  PFN_glGetIntegerv GetIntegerv = nullptr;
  PFN_glPixelStorei PixelStorei = nullptr;
  PFN_glBindTexture BindTexture = nullptr;
  PFN_glIsTexture IsTexture = nullptr;
  PFN_glGetTexLevelParameteriv GetTexLevelParameteriv = nullptr;
  PFN_glGetTexImage GetTexImage = nullptr;
  PFN_glFenceSync FenceSync = nullptr;
  PFN_glClientWaitSync ClientWaitSync = nullptr;
  PFN_glDeleteSync DeleteSync = nullptr;
  bool core_loaded = false;
  bool fence_loaded = false;
};

std::mutex g_mutex;
GlApi g_api;

#if defined(_WIN32)
using PFN_wglGetProcAddress = PROC(__stdcall *)(LPCSTR);
HMODULE g_opengl32 = nullptr;
PFN_wglGetProcAddress g_wglGetProcAddress = nullptr;

void * resolve(const char * name)
{
  void * p = nullptr;
  if (g_wglGetProcAddress != nullptr) {
    p = reinterpret_cast<void *>(g_wglGetProcAddress(name));
    // wglGetProcAddress returns 0, 1, 2, 3 or -1 for "not available".
    if (p == reinterpret_cast<void *>(0) || p == reinterpret_cast<void *>(1) ||
      p == reinterpret_cast<void *>(2) || p == reinterpret_cast<void *>(3) ||
      p == reinterpret_cast<void *>(-1))
    {
      p = nullptr;
    }
  }
  if (p == nullptr && g_opengl32 != nullptr) {
    p = reinterpret_cast<void *>(GetProcAddress(g_opengl32, name));
  }
  return p;
}
#else
void * resolve(const char *) { return nullptr; }  // non-Windows: a glX/EGL loader would go here.
#endif

// Load the GL 1.1 readback core (no current context required). Caller holds lock.
bool load_core_locked()
{
  if (g_api.core_loaded) {
    return true;
  }
#if defined(_WIN32)
  if (g_opengl32 == nullptr) {
    g_opengl32 = LoadLibraryW(L"opengl32.dll");
  }
  if (g_opengl32 == nullptr) {
    return false;
  }
  if (g_wglGetProcAddress == nullptr) {
    g_wglGetProcAddress =
      reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(g_opengl32, "wglGetProcAddress"));
  }
#endif
  g_api.GetError = reinterpret_cast<PFN_glGetError>(resolve("glGetError"));
  g_api.GetIntegerv = reinterpret_cast<PFN_glGetIntegerv>(resolve("glGetIntegerv"));
  g_api.PixelStorei = reinterpret_cast<PFN_glPixelStorei>(resolve("glPixelStorei"));
  g_api.BindTexture = reinterpret_cast<PFN_glBindTexture>(resolve("glBindTexture"));
  g_api.IsTexture = reinterpret_cast<PFN_glIsTexture>(resolve("glIsTexture"));
  g_api.GetTexLevelParameteriv =
    reinterpret_cast<PFN_glGetTexLevelParameteriv>(resolve("glGetTexLevelParameteriv"));
  g_api.GetTexImage = reinterpret_cast<PFN_glGetTexImage>(resolve("glGetTexImage"));

  g_api.core_loaded = g_api.GetError && g_api.GetIntegerv && g_api.PixelStorei &&
    g_api.BindTexture && g_api.IsTexture && g_api.GetTexLevelParameteriv && g_api.GetTexImage;
  return g_api.core_loaded;
}

// Load the ARB_sync fence entry points (needs a current context via wgl). Caller
// holds lock. Retried each call until it succeeds (cheap once loaded).
bool load_fence_locked()
{
  if (g_api.fence_loaded) {
    return true;
  }
  if (!load_core_locked()) {
    return false;
  }
  g_api.FenceSync = reinterpret_cast<PFN_glFenceSync>(resolve("glFenceSync"));
  g_api.ClientWaitSync = reinterpret_cast<PFN_glClientWaitSync>(resolve("glClientWaitSync"));
  g_api.DeleteSync = reinterpret_cast<PFN_glDeleteSync>(resolve("glDeleteSync"));
  g_api.fence_loaded = g_api.FenceSync && g_api.ClientWaitSync && g_api.DeleteSync;
  return g_api.fence_loaded;
}

}  // namespace

bool ensure_loaded()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return load_core_locked();
}

uint32_t gl_texture_2d() { return static_cast<uint32_t>(GL_TEXTURE_2D_); }

uint32_t get_error()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!load_core_locked()) {
    return 0u;
  }
  return static_cast<uint32_t>(g_api.GetError());
}

bool is_texture(uint32_t id)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!load_core_locked() || id == 0u) {
    return false;
  }
  return g_api.IsTexture(static_cast<GLuint>(id)) == GL_TRUE_;
}

bool texture_2d_dimensions(uint32_t id, uint32_t & width, uint32_t & height)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  width = 0;
  height = 0;
  if (!load_core_locked() || id == 0u) {
    return false;
  }
  GLint prev = 0;
  g_api.GetIntegerv(GL_TEXTURE_BINDING_2D_, &prev);
  g_api.BindTexture(GL_TEXTURE_2D_, static_cast<GLuint>(id));
  GLint w = 0;
  GLint h = 0;
  g_api.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_WIDTH_, &w);
  g_api.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_HEIGHT_, &h);
  g_api.BindTexture(GL_TEXTURE_2D_, static_cast<GLuint>(prev));
  width = (w > 0) ? static_cast<uint32_t>(w) : 0u;
  height = (h > 0) ? static_cast<uint32_t>(h) : 0u;
  return g_api.GetError() == GL_NO_ERROR_ && width > 0 && height > 0;
}

bool read_texture_2d_rgba8(
  uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t> & out_rgba)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!load_core_locked() || id == 0u || width == 0u || height == 0u) {
    return false;
  }
  out_rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);

  GLint prev = 0;
  g_api.GetIntegerv(GL_TEXTURE_BINDING_2D_, &prev);
  GLint prev_align = 4;
  g_api.GetIntegerv(GL_PACK_ALIGNMENT_, &prev_align);

  g_api.BindTexture(GL_TEXTURE_2D_, static_cast<GLuint>(id));
  g_api.PixelStorei(GL_PACK_ALIGNMENT_, 1);
  // Raw, Ogre-independent readback of the native texture object.
  g_api.GetTexImage(GL_TEXTURE_2D_, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, out_rgba.data());

  g_api.PixelStorei(GL_PACK_ALIGNMENT_, prev_align);
  g_api.BindTexture(GL_TEXTURE_2D_, static_cast<GLuint>(prev));
  return g_api.GetError() == GL_NO_ERROR_;
}

const char * to_string(FenceWaitStatus status)
{
  switch (status) {
    case FenceWaitStatus::AlreadySignaled:
      return "GL_ALREADY_SIGNALED";
    case FenceWaitStatus::ConditionSatisfied:
      return "GL_CONDITION_SATISFIED";
    case FenceWaitStatus::TimeoutExpired:
      return "GL_TIMEOUT_EXPIRED";
    case FenceWaitStatus::WaitFailed:
      return "GL_WAIT_FAILED";
    case FenceWaitStatus::NoFence:
    default:
      return "NO_FENCE";
  }
}

void * fence_insert()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!load_fence_locked()) {
    return nullptr;
  }
  GLsync sync = g_api.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0);
  return reinterpret_cast<void *>(sync);
}

FenceWaitStatus fence_client_wait(void * sync, uint64_t timeout_ns)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (sync == nullptr || !load_fence_locked()) {
    return FenceWaitStatus::NoFence;
  }
  const GLenum r = g_api.ClientWaitSync(
    reinterpret_cast<GLsync>(sync), GL_SYNC_FLUSH_COMMANDS_BIT_,
    static_cast<GLuint64>(timeout_ns));
  switch (r) {
    case GL_ALREADY_SIGNALED_:
      return FenceWaitStatus::AlreadySignaled;
    case GL_CONDITION_SATISFIED_:
      return FenceWaitStatus::ConditionSatisfied;
    case GL_TIMEOUT_EXPIRED_:
      return FenceWaitStatus::TimeoutExpired;
    case GL_WAIT_FAILED_:
    default:
      return FenceWaitStatus::WaitFailed;
  }
}

void fence_delete(void * sync)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (sync == nullptr || !load_fence_locked()) {
    return;
  }
  g_api.DeleteSync(reinterpret_cast<GLsync>(sync));
}

}  // namespace gl_interop
}  // namespace qar_ogre_streaming
