// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__FRAME_WORKER_HPP_
#define QAR_QAR_ADAPTER__FRAME_WORKER_HPP_

/// @file frame_worker.hpp
/// @brief One thread per sender that makes the QarOS frame calls, so that the GUI and render
/// thread never waits for QarOS.
///
/// A sender (a 3D stream or a panel) hands its frame work to a FrameWorker as two jobs. The GUI
/// thread asks for them with kick() and hand_over(); both only set a flag under a mutex and wake
/// the worker, so they return at once however long the worker's current QarOS call takes.
///
/// Rules for the code that owns a worker:
///  - Every QarOS frame call of the sender (begin-frame, the frame-info getters, the hands, the
///    frame access, show-frame, the destroy of a frame info) happens inside a job, on the worker's
///    thread, written with QAR_CALL. No other thread makes one for this sender while the worker
///    lives.
///  - The jobs share state with the owner through an object they capture (shared). The worker never
///    touches the owner itself. That shared object is guarded by a mutex that no thread holds
///    across a QarOS call, because the GUI thread takes it too.
///  - A job never waits for the GUI thread, and never sleeps to pace itself: the worker is woken by
///    kick(), hand_over() and stop() only (a one millisecond sleep lasts about 16 ms on Windows).
///  - The handles of the sender belong to the worker. When the worker is told to stop, the `leave`
///    job posts their release on the lane (IReleaseLane::post_release) and does not call QarOS
///    itself. A worker that never returns from a call never runs `leave`: its handles stay
///    unreleased, which is the safe outcome.
///  - The GUI thread never joins a worker. stop() hands the thread to a process-wide reaper that
///    joins only threads that have ended.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief The limits of a frame worker. Tests scale them.
struct FrameWorkerTimings
{
  /// Two accepted kick() calls are at least this far apart; a closer one is dropped. The caller
  /// kicks again at its next tick.
  std::chrono::microseconds kick_min{500};
  /// A QarOS call of the worker that takes this long or longer is reported (the first five per
  /// call name, then one in a hundred).
  std::chrono::milliseconds slow_call{250};
  /// A call that has run this long is announced once as "waiting in <call>", and waiting() and
  /// stop() treat the worker as stuck from then on.
  std::chrono::milliseconds wait_notice{1000};
};

/// @brief How a frame worker words and writes its log lines. Every line reads
/// `<prefix> <HH:MM:SS.mmm> <name> <text>`.
struct FrameWorkerLog
{
  /// The tag in front of every line, for example `[qar-adapter]` or `[qar-panel-adapter]`.
  std::string prefix = "[qar-adapter]";
  /// What stays unreleased when the worker is left behind: `stream handle` or `video sender`.
  std::string handle = "stream handle";
  /// Receives each line (without a line end). Empty: standard error. It is called on the
  /// worker's thread and on the thread that calls kick(), hand_over() or stop().
  std::function<void(const std::string &)> sink;
};

/// @brief The thread that makes the QarOS frame calls of one sender.
///
/// Lines it writes, with `<head>` = `<prefix> <time> <name>`:
///   `<head> frame worker started tid=<n>`
///   `<head> frame call <call> took <ms> ms on the frame worker`
///   `<head> frame worker waiting in <call> for <s> s`
///   `<head> frame worker free again after <s> s`
///   `<head> frame worker left running inside <call> after <s> s; its <handle> stays unreleased`
class FrameWorker
{
public:
  /// @brief The work of the sender. Each job runs on the worker's thread, one at a time. A job
  /// reports its own failures: an exception that leaves a job is caught and dropped.
  struct Jobs
  {
    /// One poll: try-begin and, when a frame is ready, everything up to the published cycle.
    /// Runs after kick(). May be empty.
    std::function<void()> poll;
    /// The frame the GUI thread handed over: show it and prime the next one. Runs after
    /// hand_over(); it has priority over a poll. May be empty.
    std::function<void()> show;
    /// Leaving: release the handles by posting them on the lane. Runs once after stop() when the
    /// current job returned, never when the worker was left behind. May be empty.
    std::function<void()> leave;
  };

  /// @brief What waiting() reports.
  struct Wait
  {
    std::string call;      ///< The QarOS call the worker is inside.
    double seconds = 0.0;  ///< Since when.
  };

  /// @brief Registers the worker with @p lane and starts its thread. GUI thread (any thread).
  /// @param name    Names the sender in the log lines: `peer 9733c6bf`, `panel 'dock:Views'`.
  /// @param lane    The release lane of the owner (IQarSessionOwner::release_lane()). The owner
  ///   waits for the registered workers before it destroys the session. Null: a host without an
  ///   owner, nothing is registered.
  /// @param jobs    The work; copied.
  /// @param timings The limits.
  /// @param log     How the lines are worded and where they go.
  /// @return null when the lane refused (the owner's thread has ended or was left behind) or the
  ///   thread could not start: the caller then keeps the frame calls on its own thread.
  static std::shared_ptr<FrameWorker> start(
    std::string name, std::shared_ptr<IReleaseLane> lane, Jobs jobs,
    const FrameWorkerTimings & timings = FrameWorkerTimings(),
    FrameWorkerLog log = FrameWorkerLog());

  /// @brief Calls stop().
  ~FrameWorker();

  FrameWorker(const FrameWorker &) = delete;
  FrameWorker & operator=(const FrameWorker &) = delete;

  /// @brief Asks for one poll soon. GUI thread; never blocks and never calls QarOS. Calls are
  /// coalesced: one that follows an accepted call within FrameWorkerTimings::kick_min is dropped,
  /// so call it again at the next tick. It also announces a wait of the worker (see waiting()).
  void kick();

  /// @brief Asks for the `show` job. GUI thread; never blocks and never calls QarOS. Never
  /// dropped. The kicks that arrived before the job finishes are dropped with it: `show` primes
  /// the next frame itself.
  void hand_over();

  /// @brief Tells the worker to run no further job. GUI thread; never blocks, never joins. The
  /// `leave` job runs when the job in progress returns, unless the owner left the worker behind.
  /// The thread goes to the reaper. When the worker is inside a QarOS call that has run for
  /// FrameWorkerTimings::wait_notice or longer, the "left running" line is written and the module
  /// is pinned, because the thread may return into this code at any time. Safe to call again.
  void stop();

  /// @brief Whether stop() was called.
  bool stopped() const;

  /// @brief Whether the worker's thread has finished its last job and the `leave` job.
  bool exited() const;

  /// @brief Whether the owner left the worker behind: it makes no further QarOS call.
  bool abandoned() const;

  /// @brief Tells whether the worker has been inside one QarOS call for
  /// FrameWorkerTimings::wait_notice or longer, and for which call. Any thread; it never blocks.
  /// The first true answer of a wait also writes the "waiting in" line. Use it for the skip detail
  /// and the paused state of the sender.
  /// @param out Receives the call and the seconds when the answer is true. May be null.
  bool waiting(Wait * out = nullptr);

  /// @brief The QarOS call the worker is inside and since when. Any thread.
  const OwnerCallClock & clock() const;

  /// @brief How many poll and show jobs the worker has started.
  unsigned long long polls() const;
  unsigned long long shows() const;

  /// @brief The number of frame workers of this process whose thread has not ended.
  static int running();

  /// @brief Joins the threads of stopped workers that have ended. Cheap; any thread, but not the
  /// worker's own. Call it from a periodic tick of the sender's factory and when an owner ends.
  static void reap();

  /// @brief The number of stopped workers whose thread the reaper still holds (ended or not).
  static int parked();

private:
  struct State;
  explicit FrameWorker(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
  std::thread thread_;
  std::atomic<bool> stopped_{false};
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__FRAME_WORKER_HPP_
