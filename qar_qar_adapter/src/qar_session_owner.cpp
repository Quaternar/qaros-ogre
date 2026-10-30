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
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/identity_store.hpp"

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
constexpr std::chrono::seconds kHubDownGrace{3};
constexpr std::chrono::milliseconds kBusyRetryDelay{1500};
constexpr std::uint32_t kMaxApprovalTimeoutMs = 600000;
constexpr double kSlowTaskMs = 1000.0;
constexpr std::uint64_t kSlowTaskLogFirst = 20;
constexpr std::uint64_t kSlowTaskLogEvery = 50;

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
    case SessionState::kRejoining: return "rejoining";
    case SessionState::kNeedsCode: return "needs_code";
    case SessionState::kPairing: return "pairing";
    case SessionState::kRunning: return "running";
    case SessionState::kReconnecting: return "reconnecting";
    case SessionState::kStopping: return "stopping";
    case SessionState::kFailed: return "failed";
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
    // commands (kTask: a call of the consumer, run in order with the others)
    kConnect, kSubmitCode, kCancel, kDisconnect, kForget, kConfigure, kShutdown, kTask,
    // from QAROS threads
    kOnboardResult, kProgress, kInviteResult, kHubPresence
  };
  Type type = Type::kConnect;
  // kSubmitCode, kConfigure
  std::string code;
  HubEndpoint hub;
  // kConfigure
  HubMode mode = HubMode::kAuto;
  // kOnboardResult, kProgress
  std::uint64_t attempt = 0;
  QarResult result{};
  QarOnboardingId onboarding_id{};
  bool has_onboarding_id = false;
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
  return type == Msg::Type::kConnect || type == Msg::Type::kSubmitCode ||
         type == Msg::Type::kCancel || type == Msg::Type::kDisconnect ||
         type == Msg::Type::kForget || type == Msg::Type::kConfigure ||
         type == Msg::Type::kShutdown || type == Msg::Type::kTask;
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

  // One onboarding or rejoin attempt. The init blocks and the strings they point at live here,
  // because the library may read them until the result callback has fired.
  struct OpCtx
  {
    Impl * impl = nullptr;
    std::uint64_t attempt = 0;
    OnboardingContext kind = OnboardingContext::kLocal;
    QarOnboardInit onboard{};
    QarRejoinInit rejoin{};
    QarOnboardLocalAppExt local{};
    QarOnboardCodeExt code_ext{};
    QarOnboardHostExt host_ext{};
    std::string display_name, app_version, custom_info;
    std::string code;
    HubEndpoint hub;
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

  // --- constants of the program -----------------------------------------------------------
  SessionOwnerConfig cfg;
  std::unique_ptr<ILibraryLoader> loader;
  std::string identity_dir, identity_path, storage_dir;

  // --- state seen by every thread -----------------------------------------------------------
  std::atomic<SessionState> state{SessionState::kIdle};
  std::atomic<HubMode> mode{HubMode::kAuto};
  std::atomic<bool> consumer_ready{false};
  // Tasks are accepted from the hand-over of the session (poll()) until a disconnect, a forget or
  // the destructor is called, or the state leaves running. `task_closing` is set on the caller's
  // thread by those three, so a task cannot be posted after them even while the session is up.
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
  std::atomic<bool> identity_present{false};
  std::atomic<QarSession *> session_for_consumer{nullptr};

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
  bool loaded_library = false;
  bool library_initialised = false;
  bool leak_on_destroy = false;
  bool busy_retried = false;
  OnboardingContext retry_kind = OnboardingContext::kLocal;
  bool retry_armed = false;
  Clock::time_point retry_at{};
  bool hub_down_armed = false;
  Clock::time_point hub_down_at{};
  Clock::time_point joining_started{};
  bool config_pending = false;
  HubMode pending_mode = HubMode::kAuto;
  HubEndpoint pending_remote;

  explicit Impl(SessionOwnerConfig config, std::unique_ptr<ILibraryLoader> library_loader)
  : cfg(std::move(config)), loader(std::move(library_loader))
  {
    mode.store(cfg.mode);
    identity_dir = cfg.identity_dir.empty() ?
      default_identity_dir(cfg.program_tag, cfg.instance) : cfg.identity_dir;
    if (!identity_dir.empty()) {
      identity_path = identity_file_path(identity_dir);
      storage_dir = (std::filesystem::u8path(identity_dir) / "runtime").u8string();
    }
    IdentityRecord record;
    std::string error;
    identity_present.store(
      !identity_path.empty() &&
      read_identity(identity_path, record, error) != IdentityRead::kAbsent);
    thread = std::thread([this] { run(); });
  }

  // =========================================================================================
  // Logging and events
  // =========================================================================================

  void log(const std::string & text)
  {
    const std::string line = "QAROS: " + text;
    if (cfg.log_sink) {
      cfg.log_sink(line);
    } else {
      std::fprintf(stderr, "%s\n", line.c_str());
      std::fflush(stderr);
    }
  }

  void push(SessionEvent event)
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(std::move(event));
  }

  void store_state(SessionState next)
  {
    std::lock_guard<std::mutex> lock(ready_mutex);
    state.store(next);
    if (next != SessionState::kRunning && next != SessionState::kReconnecting) {
      consumer_ready.store(false);
      accepting_tasks.store(false);
    }
  }

  void set_state(SessionState next, const StatusText * cause = nullptr)
  {
    store_state(next);
    SessionEvent event;
    event.kind = SessionEvent::Kind::kState;
    event.state = next;
    if (cause != nullptr) {
      event.status = *cause;
    }
    push(std::move(event));
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

  void run()
  {
    mark_owner_thread(
      [this](const char * call, double took_ms) { report_slow_call(call, took_ms); });
    for (;;) {
      Msg message;
      bool has_deadline = false;
      Clock::time_point deadline{};
      if (retry_armed) {
        has_deadline = true;
        deadline = retry_at;
      }
      if (hub_down_armed && (!has_deadline || hub_down_at < deadline)) {
        has_deadline = true;
        deadline = hub_down_at;
      }
      if (!pop(message, false, has_deadline, deadline)) {
        on_timers();
        continue;
      }
      if (message.type == Msg::Type::kShutdown) {
        shutdown();
        drop_queued_tasks("the owner ended");
        mark_owner_thread({});
        return;
      }
      handle(message);
      if (message.type == Msg::Type::kTask) {
        // A backlog of tasks never keeps the timers from firing: they are due after at most one
        // task.
        on_timers();
      }
    }
  }

  void handle(Msg & message)
  {
    switch (message.type) {
      case Msg::Type::kConnect: on_connect(); break;
      case Msg::Type::kSubmitCode: on_submit_code(message); break;
      case Msg::Type::kCancel: on_cancel(); break;
      case Msg::Type::kDisconnect: on_disconnect(false); break;
      case Msg::Type::kForget: on_disconnect(true); break;
      case Msg::Type::kConfigure: on_configure(message); break;
      case Msg::Type::kOnboardResult: on_onboard_result(message); break;
      case Msg::Type::kProgress: on_progress(message); break;
      case Msg::Type::kInviteResult: on_invite_result(message); break;
      case Msg::Type::kHubPresence: on_hub_presence(message); break;
      case Msg::Type::kShutdown: break;
      case Msg::Type::kTask: on_task(message); break;
    }
  }

  // =========================================================================================
  // Tasks of the consumer
  // =========================================================================================

  void arm_drain()
  {
    drain_deadline.store((Clock::now() + QarSessionOwner::kTaskDrainBudget).time_since_epoch()
      .count());
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
    if (message.task_epoch != session_epoch.load() || session == nullptr) {
      drop_task(message, "the session it was posted for ended");
      return;
    }
    if (drain_expired()) {
      drop_task(message, "teardown: drain budget " +
        std::to_string(QarSessionOwner::kTaskDrainBudget.count()) + " s spent");
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
        start_attempt(retry_kind);
      }
    }
    if (hub_down_armed && now >= hub_down_at) {
      hub_down_armed = false;
      if (state.load() == SessionState::kRunning) {
        StatusText cause;
        cause.message = "The QAROS Hub does not answer. Waiting for it to come back.";
        set_state(SessionState::kReconnecting, &cause);
      }
    }
  }

  // =========================================================================================
  // Starting
  // =========================================================================================

  void fail(
    int code, const char * detail, OnboardingContext context, const char * address = nullptr)
  {
    StatusText cause = describe_status(code, context, detail, address);
    set_state(SessionState::kFailed, &cause);
  }

  bool ensure_runtime(OnboardingContext context)
  {
    if (runtime != nullptr) {
      return true;
    }
    if (!qar_is_library_loaded()) {
      std::string error;
      if (!loader->load(cfg.library_path, error)) {
        fail(kStatusNoRuntime, error.c_str(), context);
        return false;
      }
      loaded_library = true;
    }

    QarRuntimeInfo info{};
    if (!failed(qar_runtime_info(&info))) {
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

    if (!library_initialised) {
      if (!init_library(context)) {
        return false;
      }
    }

    QarRuntimeInit init = qar_runtime_init_default();
    if (!storage_dir.empty()) {
      init.storage_folder_path = storage_dir.c_str();
    }
    QarResult result{};
#ifdef _WIN32
    if (guarded_runtime_create(&init, &runtime, &result) != 0) {
      runtime = nullptr;
      fail(QAR_STATUS_UNCLASSIFIED, "qar_runtime_create faulted (see the SEH FAULT line)", context);
      return false;
    }
#else
    result = qar_runtime_create(&init, &runtime);
#endif
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
    if (guarded_library_init(&init, &result) != 0) {
      fail(QAR_STATUS_UNCLASSIFIED, "qar_library_init faulted (see the SEH FAULT line)", context);
      return false;
    }
#else
    result = qar_library_init(&init);
#endif
    if (failed(result)) {
      fail(static_cast<int>(result.code), result_message(result).c_str(), context);
      return false;
    }
    library_initialised = true;
    return true;
  }

  void apply_config(HubMode new_mode, const HubEndpoint & remote)
  {
    cfg.mode = new_mode;
    cfg.remote = remote;
    mode.store(new_mode);
    config_pending = false;
  }

  void on_configure(const Msg & message)
  {
    const SessionState now_state = state.load();
    if (now_state == SessionState::kIdle || now_state == SessionState::kFailed) {
      apply_config(message.mode, message.hub);
      return;
    }
    config_pending = true;
    pending_mode = message.mode;
    pending_remote = message.hub;
    log(std::string("settings kept for the next connect (state ") + state_name(now_state) + ")");
  }

  void on_connect()
  {
    const SessionState now_state = state.load();
    if (now_state != SessionState::kIdle && now_state != SessionState::kFailed) {
      return;
    }
    if (config_pending) {
      apply_config(pending_mode, pending_remote);
    }
    busy_retried = false;
    retry_armed = false;
    task_closing.store(false);
    drain_deadline.store(0);
    joining_started = Clock::now();
    set_state(SessionState::kLoading);
    if (!ensure_runtime(OnboardingContext::kLocal)) {
      return;
    }

    IdentityRecord record;
    std::string error;
    bool has_identity = false;
    if (!identity_path.empty()) {
      const IdentityRead read = read_identity(identity_path, record, error);
      has_identity = (read == IdentityRead::kOk);
      identity_present.store(read != IdentityRead::kAbsent);
      if (read == IdentityRead::kMalformed) {
        log("identity file damaged: pair again (" + error + ")");
        SessionEvent event;
        event.kind = SessionEvent::Kind::kLog;
        event.text = "The stored pairing is damaged. Pair again with a code.";
        push(std::move(event));
      }
    }
    std::string installed_folder;
    const bool installed = loader->installed_bin_folder(installed_folder);
    const char * reason = "";
    const HubMode chosen = resolve_hub_mode(cfg.mode, has_identity, installed, &reason);
    mode.store(chosen);
    log(std::string("hub mode ") + (chosen == HubMode::kLocal ? "local" : "remote") + " (" +
      reason + ")");

    if (chosen == HubMode::kLocal) {
      start_attempt(OnboardingContext::kLocal);
    } else if (has_identity) {
      start_attempt(OnboardingContext::kRejoin);
    } else {
      set_state(SessionState::kNeedsCode);
    }
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
    Msg message;
    message.type = Msg::Type::kOnboardResult;
    message.attempt = ctx->attempt;
    message.result = status;
    if (id != nullptr) {
      message.onboarding_id = *id;
      message.has_onboarding_id = true;
    }
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

  // Starts a local onboarding, a rejoin, or (after submit_code) a code pairing, according to
  // @p kind. The code and the Hub of a pairing are in op->code and op->hub already.
  void start_attempt(OnboardingContext kind, std::unique_ptr<OpCtx> prepared = nullptr)
  {
    std::unique_ptr<OpCtx> ctx = prepared ? std::move(prepared) : std::make_unique<OpCtx>();
    ctx->impl = this;
    ctx->attempt = next_attempt++;
    ctx->kind = kind;
    ctx->started = Clock::now();

    if (kind == OnboardingContext::kRejoin) {
      IdentityRecord record;
      std::string error;
      if (read_identity(identity_path, record, error) != IdentityRead::kOk) {
        set_state(SessionState::kNeedsCode);
        return;
      }
      ctx->hub.host = record.host;
      ctx->hub.port = record.port;
      QarOnboardingId id = qar_onboarding_id_default();
      if (failed(qar_uuid_from_string(record.id.c_str(), id.data))) {
        log("identity file damaged: pair again (the id is not a UUID)");
        set_state(SessionState::kNeedsCode);
        return;
      }
      ctx->rejoin = qar_rejoin_init_default();
      ctx->rejoin.onboarding_id = id;
      fill_presentation(*ctx, ctx->rejoin.presentation);
    } else {
      ctx->onboard = qar_onboard_init_default();
      fill_presentation(*ctx, ctx->onboard.presentation);
      if (kind == OnboardingContext::kLocal) {
        ctx->local = qar_onboard_local_app_ext_default();
        ctx->local.approval_timeout_ms = std::min(cfg.approval_timeout_ms, kMaxApprovalTimeoutMs);
        ctx->onboard.header.next = &ctx->local.header;
      } else {
        ctx->code_ext = qar_onboard_code_ext_default();
        ctx->code_ext.code = ctx->code.c_str();
        ctx->host_ext = qar_onboard_host_ext_default();
        ctx->host_ext.hostname = ctx->hub.host.c_str();
        ctx->host_ext.port = ctx->hub.port;
        ctx->code_ext.header.next = &ctx->host_ext.header;
        ctx->onboard.header.next = &ctx->code_ext.header;
      }
    }

    if (failed(qar_cancel_token_create(&ctx->token)) || ctx->token == nullptr) {
      ctx->token = nullptr;
      fail(QAR_STATUS_UNCLASSIFIED, "the cancel token cannot be created", kind);
      return;
    }

    SessionState next_state = SessionState::kWaitingForApproval;
    if (kind == OnboardingContext::kRejoin) {
      next_state = SessionState::kRejoining;
    } else if (kind == OnboardingContext::kRemote) {
      next_state = SessionState::kPairing;
    }
    set_state(next_state);

    QarResult result{};
    OpCtx * raw = ctx.get();
    if (kind == OnboardingContext::kRejoin) {
      result = qar_runtime_rejoin_async(
        runtime, &raw->rejoin, &Impl::onboard_result_trampoline, &Impl::progress_trampoline, raw,
        raw->token);
    } else {
      result = qar_runtime_onboard_async(
        runtime, &raw->onboard, &Impl::onboard_result_trampoline, &Impl::progress_trampoline, raw,
        raw->token);
    }
    // The library copied the code before the call returned.
    std::fill(raw->code.begin(), raw->code.end(), '\0');
    raw->code.clear();
    if (failed(result)) {
      const std::string detail = result_message(result);
      const int code = static_cast<int>(result.code);
      qar_cancel_token_handle_destroy(raw->token);
      raw->token = nullptr;
      const HubEndpoint hub = raw->hub;
      // The library may still call the result callback with this context.
      refused_attempts.push_back(std::move(ctx));
      handle_failure(code, kind, detail, hub);
      return;
    }
    op = std::move(ctx);
  }

  // =========================================================================================
  // Commands
  // =========================================================================================

  void on_submit_code(Msg & message)
  {
    if (state.load() != SessionState::kNeedsCode) {
      return;
    }
    if (message.hub.host.empty() || message.hub.port == 0) {
      refuse_code(describe_status(
          QAR_STATUS_ARGUMENT_NOT_SUPPORTED, OnboardingContext::kRemote, nullptr));
      return;
    }
    if (message.code.empty()) {
      StatusText cause;
      cause.code = QAR_STATUS_ARGUMENT_NOT_SUPPORTED;
      cause.symbol = status_symbol(cause.code);
      cause.message = "Type the pairing code the Hub shows.";
      cause.retry_offered = true;
      refuse_code(cause);
      return;
    }
    auto ctx = std::make_unique<OpCtx>();
    ctx->code = std::move(message.code);
    ctx->hub = message.hub;
    joining_started = Clock::now();
    start_attempt(OnboardingContext::kRemote, std::move(ctx));
  }

  void refuse_code(const StatusText & cause)
  {
    SessionEvent event;
    event.kind = SessionEvent::Kind::kState;
    event.state = SessionState::kNeedsCode;
    event.status = cause;
    push(std::move(event));
    log("status " + cause.symbol + ": " + cause.message);
  }

  void on_cancel()
  {
    const SessionState current = state.load();
    if (current == SessionState::kNeedsCode) {
      set_state(SessionState::kIdle);
      return;
    }
    if ((current == SessionState::kWaitingForApproval || current == SessionState::kRejoining ||
      current == SessionState::kPairing) && op)
    {
      op->cancel_requested = true;
      qar_cancel_token_cancel(op->token);
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
  }

  // Ends the attempt: destroys its token and its context. The result callback has fired, so
  // the library does not read either again.
  void finish_attempt()
  {
    if (!op) {
      return;
    }
    if (op->token != nullptr) {
      qar_cancel_token_handle_destroy(op->token);
      op->token = nullptr;
    }
    op.reset();
  }

  void on_onboard_result(Msg & message)
  {
    if (!op || message.attempt != op->attempt) {
      if (message.session != nullptr) {
        qar_session_handle_destroy(message.session);
      }
      return;
    }
    const OnboardingContext kind = op->kind;
    const bool cancelled = op->cancel_requested;
    const HubEndpoint hub = op->hub;
    const double elapsed = seconds_since(op->started);
    finish_attempt();

    const bool ok = !failed(message.result) && message.session != nullptr;
    if (cancelled) {
      if (message.session != nullptr) {
        qar_session_handle_destroy(message.session);
      }
      set_state(
        kind == OnboardingContext::kRemote ? SessionState::kNeedsCode : SessionState::kIdle);
      return;
    }
    if (!ok) {
      if (message.session != nullptr) {
        qar_session_handle_destroy(message.session);
      }
      const int code = failed(message.result) ? static_cast<int>(message.result.code) :
        static_cast<int>(QAR_STATUS_ONBOARDING_FAILED);
      handle_failure(code, kind, result_message(message.result), hub);
      return;
    }
    on_joined(message, kind, hub, elapsed);
  }

  void handle_failure(int code, OnboardingContext kind, const std::string & detail,
    const HubEndpoint & hub)
  {
    const std::string address = hub.host.empty() ? std::string() :
      hub.host + ":" + std::to_string(hub.port);
    StatusText cause = describe_status(code, kind, detail.c_str(), address.c_str());

    if (code == QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING && !busy_retried &&
      kind != OnboardingContext::kRemote)
    {
      busy_retried = true;
      retry_armed = true;
      retry_kind = kind;
      retry_at = Clock::now() + kBusyRetryDelay;
      SessionEvent event;
      event.kind = SessionEvent::Kind::kProgress;
      event.text = cause.message;
      push(std::move(event));
      log("progress " + cause.message);
      store_state(SessionState::kLoading);
      return;
    }

    const bool needs_pairing =
      code == QAR_STATUS_PAKE_ERROR || code == QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND ||
      code == QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED ||
      code == QAR_STATUS_ONBOARDING_RENEWAL_POSSESSION_UNPROVEN;
    if (kind == OnboardingContext::kRemote ||
      (kind == OnboardingContext::kRejoin && needs_pairing))
    {
      set_state(SessionState::kNeedsCode, &cause);
    } else {
      set_state(SessionState::kFailed, &cause);
    }
  }

  void on_joined(Msg & message, OnboardingContext kind, const HubEndpoint & hub, double elapsed)
  {
    session = message.session;

    if (kind == OnboardingContext::kRemote && message.has_onboarding_id) {
      char text[64] = {0};
      IdentityRecord record;
      if (!failed(qar_uuid_to_string(message.onboarding_id.data, text, sizeof(text)))) {
        record.id = text;
        record.host = hub.host;
        record.port = hub.port;
        record.paired_unix_s = static_cast<std::int64_t>(std::time(nullptr));
        std::string error;
        if (identity_path.empty() || !write_identity_atomic(identity_path, record, error)) {
          log("identity file not written: " +
            (identity_path.empty() ? std::string("no folder") : error));
          SessionEvent event;
          event.kind = SessionEvent::Kind::kLog;
          event.text = "The pairing could not be stored; the next start needs a code again.";
          push(std::move(event));
        } else {
          identity_present.store(true);
        }
      }
    }

    {
      QarPeerSpec * mine = nullptr;
      if (!failed(qar_session_get_my_spec(session, &mine)) && mine != nullptr) {
        QarPeerId id = qar_peer_id_default();
        if (!failed(qar_peer_spec_get_id(mine, &id))) {
          std::lock_guard<std::mutex> lock(targets_mutex);
          self = id;
          has_self = true;
        }
        qar_peer_spec_handle_destroy(mine);
      }
    }

    if (failed(qar_cancel_token_create(&peer_token)) || peer_token == nullptr) {
      peer_token = nullptr;
      abandon_session("the peer token cannot be created");
      return;
    }
    const QarResult subscribed =
      qar_peer_subscribe_updates(session, &Impl::peer_trampoline, this, peer_token);
    if (failed(subscribed)) {
      const std::string detail = result_message(subscribed);
      abandon_session(detail.c_str());
      return;
    }
    query_initial_peers();

    const char * how = kind == OnboardingContext::kLocal ? "local" :
      (kind == OnboardingContext::kRejoin ? "rejoin" : "pairing");
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", elapsed);
    QarPeerId mine_id{};
    {
      std::lock_guard<std::mutex> lock(targets_mutex);
      mine_id = self;
    }
    log("joined as peer " + peer_hex8(mine_id) + " (" + how + ") in " + seconds + " s");

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
      qar_session_handle_destroy(session);
      session = nullptr;
    }
    fail(QAR_STATUS_ONBOARDING_FAILED, detail, OnboardingContext::kLocal);
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
    if (failed(qar_query_peer_specs_count(session, &count)) || count == 0) {
      return;
    }
    std::vector<QarPeerSpec *> handles(count, nullptr);
    std::size_t written = 0;
    if (failed(qar_query_peer_specs(session, handles.data(), count, &written))) {
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
      qar_peer_spec_handle_destroy(handle);   // owned: the query hands the handles over
    }
  }

  void on_hub_presence(const Msg & message)
  {
    if (message.hub_down) {
      if (state.load() == SessionState::kRunning) {
        hub_down_armed = true;
        hub_down_at = Clock::now() + kHubDownGrace;
      }
      return;
    }
    hub_down_armed = false;
    if (state.load() == SessionState::kReconnecting) {
      set_state(SessionState::kRunning);
    }
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
      qar_cancel_token_handle_destroy(finished->token);
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
  void abort_attempt()
  {
    if (!op) {
      return;
    }
    op->cancel_requested = true;
    const Clock::time_point start = Clock::now();
    if (op->token != nullptr) {
      qar_cancel_token_cancel(op->token);
    }
    const Clock::time_point deadline = start + kOperationWait;
    while (op) {
      Msg message;
      if (!pop(message, true, true, deadline)) {
        log("teardown: the onboarding result did not arrive within " +
          std::to_string(kOperationWait.count()) + " s");
        leak_on_destroy = true;
        (void)op.release();   // the library may still read it
        break;
      }
      if (message.type == Msg::Type::kOnboardResult && message.attempt == op->attempt) {
        if (message.session != nullptr) {
          qar_session_handle_destroy(message.session);
        }
        finish_attempt();
      } else {
        handle(message);
      }
    }
    log("teardown: onboarding attempt ended in " + std::to_string(millis_since(start)) + " ms");
  }

  void end_invites()
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
        qar_cancel_token_cancel(invite->token);
      }
    }
    const Clock::time_point deadline = start + kOperationWait;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(invites_mutex);
        if (invites.empty()) {
          break;
        }
      }
      Msg message;
      if (!pop(message, true, true, deadline)) {
        log("teardown: an invite result did not arrive within " +
          std::to_string(kOperationWait.count()) + " s");
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
    qar_cancel_token_cancel(peer_token);
    qar_cancel_token_handle_destroy(peer_token);
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
  void release_session()
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
    end_invites();
    end_peer_subscription();
    clear_targets();
    if (session != nullptr) {
      const Clock::time_point start = Clock::now();
      qar_session_handle_destroy(session);
      session = nullptr;
      log("teardown: session released in " + std::to_string(millis_since(start)) + " ms");
    }
    drop_queued_tasks("the session ended");
  }

  void on_disconnect(bool forgetting)
  {
    const SessionState current = state.load();
    if (current == SessionState::kStopping) {
      return;
    }
    if (current == SessionState::kLoading && retry_armed && !forgetting) {
      retry_armed = false;
      set_state(SessionState::kIdle);
      return;
    }
    if (forgetting && !identity_present.load() && mode.load() != HubMode::kRemote &&
      cfg.mode != HubMode::kRemote)
    {
      log("forget ignored: no pairing with another PC is stored");
      task_closing.store(false);
      // The session goes on, so the drain deadline forget() armed must not drop its tasks.
      drain_deadline.store(0);
      return;
    }
    if (!forgetting && current == SessionState::kIdle) {
      return;
    }
    const bool had_work = op || session != nullptr;
    if (had_work) {
      set_state(SessionState::kStopping);
    }
    abort_attempt();
    release_session();
    if (forgetting) {
      forget_pairing();
    }
    set_state(SessionState::kIdle);
  }

  void forget_pairing()
  {
    SessionEvent event;
    event.kind = SessionEvent::Kind::kForgetDone;
    event.status.message = "The pairing was erased.";
    IdentityRecord record;
    std::string error;
    const IdentityRead read =
      identity_path.empty() ? IdentityRead::kAbsent : read_identity(identity_path, record, error);
    bool erase_file = true;
    if (read == IdentityRead::kOk) {
      QarOnboardingId id = qar_onboarding_id_default();
      if (!ensure_runtime(OnboardingContext::kForget)) {
        erase_file = false;
        event.status = describe_status(kStatusNoRuntime, OnboardingContext::kForget, nullptr);
      } else if (!failed(qar_uuid_from_string(record.id.c_str(), id.data))) {
        QarForgetInit init = qar_forget_init_default();
        init.onboarding_id = id;
        const QarResult result = qar_runtime_forget(runtime, &init);
        if (failed(result)) {
          const int code = static_cast<int>(result.code);
          erase_file = (code == QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND);
          if (!erase_file) {
            event.status = describe_status(
              code, OnboardingContext::kForget, result_message(result).c_str());
          }
        }
      }
    }
    if (erase_file && !identity_path.empty()) {
      if (remove_identity(identity_path)) {
        identity_present.store(false);
      } else {
        erase_file = false;
        event.status.code = QAR_STATUS_UNCLASSIFIED;
        event.status.message = "The identity file could not be deleted.";
      }
    }
    log(std::string("forget ") + (erase_file ? "done" : "failed: " + event.status.message));
    push(std::move(event));
  }

  // The whole teardown of the owner: everything the owner created, in order.
  void shutdown()
  {
    abort_attempt();
    release_session();
    if (runtime != nullptr) {
      const Clock::time_point start = Clock::now();
      qar_runtime_handle_destroy(runtime);
      runtime = nullptr;
      log("teardown: runtime released in " + std::to_string(millis_since(start)) + " ms");
    }
    if (library_initialised) {
      const Clock::time_point start = Clock::now();
      qar_library_destroy();
      library_initialised = false;
      log("teardown: library destroyed in " + std::to_string(millis_since(start)) + " ms");
    }
    if (loaded_library) {
      loader->unload();
      loaded_library = false;
    }
    store_state(SessionState::kIdle);
  }
};

// ---------------------------------------------------------------------------------------------
// QarSessionOwner
// ---------------------------------------------------------------------------------------------

QarSessionOwner::QarSessionOwner(SessionOwnerConfig config, std::unique_ptr<ILibraryLoader> loader)
: impl_(std::make_unique<Impl>(std::move(config), std::move(loader)))
{
}

QarSessionOwner::~QarSessionOwner()
{
  if (!impl_) {
    return;
  }
  impl_->task_closing.store(true);
  impl_->arm_drain();
  Msg message;
  message.type = Msg::Type::kShutdown;
  impl_->post(std::move(message));
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  if (impl_->leak_on_destroy) {
    // A callback of the library may still fire into the state: it stays allocated.
    (void)impl_.release();
  }
}

void QarSessionOwner::connect()
{
  Msg message;
  message.type = Msg::Type::kConnect;
  impl_->post(std::move(message));
}

void QarSessionOwner::submit_code(std::string code, HubEndpoint hub)
{
  Msg message;
  message.type = Msg::Type::kSubmitCode;
  message.code = std::move(code);
  message.hub = std::move(hub);
  impl_->post(std::move(message));
}

void QarSessionOwner::cancel()
{
  Msg message;
  message.type = Msg::Type::kCancel;
  impl_->post(std::move(message));
}

void QarSessionOwner::disconnect()
{
  impl_->task_closing.store(true);
  impl_->arm_drain();
  Msg message;
  message.type = Msg::Type::kDisconnect;
  impl_->post(std::move(message));
}

void QarSessionOwner::forget()
{
  impl_->task_closing.store(true);
  impl_->arm_drain();
  Msg message;
  message.type = Msg::Type::kForget;
  impl_->post(std::move(message));
}

void QarSessionOwner::configure(HubMode mode, HubEndpoint remote)
{
  Msg message;
  message.type = Msg::Type::kConfigure;
  message.mode = mode;
  message.hub = std::move(remote);
  impl_->post(std::move(message));
}

void QarSessionOwner::set_target_listener(std::weak_ptr<ITargetListener> listener)
{
  if (t_in_listener) {
    // Called from inside the listener, which already holds the delivery lock: the listener is
    // swapped without waiting and without a replay.
    std::lock_guard<std::mutex> lock(impl_->listener_mutex);
    impl_->listener = listener;
    return;
  }
  std::shared_ptr<ITargetListener> target = listener.lock();
  // Waits for the delivery in progress. When the new listener is empty, no delivery uses the old
  // one after this returns, so the consumer may release it.
  std::lock_guard<std::mutex> delivery(impl_->delivery_mutex);
  {
    std::lock_guard<std::mutex> lock(impl_->listener_mutex);
    impl_->listener = listener;
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
  std::vector<SessionEvent> taken;
  {
    std::lock_guard<std::mutex> lock(impl_->events_mutex);
    taken.swap(impl_->events);
  }
  if (taken.empty()) {
    return;
  }
  {
    // The flag, the state and the session are read and the flag is set under the lock that
    // clears it, so a state that has moved on cannot be declared ready.
    std::lock_guard<std::mutex> lock(impl_->ready_mutex);
    bool joined = impl_->consumer_ready.load();
    for (const SessionEvent & event : taken) {
      if (event.kind == SessionEvent::Kind::kState) {
        joined = (event.state == SessionState::kRunning ||
          event.state == SessionState::kReconnecting);
      }
    }
    const SessionState now_state = impl_->state.load();
    const bool running =
      (now_state == SessionState::kRunning || now_state == SessionState::kReconnecting);
    if (poll_hook_) {
      poll_hook_();
    }
    const bool up = joined && running && impl_->session_for_consumer.load() != nullptr;
    impl_->consumer_ready.store(up);
    impl_->accepting_tasks.store(up);
  }
  for (SessionEvent & event : taken) {
    out.push_back(std::move(event));
  }
}

SessionState QarSessionOwner::state() const
{
  return impl_->state.load();
}

HubMode QarSessionOwner::effective_mode() const
{
  return impl_->mode.load();
}

bool QarSessionOwner::has_remote_identity() const
{
  return impl_->identity_present.load();
}

std::vector<TargetInfo> QarSessionOwner::targets() const
{
  std::vector<TargetInfo> out;
  std::lock_guard<std::mutex> lock(impl_->targets_mutex);
  for (const Impl::TargetEntry & entry : impl_->table) {
    if (entry.present) {
      out.push_back(entry.info);
    }
  }
  return out;
}

QarPeerId QarSessionOwner::self_peer() const
{
  std::lock_guard<std::mutex> lock(impl_->targets_mutex);
  return impl_->has_self ? impl_->self : qar_peer_id_default();
}

std::string QarSessionOwner::runtime_description() const
{
  std::lock_guard<std::mutex> lock(impl_->description_mutex);
  return impl_->description;
}

bool QarSessionOwner::ready() const
{
  return impl_->consumer_ready.load();
}

QarSession * QarSessionOwner::session() const
{
  return ready() ? impl_->session_for_consumer.load() : nullptr;
}

bool QarSessionOwner::post_session_task(
  const char * name, SessionTask task, std::function<void()> dropped)
{
  if (!task) {
    return false;
  }
  // The epoch is read before the flags: a task that races with a release carries the old epoch
  // and is dropped by the owner thread, never run on the next session.
  const std::uint64_t epoch = impl_->session_epoch.load();
  if (!impl_->accepting_tasks.load() || impl_->task_closing.load()) {
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
  impl_->tasks_queued.fetch_add(1);
  impl_->post(std::move(message));
  return true;
}

QarSessionOwner::TaskStats QarSessionOwner::task_stats() const
{
  TaskStats out;
  {
    std::lock_guard<std::mutex> lock(impl_->task_stats_mutex);
    out = impl_->task_stats_data;
    if (!out.running.empty()) {
      out.running_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - impl_->task_running_since).count();
    }
  }
  out.queued = impl_->tasks_queued.load();
  return out;
}

std::string QarSessionOwner::invite_device(DeviceKind kind, const std::string & host)
{
  if (!ready()) {
    return "QAROS is not connected yet.";
  }
  if (impl_->mode.load() != HubMode::kLocal) {
    return "Only a program on the Hub's own PC can invite a device; invite it in the Visualizer "
           "of the Hub.";
  }
  const std::size_t first = host.find_first_not_of(" \t");
  const std::size_t last = host.find_last_not_of(" \t");
  if (first == std::string::npos) {
    return "Type the host name or the address of the device.";
  }
  QarSession * const open_session = impl_->session_for_consumer.load();
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
    std::lock_guard<std::mutex> lock(impl_->invites_mutex);
    invite->id = impl_->next_invite++;
  }
  invite->ctx.impl = impl_.get();
  invite->ctx.id = invite->id;
  if (failed(QAR_CALL(qar_cancel_token_create, &invite->token)) || invite->token == nullptr) {
    return "The invite cannot be started (no cancel token).";
  }
  Impl::InviteOp * raw = invite.get();
  {
    std::lock_guard<std::mutex> lock(impl_->invites_mutex);
    impl_->invites.push_back(std::move(invite));
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
      std::lock_guard<std::mutex> lock(impl_->invites_mutex);
      for (auto it = impl_->invites.begin(); it != impl_->invites.end(); ++it) {
        if (it->get() == raw) {
          // The library may still call the result callback with this context.
          impl_->refused_invites.push_back(std::move(*it));
          impl_->invites.erase(it);
          break;
        }
      }
    }
    QAR_CALL(qar_cancel_token_handle_destroy, token);
    return text.message;
  }
  return std::string();
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
    if ((current == SessionState::kRunning && owner.ready()) ||
      current == SessionState::kFailed || current == SessionState::kNeedsCode)
    {
      return current;
    }
    if (Clock::now() >= end) {
      return current;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace qar_qar_adapter
