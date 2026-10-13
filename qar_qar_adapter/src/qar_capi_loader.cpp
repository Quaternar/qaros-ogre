// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// The program's ONE definition of the QarOS C API function tables (g_qar_*_api) and of
// g_qar_dynamic_library_handle, through which every adapter component dispatches. It is its
// own translation unit in the base component `capi`, so render, panels, url and gestures
// each link without the others.
//
// Vendor-recommended loading model: the application links NO import library; the
// single-header wrapper resolves every `qar_impl_*` from the QAR DLL at run time.
// qar_loadlib() loads the DLL by its full path with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, so Windows searches for the DLL's dependencies in the
// folder of the DLL first, then in the application directory, the directories that the
// program added and System32, and never in PATH or the current directory. The SDK's own
// runtime files therefore come before an older MSVCP140/VCRUNTIME140 next to the host
// program (the ROS distro keeps one next to rviz2.exe). That is the order of the search
// and no guarantee: a module of the same name that the process already holds is used
// whichever folder it came from.
//
// This macro must appear EXACTLY ONCE in the program, at global scope: it defines the
// per-module function tables (g_qar_*_api) and g_qar_dynamic_library_handle that the
// header's inline wrappers dispatch through.

#include "qar_ar_core/qar_streaming_c.hpp"

#ifndef QAR_ENABLE_DYNAMIC_LOADING
#  error "qar_capi_loader.cpp needs QAR_ENABLE_DYNAMIC_LOADING (a usage requirement of qar_qar_adapter_capi)"
#endif

QAR_IMPLEMENT_DYNAMIC_LOADING()
