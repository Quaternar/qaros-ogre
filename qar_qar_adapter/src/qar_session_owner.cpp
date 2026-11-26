// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_session_owner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "module_pin.hpp"

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

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::seconds kOperationWait{10};
constexpr std::chrono::milliseconds kBusyRetryDelay{1500};
constexpr std::uint32_t kMaxApprovalTimeoutMs = 600000;
constexpr std::uint32_t kDefaultApprovalTimeoutMs = 120000;
constexpr double kSlowTaskMs = 1000.0;
constexpr std::uint64_t kSlowTaskLogFirst = 20;
constexpr std::uint64_t kSlowTaskLogEvery = 50;
// Abandoning an owner waits this long for the supervisor to leave, then detaches it.
constexpr std::chrono::milliseconds kAbandonWatchWait{100};
// The least time the exit gives a thread that has ended to leave the OS.
constexpr std::chrono::milliseconds kExitLeaveGrace{150};

bool failed(const QarResult & result)
{
  return qar_result_is_error(result);
}

std::string result_message(const QarResult & result)
{
  char buffer[512] = {0};
  qar_result_message(result, buffer, sizeof(buffer) - 1);
  return std::string(buffer);
}

std::string peer_hex8(const QarPeerId & id)
{
  static const char * kHex = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 4; ++i) {
    out.push_back(kHex[(id.data[i] >> 4) & 0x0F]);
    out.push_back(kHex[id.data[i] & 0x0F]);
  }
  return out;
}

bool same_peer(const QarPeerId & a, const QarPeerId & b)
{
  return std::memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

const char * state_name(SessionState state)
{
  switch (state) {
    case SessionState::kIdle: return "idle";
    case SessionState::kLoading: return "loading";
    case SessionState::kWaitingForApproval: return "waiting_for_approval";
    case SessionState::kRunning: return "running";
    case SessionState::kReconnecting: return "reconnecting";
    case SessionState::kStopping: return "stopping";
    case SessionState::kFailed: return "failed";
    case SessionState::kLost: return "lost";
  }
  return "unknown";
}

const char * category_name(QarDeviceCategory category)
{
  switch (category) {
    case QAR_DEVICE_CATEGORY_DESKTOP_PC: return "desktop";
    case QAR_DEVICE_CATEGORY_SERVER: return "server";
    case QAR_DEVICE_CATEGORY_HEADSET: return "headset";
    case QAR_DEVICE_CATEGORY_VISUALIZER: return "visualizer";
    case QAR_DEVICE_CATEGORY_CAMERA: return "camera";
    case QAR_DEVICE_CATEGORY_OBSERVER: return "observer";
    case QAR_DEVICE_CATEGORY_UNKNOWN:
    default: return "unknown";
  }
}

const char * presence_name(QarPeerPresence presence)
{
  switch (presence) {
    case QAR_PEER_PRESENCE_ONLINE: return "online";
    case QAR_PEER_PRESENCE_OFFLINE: return "offline";
    case QAR_PEER_PRESENCE_LEAVING: return "leaving";
    case QAR_PEER_PRESENCE_RETIRED: return "retired";
    case QAR_PEER_PRESENCE_UNKNOWN:
    default: return "unknown";
  }
}

double seconds_since(Clock::time_point start)
{
  return std::chrono::duration<double>(Clock::now() - start).count();
}

long long millis_since(Clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string family_text(const std::string & text)
{
  return text.empty() ? std::string("-") : text;
}

// Owner threads this process left running, and how many of them have not returned yet.
std::atomic<int> g_abandoned_owners{0};
std::atomic<int> g_abandoned_running{0};
// Hands every owner its own range of attempt numbers, so that a late result of a thread that was
// left behind never matches an attempt of its replacement.
std::atomic<std::uint64_t> g_attempt_ranges{0};
constexpr std::uint64_t kAttemptRange = 1000000;

// The QarOS library belongs to the process, not to one owner: it is destroyed and unloaded by the
// last owner that uses it, and never once an owner thread was left behind.
struct LibraryLease
{
  std::mutex m;
  int users = 0;
  bool loaded_by_owner = false;
  bool initialised = false;
  std::atomic<bool> poisoned{false};
};

LibraryLease & library_lease()
{
  static LibraryLease lease;
  return lease;
}

#ifdef _WIN32
// A hardware fault inside a library call kills the host process before any error text can be
// shown. The two calls that start the library run under a structured-exception guard: the filter
// names the faulting module and the call returns with a failure instead.
LONG report_fault(struct _EXCEPTION_POINTERS * pointers, const char * where)
{
  const DWORD code = pointers->ExceptionRecord->ExceptionCode;
  void * address = pointers->ExceptionRecord->ExceptionAddress;
  char module_path[MAX_PATH] = "<unknown>";
  HMODULE module = nullptr;
  if (GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(address), &module) && module != nullptr)
  {
    GetModuleFileNameA(module, module_path, MAX_PATH);
  }
  std::fprintf(
    stderr, "[qar-adapter] *** SEH FAULT in %s: code=0x%08lX addr=%p module=%s\n", where,
    static_cast<unsigned long>(code), address, module_path);
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Returns 0 on a normal return (result in *out), 1 when the call faulted.
int guarded_library_init(const QarLibraryInit * init, QarResult * out)
{
  __try {
    *out = qar_library_init(init);
    return 0;
  } __except (report_fault(GetExceptionInformation(), "qar_library_init")) {
    return 1;
  }
}

int guarded_runtime_create(const QarRuntimeInit * init, QarRuntime ** runtime, QarResult * out)
{
  __try {
    *out = qar_runtime_create(init, runtime);
    return 0;
  } __except (report_fault(GetExceptionInformation(), "qar_runtime_create")) {
    return 1;
  }
}
#endif  // _WIN32

// ---------------------------------------------------------------------------------------------
// The loader of the real library
// ---------------------------------------------------------------------------------------------

class DefaultLibraryLoader : public ILibraryLoader
{
public:
  bool load(const std::string & path_or_empty, std::string & out_error) override
  {
    if (qar_library_load(path_or_empty.empty() ? nullptr : path_or_empty.c_str())) {
      return true;
    }
    out_error = path_or_empty.empty() ?
      std::string("no QAROS runtime was found (see the lookup lines above)") :
      "qar_library_load failed for '" + path_or_empty + "'";
    return false;
  }

  void unload() override { qar_library_unload(); }

  bool installed_bin_folder(std::string & out_utf8) override
  {
    char buffer[1024] = {0};
    if (!qar_installed_bin_folder(buffer, sizeof(buffer))) {
      return false;
    }
    out_utf8 = buffer;
    return true;
  }

  std::string loaded_from() override
  {
#ifdef _WIN32
    wchar_t wide[MAX_PATH] = {0};
    if (g_qar_dynamic_library_handle == nullptr ||
      GetModuleFileNameW(g_qar_dynamic_library_handle, wide, MAX_PATH) == 0)
    {
      return std::string();
    }
    char utf8[4 * MAX_PATH] = {0};
    if (WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr) <= 0)
    {
      return std::string();
    }
    return std::string(utf8);
#else
    return std::string();
#endif
  }

  // The library obeys the QAR_LOG_SEVERITY variable over QarLibraryInit, so a stale value in the
  // environment beats the request. The variable is read through the C runtime's table, which only
  // _putenv keeps in step with; a bare Win32 write has no effect. The processes the runtime starts
  // inherit the value.
  std::string export_log_severity(LogSeverity severity) override
  {
    std::string previous;
#ifdef _WIN32
    char * before = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&before, &length, "QAR_LOG_SEVERITY") == 0 && before != nullptr) {
      previous = before;
      std::free(before);
    }
    _putenv_s("QAR_LOG_SEVERITY", log_severity_name(severity));
#else
    const char * before = std::getenv("QAR_LOG_SEVERITY");
    previous = (before != nullptr) ? before : "";
    setenv("QAR_LOG_SEVERITY", log_severity_name(severity), 1);
#endif
    return previous;
  }

  // The transport reports why a session fails (a listener that cannot open, a multicast group
  // that cannot be joined) only in its own log, and that logger starts only when the host calls
  // it. QAR_ZENOH_LOG asks for it, and only a trace or debug severity allows it: at debug the
  // transport writes thousands of lines a minute.
  void start_transport_logging(LogSeverity severity) override
  {
    const std::string filter_text = read_environment("QAR_ZENOH_LOG");
    const char * const filter = filter_text.c_str();
    if (filter_text.empty() || *filter == '\0') {
      std::fprintf(stderr, "[qar-adapter] zenoh logging: off (QAR_ZENOH_LOG not set)\n");
      std::fflush(stderr);
      return;
    }
    if (!zenoh_logging_allowed(severity)) {
      std::fprintf(
        stderr,
        "[qar-adapter] zenoh logging: OFF - QAR_ZENOH_LOG='%s' is ignored because the library "
        "log severity is %s; use severity debug or trace to get it\n",
        filter, log_severity_name(severity));
      std::fflush(stderr);
      return;
    }
#ifdef _WIN32
    HMODULE transport = GetModuleHandleA("zenohc.dll");
    if (transport == nullptr) {
      transport = LoadLibraryA("zenohc.dll");
    }
    if (transport == nullptr) {
      std::fprintf(stderr, "[qar-adapter] QAR_ZENOH_LOG set but zenohc.dll is not loaded\n");
      std::fflush(stderr);
      return;
    }
    using InitLogOr = void (*)(const char *);
    auto init_log = reinterpret_cast<InitLogOr>(
      reinterpret_cast<void *>(::GetProcAddress(transport, "zc_init_log_from_env_or")));
    if (init_log == nullptr) {
      std::fprintf(stderr, "[qar-adapter] zenohc.dll has no zc_init_log_from_env_or\n");
      std::fflush(stderr);
      return;
    }
    init_log(filter);
    std::fprintf(stderr, "[qar-adapter] zenoh logging enabled (filter=%s)\n", filter);
    std::fflush(stderr);
#endif
  }
};

// ---------------------------------------------------------------------------------------------
// Messages of the owner thread
// ---------------------------------------------------------------------------------------------

struct Msg
{
  enum class Type
  {
    // commands (kTask: a call of the consumer, run in order with the others; kRelease: handles
    // to release on this thread)
    kConnect, kCancel, kDisconnect, kShutdown, kTask,
    kRelease, kHandBack, kRejoinNow, kDeclareLost, kTick,
    // from QAROS threads
    kOnboardResult, kProgress, kInviteResult, kHubPresence
  };
  Type type = Type::kConnect;
  // kOnboardResult, kProgress
  std::uint64_t attempt = 0;
  QarResult result{};
  QarSession * session = nullptr;
  QarActionSeverity severity = QAR_ACTION_SEVERITY_INFO;
  float percent = -1.0f;
  std::string text;
  // kInviteResult
  std::uint64_t invite = 0;
  QarPeerId peer{};
  bool has_peer = false;
  // kHubPresence
  bool hub_down = false;
  // kTask
  std::string task_name;
  std::function<void(QarSession *)> task;
  std::function<void()> dropped;
  std::uint64_t task_epoch = 0;
  // kRelease
  std::function<void()> release;
};

// True on a thread that is inside a call of the target listener. Such a thread already holds the
// delivery lock, so set_target_listener() must not wait for it there.
thread_local bool t_in_listener = false;

struct ListenerScope
{
  ListenerScope() { t_in_listener = true; }
  ~ListenerScope() { t_in_listener = false; }
  ListenerScope(const ListenerScope &) = delete;
  ListenerScope & operator=(const ListenerScope &) = delete;
};

bool is_command(Msg::Type type)
{
  return type == Msg::Type::kConnect || type == Msg::Type::kCancel ||
         type == Msg::Type::kDisconnect || type == Msg::Type::kShutdown ||
         type == Msg::Type::kTask ||
         type == Msg::Type::kRelease || type == Msg::Type::kHandBack ||
         type == Msg::Type::kRejoinNow || type == Msg::Type::kDeclareLost ||
         type == Msg::Type::kTick;
}

}  // namespace

std::unique_ptr<ILibraryLoader> make_default_library_loader()
{
  return std::make_unique<DefaultLibraryLoader>();
}

// ---------------------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------------------

struct QarSessionOwner::Impl
{
  struct OpCtx;
  struct InviteOp;

  struct TargetEntry
  {
    TargetInfo info;
    bool present = false;
  };

  struct Notify
  {
    bool up = false;
    TargetInfo info;
    QarPeerPresence presence = QAR_PEER_PRESENCE_UNKNOWN;
  };

  // What one peer update changed: the calls for the listener and the log lines. The lines are
  // written after targets_mutex is released, because the log sink may be slow.
  struct Changes
  {
    std::vector<Notify> notify;
    std::vector<std::string> lines;
  };

  struct PeerSnapshot
  {
    QarPeerId id{};
    QarPeerRole role = QAR_PEER_ROLE_INTERNAL;
    TargetInfo info;
  };

  // One onboarding attempt. The init blocks and the strings they point at live here, because
  // the library may read them until the result callback has fired.
  struct OpCtx
  {
    Impl * impl = nullptr;
    std::uint64_t attempt = 0;
    QarOnboardInit onboard{};
    QarOnboardLocalAppExt local{};
    std::string display_name, app_version, custom_info;
    QarCancelToken * token = nullptr;
    bool cancel_requested = false;
    Clock::time_point started = Clock::now();
  };

  struct InviteCtx
  {
    Impl * impl = nullptr;
    std::uint64_t id = 0;
  };

  struct InviteOp
  {
    std::uint64_t id = 0;
    QarCancelToken * token = nullptr;
    std::string kind_text, host, connection;
    QarSessionInviteTargetAppInit init{};
    InviteCtx ctx;
    bool done = false;
  };

  // The lane that lets other threads release handles on the owner thread. It outlives the Impl
  // and refuses once the Impl is closed. It also counts the frame workers that still use handles
  // of the session, so that the owner waits for them before it destroys the session.
  struct Lane : public IReleaseLane
  {
    mutable std::mutex m;
    std::condition_variable workers_cv;
    Impl * impl = nullptr;
    int workers = 0;
    std::vector<std::weak_ptr<WorkerSlot>> slots;

    bool post_release(const char * name, std::function<void()> release) override
    {
      if (!release) {
        return false;
      }
      std::lock_guard<std::mutex> lock(m);
      if (impl == nullptr) {
        return false;
      }
      Msg message;
      message.type = Msg::Type::kRelease;
      message.task_name = name != nullptr ? name : "release";
      message.release = std::move(release);
      impl->post(std::move(message));
      return true;
    }

    bool enter_worker(const std::shared_ptr<WorkerSlot> & slot) override
    {
      std::lock_guard<std::mutex> lock(m);
      if (impl == nullptr) {
        return false;
      }
      slots.erase(
        std::remove_if(slots.begin(), slots.end(),
        [](const std::weak_ptr<WorkerSlot> & held) { return held.expired(); }),
        slots.end());
      ++workers;
      if (slot) {
        slots.push_back(slot);
      }
      return true;
    }

    void leave_worker() override
    {
      {
        std::lock_guard<std::mutex> lock(m);
        if (workers > 0) {
          --workers;
        }
      }
      workers_cv.notify_all();
    }

    // A worker that was given up on (see give_up_workers()) is not in the list any more: its
    // leave changes no count.
    void leave_worker(const std::shared_ptr<WorkerSlot> & slot) override
    {
      if (!slot) {
        leave_worker();
        return;
      }
      {
        std::lock_guard<std::mutex> lock(m);
        const auto it = std::find_if(slots.begin(), slots.end(),
          [&slot](const std::weak_ptr<WorkerSlot> & held) { return held.lock() == slot; });
        if (it != slots.end()) {
          slots.erase(it);
          if (workers > 0) {
            --workers;
          }
        }
      }
      workers_cv.notify_all();
    }

    int workers_alive() const override
    {
      std::lock_guard<std::mutex> lock(m);
      return workers;
    }

    // Owner thread: waits until every worker has left, or @p wait has passed.
    // @return the number of workers still registered.
    int wait_workers(std::chrono::milliseconds wait)
    {
      std::unique_lock<std::mutex> lock(m);
      workers_cv.wait_for(lock, wait, [this] { return workers == 0; });
      return workers;
    }

    // The owner leaves the session of the registered workers unreleased: from here on none of them
    // makes a QarOS call, and none is counted (a worker that returns later changes no count).
    // @return how many were registered.
    int give_up_workers()
    {
      std::lock_guard<std::mutex> lock(m);
      const int given_up = workers;
      for (const std::weak_ptr<WorkerSlot> & held : slots) {
        if (const std::shared_ptr<WorkerSlot> slot = held.lock()) {
          slot->abandoned.store(true);
        }
      }
      slots.clear();
      workers = 0;
      return given_up;
    }

    // From here on no registered worker makes a QarOS call.
    void abandon_workers()
    {
      std::lock_guard<std::mutex> lock(m);
      for (const std::weak_ptr<WorkerSlot> & held : slots) {
        if (const std::shared_ptr<WorkerSlot> slot = held.lock()) {
          slot->abandoned.store(true);
        }
      }
    }

    void close()
    {
      std::lock_guard<std::mutex> lock(m);
      impl = nullptr;
      for (const std::weak_ptr<WorkerSlot> & held : slots) {
        if (const std::shared_ptr<WorkerSlot> slot = held.lock()) {
          slot->abandoned.store(true);
        }
      }
    }
  };

  // How an Impl starts: normally idle, or in the state a replacement takes over from a thread
  // that was left behind.
  enum class Start { kNormal, kLost, kIdle, kFailed };

  // --- constants of the program -----------------------------------------------------------
  SessionOwnerConfig cfg;
  OutageTimings timings;
  OutagePolicy policy;
  std::shared_ptr<ILibraryLoader> loader;
  std::shared_ptr<Lane> lane = std::make_shared<Lane>();

  // --- state seen by every thread -----------------------------------------------------------
  std::atomic<SessionState> state{SessionState::kIdle};
  std::atomic<bool> consumer_ready{false};
  // Tasks are accepted from the hand-over of the session (poll()) until a disconnect or the
  // destructor is called, or the state leaves running. `task_closing` is set on the caller's
  // thread by those two, so a task cannot be posted after them even while the session is up.
  std::atomic<bool> accepting_tasks{false};
  std::atomic<bool> task_closing{false};
  // Counts up on the owner thread each time a session is released. A task carries the number it
  // was posted under and runs only under the same one.
  std::atomic<std::uint64_t> session_epoch{0};
  // Steady-clock count after which a task whose turn comes is dropped; 0 is "not armed".
  std::atomic<Clock::rep> drain_deadline{0};
  std::atomic<std::size_t> tasks_queued{0};
  mutable std::mutex task_stats_mutex;
  QarSessionOwner::TaskStats task_stats_data;
  Clock::time_point task_running_since{};
  std::map<std::string, std::uint64_t> slow_task_counts;
  // Owner thread only: how often each QarOS call was reported as slow.
  std::map<std::string, std::uint64_t> slow_call_counts;
  std::atomic<QarSession *> session_for_consumer{nullptr};

  // --- the outage handling ----------------------------------------------------------------------
  // What the owner thread is inside, for the supervisor.
  OwnerCallClock call_clock;
  // Why the Hub is silent. The supervisor and the owner thread both change the bits; only the
  // owner thread moves the state anywhere but between kRunning and kReconnecting.
  mutable std::mutex silence_mutex;
  std::uint32_t silence_bits = 0;
  Clock::time_point silent_since{};
  bool silent_active = false;
  std::string silent_note;          // the call or task the owner thread is stuck in
  std::int64_t silent_note_ms = 0;
  // Hints, written by the supervisor, read by the owner thread.
  std::atomic<bool> endpoint_gone_long{false};
  std::atomic<bool> hub_process_gone_long{false};
  std::atomic<bool> hub_back_pending{false};
  /// The Hub process as the watch saw it; see HubProcessInfo.
  std::atomic<bool> hub_pid_known{false};
  std::atomic<std::uint32_t> hub_pid_now{0};
  std::atomic<std::uint32_t> hub_gone_count{0};
  std::atomic<std::uint32_t> hub_last_gone_pid{0};
  std::atomic<bool> abandoned{false};
  std::atomic<bool> abandon_requested{false};
  // A person's disconnect or cancel: how many were posted and how many the owner thread
  // finished. The difference is what a stuck thread never ran.
  std::atomic<int> disconnects_posted{0};
  std::atomic<int> disconnects_done{0};
  std::atomic<int> cancels_posted{0};
  std::atomic<int> cancels_done{0};
  std::atomic<bool> exiting{false};
  std::atomic<bool> tick_pending{false};
  std::atomic<std::int64_t> drain_budget_ms{0};
  std::atomic<std::int64_t> abandoned_in_ms{0};
  // The owner thread has left run(). Guarded by finished_mutex together with `abandoned`.
  std::mutex finished_mutex;
  std::condition_variable finished_cv;
  bool owner_done = false;
  std::thread watch_thread;
  std::mutex watch_mutex;
  std::condition_variable watch_cv;
  bool watch_stop = false;
  // The supervisor has left watch(). Guarded by watch_mutex; watch_done_cv wakes the waiters.
  bool watch_done = false;
  std::condition_variable watch_done_cv;
  // The automatic rejoin as the consumer sees it.
  mutable std::mutex rejoin_mutex;
  RejoinInfo rejoin_view;
  Clock::time_point rejoin_next_at{};
  bool rejoin_next_armed = false;
  Clock::time_point lost_since_view{};
  bool lost_view_active = false;
  // Guards abandoned_in and abandoned_in_ms, which are written before abandon_requested is set.
  std::mutex abandon_mutex;
  std::string abandoned_in;

  // --- the inbox of the owner thread ---------------------------------------------------------
  std::mutex inbox_mutex;
  std::condition_variable inbox_cv;
  std::deque<Msg> inbox;

  // --- events ---------------------------------------------------------------------------
  mutable std::mutex events_mutex;
  std::vector<SessionEvent> events;

  // --- targets ---------------------------------------------------------------------------
  mutable std::mutex targets_mutex;
  std::vector<TargetEntry> table;
  std::map<std::string, bool> runtime_peers_gone;   // peer id hex -> gone
  QarPeerId self{};
  bool has_self = false;
  bool hub_down_reported = false;
  std::mutex listener_mutex;
  std::weak_ptr<ITargetListener> listener;
  // Held while the listener is called: by a delivery of a peer update and by the replay of
  // set_target_listener(). The owner thread never waits on it while it ends the peer
  // subscription, so a blocked listener cannot stall the teardown.
  std::mutex delivery_mutex;

  // --- the hand-over of the session -------------------------------------------------------------
  // state, consumer_ready and session_for_consumer change together under this mutex, so that
  // poll() cannot declare the consumer ready from a state that has already moved on.
  std::mutex ready_mutex;

  // --- text ---------------------------------------------------------------------------------
  mutable std::mutex description_mutex;
  std::string description;

  // --- invites (consumer thread starts them, the owner thread finishes them) -------------------
  std::mutex invites_mutex;
  std::vector<std::unique_ptr<InviteOp>> invites;
  // Invites whose start failed synchronously. The library may still read the context, so it
  // stays allocated until the owner is destroyed.
  std::vector<std::unique_ptr<InviteOp>> refused_invites;
  std::uint64_t next_invite = 1;

  // --- owner thread only ---------------------------------------------------------------------
  std::thread thread;
  QarRuntime * runtime = nullptr;
  QarSession * session = nullptr;
  QarCancelToken * peer_token = nullptr;
  std::unique_ptr<OpCtx> op;
  // Attempts whose start failed synchronously; kept for the same reason as refused_invites.
  std::vector<std::unique_ptr<OpCtx>> refused_attempts;
  std::uint64_t next_attempt = 1;
  bool leased = false;
  bool leak_on_destroy = false;
  // Owner thread only: a session was left unreleased under frame workers that never returned, so
  // its runtime must not be destroyed either.
  bool left_session_behind = false;
  bool busy_retried = false;
  bool retry_armed = false;
  Clock::time_point retry_at{};
  bool hub_down_armed = false;
  Clock::time_point hub_down_at{};
  Clock::time_point joining_started{};
  // The automatic rejoin (owner thread only).
  enum class LostPhase { kNone, kAwaitHandBack, kWaitRejoin, kAttempt };
  LostPhase lost_phase = LostPhase::kNone;
  Clock::time_point hand_back_at{};
  Clock::time_point lost_since{};
  Clock::time_point rejoin_at{};
  Clock::time_point last_attempt_start{};
  bool rejoin_asap = false;
  int rejoin_attempt = 0;
  bool auto_attempt = false;
  Clock::time_point attempt_deadline{};
  bool attempt_deadline_armed = false;
  std::string lost_cause_text;

  Impl(
    SessionOwnerConfig config, std::shared_ptr<ILibraryLoader> library_loader,
    Start start = Start::kNormal, const std::string & start_text = std::string())
  : cfg(std::move(config)), timings(cfg.timings), policy(cfg.timings),
    loader(std::move(library_loader))
  {
    lane->impl = this;
    next_attempt = 1 + kAttemptRange * (g_attempt_ranges.fetch_add(1) + 1);
    if (start == Start::kLost) {
      begin_lost(start_text, true);
    } else if (start == Start::kFailed) {
      StatusText cause = describe_status(kStatusOwnerAbandoned, OnboardingContext::kLocal, nullptr);
      if (!start_text.empty()) {
        cause.message = start_text;
      }
      set_state(SessionState::kFailed, &cause);
    } else if (start == Start::kIdle) {
      log("owner replaced: " + start_text);
    }
    thread = std::thread([this] { run(); });
    watch_thread = std::thread([this] { watch(); });
  }

  // =========================================================================================
  // Logging and events
  // =========================================================================================

  // Writes a line even when this owner was left behind (the caller that left it says so).
  void log_raw(const std::string & text)
  {
    const std::string line = "QAROS: " + text;
    if (cfg.log_sink) {
      cfg.log_sink(line);
    } else {
      std::fprintf(stderr, "%s\n", line.c_str());
      std::fflush(stderr);
    }
  }

  // A thread that was left behind writes nothing: the host may be gone when its call returns.
  void log(const std::string & text)
  {
    if (!abandoned.load()) {
      log_raw(text);
    }
  }

  void push(SessionEvent event)
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(std::move(event));
  }

  // Caller holds ready_mutex.
  void store_state_locked(SessionState next)
  {
    state.store(next);
    if (next != SessionState::kRunning && next != SessionState::kReconnecting) {
      consumer_ready.store(false);
      accepting_tasks.store(false);
    }
  }

  void store_state(SessionState next)
  {
    std::lock_guard<std::mutex> lock(ready_mutex);
    store_state_locked(next);
  }

  // The state and its event change together under ready_mutex, so that the supervisor, which
  // moves the state between kRunning and kReconnecting, cannot put its event in between. The
  // event goes first: a thread that reads the state without the lock and sees it change finds
  // the event in the queue at its next poll().
  void set_state(SessionState next, const StatusText * cause = nullptr, bool quiet = false)
  {
    SessionEvent event;
    event.kind = SessionEvent::Kind::kState;
    event.state = next;
    if (cause != nullptr) {
      event.status = *cause;
    }
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      push(std::move(event));
      store_state_locked(next);
    }
    if (quiet) {
      return;
    }
    log(std::string("state ") + state_name(next));
    if (cause != nullptr && !cause->message.empty() && cause->code != 0) {
      log("status " + cause->symbol + ": " + cause->message +
        (cause->detail.empty() ? std::string() : " (" + cause->detail + ")"));
    }
  }

  void post(Msg message)
  {
    {
      std::lock_guard<std::mutex> lock(inbox_mutex);
      inbox.push_back(std::move(message));
    }
    inbox_cv.notify_all();
  }

  // =========================================================================================
  // The inbox
  // =========================================================================================

  // Takes the next message. With @p results_only a command stays in the queue. Returns false
  // when none arrived by @p deadline (or when no deadline is set and the wait was woken).
  bool pop(Msg & out, bool results_only, bool has_deadline, Clock::time_point deadline)
  {
    std::unique_lock<std::mutex> lock(inbox_mutex);
    for (;;) {
      for (auto it = inbox.begin(); it != inbox.end(); ++it) {
        if (results_only && is_command(it->type)) {
          continue;
        }
        out = std::move(*it);
        inbox.erase(it);
        return true;
      }
      if (has_deadline) {
        if (inbox_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
          for (auto it = inbox.begin(); it != inbox.end(); ++it) {
            if (results_only && is_command(it->type)) {
              continue;
            }
            out = std::move(*it);
            inbox.erase(it);
            return true;
          }
          return false;
        }
      } else {
        inbox_cv.wait(lock);
      }
    }
  }

  // Names a QarOS call of this thread that took half a second or longer, so that the log says
  // which call a slow task spent its time in. Rate limited per call name like the slow tasks.
  void report_slow_call(const char * call, double took_ms)
  {
    const std::uint64_t count = ++slow_call_counts[call];
    if (count <= kSlowTaskLogFirst || (count % kSlowTaskLogEvery) == 0) {
      log(std::string("call ") + call + " took " +
        std::to_string(static_cast<long long>(took_ms)) + " ms");
    }
  }

  // The earliest moment at which on_timers() has something to do. False when there is none.
  bool next_deadline(Clock::time_point & out)
  {
    bool has = false;
    auto consider = [&](bool armed, Clock::time_point at) {
        if (armed && (!has || at < out)) {
          out = at;
          has = true;
        }
      };
    consider(retry_armed, retry_at);
    consider(hub_down_armed, hub_down_at);
    consider(attempt_deadline_armed, attempt_deadline);
    const SessionState current = state.load();
    if (current == SessionState::kReconnecting) {
      std::lock_guard<std::mutex> lock(silence_mutex);
      if (silent_active) {
        const bool hint = endpoint_gone_long.load() || hub_process_gone_long.load();
        consider(true, silent_since + (hint ? timings.lost_after_hint : timings.lost_after));
      }
    } else if (current == SessionState::kLost) {
      consider(lost_phase == LostPhase::kAwaitHandBack, hand_back_at);
      consider(lost_phase == LostPhase::kWaitRejoin, rejoin_at);
    }
    return has;
  }

  void run()
  {
    mark_owner_thread(
      [this](const char * call, double took_ms) { report_slow_call(call, took_ms); },
      &call_clock, &abandoned);
    for (;;) {
      if (abandoned.load()) {
        break;
      }
      Msg message;
      Clock::time_point deadline{};
      const bool has_deadline = next_deadline(deadline);
      if (!pop(message, false, has_deadline, deadline)) {
        on_timers();
        continue;
      }
      if (message.type == Msg::Type::kShutdown) {
        shutdown();
        if (!abandoned.load()) {
          drop_queued_tasks("the owner ended");
        }
        break;
      }
      handle(message);
      if (abandoned.load()) {
        break;
      }
      // A backlog of tasks never keeps the timers from firing: they are due after at most one
      // message.
      on_timers();
    }
    mark_owner_thread({}, nullptr, nullptr);
    // The lane refuses from here on; what was posted before that still runs.
    lane->close();
    if (!abandoned.load()) {
      run_queued_releases();
    }
    bool left_behind = false;
    {
      std::lock_guard<std::mutex> lock(finished_mutex);
      owner_done = true;
      left_behind = abandoned.load();
    }
    finished_cv.notify_all();
    if (left_behind) {
      g_abandoned_running.fetch_sub(1);
    }
  }

  void handle(Msg & message)
  {
    switch (message.type) {
      case Msg::Type::kConnect: on_connect(); break;
      case Msg::Type::kCancel: on_cancel(); break;
      case Msg::Type::kDisconnect: on_disconnect(); break;
      case Msg::Type::kOnboardResult: on_onboard_result(message); break;
      case Msg::Type::kProgress: on_progress(message); break;
      case Msg::Type::kInviteResult: on_invite_result(message); break;
      case Msg::Type::kHubPresence: on_hub_presence(message); break;
      case Msg::Type::kShutdown: break;
      case Msg::Type::kTask: on_task(message); break;
      case Msg::Type::kRelease: on_release(message); break;
      case Msg::Type::kHandBack: on_hand_back(false); break;
      case Msg::Type::kRejoinNow: on_rejoin_now(); break;
      case Msg::Type::kDeclareLost: on_declare_lost(message); break;
      case Msg::Type::kTick: tick_pending.store(false); break;
    }
    // A thread that was left behind did not finish what it was doing: its blocked calls only
    // looked like an end.
    if (abandoned.load()) {
      return;
    }
    switch (message.type) {
      case Msg::Type::kDisconnect: disconnects_done.fetch_add(1); break;
      case Msg::Type::kCancel: cancels_done.fetch_add(1); break;
      default: break;
    }
  }

  // =========================================================================================
  // Tasks of the consumer
  // =========================================================================================

  void arm_drain(std::chrono::milliseconds budget)
  {
    drain_budget_ms.store(budget.count());
    drain_deadline.store((Clock::now() + budget).time_since_epoch().count());
  }

  void arm_drain()
  {
    arm_drain(std::chrono::duration_cast<std::chrono::milliseconds>(
        QarSessionOwner::kTaskDrainBudget));
  }

  bool drain_expired() const
  {
    const Clock::rep deadline = drain_deadline.load();
    return deadline != 0 && Clock::now().time_since_epoch().count() > deadline;
  }

  // Runs the `dropped` function of a task that will not run. Owner thread.
  void drop_task(Msg & message, const std::string & why)
  {
    {
      std::lock_guard<std::mutex> lock(task_stats_mutex);
      ++task_stats_data.dropped;
    }
    log("task '" + message.task_name + "' dropped (" + why + ")");
    if (message.dropped) {
      try {
        message.dropped();
      } catch (...) {
        log("task '" + message.task_name + "': the dropped function threw");
      }
    }
  }

  // Drops every task still in the inbox. Owner thread, with no session to run them on.
  void drop_queued_tasks(const char * why)
  {
    std::vector<Msg> tasks;
    {
      std::lock_guard<std::mutex> lock(inbox_mutex);
      for (auto it = inbox.begin(); it != inbox.end();) {
        if (it->type == Msg::Type::kTask) {
          tasks.push_back(std::move(*it));
          it = inbox.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Msg & task : tasks) {
      tasks_queued.fetch_sub(1);
      drop_task(task, why);
    }
  }

  void on_task(Msg & message)
  {
    tasks_queued.fetch_sub(1);
    if (message.task_epoch != session_epoch.load() || session == nullptr ||
      state.load() == SessionState::kLost)
    {
      drop_task(message, "the session it was posted for ended");
      return;
    }
    if (drain_expired()) {
      drop_task(message, "teardown: drain budget " +
        seconds_text(std::chrono::milliseconds(drain_budget_ms.load())) + " s spent");
      return;
    }
    const Clock::time_point start = Clock::now();
    {
      std::lock_guard<std::mutex> lock(task_stats_mutex);
      task_stats_data.running = message.task_name;
      task_running_since = start;
    }
    try {
      message.task(session);
    } catch (...) {
      log("task '" + message.task_name + "' ended with an exception");
    }
    const double took_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    bool log_it = false;
    {
      std::lock_guard<std::mutex> lock(task_stats_mutex);
      task_stats_data.running.clear();
      ++task_stats_data.done;
      if (took_ms > task_stats_data.slowest_ms) {
        task_stats_data.slowest_ms = took_ms;
        task_stats_data.slowest = message.task_name;
      }
      if (took_ms >= kSlowTaskMs) {
        const std::uint64_t count = ++slow_task_counts[message.task_name];
        log_it = count <= kSlowTaskLogFirst || (count % kSlowTaskLogEvery) == 0;
      }
    }
    if (log_it) {
      log("task '" + message.task_name + "' took " +
        std::to_string(static_cast<long long>(took_ms)) + " ms");
    }
  }

  void on_timers()
  {
    const Clock::time_point now = Clock::now();
    if (retry_armed && now >= retry_at) {
      retry_armed = false;
      if (state.load() == SessionState::kLoading) {
        start_attempt();
      }
    }
    if (hub_down_armed && now >= hub_down_at) {
      hub_down_armed = false;
      set_silence(kSilenceHubPeersGone, true);
    }
    check_lost(now);
    check_lost_phase(now);
    check_attempt_deadline(now);
  }

  // =========================================================================================
  // Starting
  // =========================================================================================

  void fail(int code, const char * detail, OnboardingContext context)
  {
    StatusText cause = describe_status(code, context, detail);
    set_state(SessionState::kFailed, &cause);
  }

  bool ensure_runtime(OnboardingContext context)
  {
    if (runtime != nullptr) {
      return true;
    }
    LibraryLease & lease = library_lease();
    if (!qar_is_library_loaded()) {
      // Nothing of this process uses the library, so no owner can hold it.
      std::lock_guard<std::mutex> lock(lease.m);
      lease.users = 0;
      lease.loaded_by_owner = false;
      lease.initialised = false;
      lease.poisoned.store(false);
      leased = false;
    }
    if (!qar_is_library_loaded()) {
      std::string error;
      bool loaded = false;
      {
        QAR_GUI_TIMED("qar_library_load");
        loaded = loader->load(cfg.library_path, error);
      }
      if (!loaded) {
        fail(kStatusNoRuntime, error.c_str(), context);
        return false;
      }
      std::lock_guard<std::mutex> lock(lease.m);
      lease.loaded_by_owner = true;
    }
    if (abandoned.load()) {
      return false;
    }
    if (!leased) {
      std::lock_guard<std::mutex> lock(lease.m);
      ++lease.users;
      leased = true;
    }

    QarRuntimeInfo info{};
    if (!failed(QAR_CALL(qar_runtime_info, &info))) {
      std::string from = loader->loaded_from();
      if (from.empty()) {
        from = cfg.library_path.empty() ? std::string("the vendor lookup") : cfg.library_path;
      }
      const std::string text = std::string(info.version != nullptr ? info.version : "?") + " " +
        (info.flavor != nullptr ? info.flavor : "?") + " " +
        (info.c_api != nullptr ? info.c_api : "?") + " from " + from;
      {
        std::lock_guard<std::mutex> lock(description_mutex);
        description = text;
      }
      log("runtime " + text);
    }

    bool initialised = false;
    {
      std::lock_guard<std::mutex> lock(lease.m);
      initialised = lease.initialised;
    }
    if (abandoned.load()) {
      return false;
    }
    if (!initialised) {
      if (!init_library(context)) {
        return false;
      }
    }

    // A local app persists nothing, so the runtime keeps its default storage folder.
    QarRuntimeInit init = qar_runtime_init_default();
    QarResult result{};
#ifdef _WIN32
    int faulted = 0;
    {
      QAR_GUI_TIMED("qar_runtime_create");
      faulted = guarded_runtime_create(&init, &runtime, &result);
    }
    if (faulted != 0) {
      runtime = nullptr;
      fail(QAR_STATUS_UNCLASSIFIED, "qar_runtime_create faulted (see the SEH FAULT line)", context);
      return false;
    }
#else
    result = QAR_CALL(qar_runtime_create, &init, &runtime);
#endif
    if (abandoned.load()) {
      // The runtime of a thread that was left behind belongs to nobody; it is not used.
      return false;
    }
    if (failed(result) || runtime == nullptr) {
      runtime = nullptr;
      fail(static_cast<int>(result.code), result_message(result).c_str(), context);
      return false;
    }
    return true;
  }

  bool init_library(OnboardingContext context)
  {
    QarLibraryInit init = qar_library_init_default();
    std::string folder = cfg.log_folder_path;
    if (folder.empty()) {
      std::string temp = read_environment("TEMP");
      if (temp.empty()) {
        temp = read_environment("TMP");
      }
      folder = !temp.empty() ? (std::filesystem::u8path(temp) / "quaternar").u8string() :
        std::string("quaternar");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
    init.log_folder_path = folder.c_str();
    init.log_severity = cfg.log_severity;
    init.enable_console_logging = cfg.enable_console_logging;

    const LogSeverity severity = static_cast<LogSeverity>(static_cast<int>(cfg.log_severity));
    const std::string before = loader->export_log_severity(severity);
    std::fprintf(
      stderr, "[qar-adapter] library logging: severity=%s console=%s folder=%s%s\n",
      log_severity_name(severity), cfg.enable_console_logging ? "on" : "off", folder.c_str(),
      (!before.empty() && before != log_severity_name(severity)) ?
      (std::string("  [QAR_LOG_SEVERITY='") + before +
      "' was in the environment and takes precedence over the init struct inside the vendor "
      "library - overwritten with '" + log_severity_name(severity) + "']").c_str() : "");
    std::fflush(stderr);
    loader->start_transport_logging(severity);

    QarResult result{};
#ifdef _WIN32
    int faulted = 0;
    {
      QAR_GUI_TIMED("qar_library_init");
      faulted = guarded_library_init(&init, &result);
    }
    if (faulted != 0) {
      fail(QAR_STATUS_UNCLASSIFIED, "qar_library_init faulted (see the SEH FAULT line)", context);
      return false;
    }
#else
    result = QAR_CALL(qar_library_init, &init);
#endif
    if (failed(result)) {
      fail(static_cast<int>(result.code), result_message(result).c_str(), context);
      return false;
    }
    {
      LibraryLease & lease = library_lease();
      std::lock_guard<std::mutex> lock(lease.m);
      lease.initialised = true;
    }
    return true;
  }

  void on_connect()
  {
    const SessionState now_state = state.load();
    if (now_state != SessionState::kIdle && now_state != SessionState::kFailed) {
      return;
    }
    busy_retried = false;
    retry_armed = false;
    task_closing.store(false);
    drain_deadline.store(0);
    joining_started = Clock::now();
    set_state(SessionState::kLoading);
    if (!ensure_runtime(OnboardingContext::kLocal) || abandoned.load()) {
      return;
    }

    start_attempt();
  }

  void fill_presentation(OpCtx & ctx, QarPeerPresentation & presentation)
  {
    ctx.display_name = cfg.display_name.empty() ? cfg.program_tag : cfg.display_name;
    ctx.app_version = cfg.app_version;
    ctx.custom_info = "qaros-ros;program=" + cfg.program_tag;
    presentation = qar_peer_presentation_default();
    presentation.display_name = ctx.display_name.c_str();
    presentation.app_version = ctx.app_version.c_str();
    presentation.app_custom_peer_info = ctx.custom_info.c_str();
  }

  static void onboard_result_trampoline(
    QarResult status, const QarOnboardingId * id, QarSession * session_handle, void * user_state)
  {
    auto * ctx = static_cast<OpCtx *>(user_state);
    Impl * impl = ctx->impl;
    // A local app gets no onboarding id worth keeping: the program persists nothing.
    (void)id;
    Msg message;
    message.type = Msg::Type::kOnboardResult;
    message.attempt = ctx->attempt;
    message.result = status;
    message.session = session_handle;
    impl->post(std::move(message));
  }

  static void progress_trampoline(
    QarActionSeverity severity, float percent, const char * text, void * user_state)
  {
    auto * ctx = static_cast<OpCtx *>(user_state);
    Impl * impl = ctx->impl;
    Msg message;
    message.type = Msg::Type::kProgress;
    message.attempt = ctx->attempt;
    message.severity = severity;
    message.percent = percent;
    if (text != nullptr) {
      message.text = text;
    }
    impl->post(std::move(message));
  }

  // Starts the onboarding as a local app of the Hub of this PC (QarOnboardLocalAppExt).
  void start_attempt()
  {
    if (abandoned.load()) {
      return;
    }
    auto ctx = std::make_unique<OpCtx>();
    ctx->impl = this;
    ctx->attempt = next_attempt++;
    ctx->started = Clock::now();
    ctx->onboard = qar_onboard_init_default();
    fill_presentation(*ctx, ctx->onboard.presentation);
    ctx->local = qar_onboard_local_app_ext_default();
    ctx->local.approval_timeout_ms = std::min(cfg.approval_timeout_ms, kMaxApprovalTimeoutMs);
    ctx->onboard.header.next = &ctx->local.header;

    if (failed(QAR_CALL(qar_cancel_token_create, &ctx->token)) || ctx->token == nullptr) {
      ctx->token = nullptr;
      if (!abandoned.load()) {
        fail(
          QAR_STATUS_UNCLASSIFIED, "the cancel token cannot be created", OnboardingContext::kLocal);
      }
      return;
    }
    if (abandoned.load()) {
      return;
    }

    set_state(SessionState::kWaitingForApproval);

    OpCtx * raw = ctx.get();
    const QarResult result = QAR_CALL(
      qar_runtime_onboard_async, runtime, &raw->onboard, &Impl::onboard_result_trampoline,
      &Impl::progress_trampoline, raw, raw->token);
    if (failed(result)) {
      const std::string detail = result_message(result);
      const int code = static_cast<int>(result.code);
      QAR_CALL(qar_cancel_token_handle_destroy, raw->token);
      raw->token = nullptr;
      // The library may still call the result callback with this context.
      refused_attempts.push_back(std::move(ctx));
      handle_failure(code, detail);
      return;
    }
    op = std::move(ctx);
  }

  // =========================================================================================
  // Commands
  // =========================================================================================

  void on_cancel()
  {
    const SessionState current = state.load();
    if (current == SessionState::kLost) {
      // Ends the automatic rejoin. A session that was not handed back yet is released first.
      if (lost_phase == LostPhase::kAwaitHandBack) {
        run_queued_releases();
        abort_attempt(timings.lost_operation_wait);
        release_session(timings.lost_operation_wait);
      }
      end_auto_rejoin("cancelled");
      set_state(SessionState::kIdle);
      return;
    }
    if (current == SessionState::kWaitingForApproval && op) {
      op->cancel_requested = true;
      QAR_CALL(qar_cancel_token_cancel, op->token);
    }
  }

  // =========================================================================================
  // Results
  // =========================================================================================

  void on_progress(const Msg & message)
  {
    if (!op || message.attempt != op->attempt) {
      return;
    }
    SessionEvent event;
    event.kind = SessionEvent::Kind::kProgress;
    event.text = message.text;
    event.percent = message.percent;
    push(std::move(event));
    log("progress " + message.text);
    if (auto_attempt && message.text.find("Waiting for approval") != std::string::npos) {
      // The Hub shows its prompt: the person gets the whole approval time plus a margin.
      const std::uint32_t approval_ms =
        cfg.approval_timeout_ms != 0 ? std::min(cfg.approval_timeout_ms, kMaxApprovalTimeoutMs) :
        kDefaultApprovalTimeoutMs;
      attempt_deadline = Clock::now() + std::chrono::milliseconds(approval_ms) +
        std::chrono::seconds(10);
      attempt_deadline_armed = true;
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_view.prompt_open = true;
    }
  }

  // Ends the attempt: destroys its token and its context. The result callback has fired, so
  // the library does not read either again.
  void finish_attempt()
  {
    if (!op) {
      return;
    }
    if (op->token != nullptr) {
      QAR_CALL(qar_cancel_token_handle_destroy, op->token);
      op->token = nullptr;
    }
    op.reset();
  }

  void on_onboard_result(Msg & message)
  {
    if (!op || message.attempt != op->attempt) {
      if (message.session != nullptr) {
        QAR_CALL(qar_session_handle_destroy, message.session);
      }
      return;
    }
    const bool cancelled = op->cancel_requested;
    const double elapsed = seconds_since(op->started);
    finish_attempt();

    const bool ok = !failed(message.result) && message.session != nullptr;
    if (cancelled) {
      if (message.session != nullptr) {
        QAR_CALL(qar_session_handle_destroy, message.session);
      }
      if (auto_attempt) {
        end_auto_rejoin("cancelled");
      }
      set_state(SessionState::kIdle);
      return;
    }
    if (!ok) {
      if (message.session != nullptr) {
        QAR_CALL(qar_session_handle_destroy, message.session);
      }
      const int code = failed(message.result) ? static_cast<int>(message.result.code) :
        static_cast<int>(QAR_STATUS_ONBOARDING_FAILED);
      handle_failure(code, result_message(message.result));
      return;
    }
    on_joined(message, elapsed);
  }

  void handle_failure(int code, const std::string & detail)
  {
    StatusText cause = describe_status(code, OnboardingContext::kLocal, detail.c_str());

    if (auto_attempt && retriable_in_rejoin(code)) {
      back_to_lost(cause);
      return;
    }

    if (code == QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING && !busy_retried) {
      busy_retried = true;
      retry_armed = true;
      retry_at = Clock::now() + kBusyRetryDelay;
      SessionEvent event;
      event.kind = SessionEvent::Kind::kProgress;
      event.text = cause.message;
      push(std::move(event));
      log("progress " + cause.message);
      store_state(SessionState::kLoading);
      return;
    }

    set_state(SessionState::kFailed, &cause);
    if (auto_attempt) {
      end_auto_rejoin(cause.symbol);
    }
  }

  void on_joined(Msg & message, double elapsed)
  {
    if (abandoned.load()) {
      // The session of a thread that was left behind is not used; nothing here may destroy it.
      return;
    }
    session = message.session;

    {
      QarPeerSpec * mine = nullptr;
      if (!failed(QAR_CALL(qar_session_get_my_spec, session, &mine)) && mine != nullptr) {
        QarPeerId id = qar_peer_id_default();
        if (!failed(qar_peer_spec_get_id(mine, &id))) {
          std::lock_guard<std::mutex> lock(targets_mutex);
          self = id;
          has_self = true;
        }
        QAR_CALL(qar_peer_spec_handle_destroy, mine);
      }
    }

    if (abandoned.load()) {
      return;
    }
    if (failed(QAR_CALL(qar_cancel_token_create, &peer_token)) || peer_token == nullptr) {
      peer_token = nullptr;
      abandon_session("the peer token cannot be created");
      return;
    }
    const QarResult subscribed = QAR_CALL(
      qar_peer_subscribe_updates, session, &Impl::peer_trampoline, this, peer_token);
    if (failed(subscribed)) {
      const std::string detail = result_message(subscribed);
      abandon_session(detail.c_str());
      return;
    }
    query_initial_peers();

    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", elapsed);
    QarPeerId mine_id{};
    {
      std::lock_guard<std::mutex> lock(targets_mutex);
      mine_id = self;
    }
    if (auto_attempt) {
      log("rejoined after " + std::to_string(rejoin_attempt) + " attempt(s), " +
        std::to_string(millis_since(lost_since)) + " ms after the loss");
      finish_auto_rejoin();
    }
    log("joined as peer " + peer_hex8(mine_id) + " (local) in " + seconds + " s");

    drain_deadline.store(0);
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      session_for_consumer.store(session);
    }
    set_state(SessionState::kRunning);
  }

  // The session cannot be used: releases it and reports the failure.
  void abandon_session(const char * detail)
  {
    accepting_tasks.store(false);
    session_epoch.fetch_add(1);
    end_peer_subscription();
    if (session != nullptr) {
      QAR_CALL(qar_session_handle_destroy, session);
      session = nullptr;
    }
    fail(QAR_STATUS_ONBOARDING_FAILED, detail, OnboardingContext::kLocal);
    if (auto_attempt) {
      end_auto_rejoin("the session cannot be used");
    }
  }

  // =========================================================================================
  // Peers
  // =========================================================================================

  static std::string read_text(
    QarResult (*getter)(QarPeerSpec *, char *, size_t), QarPeerSpec * spec)
  {
    char buffer[256] = {0};
    if (failed(getter(spec, buffer, sizeof(buffer) - 1))) {
      return std::string();
    }
    return std::string(buffer);
  }

  static bool snapshot_of(QarPeerSpec * spec, PeerSnapshot & out)
  {
    if (spec == nullptr || failed(qar_peer_spec_get_id(spec, &out.id))) {
      return false;
    }
    out.info.id = out.id;
    QarPeerRole role = QAR_PEER_ROLE_INTERNAL;
    if (!failed(qar_peer_spec_get_role(spec, &role))) {
      out.role = role;
    }
    QarPeerPresence presence = QAR_PEER_PRESENCE_UNKNOWN;
    if (!failed(qar_peer_spec_get_presence(spec, &presence))) {
      out.info.presence = presence;
    }
    QarDeviceCategory category = QAR_DEVICE_CATEGORY_UNKNOWN;
    if (!failed(qar_peer_spec_get_category(spec, &category))) {
      out.info.category = category;
    }
    out.info.device_id = read_text(&qar_peer_spec_get_device_id, spec);
    out.info.device_family = read_text(&qar_peer_spec_get_device_family, spec);
    out.info.device_name = read_text(&qar_peer_spec_get_device_name, spec);
    out.info.display_name = read_text(&qar_peer_spec_get_display_name, spec);
    return true;
  }

  // Runs on a library thread. The spec is borrowed for the duration of the call: it is read and
  // never destroyed.
  static void peer_trampoline(QarPeerSpec * spec, void * user_state)
  {
    auto * impl = static_cast<Impl *>(user_state);
    PeerSnapshot snapshot;
    if (!snapshot_of(spec, snapshot)) {
      return;
    }
    impl->apply_peer(snapshot);
  }

  std::string label_of(const TargetInfo & info) const
  {
    return !info.device_name.empty() ? info.device_name :
      (!info.display_name.empty() ? info.display_name : info.device_id);
  }

  std::size_t served_count_locked() const
  {
    std::size_t count = 0;
    for (const TargetEntry & entry : table) {
      if (entry.present && entry.info.served) {
        ++count;
      }
    }
    return count;
  }

  void apply_peer(const PeerSnapshot & peer)
  {
    Changes changes;
    bool post_hub = false;
    bool hub_down_now = false;
    // The delivery lock is taken before the table changes, so the listener hears the changes in
    // the order of the table and a replay (set_target_listener) cannot interleave with them.
    std::unique_lock<std::mutex> delivery(delivery_mutex, std::defer_lock);
    if (peer.role != QAR_PEER_ROLE_RUNTIME) {
      delivery.lock();
    }
    {
      std::lock_guard<std::mutex> lock(targets_mutex);
      if (has_self && same_peer(peer.id, self)) {
        return;
      }
      if (peer.role == QAR_PEER_ROLE_RUNTIME) {
        runtime_peers_gone[peer_hex8(peer.id)] = is_gone(peer.info.presence);
        bool all_gone = !runtime_peers_gone.empty();
        for (const auto & entry : runtime_peers_gone) {
          all_gone = all_gone && entry.second;
        }
        if (all_gone != hub_down_reported) {
          hub_down_reported = all_gone;
          post_hub = true;
          hub_down_now = all_gone;
        }
      } else {
        apply_target_locked(peer, changes);
      }
    }
    if (post_hub) {
      Msg message;
      message.type = Msg::Type::kHubPresence;
      message.hub_down = hub_down_now;
      post(std::move(message));
    }
    for (const std::string & line : changes.lines) {
      log(line);
    }
    deliver(changes.notify);
  }

  void apply_target_locked(const PeerSnapshot & peer, Changes & changes)
  {
    const bool is_target = is_render_target(peer.role, peer.info.presence, false);
    auto found = std::find_if(table.begin(), table.end(), [&peer](const TargetEntry & entry) {
      return same_peer(entry.info.id, peer.id);
    });

    if (found == table.end()) {
      if (!is_target) {
        return;
      }
      TargetEntry entry;
      entry.info = peer.info;
      entry.present = true;
      table.push_back(entry);
      announce_up_locked(table.back(), changes);
      return;
    }

    const bool was_served = found->info.served;
    const std::string reason = found->info.not_served_reason;
    const bool was_present = found->present;
    found->info = peer.info;
    found->info.served = was_served;
    found->info.not_served_reason = reason;

    if (is_target) {
      if (!was_present) {
        found->present = true;
        announce_up_locked(*found, changes);
      }
      return;
    }
    if (was_present) {
      found->present = false;
      const bool had_stream = found->info.served;
      found->info.served = false;
      SessionEvent event;
      event.kind = SessionEvent::Kind::kTargetDown;
      event.target = found->info;
      push(std::move(event));
      changes.lines.push_back(
        "target " + peer_hex8(found->info.id) + " " + presence_name(found->info.presence) +
        " -> stream stopped");
      if (had_stream) {
        Notify note;
        note.up = false;
        note.info = found->info;
        note.presence = peer.info.presence;
        changes.notify.push_back(note);
      }
    }
    if (peer.info.presence == QAR_PEER_PRESENCE_RETIRED) {
      table.erase(found);
    }
    promote_unserved_locked(changes);
  }

  // A target that is here: gives it a stream when the cap allows, and says so.
  void announce_up_locked(TargetEntry & entry, Changes & changes)
  {
    entry.info.served = false;
    if (served_count_locked() < cfg.max_targets) {
      entry.info.served = true;
      entry.info.not_served_reason.clear();
    } else {
      entry.info.not_served_reason = "cap reached";
    }
    SessionEvent event;
    event.kind = SessionEvent::Kind::kTargetUp;
    event.target = entry.info;
    push(std::move(event));
    changes.lines.push_back(
      "target " + peer_hex8(entry.info.id) + " " + category_name(entry.info.category) + " '" +
      label_of(entry.info) + "' " + presence_name(entry.info.presence) + " -> " +
      (entry.info.served ? std::string("served") :
      "not served (" + entry.info.not_served_reason + ")"));
    if (entry.info.served) {
      Notify note;
      note.up = true;
      note.info = entry.info;
      changes.notify.push_back(note);
    }
  }

  // A stream slot became free: the first target without a stream gets it.
  void promote_unserved_locked(Changes & changes)
  {
    if (served_count_locked() >= cfg.max_targets) {
      return;
    }
    for (TargetEntry & entry : table) {
      if (entry.present && !entry.info.served) {
        entry.info.served = true;
        entry.info.not_served_reason.clear();
        SessionEvent event;
        event.kind = SessionEvent::Kind::kTargetUp;
        event.target = entry.info;
        push(std::move(event));
        changes.lines.push_back(
          "target " + peer_hex8(entry.info.id) + " " + category_name(entry.info.category) +
          " '" + label_of(entry.info) + "' " + presence_name(entry.info.presence) +
          " -> served");
        Notify note;
        note.up = true;
        note.info = entry.info;
        changes.notify.push_back(note);
        return;
      }
    }
  }

  // Calls the listener. The caller holds delivery_mutex and no other lock: the listener may block,
  // and the owner thread must be able to end the subscription (which waits for this callback)
  // while it does. The strong reference ends before the caller releases delivery_mutex, so a
  // consumer that waits in set_target_listener() keeps the listener alive until then.
  void deliver(const std::vector<Notify> & notify)
  {
    if (notify.empty()) {
      return;
    }
    ListenerScope in_listener;
    std::shared_ptr<ITargetListener> target;
    {
      std::lock_guard<std::mutex> lock(listener_mutex);
      target = listener.lock();
    }
    if (!target) {
      return;
    }
    for (const Notify & note : notify) {
      if (note.up) {
        target->on_target_up(note.info);
      } else {
        target->on_target_down(note.info.id, note.presence);
      }
    }
  }

  void query_initial_peers()
  {
    std::size_t count = 0;
    if (failed(QAR_CALL(qar_query_peer_specs_count, session, &count)) || count == 0) {
      return;
    }
    std::vector<QarPeerSpec *> handles(count, nullptr);
    std::size_t written = 0;
    if (failed(QAR_CALL(qar_query_peer_specs, session, handles.data(), count, &written))) {
      return;
    }
    for (std::size_t i = 0; i < written && i < handles.size(); ++i) {
      QarPeerSpec * handle = handles[i];
      if (handle == nullptr) {
        continue;
      }
      PeerSnapshot snapshot;
      if (snapshot_of(handle, snapshot)) {
        apply_peer(snapshot);
      }
      QAR_CALL(qar_peer_spec_handle_destroy, handle);   // owned: the query hands them over
    }
  }

  void on_hub_presence(const Msg & message)
  {
    if (message.hub_down) {
      const SessionState current = state.load();
      if (current == SessionState::kRunning || current == SessionState::kReconnecting) {
        hub_down_armed = true;
        hub_down_at = Clock::now() + timings.peers_gone_grace;
      }
      return;
    }
    hub_down_armed = false;
    set_silence(kSilenceHubPeersGone, false);
  }

  // =========================================================================================
  // Invites
  // =========================================================================================

  static void invite_trampoline(QarResult status, const QarPeerId * peer, void * user_state)
  {
    auto * ctx = static_cast<InviteCtx *>(user_state);
    Impl * impl = ctx->impl;
    Msg message;
    message.type = Msg::Type::kInviteResult;
    message.invite = ctx->id;
    message.result = status;
    if (peer != nullptr) {
      message.peer = *peer;
      message.has_peer = true;
    }
    impl->post(std::move(message));
  }

  void on_invite_result(const Msg & message)
  {
    std::unique_ptr<InviteOp> finished;
    {
      std::lock_guard<std::mutex> lock(invites_mutex);
      for (auto it = invites.begin(); it != invites.end(); ++it) {
        if ((*it)->id == message.invite) {
          finished = std::move(*it);
          invites.erase(it);
          break;
        }
      }
    }
    if (!finished) {
      return;
    }
    if (finished->token != nullptr) {
      QAR_CALL(qar_cancel_token_handle_destroy, finished->token);
      finished->token = nullptr;
    }
    SessionEvent event;
    event.kind = SessionEvent::Kind::kInviteDone;
    event.text = finished->kind_text + " '" + finished->host + "'";
    const bool ok = !failed(message.result);
    if (ok && message.has_peer) {
      event.target.id = message.peer;
      log("invite " + finished->kind_text + " '" + finished->host + "' -> " +
        peer_hex8(message.peer));
    } else {
      event.status = describe_status(
        static_cast<int>(message.result.code), OnboardingContext::kInvite,
        result_message(message.result).c_str());
      log("invite " + finished->kind_text + " '" + finished->host + "' -> error: " +
        event.status.message);
    }
    push(std::move(event));
  }

  // =========================================================================================
  // Teardown
  // =========================================================================================

  // Waits for the result message of the attempt in flight (the library fires it also on
  // cancellation). Gives up after the bound; then the context must outlive the owner.
  std::chrono::milliseconds operation_wait() const
  {
    return exiting.load() ? timings.exit_operation_wait :
           std::chrono::duration_cast<std::chrono::milliseconds>(kOperationWait);
  }

  void abort_attempt() { abort_attempt(operation_wait()); }

  void abort_attempt(std::chrono::milliseconds wait)
  {
    if (!op) {
      return;
    }
    op->cancel_requested = true;
    const Clock::time_point start = Clock::now();
    if (op->token != nullptr) {
      QAR_CALL(qar_cancel_token_cancel, op->token);
    }
    const Clock::time_point deadline = start + wait;
    while (op) {
      Msg message;
      if (abandoned.load() || !pop(message, true, true, deadline)) {
        log("teardown: the onboarding result did not arrive within " +
          seconds_text(wait) + " s");
        leak_on_destroy = true;
        (void)op.release();   // the library may still read it
        break;
      }
      if (message.type == Msg::Type::kOnboardResult && message.attempt == op->attempt) {
        if (message.session != nullptr) {
          QAR_CALL(qar_session_handle_destroy, message.session);
        }
        finish_attempt();
      } else {
        handle(message);
      }
    }
    log("teardown: onboarding attempt ended in " + std::to_string(millis_since(start)) + " ms");
  }

  void end_invites() { end_invites(operation_wait()); }

  void end_invites(std::chrono::milliseconds wait)
  {
    const Clock::time_point start = Clock::now();
    std::vector<InviteOp *> pending;
    {
      std::lock_guard<std::mutex> lock(invites_mutex);
      for (auto & invite : invites) {
        pending.push_back(invite.get());
      }
    }
    for (InviteOp * invite : pending) {
      if (invite->token != nullptr) {
        QAR_CALL(qar_cancel_token_cancel, invite->token);
      }
    }
    const Clock::time_point deadline = start + wait;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(invites_mutex);
        if (invites.empty()) {
          break;
        }
      }
      Msg message;
      if (!pop(message, true, true, deadline)) {
        log("teardown: an invite result did not arrive within " + seconds_text(wait) + " s");
        leak_on_destroy = true;
        std::lock_guard<std::mutex> lock(invites_mutex);
        for (auto & invite : invites) {
          (void)invite.release();
        }
        invites.clear();
        break;
      }
      handle(message);
    }
    if (!pending.empty()) {
      log("teardown: invites ended in " + std::to_string(millis_since(start)) + " ms");
    }
  }

  // Cancels and destroys the peer token with no lock held: the call waits for a running peer
  // callback, and that callback takes the target table lock and calls the listener.
  void end_peer_subscription()
  {
    if (peer_token == nullptr) {
      return;
    }
    const Clock::time_point start = Clock::now();
    QAR_CALL(qar_cancel_token_cancel, peer_token);
    QAR_CALL(qar_cancel_token_handle_destroy, peer_token);
    peer_token = nullptr;
    log("teardown: peer subscription ended in " + std::to_string(millis_since(start)) + " ms");
  }

  void clear_targets()
  {
    std::vector<TargetEntry> dropped;
    {
      std::lock_guard<std::mutex> lock(targets_mutex);
      dropped.swap(table);
      runtime_peers_gone.clear();
      hub_down_reported = false;
      has_self = false;
    }
    for (const TargetEntry & entry : dropped) {
      if (entry.present) {
        SessionEvent event;
        event.kind = SessionEvent::Kind::kTargetDown;
        event.target = entry.info;
        push(std::move(event));
      }
    }
  }

  // Steps 7 to 9 of the teardown: invites, peer subscription, the session.
  void release_session() { release_session(operation_wait()); }

  void release_session(std::chrono::milliseconds wait)
  {
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      session_for_consumer.store(nullptr);
      consumer_ready.store(false);
      accepting_tasks.store(false);
    }
    // From here on no task runs on the session that is being released.
    session_epoch.fetch_add(1);
    hub_down_armed = false;
    reset_silence();
    end_invites(wait);
    if (abandoned.load()) {
      return;
    }
    end_peer_subscription();
    clear_targets();
    if (abandoned.load()) {
      return;
    }
    if (session != nullptr) {
      // A frame worker that is still inside a QarOS call on a stream of this session makes the
      // destroy of the session unsafe. The session is then left as it is (the runtime and the
      // library stay loaded under it), the workers are given up on and not counted after that,
      // and the owner goes on: the next session is made on this thread, as after any release.
      const int given_up = settle_frame_workers() > 0 ? lane->give_up_workers() : 0;
      if (given_up > 0) {
        log("release: " + std::to_string(given_up) +
          " frame worker(s) still inside a QarOS call; their session is left unreleased");
        left_session_behind = true;
        library_lease().poisoned.store(true);
        pin_this_module();
        session = nullptr;
        drop_queued_tasks("the session is left unreleased");
        return;
      }
      const Clock::time_point start = Clock::now();
      QAR_CALL(qar_session_handle_destroy, session);
      session = nullptr;
      log("teardown: session released in " + std::to_string(millis_since(start)) + " ms");
    }
    drop_queued_tasks("the session ended");
  }

  void on_disconnect()
  {
    const SessionState current = state.load();
    if (current == SessionState::kStopping) {
      return;
    }
    if (current == SessionState::kLoading && retry_armed) {
      retry_armed = false;
      set_state(SessionState::kIdle);
      return;
    }
    if (current == SessionState::kIdle) {
      return;
    }
    if (lost_phase != LostPhase::kNone || auto_attempt) {
      run_queued_releases();
      end_auto_rejoin("disconnected");
    }
    const bool had_work = op || session != nullptr;
    if (had_work) {
      set_state(SessionState::kStopping);
    }
    abort_attempt();
    release_session();
    set_state(SessionState::kIdle);
  }

  // Waits at most timings.frame_worker_settle for the frame workers to leave, then runs what they
  // posted. Owner thread. @return the number of workers still registered.
  int settle_frame_workers()
  {
    const int left = lane->wait_workers(timings.frame_worker_settle);
    run_queued_releases();
    return left;
  }

  // The whole teardown of the owner: everything the owner created, in order.
  void shutdown()
  {
    if (abandoned.load()) {
      return;
    }
    const int stuck = settle_frame_workers();
    if (stuck > 0) {
      // A frame worker is still inside a QarOS call, or its sender is not released: the session,
      // the runtime and the library stay loaded under it, and the process is ending.
      log("exit: " + std::to_string(stuck) + " frame worker(s) still inside a QarOS call; "
        "the session, the runtime and the library stay loaded");
      lane->abandon_workers();
      library_lease().poisoned.store(true);
      leak_on_destroy = true;
      pin_this_module();
      return;
    }
    abort_attempt();
    if (abandoned.load()) {
      return;
    }
    run_queued_releases();
    release_session();
    if (abandoned.load()) {
      return;
    }
    if (runtime != nullptr && left_session_behind) {
      // A frame worker may still be inside a stream of this runtime.
      log("teardown: the runtime stays: a session was left unreleased");
      runtime = nullptr;
    } else if (runtime != nullptr) {
      const Clock::time_point start = Clock::now();
      QAR_CALL(qar_runtime_handle_destroy, runtime);
      runtime = nullptr;
      log("teardown: runtime released in " + std::to_string(millis_since(start)) + " ms");
    }
    if (abandoned.load()) {
      return;
    }
    release_library();
    store_state(SessionState::kIdle);
  }

  // The owner gives its share of the library back. The last owner destroys and unloads it, unless
  // an owner thread was left behind: that thread may still be inside the library.
  void release_library()
  {
    if (!leased) {
      return;
    }
    LibraryLease & lease = library_lease();
    bool destroy = false;
    bool unload = false;
    {
      std::lock_guard<std::mutex> lock(lease.m);
      leased = false;
      if (--lease.users <= 0 && !lease.poisoned.load()) {
        lease.users = 0;
        destroy = lease.initialised;
        unload = lease.loaded_by_owner;
        lease.initialised = false;
        lease.loaded_by_owner = false;
      }
    }
    if (destroy) {
      const Clock::time_point start = Clock::now();
      QAR_CALL(qar_library_destroy);
      log("teardown: library destroyed in " + std::to_string(millis_since(start)) + " ms");
    }
    if (unload) {
      QAR_GUI_TIMED("qar_library_unload");
      loader->unload();
    }
  }

  // =========================================================================================
  // Outage: silence, lost, rejoin
  // =========================================================================================

  static constexpr std::uint32_t kSilentMask = kSilenceHubPeersGone | kSilenceCallStalled;

  static std::string seconds_text(std::chrono::milliseconds ms)
  {
    char buffer[32] = {0};
    if (ms.count() % 1000 == 0) {
      std::snprintf(buffer, sizeof(buffer) - 1, "%lld", static_cast<long long>(ms.count() / 1000));
    } else {
      std::snprintf(buffer, sizeof(buffer) - 1, "%.1f", static_cast<double>(ms.count()) / 1000.0);
    }
    return std::string(buffer);
  }

  static bool retriable_in_rejoin(int code)
  {
    return code == QAR_STATUS_ONBOARDING_HUB_UNREACHABLE ||
           code == QAR_STATUS_ONBOARDING_FAILED || code == QAR_STATUS_TIMEOUT ||
           code == QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING;
  }

  // One line per attempt up to the twentieth, then one in ten.
  static bool attempt_line_due(int attempt) { return attempt <= 20 || (attempt % 10) == 0; }

  // Caller holds silence_mutex.
  std::string silence_reasons_locked() const
  {
    std::string out;
    if ((silence_bits & kSilenceHubPeersGone) != 0) {
      out += "hub-peers-gone";
    }
    if ((silence_bits & kSilenceCallStalled) != 0) {
      if (!out.empty()) {
        out += ", ";
      }
      const std::string ms = std::to_string(static_cast<long long>(silent_note_ms));
      if (silent_note.rfind("task ", 0) == 0) {
        out += "task-stalled " + silent_note.substr(5) + " " + ms + " ms";
      } else {
        out += "call-stalled " + silent_note + " " + ms + " ms";
      }
    }
    return out;
  }

  // Caller holds silence_mutex.
  std::string silence_cause_locked() const
  {
    std::string what;
    if ((silence_bits & kSilenceCallStalled) != 0 && !silent_note.empty()) {
      what = " (" + silent_note + " has not returned)";
    }
    return "The QAROS Hub does not answer" + what + ". Waiting for it to come back.";
  }

  void reset_silence()
  {
    std::lock_guard<std::mutex> lock(silence_mutex);
    silence_bits &= ~kSilentMask;
    silent_active = false;
    silent_note.clear();
    silent_note_ms = 0;
  }

  // Sets or clears one silence bit. The silent bits move the state between kRunning and
  // kReconnecting; every other state is the owner thread's alone. Any thread that owns a bit.
  void set_silence(
    std::uint32_t bit, bool on, const std::string & note = std::string(), std::int64_t note_ms = 0)
  {
    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(silence_mutex);
      if (on && (bit & kSilentMask) != 0) {
        // The supervisor judged a state that has moved on: a lost session is not silent again.
        std::lock_guard<std::mutex> ready(ready_mutex);
        if (state.load() == SessionState::kLost) {
          return;
        }
      }
      const std::uint32_t before = silence_bits;
      if (on) {
        silence_bits |= bit;
      } else {
        silence_bits &= ~bit;
      }
      if (bit == kSilenceCallStalled) {
        silent_note = on ? note : std::string();
        silent_note_ms = on ? note_ms : 0;
      }
      const bool was_silent = silent_active;
      const bool silent = (silence_bits & kSilentMask) != 0;
      std::string answers_again;
      if (silent && (!was_silent || (silence_bits & kSilentMask) != (before & kSilentMask))) {
        if (!was_silent) {
          silent_active = true;
          silent_since = Clock::now();
        }
        lines.push_back("hub silent (" + silence_reasons_locked() + ") at " + wall_clock_hms_ms());
      } else if (!silent && was_silent) {
        silent_active = false;
        answers_again = "hub answers again after " + std::to_string(millis_since(silent_since)) +
          " ms";
      }
      bool moved = false;
      SessionState to = SessionState::kRunning;
      {
        std::lock_guard<std::mutex> ready(ready_mutex);
        const SessionState current = state.load();
        if (silent && current == SessionState::kRunning) {
          to = SessionState::kReconnecting;
          moved = true;
        } else if (!silent && was_silent && current == SessionState::kReconnecting) {
          to = SessionState::kRunning;
          moved = true;
        }
        if (!answers_again.empty() && current != SessionState::kLost) {
          lines.push_back(answers_again);
        }
        if (moved) {
          state.store(to);
          SessionEvent event;
          event.kind = SessionEvent::Kind::kState;
          event.state = to;
          if (to == SessionState::kReconnecting) {
            event.status.message = silence_cause_locked();
          }
          push(std::move(event));
        }
      }
      if (moved) {
        lines.push_back(std::string("state ") + state_name(to));
      }
    }
    for (const std::string & line : lines) {
      log(line);
    }
  }

  std::string lost_cause_of(std::uint32_t bits, const std::string & note, std::int64_t silent_ms)
  {
    std::string what;
    if ((bits & kSilenceHubPeersGone) != 0) {
      what = " (its peers are gone)";
    } else if ((bits & kSilenceCallStalled) != 0 && !note.empty()) {
      what = " (" + note + " has not returned)";
    }
    return "The QAROS Hub stopped answering" + what + " for " +
           seconds_text(std::chrono::milliseconds(silent_ms)) + " s.";
  }

  void check_lost(Clock::time_point now)
  {
    if (state.load() != SessionState::kReconnecting) {
      return;
    }
    WatchInput in;
    in.now = now;
    in.joined = true;
    std::string note;
    {
      std::lock_guard<std::mutex> lock(silence_mutex);
      in.silence = silence_bits;
      in.silent_ms = silent_active ?
        std::chrono::duration_cast<std::chrono::milliseconds>(now - silent_since).count() : 0;
      note = silent_note;
    }
    in.endpoint_gone_long = endpoint_gone_long.load();
    in.hub_process_gone_long = hub_process_gone_long.load();
    if (policy.judge(in).lost_due) {
      begin_lost(lost_cause_of(in.silence, note, in.silent_ms), false);
    }
  }

  // The session is given up: tasks of it are dropped, the consumer is told, and the owner waits
  // for the consumer to hand the session back before it releases it.
  void begin_lost(const std::string & cause_text, bool initial)
  {
    reset_silence();
    hub_down_armed = false;
    hub_back_pending.store(false);
    lost_cause_text = cause_text;
    StatusText cause;
    cause.code = kStatusSessionLost;
    cause.symbol = status_symbol(cause.code);
    cause.message = cause_text;
    lost_since = Clock::now();
    hand_back_at = lost_since + timings.hand_back_wait;
    lost_phase = LostPhase::kAwaitHandBack;
    rejoin_attempt = 0;
    auto_attempt = false;
    rejoin_asap = false;
    attempt_deadline_armed = false;
    last_attempt_start = Clock::time_point{};
    {
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_view = RejoinInfo();
      rejoin_view.automatic = true;
      rejoin_view.lost_cause = cause_text;
      rejoin_next_armed = false;
      lost_since_view = lost_since;
      lost_view_active = true;
    }
    // The state first: it closes the door for tasks (the flags are cleared and poll() cannot open
    // it again). The epoch follows, as in release_session(), so that a task posted under the old
    // epoch is dropped and none can be posted under the new one.
    set_state(SessionState::kLost, &cause);
    session_epoch.fetch_add(1);
    if (!initial) {
      drop_queued_tasks("the session was lost");
    }
  }

  void check_lost_phase(Clock::time_point now)
  {
    if (state.load() != SessionState::kLost) {
      return;
    }
    if (hub_back_pending.exchange(false)) {
      request_rejoin_at_once();
    }
    if (lost_phase == LostPhase::kAwaitHandBack && now >= hand_back_at) {
      on_hand_back(true);
    } else if (lost_phase == LostPhase::kWaitRejoin && now >= rejoin_at) {
      start_rejoin_attempt();
    }
  }

  // The next attempt starts as soon as the spacing allows.
  void request_rejoin_at_once()
  {
    if (lost_phase == LostPhase::kAwaitHandBack) {
      rejoin_asap = true;
    } else if (lost_phase == LostPhase::kWaitRejoin) {
      Clock::time_point at = Clock::now();
      if (last_attempt_start != Clock::time_point{}) {
        at = std::max(at, last_attempt_start + timings.rejoin_spacing);
      }
      rejoin_at = at;
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_next_at = at;
      rejoin_next_armed = true;
    }
  }

  void on_rejoin_now()
  {
    if (state.load() == SessionState::kLost) {
      request_rejoin_at_once();
    }
  }

  void on_declare_lost(const Msg & message)
  {
    const SessionState current = state.load();
    if (current != SessionState::kRunning && current != SessionState::kReconnecting) {
      log(std::string("declare_lost ignored (state ") + state_name(current) + ")");
      return;
    }
    begin_lost(message.text.empty() ? std::string("The session was given up.") : message.text,
      false);
  }

  void on_hand_back(bool timed_out)
  {
    if (state.load() != SessionState::kLost || lost_phase != LostPhase::kAwaitHandBack) {
      return;
    }
    if (timed_out) {
      log("lost: no hand-back within " + seconds_text(timings.hand_back_wait) +
        " s; releasing anyway");
    } else {
      log("lost: the consumer handed the session back in " +
        std::to_string(millis_since(lost_since)) + " ms");
    }
    run_queued_releases();
    abort_attempt(timings.lost_operation_wait);
    release_session(timings.lost_operation_wait);
    if (abandoned.load()) {
      return;
    }
    lost_phase = LostPhase::kWaitRejoin;
    schedule_rejoin();
  }

  // Plans the next attempt: the pause doubles from rejoin_first to rejoin_cap, and two attempts
  // never start closer than rejoin_spacing.
  void schedule_rejoin()
  {
    const int n = rejoin_attempt + 1;
    std::chrono::milliseconds delay = timings.rejoin_first;
    for (int i = 1; i < n && delay < timings.rejoin_cap; ++i) {
      delay *= 2;
    }
    if (delay > timings.rejoin_cap) {
      delay = timings.rejoin_cap;
    }
    const Clock::time_point now = Clock::now();
    Clock::time_point at = rejoin_asap ? now : now + delay;
    rejoin_asap = false;
    if (last_attempt_start != Clock::time_point{}) {
      at = std::max(at, last_attempt_start + timings.rejoin_spacing);
    }
    rejoin_at = at;
    lost_phase = LostPhase::kWaitRejoin;
    {
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_next_at = at;
      rejoin_next_armed = true;
    }
    if (attempt_line_due(n)) {
      log("rejoin " + std::to_string(n) + " in " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(at - now).count()) +
        " ms");
    }
  }

  void start_rejoin_attempt()
  {
    ++rejoin_attempt;
    const int n = rejoin_attempt;
    last_attempt_start = Clock::now();
    auto_attempt = true;
    lost_phase = LostPhase::kAttempt;
    {
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_view.attempt = n;
      rejoin_view.prompt_open = false;
      rejoin_next_armed = false;
    }
    if (n >= timings.fresh_runtime_from_attempt && runtime != nullptr) {
      // A runtime that talked to a Hub that is gone may not recover: the next one starts clean.
      if (attempt_line_due(n)) {
        log("rejoin " + std::to_string(n) + ": fresh runtime");
      }
      if (!left_session_behind) {
        QAR_CALL(qar_runtime_handle_destroy, runtime);
      }
      runtime = nullptr;
    }
    if (!ensure_runtime(OnboardingContext::kLocal)) {
      if (!abandoned.load()) {
        end_auto_rejoin("the runtime cannot be made");
      }
      return;
    }
    if (abandoned.load()) {
      return;
    }
    busy_retried = false;
    retry_armed = false;
    joining_started = last_attempt_start;
    attempt_deadline = last_attempt_start + timings.attempt_timeout;
    attempt_deadline_armed = true;
    if (attempt_line_due(n)) {
      log("rejoin " + std::to_string(n) + " started (local)");
    }
    start_attempt();
    if (!op && state.load() != SessionState::kLost) {
      end_auto_rejoin("the attempt could not start");
    }
  }

  // The attempt failed in a way that a later attempt may fix: back to kLost, next back-off.
  void back_to_lost(const StatusText & why, bool say_failed = true)
  {
    const int n = rejoin_attempt;
    attempt_deadline_armed = false;
    auto_attempt = false;
    if (say_failed && attempt_line_due(n)) {
      log("rejoin " + std::to_string(n) + " failed: " + why.symbol + " after " +
        std::to_string(millis_since(last_attempt_start)) + " ms");
    }
    StatusText cause;
    cause.code = kStatusSessionLost;
    cause.symbol = status_symbol(cause.code);
    cause.message = lost_cause_text;
    cause.detail = why.detail;
    {
      std::lock_guard<std::mutex> lock(rejoin_mutex);
      rejoin_view.prompt_open = false;
    }
    set_state(SessionState::kLost, &cause, true);
    schedule_rejoin();
  }

  void check_attempt_deadline(Clock::time_point now)
  {
    if (!attempt_deadline_armed || now < attempt_deadline) {
      return;
    }
    attempt_deadline_armed = false;
    if (!auto_attempt || !op) {
      return;
    }
    if (attempt_line_due(rejoin_attempt)) {
      log("rejoin " + std::to_string(rejoin_attempt) + " timed out after " +
        std::to_string(millis_since(last_attempt_start)) + " ms");
    }
    abort_attempt(timings.lost_operation_wait);
    back_to_lost(describe_status(QAR_STATUS_TIMEOUT, OnboardingContext::kLocal, nullptr), false);
  }

  // The automatic rejoin stops with the state the caller set.
  void end_auto_rejoin(const std::string & why)
  {
    const bool was = auto_attempt || lost_phase != LostPhase::kNone;
    finish_auto_rejoin();
    if (was) {
      log("rejoin stopped (" + why + ")");
    }
  }

  void finish_auto_rejoin()
  {
    auto_attempt = false;
    lost_phase = LostPhase::kNone;
    attempt_deadline_armed = false;
    rejoin_asap = false;
    std::lock_guard<std::mutex> lock(rejoin_mutex);
    if (lost_view_active) {
      rejoin_view.lost_for_ms = static_cast<int>(millis_since(lost_since_view));
    }
    rejoin_view.automatic = false;
    rejoin_view.prompt_open = false;
    rejoin_next_armed = false;
    lost_view_active = false;
  }

  // =========================================================================================
  // Releases posted by other threads
  // =========================================================================================

  void on_release(Msg & message)
  {
    const Clock::time_point start = Clock::now();
    try {
      if (message.release) {
        message.release();
      }
    } catch (...) {
      log("release '" + message.task_name + "' ended with an exception");
    }
    const long long took_ms = millis_since(start);
    if (took_ms >= static_cast<long long>(kSlowTaskMs)) {
      log("release '" + message.task_name + "' took " + std::to_string(took_ms) + " ms");
    }
  }

  void run_queued_releases()
  {
    std::vector<Msg> items;
    {
      std::lock_guard<std::mutex> lock(inbox_mutex);
      for (auto it = inbox.begin(); it != inbox.end();) {
        if (it->type == Msg::Type::kRelease) {
          items.push_back(std::move(*it));
          it = inbox.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Msg & item : items) {
      on_release(item);
    }
  }

  // =========================================================================================
  // The supervisor thread: it makes no QarOS call
  // =========================================================================================

  void post_tick()
  {
    if (!tick_pending.exchange(true)) {
      Msg message;
      message.type = Msg::Type::kTick;
      post(std::move(message));
    }
  }

  void request_watch_stop()
  {
    {
      std::lock_guard<std::mutex> lock(watch_mutex);
      watch_stop = true;
    }
    watch_cv.notify_all();
  }

  // Joins the supervisor thread. Only for a thread that has ended or ends at once: the delete
  // path, which runs after both threads were seen to leave.
  void stop_watch()
  {
    request_watch_stop();
    if (watch_thread.joinable()) {
      watch_thread.join();
    }
  }

  // The abandon path never joins: the supervisor may be inside the hint source, and a thread's
  // end waits for the loader lock. It is given `wait` to set watch_done and is then detached;
  // the Impl of an abandoned owner is never freed, so the thread may finish at any time.
  void stop_watch_detached(std::chrono::milliseconds wait)
  {
    request_watch_stop();
    {
      std::unique_lock<std::mutex> lock(watch_mutex);
      watch_done_cv.wait_for(lock, wait, [this] { return watch_done; });
    }
    if (watch_thread.joinable()) {
      watch_thread.detach();
    }
  }

  // Waits for a thread to leave the OS until `deadline`, then joins it; a thread that is still
  // there is detached. @return true when the thread has been joined (or was not running).
  // Windows waits on the thread handle, so a thread that is held in its end by the loader lock
  // counts as not left. Other platforms wait for `done`.
  template<typename Done>
  static bool join_within(std::thread & thread, Clock::time_point deadline, Done done)
  {
    if (!thread.joinable()) {
      return true;
    }
#ifdef _WIN32
    (void)done;
    const Clock::time_point now = Clock::now();
    const auto left = deadline > now ?
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) :
      std::chrono::milliseconds(0);
    if (WaitForSingleObject(thread.native_handle(), static_cast<DWORD>(left.count())) ==
      WAIT_OBJECT_0)
    {
      thread.join();
      return true;
    }
#else
    if (done(deadline)) {
      thread.join();
      return true;
    }
#endif
    thread.detach();
    return false;
  }

  // What the owner thread is inside, for a log line: the call, else the running task.
  std::string stalled_name()
  {
    const char * call = call_clock.call.load();
    if (call != nullptr) {
      return call;
    }
    std::lock_guard<std::mutex> lock(task_stats_mutex);
    return task_stats_data.running.empty() ? std::string("-") : "task " + task_stats_data.running;
  }

  void watch()
  {
    struct Leaving
    {
      Impl * self;
      ~Leaving()
      {
        std::lock_guard<std::mutex> lock(self->watch_mutex);
        self->watch_done = true;
        self->watch_done_cv.notify_all();
      }
    } leaving{this};
    HubHints last_hints;
    bool have_hints = false;
    Clock::time_point next_hint = Clock::now();
    bool endpoint_missing = false;
    bool process_missing = false;
    Clock::time_point endpoint_missing_since{};
    Clock::time_point process_missing_since{};
    int stall_level = 0;
    std::string stall_name;
    bool last_endpoint_long = false;
    bool last_process_long = false;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(watch_mutex);
        if (watch_cv.wait_for(lock, timings.watch_period, [this] { return watch_stop; })) {
          return;
        }
      }
      const Clock::time_point now = Clock::now();
      const SessionState current = state.load();
      bool wake_owner = false;

      // The hints: stat of the endpoint file and one process snapshot, every hint_period.
      if (cfg.watch_hub_hints && now >= next_hint) {
        next_hint = now + timings.hint_period;
        const HubHints hints = cfg.hint_source ? cfg.hint_source() : read_hub_hints();
        if (!have_hints) {
          char text[128] = {0};
          std::snprintf(
            text, sizeof(text) - 1, "hint: watching %d process names every %s s (scan %.0f ms)",
            hints.processes_known ? 4 : 0, seconds_text(timings.hint_period).c_str(),
            hints.scan_ms);
          log(text);
        }
        for (const std::string & line : describe_hint_changes(last_hints, hints)) {
          log(line);
        }
        bool back = false;
        if (have_hints) {
          if (last_hints.endpoint_known && !last_hints.endpoint_present &&
            hints.endpoint_present)
          {
            back = true;
          }
          if (last_hints.endpoint_present && hints.endpoint_present &&
            last_hints.endpoint_mtime != hints.endpoint_mtime)
          {
            back = true;
          }
          if (last_hints.processes_known && last_hints.hub_pid == 0 && hints.hub_pid != 0) {
            back = true;
          }
        }
        if (hints.endpoint_known) {
          if (!hints.endpoint_present) {
            if (!endpoint_missing) {
              endpoint_missing = true;
              endpoint_missing_since = now;
            }
          } else {
            endpoint_missing = false;
          }
        }
        if (hints.processes_known) {
          if (hints.hub_pid == 0) {
            if (!process_missing) {
              process_missing = true;
              process_missing_since = now;
            }
          } else {
            process_missing = false;
          }
        }
        if (hints.processes_known) {
          // A Hub process that ended: its pid is gone from the snapshot, or another one took
          // its place between two snapshots.
          if (have_hints && last_hints.processes_known && last_hints.hub_pid != 0 &&
            hints.hub_pid != last_hints.hub_pid)
          {
            hub_last_gone_pid.store(last_hints.hub_pid);
            hub_gone_count.fetch_add(1);
          }
          hub_pid_now.store(hints.hub_pid);
          hub_pid_known.store(true);
        }
        last_hints = hints;
        have_hints = true;
        if (back && current == SessionState::kLost) {
          hub_back_pending.store(true);
          wake_owner = true;
        }
      }
      const bool endpoint_long =
        endpoint_missing && now - endpoint_missing_since >= timings.hint_absent;
      const bool process_long =
        process_missing && now - process_missing_since >= timings.hint_absent;
      if (endpoint_long != last_endpoint_long || process_long != last_process_long) {
        last_endpoint_long = endpoint_long;
        last_process_long = process_long;
        wake_owner = true;
      }
      endpoint_gone_long.store(endpoint_long);
      hub_process_gone_long.store(process_long);
      set_silence(kSilenceEndpointGone, endpoint_long);
      set_silence(kSilenceHubProcessGone, process_long);

      // What the owner thread is inside.
      WatchInput in;
      in.now = now;
      in.joined = (current == SessionState::kRunning || current == SessionState::kReconnecting);
      in.lost = (current == SessionState::kLost);
      const char * call = call_clock.call.load();
      const std::int64_t since_ns = call_clock.since_ns.load();
      if (call != nullptr && since_ns > 0) {
        const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch()).count();
        in.call = call;
        in.call_ms = std::max<std::int64_t>(0, (now_ns - since_ns) / 1000000);
      }
      {
        std::lock_guard<std::mutex> lock(task_stats_mutex);
        if (!task_stats_data.running.empty()) {
          in.task = task_stats_data.running;
          in.task_ms = std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
              now - task_running_since).count());
        }
      }
      {
        std::lock_guard<std::mutex> lock(silence_mutex);
        in.silence = silence_bits;
        in.silent_ms = silent_active ?
          std::chrono::duration_cast<std::chrono::milliseconds>(now - silent_since).count() : 0;
      }
      in.endpoint_gone_long = endpoint_long;
      in.hub_process_gone_long = process_long;
      in.hub_came_back = hub_back_pending.load();
      const WatchVerdict verdict = policy.judge(in);

      // A loss that is due is the owner thread's to decide (check_lost after the call or task
      // returns); clearing the stall here would race with it.
      if (verdict.call_stalled || !verdict.lost_due) {
        set_silence(
          kSilenceCallStalled, verdict.call_stalled && in.joined, verdict.stalled_in,
          verdict.stalled_ms);
      }
      if (verdict.call_stalled) {
        // The log names a stall when it reaches one, two and four times its limit.
        const bool by_call = (in.call_ms >= 0 && in.call_ms >= timings.call_stall.count());
        const std::int64_t unit = (by_call ? timings.call_stall : timings.task_stall).count();
        const int level =
          verdict.stalled_ms >= 4 * unit ? 3 : (verdict.stalled_ms >= 2 * unit ? 2 : 1);
        if (stall_name != verdict.stalled_in) {
          stall_name = verdict.stalled_in;
          stall_level = 0;
        }
        if (level > stall_level) {
          stall_level = level;
          const bool is_task = verdict.stalled_in.rfind("task ", 0) == 0;
          log(std::string("watchdog: ") + (is_task ? "task " : "call ") +
            (is_task ? verdict.stalled_in.substr(5) : verdict.stalled_in) + " running for " +
            std::to_string(static_cast<long long>(verdict.stalled_ms)) + " ms on the owner thread");
        }
      } else {
        stall_name.clear();
        stall_level = 0;
      }
      if (verdict.abandon_due) {
        request_abandon(in.call, in.call_ms);
      }
      if (verdict.lost_due || verdict.rejoin_now) {
        wake_owner = true;
      }
      if (wake_owner) {
        post_tick();
      }
    }
  }

  // Leaves the owner thread running, unreferenced and silent, and makes sure the QarOS library is
  // never taken away under it. Consumer thread. @return false when the thread had ended already.
  bool abandon()
  {
    {
      std::lock_guard<std::mutex> lock(finished_mutex);
      if (owner_done) {
        return false;
      }
      abandoned.store(true);
      g_abandoned_owners.fetch_add(1);
      g_abandoned_running.fetch_add(1);
    }
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      consumer_ready.store(false);
      accepting_tasks.store(false);
      session_for_consumer.store(nullptr);
    }
    lane->close();
    {
      // The thread may wait in pop() with no deadline (its call returned before the
      // replacement): a message wakes it, and run() ends at the abandoned check.
      Msg wake;
      wake.type = Msg::Type::kTick;
      post(std::move(wake));
    }
    {
      // Peer updates of the left-behind session reach nobody: the consumer's listener belongs to
      // the fresh owner from now on. No wait: a delivery that runs finishes on its own.
      std::lock_guard<std::mutex> lock(listener_mutex);
      listener.reset();
    }
    library_lease().poisoned.store(true);
    pin_this_module_once();
    stop_watch_detached(kAbandonWatchWait);
    if (thread.joinable()) {
      thread.detach();
    }
    return true;
  }

  // Asks the consumer's next poll() to leave this owner thread behind. Any thread.
  void request_abandon(const std::string & call, std::int64_t call_ms)
  {
    if (exiting.load() || abandoned.load()) {
      return;
    }
    std::lock_guard<std::mutex> lock(abandon_mutex);
    if (abandon_requested.load()) {
      return;
    }
    abandoned_in = call;
    abandoned_in_ms.store(call_ms);
    abandon_requested.store(true);
  }

  // A person's command that the owner thread cannot run soon: when it is inside a call or a task
  // that runs far too long, the command must not wait for abandon_after, so the thread is left
  // behind at once and the fresh owner follows the command. Consumer thread, after the post.
  void abandon_for_stalled_command()
  {
    std::string name;
    std::int64_t ms = 0;
    bool stalled = false;
    const char * call = call_clock.call.load();
    const std::int64_t since_ns = call_clock.since_ns.load();
    if (call != nullptr && since_ns > 0) {
      const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
      ms = std::max<std::int64_t>(0, (now_ns - since_ns) / 1000000);
      if (ms >= timings.call_stall.count()) {
        stalled = true;
        name = call;
      }
    }
    if (!stalled) {
      std::lock_guard<std::mutex> lock(silence_mutex);
      if ((silence_bits & kSilenceCallStalled) != 0) {
        stalled = true;
        name = silent_note;
        ms = silent_note_ms;
      }
    }
    if (stalled) {
      log("a command arrived while " + name + " has run for " + std::to_string(ms) +
        " ms; leaving the owner thread behind");
      request_abandon(name, ms);
    }
  }

  ~Impl()
  {
    stop_watch();
    lane->close();
  }

};

// ---------------------------------------------------------------------------------------------
// QarSessionOwner
// ---------------------------------------------------------------------------------------------

QarSessionOwner::QarSessionOwner(SessionOwnerConfig config, std::unique_ptr<ILibraryLoader> loader)
: config_(std::move(config)), loader_(std::move(loader))
{
  // Before the first QarOS call, while nothing can hold the loader lock: the owner exit may then
  // leave threads behind without a call that changes the reference count of the module.
  pin_this_module_once();
  impl_.store(new Impl(config_, loader_));
}

// The destructor waits for the owner thread at most timings.exit_join. A thread that is still
// inside a call then is left behind (its Impl is never freed) and the library stays loaded.
QarSessionOwner::~QarSessionOwner()
{
  Impl * const impl = current();
  if (impl == nullptr) {
    return;
  }
  const Clock::time_point start = Clock::now();
  impl->task_closing.store(true);
  impl->exiting.store(true);
  impl->arm_drain(impl->timings.exit_drain);
  Msg message;
  message.type = Msg::Type::kShutdown;
  impl->post(std::move(message));
  bool finished = false;
  {
    std::unique_lock<std::mutex> lock(impl->finished_mutex);
    finished = impl->finished_cv.wait_until(
      lock, start + impl->timings.exit_join, [impl] { return impl->owner_done; });
  }
  if (!finished) {
    const std::string name = impl->stalled_name();
    if (impl->abandon()) {
      impl->log_raw(
        "exit: owner thread left running after " + std::to_string(millis_since(start)) +
        " ms (in " + name + "); QAROS stays loaded, the plugin module is pinned");
      return;
    }
  }
  // Both threads are joined only when they leave the OS within the budget; a thread that does
  // not is detached and the state stays allocated for it.
  impl->request_watch_stop();
  const Clock::time_point join_deadline = std::max(
    start + impl->timings.exit_join, Clock::now() + kExitLeaveGrace);
  const bool owner_left = Impl::join_within(impl->thread, join_deadline,
      [impl](Clock::time_point until) {
        std::unique_lock<std::mutex> lock(impl->finished_mutex);
        return impl->finished_cv.wait_until(lock, until, [impl] { return impl->owner_done; });
      });
  const bool watch_left = Impl::join_within(impl->watch_thread, join_deadline,
      [impl](Clock::time_point until) {
        std::unique_lock<std::mutex> lock(impl->watch_mutex);
        return impl->watch_done_cv.wait_until(lock, until, [impl] { return impl->watch_done; });
      });
  if (!owner_left || !watch_left) {
    impl->leak_on_destroy = true;
    impl->log_raw(
      "exit: a thread of the owner did not leave the process within the budget (owner " +
      std::string(owner_left ? "left" : "still there") + ", supervisor " +
      std::string(watch_left ? "left" : "still there") + ") after " +
      std::to_string(millis_since(start)) + " ms; left running; QAROS stays loaded");
    return;
  }
  impl->log_raw("exit: owner finished in " + std::to_string(millis_since(start)) + " ms");
  if (!impl->leak_on_destroy) {
    delete impl;
  }
  // else: a callback of the library may still fire into the state, which stays allocated.
}

void QarSessionOwner::replace_impl()
{
  Impl * const old = current();
  if (old == nullptr || !old->abandon_requested.load() || old->abandoned.load()) {
    return;
  }
  // What the stuck thread never ran is read before it is left behind: a person's command stands.
  const SessionState was = old->state.load();
  const bool joined = (was == SessionState::kRunning || was == SessionState::kReconnecting);
  const bool disconnect_unrun = old->disconnects_posted.load() > old->disconnects_done.load();
  const bool cancel_unrun = !joined && old->cancels_posted.load() > old->cancels_done.load();
  const bool closing = old->task_closing.load();
  bool automatic = false;
  {
    std::lock_guard<std::mutex> lock(old->rejoin_mutex);
    automatic = old->rejoin_view.automatic;
  }
  std::string call;
  {
    std::lock_guard<std::mutex> lock(old->abandon_mutex);
    call = old->abandoned_in.empty() ? std::string("a call") : old->abandoned_in;
  }
  const long long stuck_ms = old->abandoned_in_ms.load();
  if (!old->abandon()) {
    return;
  }
  const int count = abandoned_owner_threads();
  old->log_raw(
    "watchdog: owner thread abandoned in " + call + " after " + std::to_string(stuck_ms) +
    " ms (abandoned owners: " + std::to_string(count) + "); QAROS stays loaded");
  const std::string seconds = Impl::seconds_text(std::chrono::milliseconds(stuck_ms));
  Impl::Start start = Impl::Start::kLost;
  std::string text = "QAROS stopped answering inside " + call + " for " + seconds +
    " s; the owner thread was left behind";
  if (count >= old->timings.max_abandoned_owners) {
    start = Impl::Start::kFailed;
    text = "QAROS stopped answering inside a call (" + call + ") " +
      (count == 2 ? std::string("twice") : std::to_string(count) + " times") +
      ". RViz keeps working; restart RViz to stream again.";
  } else if (disconnect_unrun || cancel_unrun || closing) {
    // The person's command stands: the fresh owner starts idle.
    start = Impl::Start::kIdle;
    text += "; the disconnect of the person stands";
  } else if (was == SessionState::kStopping || was == SessionState::kIdle ||
    was == SessionState::kFailed)
  {
    start = Impl::Start::kIdle;
  } else if ((was == SessionState::kLoading || was == SessionState::kWaitingForApproval) &&
    !automatic)
  {
    // A join that never succeeded is not a session to rejoin: the person decides.
    start = Impl::Start::kFailed;
    text = "QAROS did not answer while joining (inside " + call + " for " + seconds +
      " s). Connect again to try once more.";
  }
  Impl * const fresh = new Impl(config_, loader_, start, text);
  impl_.store(fresh);
}

void QarSessionOwner::connect()
{
  Msg message;
  message.type = Msg::Type::kConnect;
  current()->post(std::move(message));
}

void QarSessionOwner::cancel()
{
  Impl * const impl = current();
  impl->cancels_posted.fetch_add(1);
  Msg message;
  message.type = Msg::Type::kCancel;
  impl->post(std::move(message));
  const SessionState now_state = impl->state.load();
  if (now_state != SessionState::kRunning && now_state != SessionState::kReconnecting) {
    impl->abandon_for_stalled_command();
  }
}

void QarSessionOwner::disconnect()
{
  Impl * const impl = current();
  impl->task_closing.store(true);
  impl->arm_drain();
  impl->disconnects_posted.fetch_add(1);
  Msg message;
  message.type = Msg::Type::kDisconnect;
  impl->post(std::move(message));
  impl->abandon_for_stalled_command();
}

void QarSessionOwner::release_lost_session()
{
  Msg message;
  message.type = Msg::Type::kHandBack;
  current()->post(std::move(message));
}

void QarSessionOwner::rejoin_now()
{
  Msg message;
  message.type = Msg::Type::kRejoinNow;
  current()->post(std::move(message));
}

void QarSessionOwner::declare_lost(std::string reason)
{
  Impl * const impl = current();
  Msg message;
  message.type = Msg::Type::kDeclareLost;
  message.text = std::move(reason);
  impl->post(std::move(message));
  const SessionState now_state = impl->state.load();
  if (now_state == SessionState::kRunning || now_state == SessionState::kReconnecting) {
    impl->abandon_for_stalled_command();
  }
}

void QarSessionOwner::set_target_listener(std::weak_ptr<ITargetListener> listener)
{
  Impl * const impl = current();
  if (t_in_listener) {
    // Called from inside the listener, which already holds the delivery lock: the listener is
    // swapped without waiting and without a replay.
    std::lock_guard<std::mutex> lock(impl->listener_mutex);
    impl->listener = listener;
    return;
  }
  std::shared_ptr<ITargetListener> target = listener.lock();
  // Waits for the delivery in progress. When the new listener is empty, no delivery uses the old
  // one after this returns, so the consumer may release it.
  std::lock_guard<std::mutex> delivery(impl->delivery_mutex);
  {
    std::lock_guard<std::mutex> lock(impl->listener_mutex);
    impl->listener = listener;
  }
  // A listener that arrives after targets came up hears about the ones that have a stream. The
  // table is read under the delivery lock, so a target that went down is not announced.
  if (target) {
    ListenerScope in_listener;
    for (const TargetInfo & info : targets()) {
      if (info.served) {
        target->on_target_up(info);
      }
    }
    // Released while the delivery lock is held: a caller that removes the listener and then
    // releases it (detach) must not leave the last reference to this thread.
    target.reset();
  }
}

void QarSessionOwner::poll(std::vector<SessionEvent> & out)
{
  if (current()->abandon_requested.load()) {
    replace_impl();
  }
  Impl * const impl = current();
  std::vector<SessionEvent> taken;
  {
    std::lock_guard<std::mutex> lock(impl->events_mutex);
    taken.swap(impl->events);
  }
  if (taken.empty()) {
    return;
  }
  {
    // The flag, the state and the session are read and the flag is set under the lock that
    // clears it, so a state that has moved on cannot be declared ready.
    std::lock_guard<std::mutex> lock(impl->ready_mutex);
    bool joined = impl->consumer_ready.load();
    for (const SessionEvent & event : taken) {
      if (event.kind == SessionEvent::Kind::kState) {
        joined = (event.state == SessionState::kRunning ||
          event.state == SessionState::kReconnecting);
      }
    }
    const SessionState now_state = impl->state.load();
    const bool running =
      (now_state == SessionState::kRunning || now_state == SessionState::kReconnecting);
    if (poll_hook_) {
      poll_hook_();
    }
    const bool up = joined && running && impl->session_for_consumer.load() != nullptr;
    impl->consumer_ready.store(up);
    impl->accepting_tasks.store(up);
  }
  for (SessionEvent & event : taken) {
    out.push_back(std::move(event));
  }
}

SessionState QarSessionOwner::state() const
{
  return current()->state.load();
}

std::vector<TargetInfo> QarSessionOwner::targets() const
{
  Impl * const impl = current();
  std::vector<TargetInfo> out;
  std::lock_guard<std::mutex> lock(impl->targets_mutex);
  for (const Impl::TargetEntry & entry : impl->table) {
    if (entry.present) {
      out.push_back(entry.info);
    }
  }
  return out;
}

OwnerHealth QarSessionOwner::health() const
{
  Impl * const impl = current();
  OwnerHealth out;
  {
    std::lock_guard<std::mutex> lock(impl->silence_mutex);
    out.silence = impl->silence_bits;
    if (impl->silent_active) {
      out.silent_for_ms = static_cast<int>(millis_since(impl->silent_since));
    }
    if ((impl->silence_bits & kSilenceCallStalled) != 0) {
      out.stalled_in = impl->silent_note;
      out.stalled_for_ms = static_cast<int>(impl->silent_note_ms);
    }
  }
  out.abandoned_owners = abandoned_owner_threads();
  return out;
}

RejoinInfo QarSessionOwner::rejoin_info() const
{
  Impl * const impl = current();
  std::lock_guard<std::mutex> lock(impl->rejoin_mutex);
  RejoinInfo out = impl->rejoin_view;
  if (impl->rejoin_next_armed) {
    out.next_in_ms = static_cast<int>(std::max<long long>(0,
      std::chrono::duration_cast<std::chrono::milliseconds>(
        impl->rejoin_next_at - Clock::now()).count()));
  } else {
    out.next_in_ms = -1;
  }
  if (impl->lost_view_active) {
    out.lost_for_ms = static_cast<int>(millis_since(impl->lost_since_view));
  }
  return out;
}

HubProcessInfo QarSessionOwner::hub_process() const
{
  Impl * const impl = current();
  HubProcessInfo out;
  out.known = impl->hub_pid_known.load();
  out.pid = impl->hub_pid_now.load();
  out.gone_count = impl->hub_gone_count.load();
  out.last_gone_pid = impl->hub_last_gone_pid.load();
  return out;
}

QarPeerId QarSessionOwner::self_peer() const
{
  Impl * const impl = current();
  std::lock_guard<std::mutex> lock(impl->targets_mutex);
  return impl->has_self ? impl->self : qar_peer_id_default();
}

std::string QarSessionOwner::runtime_description() const
{
  Impl * const impl = current();
  std::lock_guard<std::mutex> lock(impl->description_mutex);
  return impl->description;
}

bool QarSessionOwner::ready() const
{
  return current()->consumer_ready.load();
}

QarSession * QarSessionOwner::session() const
{
  return ready() ? current()->session_for_consumer.load() : nullptr;
}

std::shared_ptr<IReleaseLane> QarSessionOwner::release_lane() const
{
  return current()->lane;
}

bool QarSessionOwner::post_session_task(
  const char * name, SessionTask task, std::function<void()> dropped)
{
  if (!task) {
    return false;
  }
  Impl * const impl = current();
  // The epoch is read before the flags: a task that races with a release carries the old epoch
  // and is dropped by the owner thread, never run on the next session.
  const std::uint64_t epoch = impl->session_epoch.load();
  if (!impl->accepting_tasks.load() || impl->task_closing.load()) {
    return false;
  }
  if (task_post_hook_) {
    task_post_hook_();
  }
  Msg message;
  message.type = Msg::Type::kTask;
  message.task_name = name != nullptr ? name : "task";
  message.task = std::move(task);
  message.dropped = std::move(dropped);
  message.task_epoch = epoch;
  impl->tasks_queued.fetch_add(1);
  impl->post(std::move(message));
  return true;
}

QarSessionOwner::TaskStats QarSessionOwner::task_stats() const
{
  Impl * const impl = current();
  TaskStats out;
  {
    std::lock_guard<std::mutex> lock(impl->task_stats_mutex);
    out = impl->task_stats_data;
    if (!out.running.empty()) {
      out.running_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - impl->task_running_since).count();
    }
  }
  out.queued = impl->tasks_queued.load();
  return out;
}

std::string QarSessionOwner::invite_device(DeviceKind kind, const std::string & host)
{
  if (!ready()) {
    return "QAROS is not connected yet.";
  }
  Impl * const impl = current();
  const std::size_t first = host.find_first_not_of(" \t");
  const std::size_t last = host.find_last_not_of(" \t");
  if (first == std::string::npos) {
    return "Type the host name or the address of the device.";
  }
  QarSession * const open_session = impl->session_for_consumer.load();
  if (open_session == nullptr) {
    return "QAROS is not connected yet.";
  }
  auto invite = std::make_unique<Impl::InviteOp>();
  invite->kind_text = (kind == DeviceKind::kHoloLens) ? "hololens" : "headset";
  invite->host = host.substr(first, last - first + 1);
  invite->connection = invite->kind_text + ":" + invite->host;
  invite->init = qar_session_invite_target_app_init_default();
  invite->init.connection_string = invite->connection.c_str();
  {
    std::lock_guard<std::mutex> lock(impl->invites_mutex);
    invite->id = impl->next_invite++;
  }
  invite->ctx.impl = impl;
  invite->ctx.id = invite->id;
  if (failed(QAR_CALL(qar_cancel_token_create, &invite->token)) || invite->token == nullptr) {
    return "The invite cannot be started (no cancel token).";
  }
  Impl::InviteOp * raw = invite.get();
  {
    std::lock_guard<std::mutex> lock(impl->invites_mutex);
    impl->invites.push_back(std::move(invite));
  }
  const QarResult result = QAR_CALL(
      qar_session_invite_target_app_async, open_session, &raw->init, &Impl::invite_trampoline,
      nullptr, &raw->ctx, raw->token);
  if (failed(result)) {
    const StatusText text = describe_status(
      static_cast<int>(result.code), OnboardingContext::kInvite, result_message(result).c_str());
    QarCancelToken * token = raw->token;
    raw->token = nullptr;
    {
      std::lock_guard<std::mutex> lock(impl->invites_mutex);
      for (auto it = impl->invites.begin(); it != impl->invites.end(); ++it) {
        if (it->get() == raw) {
          // The library may still call the result callback with this context.
          impl->refused_invites.push_back(std::move(*it));
          impl->invites.erase(it);
          break;
        }
      }
    }
    QAR_CALL(qar_cancel_token_handle_destroy, token);
    return text.message;
  }
  return std::string();
}

int abandoned_owner_threads()
{
  return g_abandoned_owners.load();
}

int abandoned_owner_threads_running()
{
  return g_abandoned_running.load();
}

SessionState wait_for_session(
  QarSessionOwner & owner, std::chrono::milliseconds deadline, std::vector<SessionEvent> * seen)
{
  const auto end = Clock::now() + deadline;
  for (;;) {
    std::vector<SessionEvent> events;
    owner.poll(events);
    if (seen != nullptr) {
      seen->insert(seen->end(), events.begin(), events.end());
    }
    const SessionState current = owner.state();
    if ((current == SessionState::kRunning && owner.ready()) || current == SessionState::kFailed) {
      return current;
    }
    if (Clock::now() >= end) {
      return current;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace qar_qar_adapter
