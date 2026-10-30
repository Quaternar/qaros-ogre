// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The program's ONE definition of the QarOS C API function tables (g_qar_*_api) and of
// g_qar_dynamic_library_handle, through which every adapter component dispatches. It is its
// own translation unit in the base component `capi`, so render, panels, url and gestures
// each link without the others.
//
// Vendor-recommended loading model: the application links NO import library; the
// single-header wrapper resolves every `qar_impl_*` from the QAR DLL at run time.
// qar_library_load(NULL) is the vendor's lookup, and the first match wins:
//   0. a qar-streaming-c.dll that the process already holds (nothing is loaded);
//   1. the folder of the module this header is compiled into (the executable, or the plugin
//      DLL that loads QAROS);
//   2. the folder named by the environment variable QAROS_RUNTIME_DIR (when it is set, a
//      runtime that is missing there or fails to load is an error);
//   3. the installed QAROS, `<InstallFolder>\bin` from HKLM\SOFTWARE\Quaternar\QAROS (when
//      QAROS is installed, a copy that fails to load there is an error);
//   4. every folder on PATH, in order.
// A non-NULL path names the DLL file itself and only that file is tried. Each candidate loads
// from its absolute path with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,
// so the dependencies of the runtime come from its own folder and System32 only, never from
// the application directory (the ROS distro keeps an older MSVCP140/VCRUNTIME140 next to
// rviz2.exe). A copy of the DLL beside the plugin therefore wins over the installed QAROS.
//
// This macro must appear EXACTLY ONCE in the program, at global scope: it defines the
// per-module function tables (g_qar_*_api) and g_qar_dynamic_library_handle that the
// header's inline wrappers dispatch through.

#include "qar_ar_core/qar_streaming_c.hpp"

#ifndef QAR_ENABLE_DYNAMIC_LOADING
#  error "qar_capi_loader.cpp needs QAR_ENABLE_DYNAMIC_LOADING (a usage requirement of qar_qar_adapter_capi)"
#endif

QAR_IMPLEMENT_DYNAMIC_LOADING()
