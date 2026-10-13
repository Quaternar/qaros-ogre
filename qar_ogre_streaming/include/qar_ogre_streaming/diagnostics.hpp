// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__DIAGNOSTICS_HPP_
#define QAR_OGRE_STREAMING__DIAGNOSTICS_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Severity for a diagnostics message.
enum class DiagnosticSeverity
{
  Info,
  Warning,
  Error
};

const char * to_string(DiagnosticSeverity severity);

/// @brief Why a stereo tick produced no submitted frame.
///
/// Every path that abandons a render+submit cycle names itself here. Without a
/// reason a stall reads as nothing but a climbing `skip=` counter: a stream that
/// stopped submitting and the "sender simply has no frame for us" case are
/// indistinguishable from a bare count. `SenderNotReady` is the benign steady-state
/// reason — the viewer paces us below our tick rate — so a stall is
/// `SenderNotReady` forever with the submit counter frozen, and the detail string
/// carries the adapter's account of why.
enum class SkipReason : uint8_t
{
  None = 0,          ///< no skip recorded yet
  SenderNotReady,    ///< adapter has no frame this tick (begin-frame still pending)
  SenderError,       ///< adapter begin_frame() failed (SDK error / producer closed)
  TargetInitFailed,  ///< lazy initialize_targets() failed inside the batch
  BudgetShed         ///< the render budget dropped this otherwise-due frame
};

const char * to_string(SkipReason reason);

/// @brief Sink for structured diagnostics / log lines emitted by the library.
///
/// The consumer (RViz panel, standalone sample, test harness) implements this
/// so the reusable library never depends on any specific logging/UI framework.
/// Implementations must be safe to call from the render thread and should not
/// block.
class OgreQarDiagnosticsSink
{
public:
  virtual ~OgreQarDiagnosticsSink() = default;

  /// @brief Emit one diagnostic line. @p peer_id_hex may be empty for
  /// library-global messages.
  virtual void on_message(
    DiagnosticSeverity severity,
    const std::string & category,
    const std::string & message,
    const std::string & peer_id_hex) = 0;
};

/// @brief Rolling per-stream timing/accounting snapshot (one peer).
struct StreamDiagnostics
{
  std::string peer_id_hex;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t max_fps = 0;
  bool active = false;

  uint64_t frames_submitted = 0;
  uint64_t frames_skipped = 0;

  /// @brief Reason for the most recent skip, and the adapter's detail for it
  /// (empty when the adapter has nothing to add). These are what turn a frozen
  /// `sub=` counter from a mystery into a diagnosis.
  SkipReason last_skip_reason = SkipReason::None;
  std::string last_skip_detail;
  /// @brief Per-reason skip totals since stream creation. They sum to
  /// @ref frames_skipped.
  uint64_t skips_sender_not_ready = 0;
  uint64_t skips_sender_error = 0;
  uint64_t skips_target_init = 0;
  uint64_t skips_budget = 0;
  double last_render_ms = 0.0;       ///< Last both-eyes render time (ms).
  double avg_render_ms = 0.0;        ///< Cumulative mean both-eyes render time (ms).
  double last_left_render_ms = 0.0;  ///< Last left-eye render time (ms).
  double last_right_render_ms = 0.0; ///< Last right-eye render time (ms).
  /// @brief Derived achieved FPS for this peer over a recent rolling window of
  /// submitted frames (0 until enough frames to measure). Set by the scheduler.
  double achieved_fps = 0.0;

  /// @brief Per-frame trace (QAR_ADAPTER_FRAME_TRACE) row counters, both 0 when
  /// tracing is off / unsupported by this stream's sender. Feeds the panel
  /// heartbeat's rr=<frame_trace_changed_rows>/<frame_trace_rows> field.
  uint64_t frame_trace_rows = 0;
  uint64_t frame_trace_changed_rows = 0;
};

/// @brief Whole-manager diagnostics snapshot (struct-based, copyable).
///
/// A plain value type so callers can grab a coherent copy without holding a
/// lock on live library state.
struct DiagnosticsSnapshot
{
  bool streaming_active = false;
  uint32_t stream_count = 0;

  uint64_t batches_rendered = 0;
  double last_batch_ms = 0.0;
  double avg_batch_ms = 0.0;  ///< Rolling mean over the recent tick window.

  /// @brief Streams rendered / shed in the most recent non-empty batch.
  uint32_t last_batch_streams_served = 0;
  uint32_t last_batch_streams_skipped = 0;
  /// @brief Global count of frames shed by the render budget since start.
  uint64_t budget_skipped_frames = 0;

  std::string last_error;
  std::vector<StreamDiagnostics> streams;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__DIAGNOSTICS_HPP_
