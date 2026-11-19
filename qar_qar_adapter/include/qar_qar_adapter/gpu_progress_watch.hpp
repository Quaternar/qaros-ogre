// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__GPU_PROGRESS_WATCH_HPP_
#define QAR_QAR_ADAPTER__GPU_PROGRESS_WATCH_HPP_

/// @file gpu_progress_watch.hpp
/// @brief The GL fence seam of the render adapter and the watch that tells whether the GPU still
/// finishes the frames the render thread issues.
///
/// A GL call of the render thread that needs the GPU waits for as long as a GPU stall lasts, and
/// with it the whole Qt thread. Two parts keep our code out of those waits:
///  - The sender of a 3D stream ends its copy into the textures of the runtime with a fence and a
///    flush on the render thread and lets its frame worker wait for the fence (GlFenceOps,
///    FenceContext, QarCApiRenderSenderAdapter).
///  - A GpuProgressWatch follows a fence of every frame the render thread issues (RViz's frames
///    and the 3D render batch) on a thread of its own. The two kinds of frame are separate series.
///    A series is closed while its oldest unfinished frame is older than its age limit, which is
///    max(GpuGateTimings::min_age, GpuGateTimings::age_factor x the median of the last
///    GpuGateTimings::median_window completed frame times of that series). The render thread issues
///    no frame of a closed series, so a GPU that cannot keep up holds only the frames issued within
///    one age limit, and a healthy GPU (frames complete within a few frame times) never closes a
///    series.
///
/// The render thread never waits for a fence and never polls one: it inserts a fence, flushes, and
/// reads atomics that the watch thread publishes.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "qar_ogre_streaming/gpu_progress_gate.hpp"

namespace qar_qar_adapter
{

/// @brief The GL calls the fence waits and the watch make, as a table so that a test can replace
/// them (no GL in the unit tests). Every member is set or none is; see default_gl_fence_ops().
struct GlFenceOps
{
  /// Render thread, with the host's context current: inserts a fence behind the GL commands issued
  /// so far and flushes them. Never waits. @return the fence (opaque), or null.
  void * (*insert)() = nullptr;
  /// A thread whose context is bound (bind): waits at most @p timeout_ns for @p fence.
  /// @return 1 when it is signalled, 0 on a timeout, -1 on a failure.
  int (*wait)(void * fence, std::uint64_t timeout_ns) = nullptr;
  /// Deletes @p fence. Does not wait.
  void (*remove)(void * fence) = nullptr;
  /// Makes @p context current on the calling thread, once, when the thread starts.
  /// @return 0 on success, else the system error (never 0 for a failure).
  int (*bind)(void * hdc, void * context) = nullptr;
  /// Releases the context of the calling thread.
  void (*unbind)() = nullptr;
  /// Render thread: a context that shares the objects of @p share_with. @return null on failure.
  void * (*create_context)(void * hdc, void * share_with) = nullptr;
  /// Deletes a context that is current on no thread.
  void (*delete_context)(void * context) = nullptr;
  /// The system error of the last failed create_context() on the calling thread.
  int (*last_error)() = nullptr;

  /// True when every member is set.
  bool complete() const
  {
    return insert != nullptr && wait != nullptr && remove != nullptr && bind != nullptr &&
           unbind != nullptr && create_context != nullptr && delete_context != nullptr &&
           last_error != nullptr;
  }
};

/// @brief The WGL implementation; an incomplete table (nothing set) where there is no WGL.
const GlFenceOps & default_gl_fence_ops();

/// @brief A GL context that one frame worker binds for the fences of its sender. Created on the
/// render thread, bound once on the worker, deleted by the worker when it leaves. A context that
/// no worker bound is deleted by whoever drops the last reference; one that a worker bound and did
/// not release is never deleted.
class FenceContext
{
public:
  enum State { kFresh = 0, kBound = 1, kGone = 2 };

  FenceContext(const GlFenceOps & ops, void * hdc, void * context)
  : ops_(ops), hdc_(hdc), context_(context) {}
  ~FenceContext();

  FenceContext(const FenceContext &) = delete;
  FenceContext & operator=(const FenceContext &) = delete;

  void * hdc() const { return hdc_; }
  void * context() const { return context_; }
  int state() const { return state_.load(); }

  /// Worker thread: binds the context for good. @return 0 on success, else the system error.
  int bind();
  /// Worker thread: releases the context and deletes it. Idempotent.
  void release_and_delete();

private:
  GlFenceOps ops_;
  void * hdc_ = nullptr;
  void * context_ = nullptr;
  std::atomic<int> state_{kFresh};
};

/// @brief The limits of the progress gate. A test scales them.
struct GpuGateTimings
{
  /// The age limit of a series never falls below this.
  std::chrono::milliseconds min_age{50};
  /// The age limit is this many times the median completed frame time of the series.
  int age_factor = 3;
  /// The number of completed frames the median is taken over.
  int median_window = 60;
  /// The watch thread waits for a fence in slices of this length and looks at its state between
  /// them.
  std::chrono::milliseconds slice{50};
  /// QAR_GPU_GATE_TEST closes the gate this long after the first 3D batch.
  std::chrono::milliseconds test_start_after{30000};
  /// While a series is closed, RViz's frame hook inserts at most one probe fence per this time.
  std::chrono::milliseconds probe_interval{1000};
};

/// @brief True unless QAR_GPU_GATE is `off` (also 0, false, no). Read once.
bool default_gpu_gate_enabled();

/// @brief QAR_GPU_GATE_TEST in seconds: the gate is closed once, on purpose, for that long; 0 (the
/// default) when it is not set or not a positive number. Read once.
double default_gpu_gate_test_seconds();

/// @brief The line `[qar-gpu] GPU progress gate on (QAR_GPU_GATE=on): ...` or `... off ...`.
std::string gpu_gate_banner(bool enabled, const GpuGateTimings & timings = GpuGateTimings());

/// @brief Follows a fence of every frame the render thread issues, and closes the gate while the
/// GPU makes no progress.
///
/// A closed series also gets a probe fence: RViz's frame hook (rviz_gate_closed(), which runs with
/// RViz's context current) inserts one fence at most once per GpuGateTimings::probe_interval and
/// only when the previous probe has completed or failed. When a probe completes, the GPU
/// progresses: the fences issued before the probe that are still pending are dropped (deleted on
/// the watch thread), so a fence that never signals cannot hold the gate closed.
///
/// Lines (stderr):
///   `[qar-gpu] <hms> the GPU has not finished a frame for <ms> ms: RViz frames and the 3D render
///    batch pause`
///   `[qar-gpu] <hms> the GPU finishes frames again after <s> s; <n> RViz frame(s) and <m> render
///    batch(es) were skipped`
///   The first five closes print both lines; after that only every 100th close prints them, with
///   ` (close <n>)` appended to the first line.
///   `[qar-gpu] <hms> the GPU watch could not bind its GL context (err=<n>); the gate is off`
///   `[qar-gpu] <hms> the GPU watch could not wait for a fence; the gate is off` (once)
///   `[qar-gpu] <hms> a probe fence finished while <n> older fence(s) stayed unfinished; they were
///    dropped` (rate limited like the close lines)
class GpuProgressWatch : public qar_ogre_streaming::IGpuProgressGate
{
public:
  /// What a frame is: RViz's own frame, or the 3D render batch of the scheduler.
  enum class Kind { kRvizFrame, kBatch };

  /// @param enabled False: the watch never starts and the gate is always open.
  /// @param test_close_seconds QAR_GPU_GATE_TEST; 0 = off.
  GpuProgressWatch(
    const GlFenceOps & ops, bool enabled, const GpuGateTimings & timings = GpuGateTimings(),
    double test_close_seconds = 0.0);
  /// Calls stop(). Never waits for the thread.
  ~GpuProgressWatch() override;

  GpuProgressWatch(const GpuProgressWatch &) = delete;
  GpuProgressWatch & operator=(const GpuProgressWatch &) = delete;

  /// @brief Creates the context of the watch thread from the host's (@p hdc and @p share_with) and
  /// starts the thread. Render thread; once a GL context is latched. Does nothing when the watch
  /// is disabled or started, or when @p hdc or @p share_with is null.
  /// @return true when the thread was started.
  bool start(void * hdc, void * share_with);

  /// @brief Ends the watch: the gate stays open from then on. Never waits; the thread goes to a
  /// process-wide list that reap() joins once it has ended.
  void stop();

  /// @brief True from start() until stop(), unless the thread could not bind its context or a
  /// fence wait failed.
  bool active() const;

  /// @brief Render thread, after it issued a frame: inserts a fence and flushes (nothing is
  /// waited for) and queues the fence for the watch. Does nothing while the series of @p kind is
  /// closed, while the watch is not active, or when the table gives no fence.
  void note_frame_issued(Kind kind);

  /// @brief Render thread: true while the 3D render batch must not issue GL work. Reads atomics and
  /// the clock; never inserts a fence.
  bool gate_closed() override;

  /// @brief Render thread, in RViz's frameStarted (RViz's context current): true while RViz's own
  /// frame must not be rendered. Reads atomics and the clock; while the series is closed it may
  /// insert the probe fence (see the class description).
  bool rviz_gate_closed();

  /// @brief True while either series, or the QAR_GPU_GATE_TEST window, is closed. Reads atomics
  /// and the clock; never inserts a fence. For the work of the render thread that is not a frame
  /// (deleting contexts, creating senders), which waits for an open gate.
  bool any_gate_closed();

  /// @brief The age limit of the series of @p kind in milliseconds (a test reads it).
  std::int64_t age_limit_ms(Kind kind) const;

  /// @brief IGpuProgressGate: a batch was issued (note_frame_issued(Kind::kBatch)).
  void note_batch_issued() override { note_frame_issued(Kind::kBatch); }
  /// @brief IGpuProgressGate: a batch was skipped.
  void note_batch_skipped() override;
  /// @brief A frame of RViz was skipped because the gate was closed.
  void note_frame_skipped();

  /// Counters, any thread.
  std::uint64_t frames_issued() const;
  std::uint64_t frames_completed() const;
  std::uint64_t frames_pending() const;
  std::uint64_t skipped_frames() const;
  std::uint64_t skipped_batches() const;
  /// How often the gate closed.
  std::uint64_t closes() const;
  /// Probe fences inserted, and fences dropped after a probe completed.
  std::uint64_t probes() const;
  std::uint64_t dropped_fences() const;
  /// True once a fence wait of the watch failed (the watch then stopped following frames).
  bool wait_failed() const;

  /// @brief Joins the threads of stopped watches that have ended. Cheap; not from a watch thread.
  static void reap();
  /// @brief The number of watch threads of this process that have not ended (running or stopped).
  static int running_threads();

private:
  struct State;
  std::shared_ptr<State> state_;
  bool started_ = false;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__GPU_PROGRESS_WATCH_HPP_
