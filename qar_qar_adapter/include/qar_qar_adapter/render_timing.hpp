// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__RENDER_TIMING_HPP_
#define QAR_QAR_ADAPTER__RENDER_TIMING_HPP_

/// @file render_timing.hpp
/// @brief Per-stream stage timing of the 3D stereo stream (QAR_VIDEO_TIMING_DIR).
///
/// Header-only and QarOS-free (it needs only qar_ar_core/timing_log.hpp), so it is unit
/// tested without a QarOS session: qar_capi_render_sender_adapter.cpp feeds it the results of
/// its qar_render_sender_try_begin_frame polls and its submits.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qar_ar_core/timing_log.hpp"

namespace qar_qar_adapter
{

// ---------------------------------------------------------------------------
//  RenderTimingState: per-stream stage timing for the 3D stereo stream.
//
//  Opt-in via QAR_VIDEO_TIMING_DIR (qar_ar_core/timing_log.hpp). Writes
//  render3d.csv (render3d_2.csv for a second stream, ...) with epoch-microsecond
//  stamps from the same clock as the video CSVs, so the analyser can ask "was the 3D
//  stream also starved while this video panel stalled?". Rows are QUEUED here and written
//  by the TimingLog's own thread - nothing in this class touches a file, and it is only
//  called from the single render thread that drives the adapter.
//
//  The tick loop polls try_begin_frame thousands of times a second, so polls are never
//  logged one by one: consecutive "not ready" answers are folded into ONE run row that
//  closes on the first "ready". A run's length is how long QarOS offered this stream no
//  destination - the number the proof needs.
//
//  columns: kind,start_us,end_us,count,frame,detail,mid_us
//    not_ready : start = first not-ready poll (the wait began), end = the poll that found
//                it ready, count = polls in the run
//    ready     : start = end = the poll that returned ready; detail 1 = observed by
//                prime_next_frame(), 0 = by begin_frame()'s own poll
//    submit    : start = begin_frame consumed the frame, mid = the render phase was done
//                (submit_opengl_stereo_frame entered), end = show_frame returned,
//                frame = frame index, detail 1 = show_frame succeeded
//    slow_poll : a try_begin_frame call that itself took over 2 ms (it is documented
//                non-blocking; start/end bracket the call)
// ---------------------------------------------------------------------------
class RenderTimingState
{
public:
  explicit RenderTimingState(std::shared_ptr<qar_ar_core::TimingSink> sink)
  : sink_(std::move(sink))
  {
  }

  static std::vector<std::string> columns()
  {
    return {"kind", "start_us", "end_us", "count", "frame", "detail", "mid_us"};
  }
  static std::vector<std::string> kinds()
  {
    return {"not_ready", "ready", "submit", "slow_poll"};
  }
  static constexpr int kKindColumn = 0;

  /// Bracket one qar_render_sender_try_begin_frame call: `call_start_us` is read before it.
  void note_poll_call(std::int64_t call_start_us)
  {
    const std::int64_t now = qar_ar_core::timing_epoch_us();
    if (now - call_start_us > kSlowPollUs) {
      sink_->append({kSlowPoll, call_start_us, now, 1, kBlank, kBlank, kBlank});
    }
    last_poll_us_ = now;
  }

  /// The poll just made (see note_poll_call) returned "not ready".
  void note_pending()
  {
    const std::int64_t now = (last_poll_us_ != 0) ? last_poll_us_ : qar_ar_core::timing_epoch_us();
    if (run_count_ == 0) {
      run_start_us_ = now;
    }
    ++run_count_;
  }

  /// The poll just made returned ready: close the run, record the ready.
  void note_ready(bool from_prime)
  {
    const std::int64_t now = (last_poll_us_ != 0) ? last_poll_us_ : qar_ar_core::timing_epoch_us();
    if (run_count_ > 0) {
      sink_->append({kNotReady, run_start_us_, now, run_count_, kBlank, kBlank, kBlank});
      run_count_ = 0;
      run_start_us_ = 0;
    }
    sink_->append({kReady, now, now, 1, kBlank, from_prime ? 1 : 0, kBlank});
  }

  /// begin_frame handed a frame to render.
  void note_consume(std::uint64_t frame_index)
  {
    consume_us_ = qar_ar_core::timing_epoch_us();
    frame_index_ = static_cast<std::int64_t>(frame_index);
    render_done_us_ = 0;
  }

  /// submit_opengl_stereo_frame entered: the Ogre render of this frame has finished.
  void note_render_done() { render_done_us_ = qar_ar_core::timing_epoch_us(); }

  /// submit_opengl_stereo_frame is done (any way out). Writes the submit row; @p done_us is
  /// when show_frame returned (0 = it never got that far: the failure return's own time).
  void note_submit_done(bool ok, std::int64_t done_us)
  {
    if (consume_us_ == 0) {
      return;  // a submit without a consumed frame (a test driving the adapter directly)
    }
    sink_->append({kSubmit, consume_us_,
        done_us != 0 ? done_us : qar_ar_core::timing_epoch_us(), 1, frame_index_,
        ok ? 1 : 0, render_done_us_ != 0 ? render_done_us_ : kBlank});
    consume_us_ = 0;
  }

private:
  static constexpr std::int64_t kNotReady = 0;
  static constexpr std::int64_t kReady = 1;
  static constexpr std::int64_t kSubmit = 2;
  static constexpr std::int64_t kSlowPoll = 3;
  static constexpr std::int64_t kBlank = qar_ar_core::kTimingBlank;
  static constexpr std::int64_t kSlowPollUs = 2000;

  std::shared_ptr<qar_ar_core::TimingSink> sink_;
  std::int64_t run_start_us_ = 0;
  std::int64_t run_count_ = 0;
  std::int64_t last_poll_us_ = 0;
  std::int64_t consume_us_ = 0;
  std::int64_t render_done_us_ = 0;
  std::int64_t frame_index_ = 0;
};

/// RAII: writes the submit row on EVERY way out of submit_opengl_stereo_frame (it has many
/// failure returns). `show_returned` stamps the end of the row the instant show_frame comes
/// back, so the optional prime_next_frame() poll that follows is not counted as submit time.
struct SubmitTimingGuard
{
  explicit SubmitTimingGuard(RenderTimingState * state) : timing(state)
  {
    if (timing != nullptr) { timing->note_render_done(); }
  }
  ~SubmitTimingGuard()
  {
    if (timing != nullptr) { timing->note_submit_done(ok, done_us); }
  }
  SubmitTimingGuard(const SubmitTimingGuard &) = delete;
  SubmitTimingGuard & operator=(const SubmitTimingGuard &) = delete;

  void show_returned(bool show_ok)
  {
    ok = show_ok;
    if (timing != nullptr) { done_us = qar_ar_core::timing_epoch_us(); }
  }

  RenderTimingState * timing;
  bool ok = false;
  std::int64_t done_us = 0;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__RENDER_TIMING_HPP_
