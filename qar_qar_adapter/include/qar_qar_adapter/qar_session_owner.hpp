// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// ===========================================================================
//  QarSessionOwner
// ===========================================================================
//
//  Owns the QAROS session of one program: loads the library, creates the runtime, joins the
//  QAROS Hub, keeps the list of render targets, and hands the open session to ONE consumer
//  thread. Everything that can wait runs on a thread of its own, so the consumer (RViz's GUI
//  thread, or the main thread of a console program) never blocks on QAROS to connect.
//
//  Threads
//    owner thread     every QAROS call of a command (library load and init, runtime create, the
//                     start of an onboarding, the peer subscription, teardown). It is joined in
//                     the destructor.
//    QAROS threads    result, progress and peer-update callbacks. They queue a message or copy a
//                     peer into the target table and return; they call no command.
//    consumer thread  poll(), the render senders, the panels, invite_device(). It uses the
//                     session from the drained kRunning event until disconnect(), forget() or the
//                     destructor, which the consumer calls after it released everything built on
//                     the session.
//
//  A QAROS call that can wait for the Hub (a query, a panel or volume change, a subscription) must
//  not run on a GUI thread. The consumer hands such a call to post_session_task(); the owner
//  thread runs the tasks one at a time, in the order they were posted, between its other
//  messages. A task is dropped, and its `dropped` function runs instead, when the session it was
//  posted for is gone or when the drain budget of a disconnect has run out. The owner thread
//  checks its timers after every task, so the 3 s grace of the hub-down timer and the retry timer
//  are late by at most the duration of one task, however many tasks are queued. The other
//  messages (the results of the onboarding, the presence of the Hub) are handled in posting
//  order, behind the tasks posted before them.
//
//  Two ways to join the Hub: a source app on the Hub's own PC (the Hub asks for approval once,
//  no code), or a Hub on another PC (a pairing code once, then a stored pairing).

#ifndef QAR_QAR_ADAPTER__QAR_SESSION_OWNER_HPP_
#define QAR_QAR_ADAPTER__QAR_SESSION_OWNER_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/onboarding_status.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"
#include "qar_qar_adapter/target_rule.hpp"

namespace qar_qar_adapter
{

/// @brief Where the owner is in joining the Hub.
enum class SessionState
{
  kIdle,                ///< Not connected; connect() starts.
  kLoading,             ///< Loading the library, creating the runtime, choosing the mode.
  kWaitingForApproval,  ///< This-PC mode: the Hub asks the person at the PC to allow the program.
  kRejoining,           ///< Another PC's Hub: joining again with the stored pairing.
  kNeedsCode,           ///< Another PC's Hub: waiting for host, port and code (submit_code()).
  kPairing,             ///< Another PC's Hub: pairing with the code.
  kRunning,             ///< Joined; the session is the consumer's.
  kReconnecting,        ///< Joined, but the Hub does not answer; the session stays valid.
  kStopping,            ///< Releasing the session.
  kFailed               ///< Stopped by an error; connect() tries again.
};

/// @brief A kind of device the Hub can start an app for.
enum class DeviceKind
{
  kHoloLens,   ///< `hololens:<host>`
  kHeadset     ///< `headset:<host>` (Meta Quest, Android glasses)
};

/// @brief Host and onboarding port of a Hub on another PC. The port has no default.
struct HubEndpoint
{
  std::string host;
  std::uint16_t port = 0;
};

/// @brief Everything the owner needs to know before connect().
struct SessionOwnerConfig
{
  HubMode mode = HubMode::kAuto;
  HubEndpoint remote;                       ///< Prefill for the remote mode.
  std::string program_tag;                  ///< "rviz2-plugin", ...: names the folders.
  std::string instance;                     ///< A second program of the same kind on one PC.
  std::string display_name;                 ///< Shown in the Hub's approval prompt.
  std::string app_version = "0.1.0";
  std::string identity_dir;                 ///< Empty: default_identity_dir(tag, instance).
  std::string library_path;                 ///< Empty: the vendor's lookup (library_load(NULL)).
  std::uint32_t approval_timeout_ms = 0;    ///< 0: the Hub's default; at most 600000.
  std::uint32_t max_targets = 8;            ///< How many targets get a stream.
  std::string log_folder_path;              ///< Library log folder; empty: TEMP/quaternar.
  QarLogSeverity log_severity = QAR_LOG_SEVERITY_TRACE;
  bool enable_console_logging = true;
  /// Receives each `QAROS: ...` log line. Empty: the line goes to stderr.
  std::function<void(const std::string &)> log_sink;
};

/// @brief One peer that is, or was, a render target.
struct TargetInfo
{
  QarPeerId id{};
  QarPeerPresence presence = QAR_PEER_PRESENCE_UNKNOWN;
  QarDeviceCategory category = QAR_DEVICE_CATEGORY_UNKNOWN;
  std::string device_id, device_family, device_name, display_name;
  bool served = true;                ///< False when the cap of served targets was reached.
  std::string not_served_reason;     ///< Why a target is not served.
};

/// @brief Something the owner reports to the consumer. poll() hands them out in order.
struct SessionEvent
{
  enum class Kind { kState, kProgress, kTargetUp, kTargetDown, kInviteDone, kForgetDone, kLog };
  Kind kind = Kind::kLog;
  SessionState state = SessionState::kIdle;   ///< kState.
  StatusText status;                          ///< kState (the cause), kInviteDone, kForgetDone.
  std::string text;                           ///< kProgress and kLog text; kInviteDone: the device.
  float percent = -1.0f;                      ///< kProgress; negative when unknown.
  TargetInfo target;                          ///< kTargetUp, kTargetDown, kInviteDone (its id).
};

/// @brief Hears about targets. Called on a QAROS thread: it may only queue (no QAROS call, no
/// blocking, no Qt).
class ITargetListener
{
public:
  virtual ~ITargetListener() = default;
  virtual void on_target_up(const TargetInfo & target) = 0;
  virtual void on_target_down(const QarPeerId & id, QarPeerPresence presence) = 0;
};

/// @brief What the owner needs from the process around the QAROS library: loading it, finding
/// the installed one, and the settings that must be in place before the library starts. The
/// default talks to the real library; a test replaces it.
class ILibraryLoader
{
public:
  virtual ~ILibraryLoader() = default;
  /// @brief qar_library_load: @p path_or_empty names the library file; empty selects the vendor's
  /// lookup.
  virtual bool load(const std::string & path_or_empty, std::string & out_error) = 0;
  /// @brief qar_library_unload.
  virtual void unload() = 0;
  /// @brief qar_installed_bin_folder: the `bin` folder of the installed QAROS as UTF-8.
  virtual bool installed_bin_folder(std::string & out_utf8) = 0;
  /// @brief Where the loaded library came from, for the log. Empty when unknown.
  virtual std::string loaded_from() { return std::string(); }
  /// @brief Makes the environment variable QAR_LOG_SEVERITY say @p severity, because the library
  /// obeys the variable over QarLibraryInit. @return the value the variable had.
  virtual std::string export_log_severity(LogSeverity severity)
  {
    (void)severity;
    return std::string();
  }
  /// @brief Starts the transport's own logger when QAR_ZENOH_LOG asks for it. Called after the
  /// library is loaded and before it starts.
  virtual void start_transport_logging(LogSeverity severity) { (void)severity; }
};

/// @brief The loader of the real library.
std::unique_ptr<ILibraryLoader> make_default_library_loader();

class QarSessionOwner : public IQarSessionOwner
{
public:
  explicit QarSessionOwner(
    SessionOwnerConfig config,
    std::unique_ptr<ILibraryLoader> loader = make_default_library_loader());
  /// @brief shutdown(): bounded, joins the owner thread. The consumer has released everything
  /// built on the session before this runs.
  ~QarSessionOwner() override;

  QarSessionOwner(const QarSessionOwner &) = delete;
  QarSessionOwner & operator=(const QarSessionOwner &) = delete;

  // --- commands: any thread, never block, never call QAROS on the caller's thread -----------

  /// @brief kIdle or kFailed to kLoading; ignored in every other state.
  void connect();
  /// @brief kNeedsCode to kPairing. A host that is empty or a port of 0 is refused: the state
  /// stays kNeedsCode and the event says why. The code is a secret: it is not stored or logged.
  void submit_code(std::string code, HubEndpoint hub);
  /// @brief Cancels the wait for approval, the rejoin or the pairing.
  void cancel();
  /// @brief kRunning to kStopping to kIdle. The stored pairing stays.
  void disconnect();
  /// @brief Disconnects, erases the stored pairing from the runtime and deletes the identity file.
  /// Remote mode only; ignored otherwise.
  void forget();
  /// @brief Sets the Hub mode that the next connect() uses and the remote host and port that the
  /// pairing starts from. The owner thread applies it. While the owner is joining, running or
  /// stopping the values are kept and applied at the next connect(); one log line says so. The
  /// owner never has to be destroyed to change them.
  void configure(HubMode mode, HubEndpoint remote);
  /// @brief Sets who hears about targets. A new listener first hears of the targets that have a
  /// stream. An empty listener removes it, and the call returns only after a delivery that is
  /// running has ended, so the caller may release the old listener right after. Consumer thread.
  /// The call blocks while a listener method runs; a listener method that calls it only swaps
  /// the listener.
  void set_target_listener(std::weak_ptr<ITargetListener> listener);

  // --- consumer thread --------------------------------------------------------------------

  /// @brief Moves the events since the last call into @p out (appends). Thread-safe. After it
  /// handed out a kRunning event, ready() is true.
  void poll(std::vector<SessionEvent> & out);
  SessionState state() const;
  HubMode effective_mode() const;
  bool has_remote_identity() const;
  /// @brief The targets that are here (thread-safe copy).
  std::vector<TargetInfo> targets() const;
  QarPeerId self_peer() const;
  /// @brief "0.9.0 release v0 from <path>" once the library is loaded; empty before.
  std::string runtime_description() const;
  /// @brief Asks the Hub of this PC to start the app of a device. Consumer thread, session
  /// running, this-PC mode only. @return empty when the request started (the answer arrives as
  /// kInviteDone), else the refusal in plain words.
  std::string invite_device(DeviceKind kind, const std::string & host);

  // IQarSessionOwner: ready() is true only in kRunning (and kReconnecting) after the consumer
  // drained the kRunning event; session() is valid on the consumer thread only.
  bool ready() const override;
  QarSession * session() const override;

  /// @brief Queues @p task for the owner thread (see IQarSessionOwner::post_session_task).
  bool post_session_task(
    const char * name, SessionTask task, std::function<void()> dropped = {}) override;

  /// @brief How the task queue is doing, for the heartbeat line.
  struct TaskStats
  {
    std::size_t queued = 0;       ///< Waiting, not counting the running one.
    std::string running;          ///< Name of the running task; empty when idle.
    double running_ms = 0.0;      ///< How long it has run so far.
    std::uint64_t done = 0;       ///< Tasks that ran.
    std::uint64_t dropped = 0;    ///< Tasks that were dropped unrun.
    std::string slowest;          ///< Name of the slowest task so far.
    double slowest_ms = 0.0;
  };
  /// @brief A thread-safe copy of the counters of the task queue.
  TaskStats task_stats() const;

  /// @brief How long the tasks that were posted before a disconnect, a forget or the destructor
  /// may keep running; a task whose turn comes later is dropped.
  static constexpr std::chrono::seconds kTaskDrainBudget{8};

private:
  friend struct QarSessionOwnerTestAccess;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  /// Runs inside poll() after it read the state and before it sets the flag of ready(); a test
  /// sets it to hold poll() at that point. Empty in a program.
  std::function<void()> poll_hook_;
  /// Runs inside post_session_task() after it accepted a task and before it queued it; a test sets
  /// it to hold the post at that point. Empty in a program.
  std::function<void()> task_post_hook_;
};

/// @brief For console programs, which may block: polls until the session runs, joining stops
/// at a failure or at a request for a code, or @p deadline passes.
/// @param seen When not null, receives every event polled.
/// @return the state when the wait ended.
SessionState wait_for_session(
  QarSessionOwner & owner, std::chrono::milliseconds deadline,
  std::vector<SessionEvent> * seen = nullptr);

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_SESSION_OWNER_HPP_
