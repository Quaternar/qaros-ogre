// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Thin wrapper that includes the real, vendored Qar streaming C API header
// (third_party/qar/include/qar_streaming.h) while suppressing third-party
// compiler warnings so the qar_ogre_streaming library can build cleanly at /W4.
//
// The vendored header is an auto-generated single header. It defines many
// `static inline` X-macro-generated function declarations/helpers which, when
// included but unreferenced, trip MSVC C4505 ("unreferenced local function
// removed") under /W4 and equivalent warnings on GCC/Clang. Isolating the
// include here keeps our own translation units warning-clean without lowering
// the warning level for library code.
//
// qar_ogre_streaming REUSES the Qar C types directly (QarFov, QarPose,
// QarQuaternion, QarVector3, QarNearFar, QarPeerId, QarFrameLayout,
// QarPixelFormat, QarRenderFrameView, QarVideoFrameView, QarTextureSize, ...)
// so that library structures interoperate with the real Qar ABI without a
// conversion layer.

#ifndef QAR_OGRE_STREAMING__QAR_STREAMING_C_HPP_
#define QAR_OGRE_STREAMING__QAR_STREAMING_C_HPP_

// The vendored header (SDK v0.1.1) auto-enables the D3D11 backend on Windows and
// the OpenGL backend only on Linux. Our target is the OpenGL zero-copy path
// (contractor C1/C9), so on EVERY platform we must expose the OpenGL types
// (QarVideoTextureOpenGl, QarVideoFrameOpenGl, QarStreamParamsOpenGl,
// qar_render_sender_frame_opengl, QAR_GRAPHICS_API_OPENGL) by defining
// QAR_ENABLE_OPENGL BEFORE the include. It pulls in NO GL headers on Windows
// (the texture is a plain uint32_t GL id; context fields are void*/uintptr_t) —
// only Linux chains <GL/gl.h>, which is fine there.
#ifndef QAR_ENABLE_OPENGL
#  define QAR_ENABLE_OPENGL
#endif

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4505)  // unreferenced local function has been removed
#  pragma warning(disable : 4100)  // unreferenced formal parameter (in inline helpers)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <qar_streaming.h>

#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#endif  // QAR_OGRE_STREAMING__QAR_STREAMING_C_HPP_
