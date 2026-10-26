// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// PRIVATE to qar_qar_adapter (not installed): the trace and error helpers that BOTH the
// panel input source (component `panels`) and the spatial gesture source (component
// `gestures`) use. Moved verbatim out of qar_capi_panel_input_source.cpp; they keep the
// internal linkage they had there (one copy per translation unit, as before).

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_INPUT_INTERNAL_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_INPUT_INTERNAL_HPP_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_qar_adapter
{
namespace
{

/// Same shape as the other adapters' helpers: a readable "<call> failed: ..."
/// line, echoed to stderr because only the last error survives in the UI.
std::string
input_result_error(const char * what, const QarResult & r)
{
  char buf[512] = {0};
  qar_result_message(r, buf, sizeof(buf) - 1);
  std::string out = std::string(what) + " failed: code=" +
    std::to_string(static_cast<long long>(r.code));
  if (buf[0] != '\0') {
    out += " msg=";
    out += buf;
  }
  std::fprintf(stderr, "[qar-panel-input] *** %s\n", out.c_str());
  std::fflush(stderr);
  return out;
}

/// Names for the trace. The enums are plain C, so a switch here is the only way
/// a log line can say "click / instant" instead of "0 / 3".
const char *
gesture_kind_name(QarGestureKind kind)
{
  switch (kind) {
    case QAR_GESTURE_CLICK: return "click";
    case QAR_GESTURE_HOVER: return "hover";
    case QAR_GESTURE_SINGLE_POINTER_6DOF: return "single-pointer-6dof";
    case QAR_GESTURE_DUAL_POINTER_TRANSLATE_DISTANCE: return "dual-pointer-distance";
    case QAR_GESTURE_DUAL_POINTER_ROTATE: return "dual-pointer-rotate";
  }
  return "unknown-kind";
}

const char *
gesture_phase_name(QarGesturePhase phase)
{
  switch (phase) {
    case QAR_GESTURE_PHASE_STARTED: return "started";
    case QAR_GESTURE_PHASE_UPDATED: return "updated";
    case QAR_GESTURE_PHASE_ENDED: return "ended";
    case QAR_GESTURE_PHASE_INSTANT: return "instant";
    case QAR_GESTURE_PHASE_CANCELED: return "canceled";
  }
  return "unknown-phase";
}

/// Rate limit for the received-event trace: all of the first 20, then one in 50.
///
/// A hover subscription republishes at the renderer's frame rate, so an
/// unlimited trace would add tens of thousands of lines a minute to a log that
/// already reaches hundreds of megabytes. The first 20 are what actually answer
/// the question the trace exists for -- "is ANYTHING arriving?" -- and the
/// 1-in-50 tail is enough to show that a stream is still running and where it is
/// pointing.
bool
should_trace_event(std::uint64_t index)
{
  return index <= 20 || (index % 50) == 0;
}

/// Upper bound on un-printed trace records. drain() empties this every Qt tick:
/// the cap is only here so a manager that stopped ticking cannot turn the trace
/// into an unbounded allocation on the callback thread.
constexpr std::size_t kMaxPendingTrace = 256;

}  // namespace
}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_INPUT_INTERNAL_HPP_
