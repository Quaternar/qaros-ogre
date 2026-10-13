// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// THE one include of the vendored QarOS C header (third_party/qar/include/qar_streaming.h)
// for every library, adapter and plugin translation unit. Nothing else includes the
// vendored header directly; the layering guard enforces that.
//
// Why exactly one wrapper:
//  * The vendored header has ONE include guard, so its first inclusion in a translation
//    unit fixes which optional parts exist. On Windows it enables only D3D11 by itself;
//    the OpenGL producer types (QarVideoFrameOpenGl, QarStreamParamsOpenGl*, ...) and the
//    OpenGL slots of the dynamically loaded function tables exist only when
//    QAR_ENABLE_OPENGL is defined first. It is defined here AND as a PUBLIC compile
//    definition of qar_ar_core, so no include order can produce a translation unit
//    without it. On Windows it pulls in no GL header; on Linux the vendored header
//    includes <GL/gl.h>.
//  * The header is auto-generated and defines many unreferenced `static inline` helpers;
//    MSVC C4505/C4100 (GCC/Clang -Wunused-function/-Wunused-parameter) are silenced for
//    the header alone: the pragmas are pushed before its include and popped after it.
//
// QAR_ENABLE_DYNAMIC_LOADING is deliberately NOT defined here: it belongs to the QarOS
// C API adapter, which owns the program's one QAR_IMPLEMENT_DYNAMIC_LOADING().

#ifndef QAR_AR_CORE__QAR_STREAMING_C_HPP_
#define QAR_AR_CORE__QAR_STREAMING_C_HPP_

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

#endif  // QAR_AR_CORE__QAR_STREAMING_C_HPP_
