// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__MODULE_PIN_HPP_
#define QAR_QAR_ADAPTER__MODULE_PIN_HPP_

// Internal to qar_qar_adapter: not installed.

#include <atomic>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace qar_qar_adapter
{

/// Keeps the module that holds this code loaded for the rest of the process, so that a thread that
/// was left behind can always return into its own code. The owner thread and the frame workers
/// both call it; the function is inline so that every user pins the module it was linked into.
inline void pin_this_module()
{
#ifdef _WIN32
  HMODULE module = nullptr;
  GetModuleHandleExW(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
    reinterpret_cast<LPCWSTR>(&pin_this_module), &module);
#endif
}

/// Pins the module once per process: the first call does, every later call returns at once. A
/// call that changes the reference count of a module takes the loader lock, so the owner calls
/// this when it is made, at a time when nothing is stuck, and the exit path only repeats it.
inline void pin_this_module_once()
{
  static std::atomic<bool> pinned{false};
  if (!pinned.exchange(true)) {
    pin_this_module();
  }
}

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__MODULE_PIN_HPP_
