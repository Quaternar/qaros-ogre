// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// What the supervisor thread of the session owner knows and decides, without a single QarOS call:
// the timings of an outage, two read-only hints about the Hub (its endpoint file and its process)
// and the pure policy that turns what the supervisor saw into a verdict.

#ifndef QAR_QAR_ADAPTER__OWNER_WATCH_HPP_
#define QAR_QAR_ADAPTER__OWNER_WATCH_HPP_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace qar_qar_adapter
{

/// @brief Every time limit of the outage handling in one place, so that a test scales them down.
struct OutageTimings
{
  /// One QarOS call on the owner thread this long: the Hub is silent.
  std::chrono::milliseconds call_stall{10000};
  /// One task on the owner thread this long: the Hub is silent.
  std::chrono::milliseconds task_stall{30000};
  /// Silent this long: the session is lost.
  std::chrono::milliseconds lost_after{20000};
  /// The same when the endpoint file or the Hub process is gone.
  std::chrono::milliseconds lost_after_hint{5000};
  /// A hint counts once it held this long.
  std::chrono::milliseconds hint_absent{3000};
  /// The Hub's RUNTIME peers stay gone this long: the Hub is silent.
  std::chrono::milliseconds peers_gone_grace{3000};
  /// Lost: the longest wait for release_lost_session().
  std::chrono::milliseconds hand_back_wait{10000};
  /// One call this long: the owner thread is left behind.
  std::chrono::milliseconds abandon_after{45000};
  /// Back-off of the first automatic attempt; it doubles.
  std::chrono::milliseconds rejoin_first{2000};
  /// The longest back-off.
  std::chrono::milliseconds rejoin_cap{30000};
  /// No two attempts start closer than this.
  std::chrono::milliseconds rejoin_spacing{2000};
  /// An attempt with no result is cancelled after this.
  std::chrono::milliseconds attempt_timeout{30000};
  /// Lost: the waits for the attempt and invite cleanup.
  std::chrono::milliseconds lost_operation_wait{2000};
  /// Destructor: tasks may run this long.
  std::chrono::milliseconds exit_drain{3000};
  /// Destructor: the waits for the attempt and invite cleanup.
  std::chrono::milliseconds exit_operation_wait{2000};
  /// Destructor: the longest wait for the owner thread and the supervisor thread; the rest of the
  /// panel teardown must fit in the five seconds that RViz has to end.
  std::chrono::milliseconds exit_join{3500};
  /// Before the session is destroyed: the longest wait for the frame workers to leave. The owner
  /// thread alone waits. A frame call of the installed runtime takes about two seconds, so a
  /// shorter wait would count a merely slow call as a worker that never returns.
  std::chrono::milliseconds frame_worker_settle{3000};
  /// Supervisor tick.
  std::chrono::milliseconds watch_period{500};
  /// Period of the endpoint file and process snapshot.
  std::chrono::milliseconds hint_period{2000};
  /// Owner threads that may be left behind per process; then the session fails.
  int max_abandoned_owners = 2;
  /// Attempt n >= this starts with a fresh runtime.
  int fresh_runtime_from_attempt = 3;
};

/// @brief One read-only look at the Hub of this PC: its endpoint file and its processes. The file
/// is never opened; only its status, size and modification time are read.
struct HubHints
{
  bool endpoint_known = false;
  bool endpoint_present = false;
  std::int64_t endpoint_mtime = 0;       ///< Ticks of the file clock; 0 when absent.
  std::uintmax_t endpoint_size = 0;
  std::string endpoint_written;          ///< `HH:MM:SS` of the modification; empty when absent.
  bool processes_known = false;          ///< False off Windows or when the snapshot failed.
  std::uint32_t hub_pid = 0;             ///< 0: not running.
  std::uint32_t launcher_pid = 0;
  std::uint32_t viz_pid = 0;
  std::uint32_t gui_pid = 0;
  double scan_ms = 0.0;                  ///< How long the reading took.
};

/// @brief The path of the Hub's endpoint file, or empty when the environment does not say.
std::string hub_endpoint_path();

/// @brief Reads the hints: a status call on the endpoint file and one process snapshot. No QarOS
/// call, no file is opened, no process is touched.
HubHints read_hub_hints();

/// @brief The log lines (without the `QAROS: ` prefix) for what changed between two readings.
/// The first reading (before.endpoint_known and before.processes_known false) reports what it
/// found.
std::vector<std::string> describe_hint_changes(const HubHints & before, const HubHints & now);

/// @brief What the supervisor saw at one tick.
struct WatchInput
{
  std::chrono::steady_clock::time_point now;
  bool joined = false;                   ///< kRunning or kReconnecting.
  bool lost = false;                     ///< kLost.
  std::string call;                      ///< The QarOS call the owner thread is inside, if any.
  std::int64_t call_ms = -1;             ///< How long; -1 when not inside a call.
  std::string task;                      ///< The task the owner thread runs, if any.
  std::int64_t task_ms = -1;             ///< How long; -1 when none runs.
  std::uint32_t silence = 0;             ///< HubSilence bits that are set.
  std::int64_t silent_ms = 0;            ///< Since the first silent bit was set.
  bool endpoint_gone_long = false;       ///< The endpoint file is missing for hint_absent.
  bool hub_process_gone_long = false;    ///< The Hub process is missing for hint_absent.
  bool hub_came_back = false;            ///< A hint says the Hub is there again.
};

/// @brief What the supervisor does about it.
struct WatchVerdict
{
  bool call_stalled = false;             ///< Set the call-stalled silence bit.
  bool lost_due = false;                 ///< Silent long enough: the session is lost.
  bool abandon_due = false;              ///< A call ran abandon_after: leave the owner thread.
  bool rejoin_now = false;               ///< Lost, and a hint says the Hub is back.
  std::string stalled_in;                ///< "qar_query_gui_panels" or "task <task name>".
  std::int64_t stalled_ms = 0;
};

/// @brief The decisions, as a pure function of the input: unit-tested without threads.
class OutagePolicy
{
public:
  explicit OutagePolicy(const OutageTimings & timings) : timings_(timings) {}
  WatchVerdict judge(const WatchInput & in) const;

private:
  OutageTimings timings_;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__OWNER_WATCH_HPP_
