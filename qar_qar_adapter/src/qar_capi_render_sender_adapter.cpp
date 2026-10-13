// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"

#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/geo_anchor.hpp"
#include "qar_qar_adapter/render_timing.hpp"
#include "qar_ar_core/timing_log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "qar_ogre_streaming/types.hpp"  // peer_id_to_hex (peer-id tracing)

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace qar_qar_adapter
{

namespace
{

// Trace QAR peer ids (gated by QAR_ADAPTER_TRACE) so the log shows which id
// the invite returns, which the subscription surfaces, and which we create the
// render sender with — the way to diagnose a "target app id do not match" rejection.
void trace_peer_id(const char * what, const QarPeerId & id)
{
  if (std::getenv("QAR_ADAPTER_TRACE") == nullptr) { return; }
  std::fprintf(stderr, "[qar-adapter] %s peer_id=%s\n", what,
    qar_ogre_streaming::peer_id_to_hex(id).c_str());
  std::fflush(stderr);
}

// QarAppVolumeId has the same fixed-size-blob shape as QarPeerId but is a distinct
// type, so it needs its own formatter.
std::string app_volume_id_to_hex(const QarAppVolumeId & id)
{
  static const char * kHex = "0123456789abcdef";
  std::string out;
  out.reserve(QAR_MAX_ID_LENGTH * 2);
  for (unsigned char byte : id.data) {
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

const char * gesture_kind_name(QarGestureKind kind)
{
  switch (kind) {
    case QAR_GESTURE_CLICK: return "CLICK";
    case QAR_GESTURE_HOVER: return "HOVER";
    case QAR_GESTURE_SINGLE_POINTER_6DOF: return "SINGLE_POINTER_6DOF";
    case QAR_GESTURE_DUAL_POINTER_TRANSLATE_DISTANCE: return "DUAL_POINTER_TRANSLATE_DISTANCE";
    case QAR_GESTURE_DUAL_POINTER_ROTATE: return "DUAL_POINTER_ROTATE";
    default: return "UNKNOWN";
  }
}

const char * ray_line_style_name(QarAppVolumeControllerRayLineStyle style)
{
  switch (style) {
    case QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_NONE: return "NONE";
    case QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_SOLID: return "SOLID";
    case QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_DOTTED: return "DOTTED";
    default: return "UNKNOWN";
  }
}

const char * transform_mapping_name(QarGestureAppTransformMapping mapping)
{
  switch (mapping) {
    case QAR_GESTURE_APP_TRANSFORM_MAPPING_NONE: return "NONE";
    case QAR_GESTURE_APP_TRANSFORM_MAPPING_APP_POSE: return "APP_POSE";
    case QAR_GESTURE_APP_TRANSFORM_MAPPING_APP_SCALE: return "APP_SCALE";
    default: return "UNKNOWN";
  }
}

// Log what the runtime actually holds for gesture arbitration. We send NULL
// (see the comment on vol.gesture_configuration in
// QarCApiSenderFactory::connect()), so there is nothing of ours to log as
// "sent": a plain fprintf next to the call says so. This function is called
// only with label="read-back", when the
// handle can be recovered (qar_app_volume_get_gesture_configuration() on the
// created volume) -- what the runtime actually holds. Rule order IS priority
// — highest first — so the order these lines print in is exactly the order
// the runtime arbitrates with.
void log_gesture_configuration(
  const char * label, const QarAppVolumeGestureConfiguration & config)
{
  for (std::size_t i = 0; i < config.mapping_rule_count; ++i) {
    const QarAppVolumeGestureMappingRule & rule = config.mapping_rules[i];
    std::fprintf(stderr,
      "[qar-adapter] app volume gesture rule (%s) [%zu] kind=%s enabled=%s "
      "app_transform_mapping=%s enable_gestures_from_outside_of_volume=%s "
      "ray_style=%s ray_rgba=(%u,%u,%u,%u)\n",
      label, i, gesture_kind_name(rule.gesture_kind), rule.enabled ? "true" : "false",
      transform_mapping_name(rule.app_transform_mapping),
      rule.enable_gestures_from_outside_of_volume ? "true" : "false",
      ray_line_style_name(rule.controller_ray_style.line_style),
      static_cast<unsigned>(rule.controller_ray_style.color.r),
      static_cast<unsigned>(rule.controller_ray_style.color.g),
      static_cast<unsigned>(rule.controller_ray_style.color.b),
      static_cast<unsigned>(rule.controller_ray_style.color.a));
  }
  std::fflush(stderr);
}

// Per-frame submit-path breadcrumb with an incrementing counter (gated by
// QAR_ADAPTER_TRACE) for GUI-freeze triage. If the log stops at "-> X" with a low
// count, call X hard-blocks the GUI thread; if the count climbs fast, it's saturation.
bool adapter_trace_on()
{
  static const bool on = (std::getenv("QAR_ADAPTER_TRACE") != nullptr);
  return on;
}
/// Mint a random RFC-4122 v4 UUID in canonical 8-4-4-4-12 text form, so that
/// each Visualizer invite gets a distinct payload; see Config::visualizer_conn_suffix.
std::string make_random_uuid()
{
  static std::mt19937_64 rng{std::random_device{}()};
  std::uint64_t hi = rng();
  std::uint64_t lo = rng();
  // Stamp version 4 and the RFC-4122 variant so the value is a well-formed UUID
  // rather than 16 arbitrary bytes, in case the consumer validates it.
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
    static_cast<unsigned>(hi >> 32),
    static_cast<unsigned>((hi >> 16) & 0xFFFF),
    static_cast<unsigned>(hi & 0xFFFF),
    static_cast<unsigned>(lo >> 48),
    static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return std::string(buf);
}

/// Trace every frame up to this count, where a startup hang would show up.
constexpr unsigned long kTraceFullFrames = 150;
/// After that, keep one sampled frame per this many as a liveness breadcrumb.
constexpr unsigned long kTraceSampleEvery = 300;

/// Set QAR_ADAPTER_TRACE_ALL to defeat the sampling and trace every frame.
bool adapter_trace_all()
{
  static const bool on = (std::getenv("QAR_ADAPTER_TRACE_ALL") != nullptr);
  return on;
}

void trace_submit(unsigned long n, const char * msg)
{
  if (!adapter_trace_on()) { return; }
  // Unthrottled this costs FOUR flushed stderr writes per frame, on the exact
  // critical path whose cost we are trying to measure — tens of thousands of
  // lines in a few minutes. Tracing every early frame keeps the freeze-triage property
  // (a log ending at "-> X" names the blocking call), and the periodic sample
  // keeps it for steady state, without paying per frame forever.
  if (!adapter_trace_all() &&
    n > kTraceFullFrames && (n % kTraceSampleEvery) != 0)
  {
    return;
  }
  std::fprintf(stderr, "[qar-adapter] frame#%lu %s\n", n, msg);
  std::fflush(stderr);
}
/// Milliseconds since the previous trace_msg(), and since the first one.
///
/// The bootstrap is a straight line of blocking SDK calls, so "-> X" / "<- X" pairs
/// already say WHICH call is running; what they could not say is which one owns the
/// multi-second stall. The delta turns the same breadcrumbs into a profile, which is
/// what decides whether a call can be hoisted off the interactive path or has to be
/// got off the GUI thread entirely.
void trace_msg(const char * msg)
{
  if (!adapter_trace_on()) { return; }
  using clock = std::chrono::steady_clock;
  static const clock::time_point first = clock::now();
  static clock::time_point prev = first;
  const clock::time_point now = clock::now();
  const double d_ms = std::chrono::duration<double, std::milli>(now - prev).count();
  const double t_ms = std::chrono::duration<double, std::milli>(now - first).count();
  prev = now;
  std::fprintf(stderr, "[qar-adapter] [t=%8.1fms +%7.1fms] %s\n", t_ms, d_ms, msg);
  std::fflush(stderr);
}

/// Render a QarResult as "<what> failed: code=N msg=...".
///
/// A bare "<call> failed" says nothing when a session refuses to start — the reason
/// exists in the result. qar_result_message() copies the runtime's own diagnostic.
std::string result_text(const char * what, const QarResult & r)
{
  char buf[512] = {0};
  qar_result_message(r, buf, sizeof(buf) - 1);
  std::string out = std::string(what) + " failed: code=" +
    std::to_string(static_cast<long long>(r.code));
  if (buf[0] != '\0') { out += " msg="; out += buf; }
  return out;
}

/// Same, but also emit it to stderr immediately (the panel only shows the last error,
/// and a bootstrap can fail several calls deep).
std::string report_result(const char * what, const QarResult & r)
{
  std::string text = result_text(what, r);
  std::fprintf(stderr, "[qar-adapter] *** %s\n", text.c_str());
  std::fflush(stderr);
  return text;
}

// The SDK-free LogSeverity (env_config.hpp) must stay in lock-step with the vendored
// header's QarLogSeverity: the init struct takes the latter, QAR_LOG_SEVERITY and the
// launcher speak the former's names.
static_assert(static_cast<int>(LogSeverity::kTrace) == QAR_LOG_SEVERITY_TRACE, "enum drift");
static_assert(static_cast<int>(LogSeverity::kDebug) == QAR_LOG_SEVERITY_DEBUG, "enum drift");
static_assert(static_cast<int>(LogSeverity::kInfo) == QAR_LOG_SEVERITY_INFO, "enum drift");
static_assert(static_cast<int>(LogSeverity::kWarn) == QAR_LOG_SEVERITY_WARN, "enum drift");
static_assert(static_cast<int>(LogSeverity::kError) == QAR_LOG_SEVERITY_ERROR, "enum drift");
static_assert(static_cast<int>(LogSeverity::kFatal) == QAR_LOG_SEVERITY_FATAL, "enum drift");

/// Make the QAR_LOG_SEVERITY environment variable say what `QarLibraryInit.log_severity`
/// says. The vendor library reads the VARIABLE in preference to the init struct (see
/// resolve_library_logging()): a QAR_LOG_SEVERITY=trace left in the shell makes the
/// in-RViz library write hundreds of thousands of TRACE lines although the panel passed
/// WARN. Overwriting it also hands the same severity to the processes the runtime
/// spawns (launcher, mixer, Visualizer), which inherit RViz's environment.
///
/// `_putenv_s`, not SetEnvironmentVariable: the library reads the variable through the C
/// runtime's environment table, which only `_putenv` keeps in step (a bare Win32 write
/// has no effect).
///
/// @return the value the variable had before ("" if it was unset).
std::string export_library_log_severity(LogSeverity severity)
{
  std::string previous;
#ifdef _WIN32
  // _dupenv_s reads the same C-runtime table the vendor library does (and, unlike getenv,
  // does not trip C4996).
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

#ifdef _WIN32
/// WGL entry points resolved from opengl32.dll at run time, so this adapter keeps NO
/// link-time dependency on opengl32.lib (the plugin must load even where the SDK's GL
/// stack is absent). opengl32.dll is already in the process once Ogre has a context,
/// so GetModuleHandleA normally hits without loading anything.
struct WglApi
{
  HDC(WINAPI * GetCurrentDC)(void) = nullptr;
  HGLRC(WINAPI * GetCurrentContext)(void) = nullptr;
  HGLRC(WINAPI * CreateContext)(HDC) = nullptr;
  BOOL(WINAPI * DeleteContext)(HGLRC) = nullptr;
  BOOL(WINAPI * ShareLists)(HGLRC, HGLRC) = nullptr;
  BOOL(WINAPI * MakeCurrent)(HDC, HGLRC) = nullptr;
  PROC(WINAPI * GetProcAddress_)(LPCSTR) = nullptr;
  HMODULE module = nullptr;
  bool ok() const { return GetCurrentDC != nullptr && CreateContext != nullptr; }
};

void gl_finish_current_thread();

const WglApi & wgl()
{
  static WglApi api = [] {
      WglApi a;
      a.module = GetModuleHandleA("opengl32.dll");
      if (a.module == nullptr) { a.module = LoadLibraryA("opengl32.dll"); }
      if (a.module == nullptr) { return a; }
      auto sym = [&a](const char * n) {
          return reinterpret_cast<void *>(::GetProcAddress(a.module, n));
        };
      a.GetCurrentDC = reinterpret_cast<decltype(a.GetCurrentDC)>(sym("wglGetCurrentDC"));
      a.GetCurrentContext =
        reinterpret_cast<decltype(a.GetCurrentContext)>(sym("wglGetCurrentContext"));
      a.CreateContext = reinterpret_cast<decltype(a.CreateContext)>(sym("wglCreateContext"));
      a.DeleteContext = reinterpret_cast<decltype(a.DeleteContext)>(sym("wglDeleteContext"));
      a.ShareLists = reinterpret_cast<decltype(a.ShareLists)>(sym("wglShareLists"));
      a.MakeCurrent = reinterpret_cast<decltype(a.MakeCurrent)>(sym("wglMakeCurrent"));
      a.GetProcAddress_ =
        reinterpret_cast<decltype(a.GetProcAddress_)>(sym("wglGetProcAddress"));
      return a;
    }();
  return api;
}

/// Turn on zenoh's OWN logging inside this process (opt-in via QAR_ZENOH_LOG).
///
/// QAR's transport is zenoh (Rust). When a session fails to come up the C API reports
/// only "MsgInternalError Msg: -4 [ZenohMessaging.cpp:340]"; the actual reason —
/// "Unable to open listener ...", "Unable to join multicast group ... on interface ...",
/// "Unable to bind UDP port ..." — lives in zenoh's own log, which stays SILENT unless
/// somebody initialises it. Setting RUST_LOG alone does nothing here: zenoh only reads
/// it when the host calls one of the zc_init_log_* entry points, and RViz never does
/// (the Visualizer process does, which is why zenoh lines appear in its log but not
/// ours). zenohc.dll is already in the process as a dependency of qar_streaming.dll,
/// so we can start its logger ourselves.
///
/// The request is honoured only while the library log severity is trace or debug
/// (zenoh_logging_allowed): at debug zenoh writes thousands of lines per minute into
/// RViz's stdout, so under the quiet profile a QAR_ZENOH_LOG left in the shell must not
/// switch it on. The decision is always printed, so a log says whether zenoh logging
/// was on and why.
///
/// @param filter a tracing filter such as "debug" or "zenoh=trace".
void init_zenoh_logging_if_requested(LogSeverity library_severity)
{
  const char * filter = std::getenv("QAR_ZENOH_LOG");
  if (filter == nullptr || *filter == '\0') {
    std::fprintf(stderr, "[qar-adapter] zenoh logging: off (QAR_ZENOH_LOG not set)\n");
    std::fflush(stderr);
    return;
  }
  if (!zenoh_logging_allowed(library_severity)) {
    std::fprintf(stderr,
      "[qar-adapter] zenoh logging: OFF - QAR_ZENOH_LOG='%s' is ignored because the library "
      "log severity is %s; use severity debug or trace to get it\n",
      filter, log_severity_name(library_severity));
    std::fflush(stderr);
    return;
  }

  HMODULE z = GetModuleHandleA("zenohc.dll");
  if (z == nullptr) { z = LoadLibraryA("zenohc.dll"); }
  if (z == nullptr) {
    std::fprintf(stderr, "[qar-adapter] QAR_ZENOH_LOG set but zenohc.dll is not loaded\n");
    std::fflush(stderr);
    return;
  }
  using PfnInitLogOr = void (*)(const char *);
  auto init_or = reinterpret_cast<PfnInitLogOr>(
    reinterpret_cast<void *>(::GetProcAddress(z, "zc_init_log_from_env_or")));
  if (init_or == nullptr) {
    std::fprintf(stderr, "[qar-adapter] zenohc.dll has no zc_init_log_from_env_or\n");
    std::fflush(stderr);
    return;
  }
  init_or(filter);
  std::fprintf(stderr, "[qar-adapter] zenoh logging enabled (filter=%s)\n", filter);
  std::fflush(stderr);
}

/// Block until this thread's queued GL work has completed.
///
/// Required before handing our Ogre texture ids to QAR: per the GL spec (shared
/// objects across contexts), changes made to an object in one context become visible
/// in another only after the writing context flushes. QAR reads these textures from
/// its own graphics thread, so without this the Visualizer receives the right NUMBER
/// of frames whose textures are still empty — exactly the "frames arrive, scene is
/// black" symptom.
///
/// glFinish (not glFlush) because QAR gives us no completion fence to hand back
/// (its OpenGL frames are typed NoSyncFrameNeeded). Correctness first; this can be
/// relaxed to a glFenceSync/glClientWaitSync pair if it costs measurable FPS.
void gl_finish_current_thread()
{
  using PfnFinish = void (WINAPI *)(void);
  // glFinish is OpenGL 1.1 core, so it is exported by opengl32.dll itself and does
  // NOT need wglGetProcAddress (which never resolves 1.1 entry points).
  static PfnFinish fn = [] {
      const WglApi & w = wgl();
      return (w.module != nullptr)
        ? reinterpret_cast<PfnFinish>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glFinish")))
        : nullptr;
    }();
  if (fn != nullptr) { fn(); }
}
#endif  // _WIN32

#ifdef _WIN32
// Crash triage: qar_library_init works standalone but can hard-fault when
// loaded inside rviz2.exe. To identify the faulting module without a debugger, we
// run the C call under a Structured-Exception guard. On a hardware exception
// (access violation, FP, illegal instruction, ...) the filter reports the
// exception code + the module that owns the faulting address, then unwinds so the
// host (rviz2) survives instead of dying silently. These helpers are C-style (no
// unwindable C++ objects) so __try/__except is legal under /EHsc.
LONG qar_seh_report(struct _EXCEPTION_POINTERS * ep, const char * where)
{
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  void * addr = ep->ExceptionRecord->ExceptionAddress;
  char modpath[MAX_PATH] = "<unknown>";
  HMODULE hmod = nullptr;
  if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(addr), &hmod) && hmod != nullptr) {
    GetModuleFileNameA(hmod, modpath, MAX_PATH);
  }
  std::fprintf(stderr,
    "[qar-adapter] *** SEH FAULT in %s: code=0x%08lX addr=%p module=%s\n",
    where, static_cast<unsigned long>(code), addr, modpath);
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Returns 0 on normal return (result in *out), 1 if the call hardware-faulted.
int guarded_library_init(const QarLibraryInit * lib, QarResult * out)
{
  __try {
    *out = qar_library_init(lib);
    return 0;
  } __except (qar_seh_report(GetExceptionInformation(), "qar_library_init")) {
    return 1;
  }
}

int guarded_runtime_create(const QarRuntimeInit * rt, QarRuntime ** out)
{
  __try {
    QarResult r = qar_runtime_create(rt, out);
    return qar_result_is_error(r) ? 2 : 0;
  } __except (qar_seh_report(GetExceptionInformation(), "qar_runtime_create")) {
    return 1;
  }
}
#endif  // _WIN32


bool peer_eq(const QarPeerId & a, const QarPeerId & b)
{
  return std::memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

/// Split a comma-separated list, dropping empty entries and surrounding blanks.
std::vector<std::string> split_list(const std::string & text)
{
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? text.size() : comma;
    std::string item = text.substr(start, end - start);
    const std::size_t first = item.find_first_not_of(" \t");
    const std::size_t last = item.find_last_not_of(" \t");
    if (first != std::string::npos) {
      out.push_back(item.substr(first, last - first + 1));
    }
    if (comma == std::string::npos) { break; }
    start = comma + 1;
  }
  return out;
}

/// Case-insensitive substring test (ASCII), for the display-name admission policy.
bool icontains(const std::string & haystack, const std::string & needle)
{
  if (needle.empty()) { return false; }
  auto lower = [](const std::string & s) {
      std::string out = s;
      for (char & c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

/// Read every readable field of a peer spec and print it (QAR_ADAPTER_TRACE only).
///
/// Multi-peer needs to tell a real Visualizer apart from the runtime's internal app
/// peers, and nothing in the header says which field carries that. The subscription
/// surfaces FIVE distinct ids for ONE invited Visualizer, so the discriminator has to
/// be found empirically — this dumps every candidate (display name, app version,
/// custom info, state, version id, room tag) for each peer, so ONE run is enough to
/// pick the right admission filter.
void trace_peer_spec(QarPeerSpec * spec, const QarPeerId & id, const char * display_name)
{
  if (!adapter_trace_on()) { return; }
  auto str = [spec](QarResult (*fn)(QarPeerSpec *, char *, size_t), char * buf, size_t n) {
      buf[0] = '\0';
      const QarResult r = fn(spec, buf, n);
      if (qar_result_is_error(r)) { std::snprintf(buf, n, "<err %d>", static_cast<int>(r.code)); }
      return buf;
    };
  char version[128] = {0};
  char custom[256] = {0};
  char version_id[128] = {0};
  char room[128] = {0};
  QarAppState state = QAR_APP_STATE_UNKNOWN;
  qar_peer_spec_get_app_state(spec, &state);
  std::fprintf(stderr,
    "[qar-adapter] peer_spec id=%s name='%s' app_version='%s' custom='%s' "
    "version_id='%s' room='%s' state=%d\n",
    qar_ogre_streaming::peer_id_to_hex(id).c_str(),
    display_name != nullptr ? display_name : "",
    str(&qar_peer_spec_get_app_version, version, sizeof(version)),
    str(&qar_peer_spec_get_app_custom_peer_info, custom, sizeof(custom)),
    str(&qar_peer_spec_get_version_id, version_id, sizeof(version_id)),
    str(&qar_peer_spec_get_room_tag, room, sizeof(room)),
    static_cast<int>(state));
  std::fflush(stderr);
}

// Peer-update trampoline. The SDK surfaces peers joining/changing through
// qar_peer_subscribe_updates, whose callback hands a QarPeerSpec* per updated peer.
// We read its id (and its display name for the admission policy) and record it as a
// "wants a stream" peer. Runs on the QAR runtime's thread — it only records the
// id; stream creation happens later on the render thread.
void peer_update_trampoline(QarPeerSpec * spec, void * user_state)
{
  auto * factory = static_cast<QarCApiSenderFactory *>(user_state);
  if (spec == nullptr || factory == nullptr) { return; }
  QarPeerId peer = qar_peer_id_default();
  qar_peer_spec_get_id(spec, &peer);

  const char * display_name = nullptr;
  char name[256] = {0};
  if (!qar_result_is_error(qar_peer_spec_get_display_name(spec, name, sizeof(name) - 1))) {
    display_name = name;
  }
  trace_peer_spec(spec, peer, display_name);

  factory->on_render_request(peer, display_name);
  qar_peer_spec_handle_destroy(spec);
}

}  // namespace

const char * to_string(PeerKind kind)
{
  switch (kind) {
    case PeerKind::kHoloLens: return "hololens";
    case PeerKind::kQuest:    return "quest";
    case PeerKind::kZed:      return "zed";
    case PeerKind::kVisualizer:
    default:                  return "visualizer";
  }
}

bool parse_peer_kind(const std::string & text, PeerKind & out)
{
  std::string t;
  t.reserve(text.size());
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  if (t == "visualizer" || t == "viz") {
    out = PeerKind::kVisualizer;
  } else if (t == "hololens" || t == "hl" || t == "hololens2") {
    out = PeerKind::kHoloLens;
  } else if (t == "quest" || t == "metaquest") {
    out = PeerKind::kQuest;
  } else if (t == "zed") {
    out = PeerKind::kZed;
  } else {
    return false;
  }
  return true;
}

bool peer_kind_needs_hostname(PeerKind kind)
{
  // qar_session_invite_connection_string_hololens is the only builder taking one, and
  // its doc comment says the hostname "must not be NULL or empty".
  return kind == PeerKind::kHoloLens;
}

// ===========================================================================
//  Per-frame trace (QAR_ADAPTER_FRAME_TRACE) + pose re-read test
// ===========================================================================
//
// Opt-in diagnostics for the HoloLens "shaking" bug: one CSV row per submitted
// frame recording exactly when its begin-frame wait started/became ready, when its
// pose was consumed (rendered), when rendering + show_frame finished, and whether
// re-reading the SAME SDK frame handle's pose just before show_frame (or, on the
// priming fast path, back when readiness was first observed) disagrees with the
// pose we actually rendered with. See docs/ARCHITECTURE.md#diagnostics.
//
// Kept entirely off the default path: every touch point below is guarded by
// `if (frame_trace_) { ... }` in the adapter methods, so with QAR_ADAPTER_FRAME_TRACE
// unset this compiles to one extra null-pointer check per call.
namespace
{

bool frame_trace_on()
{
  static const bool on = qar_qar_adapter::frame_trace_requested(
    std::getenv("QAR_ADAPTER_FRAME_TRACE"));
  return on;
}

std::int64_t to_epoch_us(std::chrono::system_clock::time_point tp)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

/// @brief Local HH:MM:SS.ffffff, to eyeball against the SDK log (HH:MM:SS:micros).
std::string format_local_hhmmss_us(std::chrono::system_clock::time_point tp)
{
  const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  const auto since_epoch = tp.time_since_epoch();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
  const auto micros =
    std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - secs);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06lld",
    tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
    static_cast<long long>(micros.count()));
  return std::string(buf);
}

std::string resolve_trace_log_folder(const std::string & preferred)
{
  // Mirrors the SDK library-log default resolved during bootstrap (see
  // QarCApiSenderFactory's log_folder_path handling above): prefer the QAR log
  // folder so the trace sits next to the SDK's own TRACE-level logs, and fall back
  // to %TEMP%\quaternar the same way when it is empty / not yet known.
  if (!preferred.empty()) {
    return preferred;
  }
  const char * tmp = std::getenv("TEMP");
  if (tmp == nullptr) { tmp = std::getenv("TMP"); }
  return (tmp != nullptr) ? (std::string(tmp) + "\\quaternar") : std::string("quaternar");
}

std::string timestamp_yyyyMMdd_HHmmss()
{
  const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
    tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
    tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  return std::string(buf);
}

/// @brief peer-hex8, or (default-constructed / test peer id) a per-process
/// incrementing decimal index, so trace filenames never collide.
std::string make_stream_tag(const QarPeerId & peer_id)
{
  bool all_zero = true;
  for (unsigned char b : peer_id.data) {
    if (b != 0) { all_zero = false; break; }
  }
  if (!all_zero) {
    const std::string hex = qar_ogre_streaming::peer_id_to_hex(peer_id);
    if (hex.size() >= 8) { return hex.substr(0, 8); }
  }
  static std::atomic<int> next_index{0};
  return std::to_string(next_index.fetch_add(1));
}

/// @brief Position delta (mm) + angle delta (degrees) between two poses.
struct PoseDelta
{
  double pos_mm = 0.0;
  double angle_deg = 0.0;
};

bool pose_bitwise_equal(const QarPose & a, const QarPose & b)
{
  return std::memcmp(&a, &b, sizeof(QarPose)) == 0;
}

PoseDelta pose_delta(const QarPose & a, const QarPose & b)
{
  const double dx = (static_cast<double>(a.position.x) - b.position.x) * 1000.0;
  const double dy = (static_cast<double>(a.position.y) - b.position.y) * 1000.0;
  const double dz = (static_cast<double>(a.position.z) - b.position.z) * 1000.0;
  double dot =
    static_cast<double>(a.orientation.x) * b.orientation.x +
    static_cast<double>(a.orientation.y) * b.orientation.y +
    static_cast<double>(a.orientation.z) * b.orientation.z +
    static_cast<double>(a.orientation.w) * b.orientation.w;
  // Clamp against fp overshoot past +-1 and fold the quaternion double-cover
  // (q and -q are the same rotation) before the angle formula.
  dot = std::min(1.0, std::max(-1.0, std::fabs(dot)));
  constexpr double kPi = 3.14159265358979323846;
  PoseDelta d;
  d.pos_mm = std::sqrt(dx * dx + dy * dy + dz * dz);
  d.angle_deg = 2.0 * std::acos(dot) * (180.0 / kPi);
  return d;
}

/// @brief The worse (larger position delta) of the two eyes.
PoseDelta worse_eye_delta(const PoseDelta & left, const PoseDelta & right)
{
  return (left.pos_mm >= right.pos_mm) ? left : right;
}

/// @brief Timing for the CURRENTLY outstanding (started, not yet consumed)
/// begin-frame wait. Reset each time a new wait starts (see note_try_begin_frame_call).
struct FrameWaitTrace
{
  bool outstanding = false;
  bool is_primed = false;    ///< The wait was started by prime_next_frame(), not begin_frame().
  int polls = 0;             ///< try_begin_frame calls made against this wait so far.
  std::chrono::system_clock::time_point start_tp{};
  std::chrono::system_clock::time_point ready_tp{};
  bool have_a = false;       ///< (a) captured (only when a primed wait reports ready).
  QarPose a_left{};
  QarPose a_right{};
};

/// @brief One CSV row's worth of state, accumulated from consume (begin_frame) to
/// show (submit_opengl_stereo_frame). `handle` is the frame's SDK handle kept alive
/// across that span purely for the pre-show (c) pose re-read; it is NEVER
/// the primed_info_/stream ownership path and is always released by abandon_row()
/// or note_pre_show() before this row is done.
struct PendingTraceRow
{
  bool valid = false;
  std::uint64_t frame_index = 0;
  bool prime = false;
  double tick_ms = 0.1;
  std::chrono::system_clock::time_point wait_start_tp{};
  bool wait_is_prime = false;
  int polls = 0;
  std::chrono::system_clock::time_point ready_tp{};
  std::chrono::system_clock::time_point consume_tp{};
  std::chrono::system_clock::time_point render_done_tp{};
  std::chrono::system_clock::time_point show_done_tp{};
  bool show_ok = false;
  QarPose b_left{};   ///< the pose actually rendered with (left eye).
  QarPose b_right{};
  bool have_a = false;
  QarPose a_left{};
  QarPose a_right{};
  bool have_c = false;
  QarPose c_left{};
  QarPose c_right{};
  QarRenderFrameInfo * handle = nullptr;
};

}  // namespace

// FrameTraceState is forward-declared (plain qar_qar_adapter::FrameTraceState, NOT
// inside an anonymous namespace) in qar_capi_render_sender_adapter.hpp, so its
// definition must live here at the same, non-anonymous scope — an anonymous-
// namespace FrameTraceState would be a DIFFERENT, ambiguous type from the header's
// declaration (MSVC C2872). It still sees every helper/struct above via ordinary
// unqualified lookup (anonymous-namespace members remain visible for the rest of
// the enclosing namespace).
//
/// @brief Owns one stream's buffered trace CSV + the wait/row bookkeeping above.
/// One instance per QarCApiRenderSenderAdapter; every call happens on the single Qt
/// render/GUI thread that drives the whole adapter, so no locking is needed.
class FrameTraceState
{
public:
  FrameTraceState(const std::string & log_folder_path, const QarPeerId & peer_id)
  : peer_hex_(qar_ogre_streaming::peer_id_to_hex(peer_id))
  {
    const std::string folder = resolve_trace_log_folder(log_folder_path);
#ifdef _WIN32
    CreateDirectoryA(folder.c_str(), nullptr);  // ok if it already exists
#endif
    path_ = folder + "\\qar_frame_trace_" + timestamp_yyyyMMdd_HHmmss() + "_" +
      make_stream_tag(peer_id) + ".csv";
    file_ = std::fopen(path_.c_str(), "w");
    if (file_ == nullptr) {
      std::fprintf(stderr, "[qar-adapter] frame trace: failed to open '%s'\n", path_.c_str());
      std::fflush(stderr);
      path_.clear();
      return;
    }
    std::fprintf(file_,
      "frame,peer,tick_ms,prime,wait_start_us,wait_src,polls,ready_us,consume_us,"
      "render_done_us,show_done_us,show_ok,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w,"
      "consume_local,a_pos_delta_mm,a_angle_delta_deg,c_pos_delta_mm,c_angle_delta_deg,"
      "changed\n");
    last_flush_ = std::chrono::steady_clock::now();
  }

  ~FrameTraceState()
  {
    abandon_row();  // release a still-in-flight handle, if any; no row written for it
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  FrameTraceState(const FrameTraceState &) = delete;
  FrameTraceState & operator=(const FrameTraceState &) = delete;

  /// @brief Full path of the CSV actually opened ("" if it failed to open).
  const std::string & path() const { return path_; }

  /// @brief Call immediately before EVERY qar_render_sender_try_begin_frame call
  /// (both prime_next_frame()'s and begin_frame()'s own poll), so wait_start_us /
  /// wait_src / polls reflect the call that actually STARTED this frame's wait.
  void note_try_begin_frame_call(bool from_prime)
  {
    if (!wait_.outstanding) {
      wait_ = FrameWaitTrace{};
      wait_.outstanding = true;
      wait_.is_primed = from_prime;
      wait_.start_tp = std::chrono::system_clock::now();
    }
    ++wait_.polls;
  }

  /// @brief Call right after a try_begin_frame call returns ready=true, before the
  /// caller does anything else with @p handle. @p is_prime_call must match what was
  /// passed to note_try_begin_frame_call() for THIS call. On the primed fast path
  /// this call is "ready first observed" — a call strictly earlier than the eventual
  /// consume — so (a) is captured here; on the poll path this call IS the consume
  /// call, so (a) is left empty (have_a stays false).
  void note_ready(bool is_prime_call, QarRenderFrameInfo * handle)
  {
    wait_.ready_tp = std::chrono::system_clock::now();
    wait_.have_a = false;
    if (is_prime_call) {
      const QarResult rl = qar_render_frame_info_get_view_pose(handle, 0, &wait_.a_left);
      const QarResult rr = qar_render_frame_info_get_view_pose(handle, 1, &wait_.a_right);
      wait_.have_a = qar_result_is_success(rl) && qar_result_is_success(rr);
      if (!wait_.have_a) {
        std::fprintf(stderr, "[qar-adapter] frame trace: (a) pose re-read failed\n");
        std::fflush(stderr);
      }
    }
  }

  /// @brief Call from begin_frame() right after computing the pose it will render
  /// with (b). Takes ownership of @p handle until note_pre_show() (or abandon_row())
  /// releases it. Safe to call with a previous row still open (a phase between
  /// begin_frame_phase and submit_phase failed without ever reaching submit) — that
  /// abandoned row's handle is released first so nothing leaks.
  void begin_row(
    std::uint64_t frame_index, bool prime_enabled, double tick_ms,
    const QarPose & left, const QarPose & right, QarRenderFrameInfo * handle)
  {
    abandon_row();
    row_ = PendingTraceRow{};
    row_.valid = true;
    row_.frame_index = frame_index;
    row_.prime = prime_enabled;
    row_.tick_ms = tick_ms;
    row_.wait_start_tp = wait_.start_tp;
    row_.wait_is_prime = wait_.is_primed;
    row_.polls = wait_.polls;
    row_.ready_tp = wait_.ready_tp;
    row_.consume_tp = std::chrono::system_clock::now();
    row_.have_a = wait_.have_a;
    row_.a_left = wait_.a_left;
    row_.a_right = wait_.a_right;
    row_.b_left = left;
    row_.b_right = right;
    row_.handle = handle;
    wait_.outstanding = false;  // consumed; the next wait starts fresh
  }

  /// @brief Release an in-flight row's handle WITHOUT writing a CSV line for it
  /// (the frame never reached show_frame — a render/submit-mode phase failed, or
  /// the stream is being torn down mid-frame). No-op if no row is open.
  void abandon_row()
  {
    if (row_.handle != nullptr) {
      qar_render_frame_info_handle_destroy(row_.handle);
      row_.handle = nullptr;
    }
    row_.valid = false;
  }

  /// @brief Call at the very top of submit_opengl_stereo_frame(). render_done_us is
  /// stamped here rather than threaded through IQarRenderSenderAdapter's signature:
  /// render_targets_phase()/fence_phase() (fence_phase is a no-op today) already ran
  /// synchronously, on this same thread, immediately before this call.
  void note_render_done()
  {
    if (row_.valid) { row_.render_done_tp = std::chrono::system_clock::now(); }
  }

  /// @brief Call right before qar_render_sender_show_frame(), while the handle can
  /// still be read: captures (c) and releases the handle. No-op if no row is open.
  void note_pre_show()
  {
    if (!row_.valid || row_.handle == nullptr) { return; }
    QarPose c_left{};
    QarPose c_right{};
    const QarResult rl = qar_render_frame_info_get_view_pose(row_.handle, 0, &c_left);
    const QarResult rr = qar_render_frame_info_get_view_pose(row_.handle, 1, &c_right);
    row_.have_c = qar_result_is_success(rl) && qar_result_is_success(rr);
    if (row_.have_c) {
      row_.c_left = c_left;
      row_.c_right = c_right;
    } else {
      std::fprintf(stderr, "[qar-adapter] frame trace: (c) pose re-read failed\n");
      std::fflush(stderr);
    }
    qar_render_frame_info_handle_destroy(row_.handle);
    row_.handle = nullptr;
  }

  /// @brief Call right after qar_render_sender_show_frame() returns: writes the CSV
  /// row (buffered; flushed about once a second — see flush_if_due()) and closes it
  /// out. No-op if no row is open (e.g. abandon_row() already ran).
  void note_show_done(bool show_ok)
  {
    if (!row_.valid) { return; }
    row_.show_done_tp = std::chrono::system_clock::now();
    row_.show_ok = show_ok;
    write_row();
    row_.valid = false;
  }

  /// @brief Cumulative rows written, and of those, how many saw a changed (a) or
  /// (c) re-read. Feeds the panel heartbeat's rr=<changed_rows>/<rows> field.
  std::uint64_t rows_written() const { return rows_written_; }
  std::uint64_t changed_rows() const { return changed_rows_; }

private:
  void write_row()
  {
    ++rows_written_;
    const PoseDelta c_delta = worse_eye_delta(
      pose_delta(row_.c_left, row_.b_left), pose_delta(row_.c_right, row_.b_right));
    bool changed = row_.have_c &&
      (!pose_bitwise_equal(row_.c_left, row_.b_left) ||
       !pose_bitwise_equal(row_.c_right, row_.b_right));

    char a_pos_buf[32] = "";
    char a_ang_buf[32] = "";
    if (row_.have_a) {
      const PoseDelta a_delta = worse_eye_delta(
        pose_delta(row_.a_left, row_.b_left), pose_delta(row_.a_right, row_.b_right));
      std::snprintf(a_pos_buf, sizeof(a_pos_buf), "%.4f", a_delta.pos_mm);
      std::snprintf(a_ang_buf, sizeof(a_ang_buf), "%.4f", a_delta.angle_deg);
      changed = changed ||
        !pose_bitwise_equal(row_.a_left, row_.b_left) ||
        !pose_bitwise_equal(row_.a_right, row_.b_right);
    }
    if (changed) { ++changed_rows_; }

    if (file_ == nullptr) { return; }
    std::fprintf(file_,
      "%llu,%s,%.4g,%d,%lld,%s,%d,%lld,%lld,%lld,%lld,%d,"
      "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%s,%s,%.4f,%.4f,%d\n",
      static_cast<unsigned long long>(row_.frame_index),
      peer_hex_.c_str(),
      row_.tick_ms,
      row_.prime ? 1 : 0,
      static_cast<long long>(to_epoch_us(row_.wait_start_tp)),
      row_.wait_is_prime ? "prime" : "poll",
      row_.polls,
      static_cast<long long>(to_epoch_us(row_.ready_tp)),
      static_cast<long long>(to_epoch_us(row_.consume_tp)),
      static_cast<long long>(to_epoch_us(row_.render_done_tp)),
      static_cast<long long>(to_epoch_us(row_.show_done_tp)),
      row_.show_ok ? 1 : 0,
      static_cast<double>(row_.b_left.position.x),
      static_cast<double>(row_.b_left.position.y),
      static_cast<double>(row_.b_left.position.z),
      static_cast<double>(row_.b_left.orientation.x),
      static_cast<double>(row_.b_left.orientation.y),
      static_cast<double>(row_.b_left.orientation.z),
      static_cast<double>(row_.b_left.orientation.w),
      format_local_hhmmss_us(row_.consume_tp).c_str(),
      a_pos_buf, a_ang_buf,
      c_delta.pos_mm, c_delta.angle_deg,
      changed ? 1 : 0);
    flush_if_due();
  }

  void flush_if_due()
  {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last_flush_).count() >= 1.0) {
      std::fflush(file_);
      last_flush_ = now;
    }
  }

  FILE * file_ = nullptr;
  std::string path_;
  std::string peer_hex_;
  std::chrono::steady_clock::time_point last_flush_{};

  FrameWaitTrace wait_{};
  PendingTraceRow row_{};
  std::uint64_t rows_written_ = 0;
  std::uint64_t changed_rows_ = 0;
};

// ===========================================================================
//  QarCApiRenderSenderAdapter
// ===========================================================================
QarCApiRenderSenderAdapter::QarCApiRenderSenderAdapter(
  QarRenderSender * stream, const QarNearFar & near_far,
  GlSubmitMode submit_mode, std::shared_ptr<IQarGlTextureCopyHook> copy_hook,
  float depth_scale, std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot,
  QarPeerId peer_id, std::string log_folder_path)
: stream_(stream), near_far_(near_far), depth_scale_(depth_scale),
  submit_mode_(submit_mode), copy_hook_(std::move(copy_hook)),
  pointing_ray_slot_(std::move(pointing_ray_slot)),
  // QAR_ADAPTER_PRIME sets the INITIAL value only; the manager overwrites this via
  // set_prime_enabled() right after construction (see OgreQarStreamingManager::
  // create_stream), and the panel checkbox changes it live from there. Read here too
  // so an adapter built outside that flow (a test, a standalone sample) still honors
  // the env var on its own.
  prime_enabled_(qar_qar_adapter::parse_bool_env(std::getenv("QAR_ADAPTER_PRIME"), true))
{
  {
    // Per-stream stage timing (QAR_VIDEO_TIMING_DIR). The first stream is render3d.csv, a
    // second (another peer, or a rebuilt stream) render3d_2.csv, and so on.
    qar_ar_core::TimingLog & timing_log = qar_ar_core::TimingLog::instance();
    if (timing_log.enabled()) {
      static std::atomic<int> streams_created{0};
      const int ordinal = ++streams_created;
      auto sink = timing_log.open(
        ordinal == 1 ? std::string("render3d") : "render3d_" + std::to_string(ordinal),
        RenderTimingState::columns(), RenderTimingState::kinds(),
        RenderTimingState::kKindColumn);
      if (sink) { timing_ = std::make_unique<RenderTimingState>(std::move(sink)); }
    }
  }
  if (frame_trace_on()) {
    frame_trace_ = std::make_unique<FrameTraceState>(log_folder_path, peer_id);
    std::fprintf(stderr, "[qar-adapter] frame trace: %s\n",
      frame_trace_->path().empty() ? "failed to open trace file" : frame_trace_->path().c_str());
    std::fflush(stderr);
  }
}

QarCApiRenderSenderAdapter::~QarCApiRenderSenderAdapter()
{
  // Release a primed-but-unconsumed frame BEFORE the stream that owns it.
  if (primed_info_ != nullptr) {
    qar_render_frame_info_handle_destroy(primed_info_);
    primed_info_ = nullptr;
  }
  // Same ordering concern for a trace-only kept-alive handle (the pre-show pose re-read):
  // a frame caught mid-flight (between consume and show) when the stream is torn
  // down must not leak or be destroyed twice. frame_trace_'s own destructor (flush +
  // close the CSV) runs later, after stream_ below, which is fine — by then it holds
  // no QAR handle of any kind.
  if (frame_trace_) {
    frame_trace_->abandon_row();
  }
  if (stream_ != nullptr) {
    qar_render_stream_handle_destroy(stream_);
    stream_ = nullptr;
  }
}

std::string QarCApiRenderSenderAdapter::frame_trace_path() const
{
  return frame_trace_ ? frame_trace_->path() : std::string();
}

std::uint64_t QarCApiRenderSenderAdapter::frame_trace_rows() const
{
  return frame_trace_ ? frame_trace_->rows_written() : 0;
}

std::uint64_t QarCApiRenderSenderAdapter::frame_trace_changed_rows() const
{
  return frame_trace_ ? frame_trace_->changed_rows() : 0;
}

bool QarCApiRenderSenderAdapter::begin_frame(qar_ogre_streaming::QarFrameInfo & out_info)
{
  out_info = qar_ogre_streaming::QarFrameInfo{};
  out_info.should_render = false;
  last_skip_detail_.clear();
  if (stream_ == nullptr) {
    last_skip_detail_ = "no QAR render-stream handle";
    return false;
  }

  QarRenderFrameInfo * info = nullptr;
  bool ready = false;
  // Fast path: the wait for THIS frame was already started when the previous frame
  // was shown, and prime_next_frame() observed it complete. Consume it directly so
  // the frame costs no extra tick. See prime_next_frame() for why this matters.
  if (primed_info_ != nullptr) {
    info = primed_info_;
    primed_info_ = nullptr;
    ready = true;
  }
  // NON-BLOCKING begin-frame. begin_frame() is polled on RViz's Qt GUI thread every
  // render tick, so a blocking wait here freezes the whole UI: the blocking
  // qar_render_sender_begin_frame() never returns control to the event loop once
  // frames flow.
  // qar_render_sender_try_begin_frame starts the begin-frame wait on the first call
  // and polls it without blocking on subsequent calls; when the frame isn't ready
  // yet we simply decline this tick (should_render stays false) and poll next tick.
  if (!ready) {
    static unsigned long tbf_calls = 0;
    const bool tbf_trace = (++tbf_calls <= 3);
    if (tbf_trace) { trace_msg("begin_frame: -> try_begin_frame"); }
    if (frame_trace_) { frame_trace_->note_try_begin_frame_call(/*from_prime=*/false); }
    const std::int64_t poll_start_us = timing_ ? qar_ar_core::timing_epoch_us() : 0;
    const QarResult r = qar_render_sender_try_begin_frame(stream_, &info, &ready);
    if (timing_) { timing_->note_poll_call(poll_start_us); }
    if (tbf_trace) { trace_msg(ready ? "begin_frame: <- try_begin_frame (ready)"
                                      : "begin_frame: <- try_begin_frame (pending)"); }
    if (qar_result_is_error(r)) {
      if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_STREAM_IS_CLOSED)) {
        closed_.store(true);
        last_skip_detail_ = "try_begin_frame: producer stream is closed";
      } else {
        last_skip_detail_ = result_text("try_begin_frame", r);
      }
      if (info != nullptr) { qar_render_frame_info_handle_destroy(info); }
      return false;
    }
    if (timing_) {
      if (ready && info != nullptr) { timing_->note_ready(/*from_prime=*/false); }
      else { timing_->note_pending(); }
    }
    if (ready && info != nullptr && frame_trace_) {
      // Poll path: this call is BOTH "ready first observed" AND "consume" (the pose
      // getters run a few lines below), so (a) is left empty — see note_ready().
      frame_trace_->note_ready(/*is_prime_call=*/false, info);
    }
  }
  if (!ready || info == nullptr) {
    if (info != nullptr) { qar_render_frame_info_handle_destroy(info); }
    // The begin-frame wait has not completed. In the steady state this is just
    // the viewer pacing us below our tick rate. If it NEVER completes again the
    // SDK trace parks on "RenderingProducer: waiting for available frame" and no
    // further `qar/media/video/renderRequested` arrives — the freeze signature.
    // Say how long we have been waiting so the two are told apart at a glance.
    ++consecutive_pending_;
    last_skip_detail_ =
      "begin-frame pending (no render request from the viewer); consecutive=" +
      std::to_string(consecutive_pending_);
    return true;  // not an error: no frame ready this tick (should_render=false)
  }
  consecutive_pending_ = 0;

  QarPose left_pose{};
  QarFov left_fov{};
  QarPose right_pose{};
  QarFov right_fov{};
  // A frame whose pose or field of view cannot be read is not rendered: what a failed
  // getter leaves behind is a zero quaternion, which is no orientation. The frame is
  // skipped as a failure of the adapter (the stream counts a sender error), the handle
  // is released, and last_skip_detail() names the failed call.
  struct ViewRead { const char * what; QarResult result; };
  const ViewRead view_reads[] = {
    {"qar_render_frame_info_get_view_pose(0)",
      qar_render_frame_info_get_view_pose(info, 0, &left_pose)},
    {"qar_render_frame_info_get_view_fov(0)",
      qar_render_frame_info_get_view_fov(info, 0, &left_fov)},
    {"qar_render_frame_info_get_view_pose(1)",
      qar_render_frame_info_get_view_pose(info, 1, &right_pose)},
    {"qar_render_frame_info_get_view_fov(1)",
      qar_render_frame_info_get_view_fov(info, 1, &right_fov)},
  };
  for (const ViewRead & read : view_reads) {
    if (qar_result_is_error(read.result)) {
      qar_render_frame_info_handle_destroy(info);
      last_skip_detail_ = result_text(read.what, read.result);
      return false;
    }
  }

  // The AR pointing ray, while both halves are in hand. See the declaration for
  // why this must happen here and not on the gesture thread. Only needs left_pose,
  // so it runs regardless of whether `info` is about to be destroyed or kept alive.
  publish_pointing_ray(left_pose);

  out_info.frame_index = frame_index_++;
  out_info.predicted_display_time = QarTimePoint{out_info.frame_index, 0};
  out_info.left_eye.pose = left_pose;
  out_info.left_eye.fov = left_fov;
  out_info.left_eye.near_far = near_far_;
  out_info.right_eye.pose = right_pose;
  out_info.right_eye.fov = right_fov;
  out_info.right_eye.near_far = near_far_;
  out_info.should_render = true;
  if (timing_) { timing_->note_consume(out_info.frame_index); }

  if (frame_trace_) {
    // Keeps `info` alive until note_pre_show() (right before show_frame, in
    // submit_opengl_stereo_frame) — do NOT destroy it here like the non-tracing
    // path below; see PendingTraceRow (the pre-show pose re-read).
    frame_trace_->begin_row(
      out_info.frame_index, prime_enabled_, tick_ms_, left_pose, right_pose, info);
  } else {
    qar_render_frame_info_handle_destroy(info);
  }
  return true;
}

void QarCApiRenderSenderAdapter::publish_pointing_ray(const QarPose & eye_pose)
{
  if (!pointing_ray_slot_ || stream_ == nullptr) {
    return;
  }
  // Drain the device's hand stream. Documented as "returns the last sample, or
  // both hands untracked when nothing arrived since the previous call" -- so a
  // failure and an empty drain are the same thing to us, an invalid ray, and
  // publishing that invalid ray is how "the hands went away" reaches the
  // picker instead of a stale aim.
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  const QarResult hands_result = qar_render_sender_last_hands(stream_, &hands);

  qar_ar_core::PointingRayInputs inputs;
  if (!qar_result_is_error(hands_result)) {
    inputs = qar_ar_core::pointing_ray_inputs_from_hands(
      hands, QAR_HAND_RIGHT, qar_ar_core::HandRayJoint::IndexTip);
  }
  inputs.has_eye = true;
  inputs.eye_position = eye_pose.position;
  inputs.timestamp_s = static_cast<double>(hands.device_timestamp.count) *
    (hands.device_timestamp.precision == 1 ? 1e-9 : 1e-3);

  // The hand itself, published WITH the ray it produced. The ray says where the
  // operator is aiming; the probe says where their hand is, and only the second
  // one can test the vendor's claim that a gesture's action_point is a scene
  // intersection rather than the hand. Same poll, same frame, one publish -- a
  // consumer must never pair this frame's ray with another frame's hand.
  qar_ar_core::HandJointsProbe probe;
  if (!qar_result_is_error(hands_result)) {
    probe = qar_ar_core::hand_joints_probe_from(hands);
  }
  probe.has_eye = true;
  probe.eye_position = eye_pose.position;
  probe.timestamp_s = inputs.timestamp_s;

  const qar_ar_core::PointingRay ray = qar_ar_core::pointing_ray_from(inputs);
  pointing_ray_slot_->publish(ray, probe);

  // One line each way, ever. The question the log has to answer is simply
  // "does this device send hands?", and that is a one-shot fact.
  if (ray.valid && !logged_hands_tracked_) {
    logged_hands_tracked_ = true;
    std::fprintf(
      stderr,
      "[qar-adapter] hands: the device IS sending hand joints - AR pointing ray is live "
      "(%s, %s of the %s hand). origin=(%.3f, %.3f, %.3f) dir=(%.3f, %.3f, %.3f) room\n",
      qar_ar_core::to_string(ray.kind), qar_ar_core::to_string(ray.joint),
      ray.hand == QAR_HAND_LEFT ? "left" : "right",
      static_cast<double>(ray.origin.x), static_cast<double>(ray.origin.y),
      static_cast<double>(ray.origin.z), static_cast<double>(ray.direction.x),
      static_cast<double>(ray.direction.y), static_cast<double>(ray.direction.z));
    std::fflush(stderr);
  } else if (!ray.valid && !logged_hands_missing_) {
    logged_hands_missing_ = true;
    std::fprintf(
      stderr,
      "[qar-adapter] hands: no AR pointing ray yet - %s (qar_render_sender_last_hands "
      "code=%d). Plane pick targets fall back to projecting the reported action "
      "point onto the plane.\n",
      ray.error.c_str(), static_cast<int>(hands_result.code));
    std::fflush(stderr);
  }
}

void QarCApiRenderSenderAdapter::fill_frame_layout(
  QarVideoFrameOpenGl & gl, const qar_ogre_streaming::QarOpenGlStereoFrame & frame,
  bool supply_ids) const
{
  // Marshal our separated-texture stereo frame into the QAR OpenGL frame
  // (QarVideoFrameOpenGl: textures[] carry {size, texture}; NO target,
  // NO synchronization field). Canonical texture ordering (adapter <-> consumer):
  //   textures[0]=left color, [1]=right color, [2]=left depth, [3]=right depth.
  // When supply_ids is false (render-into mode) we describe sizes/formats but leave
  // texture id 0 for QAR to fill via qar_render_sender_frame_opengl.
  auto set_tex = [supply_ids](
    QarVideoTextureOpenGl & t, const qar_ogre_streaming::GlVideoTexture & src) {
    t.size = src.size;
    t.texture = supply_ids ? src.texture_id : 0u;
  };
  auto set_view = [](QarVideoFrameView & v, uint32_t idx, uint32_t w, uint32_t h,
                     QarPixelFormat fmt, QarVideoFrameViewType type, QarVideoFrameViewEye eye) {
    v.start_x = 0;
    v.start_y = 0;
    v.end_x = w;
    v.end_y = h;
    v.texture_index = idx;
    v.array_layer_index = 0;
    v.texture_format = fmt;
    v.data_type = type;
    v.eye = eye;
  };

  set_tex(gl.textures[0], frame.left_color);
  set_tex(gl.textures[1], frame.right_color);
  std::size_t tex_n = 2;
  set_view(gl.texture_views[0], 0, frame.left_color.size.width, frame.left_color.size.height,
    frame.left_color.size.format, QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_LEFT);
  set_view(gl.texture_views[1], 1, frame.right_color.size.width, frame.right_color.size.height,
    frame.right_color.size.format, QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
  std::size_t view_n = 2;

  if (frame.has_depth) {
    set_tex(gl.textures[2], frame.left_depth);
    set_tex(gl.textures[3], frame.right_depth);
    tex_n = 4;
    set_view(gl.texture_views[2], 2, frame.left_depth.size.width, frame.left_depth.size.height,
      frame.left_depth.size.format, QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_LEFT);
    set_view(gl.texture_views[3], 3, frame.right_depth.size.width, frame.right_depth.size.height,
      frame.right_depth.size.format, QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
    view_n = 4;
  }
  gl.textures_count = tex_n;
  gl.texture_views_count = view_n;
}

bool QarCApiRenderSenderAdapter::submit_opengl_stereo_frame(
  const qar_ogre_streaming::QarOpenGlStereoFrame & frame)
{
  if (stream_ == nullptr) { return false; }

  static unsigned long submit_n = 0;
  const unsigned long n = ++submit_n;

  // render_targets_phase()/fence_phase() (a no-op today) just ran, synchronously, on
  // this same thread, immediately before the stream called us — so "now" here is a
  // faithful render_done_us without threading a timestamp through the adapter seam.
  if (frame_trace_) { frame_trace_->note_render_done(); }
  // Stage timing: stamps the render-done instant now and writes the submit row on every way
  // out of this function (inert when QAR_VIDEO_TIMING_DIR is unset).
  SubmitTimingGuard submit_timing(timing_.get());

  QarVideoFrameOpenGl gl{};

  if (submit_mode_ == GlSubmitMode::kSupplyOwnTextureIds) {
    // SUPPLY-OWN: hand QAR our extracted Ogre GL ids and submit (the zero-copy
    // variant; QAR overwrites supplied ids, so against the SDK this shows a
    // black scene, see GlSubmitMode).
    fill_frame_layout(gl, frame, /*supply_ids=*/true);
#ifdef _WIN32
    // Our Ogre rendering must be COMPLETE before QAR samples these ids from its own
    // context — otherwise it reads textures we have not finished writing.
    gl_finish_current_thread();
#endif
    trace_submit(n, "-> frame_opengl (supply-own)");
    const QarResult fr = qar_render_sender_frame_opengl(stream_, &gl);
    trace_submit(n, "<- frame_opengl (supply-own)");
    if (qar_result_is_error(fr)) {
      // Abandoned before show_frame: release the trace's kept-alive handle (if any)
      // without writing a row for it — see FrameTraceState::abandon_row().
      if (frame_trace_) { frame_trace_->abandon_row(); }
      return false;
    }
  } else {
    // RENDER-INTO: describe the layout, ask QAR to hand us its owned destination
    // texture ids via frame_opengl(out_frame), then copy our rendered Ogre
    // color/depth INTO those ids before show_frame (see GlSubmitMode).
    fill_frame_layout(gl, frame, /*supply_ids=*/false);
    trace_submit(n, "-> frame_opengl (render-into)");
    const QarResult fr = qar_render_sender_frame_opengl(stream_, &gl);
    trace_submit(n, "<- frame_opengl (render-into)");
    if (qar_result_is_error(fr)) {
      if (frame_trace_) { frame_trace_->abandon_row(); }
      return false;
    }
    // gl.textures[i].texture now hold QAR-owned destination ids: copy our rendered
    // Ogre color/depth into them. WITHOUT this the frames are delivered perfectly
    // but arrive black — QAR's textures were simply never written.
    if (copy_hook_) {
      if (!copy_hook_->copy_frame_into(frame, gl)) {
        if (frame_trace_) { frame_trace_->abandon_row(); }
        return false;
      }
#ifdef _WIN32
      // The copy must be COMPLETE before QAR consumes these textures from its own
      // context: shared-object writes become visible to another context only after
      // the writing context flushes (GL spec, Appendix D).
      gl_finish_current_thread();
#endif
    } else if (!warned_no_copy_hook_) {
      warned_no_copy_hook_ = true;
      std::fprintf(stderr,
        "[qar-adapter] render-into WITHOUT a copy hook: QAR's textures are never "
        "written, so the consumer will show a BLACK scene. Wire make_gl_copy_image_hook().\n");
      std::fflush(stderr);
    }
  }

  QarRenderFrameShow show = qar_render_frame_show_default();
  show.rendered_near_far = near_far_;
  // Metric metres by default; scale to whatever encoding the consumer expects.
  show.depth_scale = depth_scale_;

  // (c) pose re-read, right before show_frame, before the handle can be released —
  // the last point the SAME SDK frame handle we consumed (b) from is still alive.
  if (frame_trace_) { frame_trace_->note_pre_show(); }

  trace_submit(n, "-> show_frame");
  const QarResult sr = qar_render_sender_show_frame(stream_, &show);
  trace_submit(n, "<- show_frame");
  const bool show_ok = qar_result_is_success(sr);
  submit_timing.show_returned(show_ok);

  if (frame_trace_) { frame_trace_->note_show_done(show_ok); }

  if (show_ok && prime_enabled_) {
    // The frame is out; immediately start the wait for the next one so it runs
    // during the gap before the next tick instead of costing that tick. Skipped
    // when QAR_ADAPTER_PRIME=0 (or the live checkbox is off): the wait then starts
    // at the NEXT tick's begin_frame() instead, and that tick pays for it.
    prime_next_frame();
  }
  return show_ok;
}

void QarCApiRenderSenderAdapter::prime_next_frame()
{
  if (stream_ == nullptr || primed_info_ != nullptr) {
    return;
  }
  QarRenderFrameInfo * info = nullptr;
  bool ready = false;
  // This call is what STARTS the next begin-frame wait; whether it also completes
  // immediately is a bonus. Non-blocking either way, so it is safe on this thread.
  if (frame_trace_) { frame_trace_->note_try_begin_frame_call(/*from_prime=*/true); }
  const std::int64_t poll_start_us = timing_ ? qar_ar_core::timing_epoch_us() : 0;
  const QarResult r = qar_render_sender_try_begin_frame(stream_, &info, &ready);
  if (timing_) { timing_->note_poll_call(poll_start_us); }
  if (qar_result_is_error(r)) {
    if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_STREAM_IS_CLOSED)) {
      closed_.store(true);
    }
    if (info != nullptr) { qar_render_frame_info_handle_destroy(info); }
    return;
  }
  if (timing_) {
    if (ready && info != nullptr) { timing_->note_ready(/*from_prime=*/true); }
    else { timing_->note_pending(); }
  }
  if (ready && info != nullptr) {
    // "Ready first observed" — capture (a) now, since the eventual consume (the
    // primed fast path in begin_frame()) is a later, separate call.
    if (frame_trace_) { frame_trace_->note_ready(/*is_prime_call=*/true, info); }
    primed_info_ = info;
  } else if (info != nullptr) {
    qar_render_frame_info_handle_destroy(info);
  }
}

// ===========================================================================
//  Render-into copy hook
// ===========================================================================
namespace
{
#ifdef _WIN32
/// Copies our Ogre color/depth textures into the QAR-owned destination textures with
/// an FBO blit. Runs on the render thread with Ogre's context current; QAR's texture
/// names resolve there because QAR's context shares Ogre's object namespace (the
/// probe below checks it with glIsTexture).
///
/// glBlitFramebuffer rather than glCopyImageSubData because it is far more forgiving
/// and does three things we need at once:
///   * RESCALES — QAR sizes its textures from the consumer's request (1920x1080)
///     while we render per-eye at our own size (1280x720).
///   * converts between compatible colour formats (RGBA8 vs BGRA8).
///   * resolves multisampling, if Ogre gave the target FSAA.
/// glCopyImageSubData requires an exact size/format-class match and rejected our
/// textures outright (GL_INVALID_ENUM) even though both names were valid.
class GlBlitCopyHook : public IQarGlTextureCopyHook
{
public:
  bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame & src,
    const QarVideoFrameOpenGl & dst) override
  {
    if (!resolve()) { return false; }

    // Canonical ordering, matching fill_frame_layout():
    //   textures[0]=left color, [1]=right color, [2]=left depth, [3]=right depth.
    const qar_ogre_streaming::GlVideoTexture * srcs[4] = {
      &src.left_color, &src.right_color,
      src.has_depth ? &src.left_depth : nullptr,
      src.has_depth ? &src.right_depth : nullptr};

    // Drain any error Ogre/QAR left in the queue, so what we read back afterwards is
    // attributable to OUR copy (glGetError reports the FIRST error since last call).
    if (get_error_ != nullptr) {
      for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
    }

    // Save Ogre's FBO bindings and restore them afterwards — we are borrowing the
    // context mid-frame and must leave its state exactly as it was.
    int prev_read = 0, prev_draw = 0;
    if (get_integerv_ != nullptr) {
      get_integerv_(kReadFramebufferBinding, &prev_read);
      get_integerv_(kDrawFramebufferBinding, &prev_draw);
    }

    bool all_ok = true;
    for (std::size_t i = 0; i < dst.textures_count && i < 4; ++i) {
      const qar_ogre_streaming::GlVideoTexture * s = srcs[i];
      const unsigned int dst_id = dst.textures[i].texture;
      if (s == nullptr || s->texture_id == 0u || dst_id == 0u) { continue; }

      const unsigned int sw = s->size.width, sh = s->size.height;
      const unsigned int dw = dst.textures[i].size.width, dh = dst.textures[i].size.height;
      if (sw == 0u || sh == 0u || dw == 0u || dh == 0u) { continue; }
      if ((sw != dw || sh != dh) && !warned_size_) {
        warned_size_ = true;
        std::fprintf(stderr,
          "[qar-adapter] size differs view=%zu ours=%ux%u QAR=%ux%u — blit rescales.\n",
          i, sw, sh, dw, dh);
        std::fflush(stderr);
      }

      // QAR may hand us an ARRAY texture (natural for a 4-view stereo frame), which
      // cannot be attached with glFramebufferTexture2D(GL_TEXTURE_2D, ...) — that
      // silently attaches nothing and the FBO reports MISSING_ATTACHMENT.
      const unsigned int dst_layers = dst.textures[i].size.array_layers;
      const unsigned int dst_layer = (i < dst.texture_views_count)
        ? dst.texture_views[i].array_layer_index : 0u;

      bind_framebuffer_(kReadFramebuffer, src_fbo_);
      const unsigned int rs = attach(kReadFramebuffer, s->texture_id, 0u);
      bind_framebuffer_(kDrawFramebuffer, dst_fbo_);
      const unsigned int ds = attach(kDrawFramebuffer, dst_id, dst_layer);

      if (rs != kFramebufferComplete || ds != kFramebufferComplete) {
        all_ok = false;
        if (!reported_[i]) {
          reported_[i] = true;  // per-view: a depth-only failure must not be masked
          std::fprintf(stderr,
            "[qar-adapter] FBO incomplete view=%zu read=0x%04X draw=0x%04X "
            "src=%u(%ux%u layers=%u) dst=%u(%ux%u layers=%u layer=%u fmt=%d)\n",
            i, rs, ds, s->texture_id, sw, sh, s->size.array_layers,
            dst_id, dw, dh, dst_layers, dst_layer,
            static_cast<int>(dst.textures[i].size.format));
          (void)dst_layers;
          std::fflush(stderr);
        }
        continue;
      }

      // Depth is carried as an R32F COLOUR texture, so it blits as colour too — but
      // with NEAREST, since interpolating metric depth would invent wrong distances.
      const bool is_depth = (i >= 2);
      // GL's origin is bottom-left; if the consumer shows the scene upside down,
      // set QAR_ADAPTER_FLIP_Y=1 to flip during the blit (no rebuild needed).
      const int dy0 = flip_y_ ? static_cast<int>(dh) : 0;
      const int dy1 = flip_y_ ? 0 : static_cast<int>(dh);
      blit_framebuffer_(
        0, 0, static_cast<int>(sw), static_cast<int>(sh),
        0, dy0, static_cast<int>(dw), dy1,
        kColorBufferBit, is_depth ? kNearest : kLinear);

      const unsigned int err = (get_error_ != nullptr) ? get_error_() : 0u;
      if (err != 0u) {
        all_ok = false;
        if (!reported_[i]) {
          reported_[i] = true;  // per-view, once: colour must not mask depth
          std::fprintf(stderr,
            "[qar-adapter] glBlitFramebuffer FAILED err=0x%04X view=%zu "
            "src=%u tgt=0x%04X (%ux%u) dst=%u tgt=0x%04X (%ux%u)\n",
            err, i, s->texture_id, detect_target(s->texture_id), sw, sh,
            dst_id, detect_target(dst_id), dw, dh);
          std::fflush(stderr);
        }
      }
    }

    // Detach so we never keep QAR's textures alive through our FBOs, then restore.
    framebuffer_texture_2d_(kReadFramebuffer, kColorAttachment0, kTexture2D, 0, 0);
    framebuffer_texture_2d_(kDrawFramebuffer, kColorAttachment0, kTexture2D, 0, 0);
    bind_framebuffer_(kReadFramebuffer, static_cast<unsigned int>(prev_read));
    bind_framebuffer_(kDrawFramebuffer, static_cast<unsigned int>(prev_draw));

    // Sample repeatedly, not once. The first copied frame shows the viewer's INITIAL
    // pose (origin, grid edge-on, markers out of shot) — useless for judging depth.
    // Later samples land after the operator has navigated to the view they are judging.
    ++probe_frames_;
    const bool probe_now = (probe_frames_ == 1) ||
      (probe_every_ > 0 && probes_done_ < kMaxProbes && (probe_frames_ % probe_every_) == 0);
    if (probe_now) {
      ++probes_done_;
      std::fprintf(stderr, "[qar-adapter] ---- probe #%u at frame %lu ----\n",
        probes_done_, probe_frames_);
      // What kind of object is QAR's destination, really? Ours is the control.
      probe_dst_texture(src.left_color.texture_id);
      probe_dst_texture(dst.textures[0].texture);
      // Decisive check: is QAR's texture name even valid in OUR context? If our own
      // src is a texture here but QAR's dst is not, then QAR's textures are NOT in
      // the share group and no client-side copy can ever reach them.
      int src_is_tex = -1, dst_is_tex = -1;
      if (is_texture_ != nullptr) {
        src_is_tex = is_texture_(src.left_color.texture_id) ? 1 : 0;
        dst_is_tex = is_texture_(dst.textures[0].texture) ? 1 : 0;
        if (get_error_ != nullptr) {
          for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
        }
      }
      std::fprintf(stderr,
        "[qar-adapter] copy Ogre->QAR: n=%zu views=%zu has_depth=%d  [0] src=%u "
        "(isTexture=%d) -> dst=%u (isTexture=%d)  ours=%ux%u layers=%u  "
        "QAR=%ux%u layers=%u fmt=%d%s\n",
        dst.textures_count, dst.texture_views_count, src.has_depth ? 1 : 0,
        src.left_color.texture_id, src_is_tex,
        dst.textures[0].texture, dst_is_tex,
        src.left_color.size.width, src.left_color.size.height,
        src.left_color.size.array_layers,
        dst.textures[0].size.width, dst.textures[0].size.height,
        dst.textures[0].size.array_layers,
        static_cast<int>(dst.textures[0].size.format),
        all_ok ? "  OK" : "  (WITH ERRORS)");
      std::fflush(stderr);

      // Colour first, as the control: it is rendered from the SAME camera in the same
      // tick, so whatever geometry appears here must also appear in depth. Read as
      // GL_RED/GL_FLOAT (values 0..1, the red channel) purely to reuse the mapper.
      probe_float_texture("SRC left-COLOR (control, red channel 0..1)",
        src.left_color.texture_id, src.left_color.size.width, src.left_color.size.height);

      // The depth question, answered directly: what floats are actually in the
      // left-depth texture on each side of the copy?
      if (src.has_depth && dst.textures_count > 2) {
        probe_float_texture("SRC left-depth (ours, pre-blit)",
          src.left_depth.texture_id, src.left_depth.size.width, src.left_depth.size.height);
        probe_float_texture("DST left-depth (QAR, post-blit)",
          dst.textures[2].texture, dst.textures[2].size.width, dst.textures[2].size.height);
      }
    }
    return all_ok;
  }

private:
  // GL enums, spelled out so this file needs no GL headers.
  static constexpr unsigned int kTexture2D = 0x0DE1;
  static constexpr unsigned int kReadFramebuffer = 0x8CA8;
  static constexpr unsigned int kDrawFramebuffer = 0x8CA9;
  static constexpr unsigned int kReadFramebufferBinding = 0x8CAA;
  static constexpr unsigned int kDrawFramebufferBinding = 0x8CA6;
  static constexpr unsigned int kColorAttachment0 = 0x8CE0;
  static constexpr unsigned int kFramebufferComplete = 0x8CD5;
  static constexpr unsigned int kColorBufferBit = 0x00004000;
  static constexpr unsigned int kNearest = 0x2600;
  static constexpr unsigned int kLinear = 0x2601;
  static constexpr unsigned int kTexture2DArray = 0x8C1A;
  static constexpr unsigned int kTexture2DMultisample = 0x9100;
  static constexpr unsigned int kTextureRectangle = 0x84F5;
  static constexpr unsigned int kTextureWidth = 0x1000;
  static constexpr unsigned int kTextureHeight = 0x1001;
  static constexpr unsigned int kTextureInternalFormat = 0x1003;
  static constexpr unsigned int kTextureSamples = 0x9106;
  static constexpr unsigned int kRed = 0x1903;    // GL_RED
  static constexpr unsigned int kFloat = 0x1406;  // GL_FLOAT

  /// Read a texture's actual float contents back and report min/max/mean plus a few
  /// samples: the decisive depth diagnostic ("did our depth RTT get real metric values,
  /// or did the transport lose them?"). Six probes at most, the first at the first copied
  /// frame and then every probe_every_ frames, whatever the trace setting; it stalls the pipeline.
  void probe_float_texture(
    const char * label, unsigned int tex, unsigned int w, unsigned int h)
  {
    if (get_tex_image_ == nullptr || bind_texture_ == nullptr || get_error_ == nullptr) {
      return;
    }
    if (tex == 0u || w == 0u || h == 0u) { return; }
    const unsigned int target = detect_target(tex);
    std::vector<float> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f);

    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
    bind_texture_(target, tex);
    get_tex_image_(target, 0, kRed, kFloat, px.data());
    const unsigned int err = get_error_();
    bind_texture_(target, 0u);
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}

    if (err != 0u) {
      std::fprintf(stderr, "[qar-adapter] depth probe %s: glGetTexImage err=0x%04X\n",
        label, err);
      std::fflush(stderr);
      return;
    }

    float mn = px[0], mx = px[0];
    double sum = 0.0;
    std::size_t nonzero = 0;
    for (const float v : px) {
      if (v < mn) { mn = v; }
      if (v > mx) { mx = v; }
      sum += static_cast<double>(v);
      if (v != 0.0f) { ++nonzero; }
    }
    const std::size_t centre =
      (static_cast<std::size_t>(h) / 2) * w + (static_cast<std::size_t>(w) / 2);
    std::fprintf(stderr,
      "[qar-adapter] depth probe %s (%ux%u): min=%.4f max=%.4f mean=%.4f "
      "nonzero=%zu/%zu centre=%.4f  %s\n",
      label, w, h, static_cast<double>(mn), static_cast<double>(mx),
      sum / static_cast<double>(px.size()), nonzero, px.size(),
      static_cast<double>(px[centre]),
      (mn == mx) ? "<-- CONSTANT" : "varies");

    // Histogram over the non-zero (geometry) pixels: shows whether depth is genuinely
    // spread across distance or piled into one bucket.
    if (nonzero > 0 && mx > 0.0f) {
      std::size_t bins[10] = {};
      for (const float v : px) {
        if (v <= 0.0f) { continue; }
        std::size_t b = static_cast<std::size_t>((v / mx) * 10.0f);
        if (b > 9) { b = 9; }
        ++bins[b];
      }
      std::fprintf(stderr, "[qar-adapter]   histogram of non-zero depth (0..%.2f m):\n",
        static_cast<double>(mx));
      for (std::size_t b = 0; b < 10; ++b) {
        std::fprintf(stderr, "[qar-adapter]     %6.2f-%6.2f m : %7zu %s\n",
          static_cast<double>(mx) * b / 10.0, static_cast<double>(mx) * (b + 1) / 10.0,
          bins[b], (bins[b] * 100 / nonzero > 50) ? "<-- most" : "");
      }
    }

    // Coarse ASCII map: ' '=no geometry, '1'..'9' = near..far. Reveals at a glance
    // WHAT is in the depth pass (markers only? grid too?) and how it is distributed.
    const unsigned int cols = 64, rows = 24;
    std::fprintf(stderr, "[qar-adapter]   depth map (' '=0, 1=near .. 9=far):\n");
    for (unsigned int ry = 0; ry < rows; ++ry) {
      std::string line;
      line.reserve(cols);
      for (unsigned int rx = 0; rx < cols; ++rx) {
        float cell_max = 0.0f;
        const unsigned int x0 = rx * w / cols, x1 = (rx + 1) * w / cols;
        const unsigned int y0 = ry * h / rows, y1 = (ry + 1) * h / rows;
        for (unsigned int y = y0; y < y1; ++y) {
          for (unsigned int x = x0; x < x1; ++x) {
            const float v = px[static_cast<std::size_t>(y) * w + x];
            if (v > cell_max) { cell_max = v; }
          }
        }
        if (cell_max <= 0.0f) {
          line.push_back(' ');
        } else {
          int d = static_cast<int>((cell_max / mx) * 9.0f) + 1;
          if (d > 9) { d = 9; }
          line.push_back(static_cast<char>('0' + d));
        }
      }
      std::fprintf(stderr, "[qar-adapter]   |%s|\n", line.c_str());
    }
    std::fflush(stderr);
  }

  /// Identify what KIND of GL object QAR actually handed us. glBindTexture raises
  /// GL_INVALID_OPERATION when a texture name is bound to a target other than the one
  /// it was created with, so the target that binds cleanly is its real type. Also
  /// reports sample count and internal format, since a multisample or oddly-formatted
  /// destination explains an INVALID_OPERATION blit that has nothing to do with size.
  void probe_dst_texture(unsigned int id)
  {
    if (bind_texture_ == nullptr || get_error_ == nullptr) { return; }
    struct { unsigned int target; const char * name; } targets[] = {
      {kTexture2D, "TEXTURE_2D"},
      {kTexture2DArray, "TEXTURE_2D_ARRAY"},
      {kTexture2DMultisample, "TEXTURE_2D_MULTISAMPLE"},
      {kTextureRectangle, "TEXTURE_RECTANGLE"},
    };
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
    for (const auto & t : targets) {
      bind_texture_(t.target, id);
      const unsigned int err = get_error_();
      if (err != 0u) { continue; }
      int w = 0, h = 0, samples = 0, ifmt = 0;
      if (get_tex_level_parameteriv_ != nullptr) {
        get_tex_level_parameteriv_(t.target, 0, kTextureWidth, &w);
        get_tex_level_parameteriv_(t.target, 0, kTextureHeight, &h);
        get_tex_level_parameteriv_(t.target, 0, kTextureSamples, &samples);
        get_tex_level_parameteriv_(t.target, 0, kTextureInternalFormat, &ifmt);
        for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
      }
      std::fprintf(stderr,
        "[qar-adapter] QAR dst texture %u is %s  %dx%d samples=%d internalFormat=0x%04X\n",
        id, t.name, w, h, samples, static_cast<unsigned int>(ifmt));
      std::fflush(stderr);
      bind_texture_(t.target, 0u);
      for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
      return;
    }
    std::fprintf(stderr,
      "[qar-adapter] QAR dst texture %u bound to NO known target (2D/2D_ARRAY/"
      "2D_MULTISAMPLE/RECTANGLE all rejected it).\n", id);
    std::fflush(stderr);
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
  }

  /// The GL target a texture name was actually created with, cached per id.
  ///
  /// QAR's QarTextureSize.array_layers reports 1, but the object it hands us is a
  /// GL_TEXTURE_2D_ARRAY. Trusting the reported layer count attaches
  /// the texture as layered, and glBlitFramebuffer rejects a layered draw framebuffer
  /// with GL_INVALID_OPERATION — so we ask GL what the object really is instead.
  unsigned int detect_target(unsigned int id)
  {
    for (const auto & e : target_cache_) {
      if (e.id == id) { return e.target; }
    }
    unsigned int found = kTexture2D;
    if (bind_texture_ != nullptr && get_error_ != nullptr) {
      const unsigned int candidates[] = {
        kTexture2D, kTexture2DArray, kTexture2DMultisample, kTextureRectangle};
      for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
      for (const unsigned int t : candidates) {
        bind_texture_(t, id);
        const bool ok = (get_error_() == 0u);
        bind_texture_(t, 0u);
        for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
        if (ok) { found = t; break; }
      }
    }
    if (target_cache_count_ < kTargetCacheMax) {
      target_cache_[target_cache_count_++] = {id, found};
    }
    return found;
  }

  /// Attach @p tex to @p fb_target's colour attachment using the form its REAL target
  /// requires, and return the resulting framebuffer status. Array textures must be
  /// attached one layer at a time (non-layered) for glBlitFramebuffer to accept them.
  unsigned int attach(unsigned int fb_target, unsigned int tex, unsigned int layer_index)
  {
    const unsigned int target = detect_target(tex);
    if (target == kTexture2DArray && framebuffer_texture_layer_ != nullptr) {
      framebuffer_texture_layer_(fb_target, kColorAttachment0, tex, 0,
        static_cast<int>(layer_index));
    } else {
      framebuffer_texture_2d_(fb_target, kColorAttachment0, target, tex, 0);
    }
    unsigned int st = check_framebuffer_status_(fb_target);
    if (st == kFramebufferComplete) { return st; }

    // Fallbacks, in case the target probe was fooled.
    if (framebuffer_texture_layer_ != nullptr) {
      framebuffer_texture_layer_(fb_target, kColorAttachment0, tex, 0,
        static_cast<int>(layer_index));
      st = check_framebuffer_status_(fb_target);
      if (st == kFramebufferComplete) { return st; }
    }
    framebuffer_texture_2d_(fb_target, kColorAttachment0, kTexture2D, tex, 0);
    return check_framebuffer_status_(fb_target);
  }

  bool resolve()
  {
    if (resolved_) { return ready_; }
    resolved_ = true;
    const WglApi & w = wgl();

    // The framebuffer-object entry points are GL 3.0+, so they come from
    // wglGetProcAddress (which needs a current context — we are on the render
    // thread). glGetError/glGetIntegerv are GL 1.1 and live in opengl32.dll itself.
    auto ext = [&w](const char * n) -> void * {
        return (w.GetProcAddress_ != nullptr)
               ? reinterpret_cast<void *>(w.GetProcAddress_(n)) : nullptr;
      };
    gen_framebuffers_ = reinterpret_cast<PfnGenFramebuffers>(ext("glGenFramebuffers"));
    delete_framebuffers_ =
      reinterpret_cast<PfnDeleteFramebuffers>(ext("glDeleteFramebuffers"));
    bind_framebuffer_ = reinterpret_cast<PfnBindFramebuffer>(ext("glBindFramebuffer"));
    framebuffer_texture_2d_ =
      reinterpret_cast<PfnFramebufferTexture2D>(ext("glFramebufferTexture2D"));
    framebuffer_texture_layer_ =
      reinterpret_cast<PfnFramebufferTextureLayer>(ext("glFramebufferTextureLayer"));
    framebuffer_texture_ =
      reinterpret_cast<PfnFramebufferTexture>(ext("glFramebufferTexture"));
    blit_framebuffer_ = reinterpret_cast<PfnBlitFramebuffer>(ext("glBlitFramebuffer"));
    check_framebuffer_status_ =
      reinterpret_cast<PfnCheckFramebufferStatus>(ext("glCheckFramebufferStatus"));
    if (w.module != nullptr) {
      get_error_ = reinterpret_cast<PfnGetError>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glGetError")));
      get_integerv_ = reinterpret_cast<PfnGetIntegerv>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glGetIntegerv")));
      is_texture_ = reinterpret_cast<PfnIsTexture>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glIsTexture")));
      bind_texture_ = reinterpret_cast<PfnBindTexture>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glBindTexture")));
      get_tex_level_parameteriv_ = reinterpret_cast<PfnGetTexLevelParameteriv>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glGetTexLevelParameteriv")));
      get_tex_image_ = reinterpret_cast<PfnGetTexImage>(
        reinterpret_cast<void *>(::GetProcAddress(w.module, "glGetTexImage")));
    }

    ready_ = gen_framebuffers_ != nullptr && bind_framebuffer_ != nullptr &&
      framebuffer_texture_2d_ != nullptr && blit_framebuffer_ != nullptr &&
      check_framebuffer_status_ != nullptr;
    if (!ready_) {
      std::fprintf(stderr,
        "[qar-adapter] FBO blit entry points unavailable (needs GL 3.0+ and a current "
        "context) — render-into cannot fill QAR's textures.\n");
      std::fflush(stderr);
      return false;
    }

    unsigned int fbos[2] = {0u, 0u};
    gen_framebuffers_(2, fbos);
    src_fbo_ = fbos[0];
    dst_fbo_ = fbos[1];
    flip_y_ = std::getenv("QAR_ADAPTER_FLIP_Y") != nullptr;
    if (const char * pe = std::getenv("QAR_ADAPTER_PROBE_EVERY")) {
      const long v = std::strtol(pe, nullptr, 10);
      probe_every_ = (v > 0) ? static_cast<unsigned int>(v) : 0u;
    }
    ready_ = src_fbo_ != 0u && dst_fbo_ != 0u;
    return ready_;
  }

  using PfnGenFramebuffers = void(WINAPI *)(int, unsigned int *);
  using PfnDeleteFramebuffers = void(WINAPI *)(int, const unsigned int *);
  using PfnBindFramebuffer = void(WINAPI *)(unsigned int, unsigned int);
  using PfnFramebufferTexture2D =
    void(WINAPI *)(unsigned int, unsigned int, unsigned int, unsigned int, int);
  using PfnFramebufferTextureLayer =
    void(WINAPI *)(unsigned int, unsigned int, unsigned int, int, int);
  using PfnFramebufferTexture =
    void(WINAPI *)(unsigned int, unsigned int, unsigned int, int);
  using PfnBlitFramebuffer = void(WINAPI *)(
    int, int, int, int, int, int, int, int, unsigned int, unsigned int);
  using PfnCheckFramebufferStatus = unsigned int (WINAPI *)(unsigned int);
  using PfnGetError = unsigned int (WINAPI *)(void);
  using PfnGetIntegerv = void(WINAPI *)(unsigned int, int *);
  using PfnIsTexture = unsigned char (WINAPI *)(unsigned int);
  using PfnBindTexture = void(WINAPI *)(unsigned int, unsigned int);
  using PfnGetTexLevelParameteriv = void(WINAPI *)(unsigned int, int, unsigned int, int *);
  using PfnGetTexImage = void(WINAPI *)(unsigned int, int, unsigned int, unsigned int, void *);

  PfnGenFramebuffers gen_framebuffers_ = nullptr;
  PfnDeleteFramebuffers delete_framebuffers_ = nullptr;
  PfnBindFramebuffer bind_framebuffer_ = nullptr;
  PfnFramebufferTexture2D framebuffer_texture_2d_ = nullptr;
  PfnFramebufferTextureLayer framebuffer_texture_layer_ = nullptr;
  PfnFramebufferTexture framebuffer_texture_ = nullptr;
  PfnBlitFramebuffer blit_framebuffer_ = nullptr;
  PfnCheckFramebufferStatus check_framebuffer_status_ = nullptr;
  PfnGetError get_error_ = nullptr;
  PfnGetIntegerv get_integerv_ = nullptr;
  PfnIsTexture is_texture_ = nullptr;
  PfnBindTexture bind_texture_ = nullptr;
  PfnGetTexLevelParameteriv get_tex_level_parameteriv_ = nullptr;
  PfnGetTexImage get_tex_image_ = nullptr;

  // Real GL target per texture id (ours + QAR's = 8 at most, so a tiny array beats a map).
  static constexpr std::size_t kTargetCacheMax = 16;
  struct TargetEntry { unsigned int id; unsigned int target; };
  TargetEntry target_cache_[kTargetCacheMax]{};
  std::size_t target_cache_count_ = 0;

  unsigned int src_fbo_ = 0u;
  unsigned int dst_fbo_ = 0u;
  bool flip_y_ = false;
  bool ready_ = false;
  bool resolved_ = false;
  bool reported_[4]{};  ///< per-view one-shot error guard (colour must not mask depth)
  bool warned_size_ = false;

  // Periodic probing. Frame 1 is always sampled; after that every probe_every_ frames
  // (QAR_ADAPTER_PROBE_EVERY, default 600 ~= 10 s at 60 fps), up to kMaxProbes.
  static constexpr unsigned int kMaxProbes = 6;
  unsigned long probe_frames_ = 0;
  unsigned int probes_done_ = 0;
  unsigned int probe_every_ = 600;
};
#endif  // _WIN32
}  // namespace

std::shared_ptr<IQarGlTextureCopyHook> make_gl_copy_image_hook()
{
#ifdef _WIN32
  return std::make_shared<GlBlitCopyHook>();
#else
  return nullptr;
#endif
}

// ===========================================================================
//  QarCApiSenderFactory
// ===========================================================================
QarCApiSenderFactory::QarCApiSenderFactory(const Config & config) : config_(config)
{
  // Diagnostic breadcrumbs: a hard fault inside any of the SDK C
  // calls below kills the whole host process (rviz2) before last_error_ can be
  // surfaced. Setting QAR_ADAPTER_TRACE=1 emits a flushed stderr line around each
  // call so the last crumb printed pinpoints the faulting call. No-op otherwise.
  const bool trace = std::getenv("QAR_ADAPTER_TRACE") != nullptr;
  // Delegate to trace_msg so the bootstrap crumbs carry the same elapsed/delta
  // stamps as everything else — this straight line of blocking calls is precisely
  // where we need to know which one owns the multi-second stall.
  auto crumb = [trace](const char * msg) {
    if (trace) { trace_msg(msg); }
  };
  crumb("ctor begin");

  // Invite flow: we will invite ONE specific Visualizer and must stream
  // ONLY to it. Arm the filter BEFORE qar_peer_subscribe_updates so the internal
  // runtime peers that surface immediately are dropped (not queued) until the
  // invited Visualizer id is known — otherwise the sender is built for whichever
  // internal peer arrives first and the Visualizer's frames get app-id-rejected.
  invite_flow_ = config_.invite_flow_admission;

#ifdef QAR_ENABLE_DYNAMIC_LOADING
  // Resolve and load the QAR implementation DLL before ANY qar_* call: with dynamic
  // loading the wrappers dispatch through tables that are empty until this succeeds
  // (calling one earlier would abort()).
  if (!qar_is_library_loaded()) {
    std::string dll = config_.sdk_library_path;
    if (dll.empty()) {
      dll = config_.runtime_binaries_folder_path.empty()
        ? std::string("qar-streaming-c.dll")
        : config_.runtime_binaries_folder_path + "/qar-streaming-c.dll";
    }
    crumb("-> qar_library_load");
    if (trace) { std::fprintf(stderr, "[qar-adapter]    dll=%s\n", dll.c_str()); }
    if (!qar_library_load(dll.c_str())) {
      last_error_ = "qar_library_load failed for '" + dll +
        "' (check the path and that its dependencies resolve)";
      crumb("<- qar_library_load FAILED");
      return;
    }
    crumb("<- qar_library_load");
    loaded_library_ = true;
  }
#endif

  QarLibraryInit lib = qar_library_init_default();
  // Vendor request: always produce full library logs (TRACE) into a known folder so a
  // bug report can carry them. Default folder: %TEMP%/quaternar (next to other logs).
  if (config_.log_folder_path.empty()) {
    const char * tmp = std::getenv("TEMP");
    if (tmp == nullptr) { tmp = std::getenv("TMP"); }
    config_.log_folder_path = (tmp != nullptr)
      ? std::string(tmp) + "\\quaternar"
      : std::string("quaternar");
  }
#ifdef _WIN32
  CreateDirectoryA(config_.log_folder_path.c_str(), nullptr);  // ok if it already exists
#endif
  lib.log_folder_path = config_.log_folder_path.c_str();
  lib.log_severity = config_.log_severity;
  lib.enable_console_logging = config_.enable_console_logging;
  {
    // THE REASON "severity=3 was passed but ignored": the vendor library obeys the
    // QAR_LOG_SEVERITY environment variable over lib.log_severity, so a stale value in
    // the environment beat the struct. State the request on both channels, and say what
    // was found. Only the real (dynamically loaded) library has this behaviour.
    const LogSeverity severity = static_cast<LogSeverity>(static_cast<int>(config_.log_severity));
    std::string env_before;
    bool env_exported = false;
#ifdef QAR_ENABLE_DYNAMIC_LOADING
    env_before = export_library_log_severity(severity);
    env_exported = true;
#endif
    std::fprintf(stderr,
      "[qar-adapter] library logging: severity=%s console=%s folder=%s%s\n",
      log_severity_name(severity), config_.enable_console_logging ? "on" : "off",
      config_.log_folder_path.c_str(),
      (env_exported && !env_before.empty() && env_before != log_severity_name(severity)) ?
      (std::string("  [QAR_LOG_SEVERITY='") + env_before +
      "' was in the environment and takes precedence over the init struct inside the vendor "
      "library - overwritten with '" + log_severity_name(severity) + "']").c_str() : "");
    std::fflush(stderr);
#ifdef _WIN32
    // Before any session work: zenohc.dll is now in the process (qar_library_load pulled
    // it in), so its logger can be started. Off unless QAR_ZENOH_LOG asks AND the severity
    // is trace/debug.
    init_zenoh_logging_if_requested(severity);
#endif
  }
  crumb("-> qar_library_init");
  QarResult r{};
#ifdef _WIN32
  if (guarded_library_init(&lib, &r) != 0) {
    last_error_ = "qar_library_init HARD-FAULTED (see '*** SEH FAULT' line above for module)";
    crumb("<- qar_library_init (SEH FAULT)");
    return;
  }
#else
  r = qar_library_init(&lib);
#endif
  crumb("<- qar_library_init");
  if (qar_result_is_error(r)) { last_error_ = report_result("qar_library_init", r); return; }

  QarRuntimeInit rt = qar_runtime_init_default();
  // Point the runtime at the folder it spawns app binaries (the
  // Visualizer) from — otherwise qar_runtime_create warns "Runtime folder path
  // not provided. Using cwd." The config string outlives this call (stored in
  // config_). Empty => keep the QAR default.
  if (!config_.runtime_binaries_folder_path.empty()) {
    rt.runtime_binaries_folder_path = config_.runtime_binaries_folder_path.c_str();
  }
  crumb("-> qar_runtime_create");
#ifdef _WIN32
  {
    const int gc = guarded_runtime_create(&rt, &runtime_);
    if (gc == 1) {
      last_error_ = "qar_runtime_create HARD-FAULTED (see '*** SEH FAULT' line above for module)";
      crumb("<- qar_runtime_create (SEH FAULT)");
      return;
    }
    if (gc != 0 || runtime_ == nullptr) {
      last_error_ = "qar_runtime_create failed";
      crumb("<- qar_runtime_create (err)");
      return;
    }
  }
#else
  r = qar_runtime_create(&rt, &runtime_);
  if (qar_result_is_error(r) || runtime_ == nullptr) {
    last_error_ = report_result("qar_runtime_create", r);
    return;
  }
#endif
  crumb("<- qar_runtime_create");

  // Session lifecycle. The SDK creates a session on the runtime (the host mints an
  // invite), then obtains an active QarSession* handle: a host "creates the session
  // manually, then invites the visualizer via invitePeer".
  //
  // The header exposes create_session -> QarSessionInvite*, but
  // the ONLY function returning a QarSession** is qar_session_join (documented "peer
  // side"); qar_runtime_join_session returns no handle. So we activate the host's own
  // session by serializing the created invite and joining it (see
  // docs/QAROS_INTEGRATION_NOTES.md#session-bootstrap).
  QarSessionCreateInit create = qar_session_create_init_default();
  // Vendor guidance: the session GUID must be UNIQUE and NON-ZERO —
  // the all-zero id (QAR_ID_DEFAULT) must not be used. (Matching the launcher's
  // default-session storage key space with an all-zero id is the wrong fix: the
  // runtime spawns and configures its own launcher, so the storage follows the
  // session we create.) Belt-and-braces: if the generated id were somehow
  // all-zero, force the last byte to 1.
  QarSessionId session_id = qar_session_id_unique();
  {
    bool all_zero = true;
    for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
      if (session_id.data[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) { session_id.data[QAR_MAX_ID_LENGTH - 1] = 1; }
  }
  create.session_id = &session_id;
  // The runtime (not us) starts qar-runtime-launcher.exe; this JSON is its config.
  // `{"enableMixer": false}` => direct RViz -> Visualizer path (debug default).
  if (!config_.launcher_config_json.empty()) {
    create.launcher_config_json = config_.launcher_config_json.c_str();
  }
  if (trace) {
    std::fprintf(stderr, "[qar-adapter]    session_id=%s launcher_cfg=%s\n",
      qar_ogre_streaming::peer_id_to_hex(
        *reinterpret_cast<const QarPeerId *>(&session_id)).c_str(),
      config_.launcher_config_json.empty() ? "(null)" : config_.launcher_config_json.c_str());
    std::fflush(stderr);
  }
  crumb("-> qar_runtime_create_session");
  r = qar_runtime_create_session(runtime_, &create, &invite_);
  crumb("<- qar_runtime_create_session");
  if (qar_result_is_error(r) || invite_ == nullptr) {
    last_error_ = report_result("qar_runtime_create_session", r);
    if (invite_ == nullptr) { last_error_ += " (invite handle is null)"; }
    return;
  }

  std::array<uint8_t, 4096> invite_bytes{};
  size_t invite_written = 0;
  crumb("-> qar_session_invite_serialize");
  r = qar_session_invite_serialize(
    invite_, invite_bytes.data(), invite_bytes.size(), &invite_written);
  crumb("<- qar_session_invite_serialize");
  if (qar_result_is_error(r)) {
    last_error_ = report_result("qar_session_invite_serialize", r);
    return;
  }

  QarSessionJoinInit join = qar_session_join_init_default();
  join.invite_data = invite_bytes.data();
  join.invite_data_size = invite_written;
  crumb("-> qar_session_join");
  r = qar_session_join(&join, &session_);
  crumb("<- qar_session_join");
  if (qar_result_is_error(r) || session_ == nullptr) {
    last_error_ = report_result("qar_session_join", r);
    if (session_ == nullptr) { last_error_ += " (session handle is null)"; }
    return;
  }

  // Learn our OWN peer id before subscribing. Every admission policy excludes it —
  // we are a peer of this session too, and streaming to ourselves is never right.
  {
    QarPeerSpec * my_spec = nullptr;
    crumb("-> qar_session_get_my_spec");
    const QarResult ms = qar_session_get_my_spec(session_, &my_spec);
    crumb("<- qar_session_get_my_spec");
    if (!qar_result_is_error(ms) && my_spec != nullptr) {
      if (!qar_result_is_error(qar_peer_spec_get_id(my_spec, &self_peer_))) {
        has_self_peer_ = true;
        trace_peer_id("self", self_peer_);
      }
      qar_peer_spec_handle_destroy(my_spec);
    }
  }

  // Create the app volume BEFORE subscribing, so it is already known by the time any
  // peer surfaces and create_sender() runs. The app volume is the mixer's unit of
  // work: it composites each incoming stream into the volume named by that stream's
  // QarRenderSenderInit::app_volume_id. With the mixer ON, a stream created with the
  // default NULL id has nowhere to land — the Visualizer renders an empty room.
  if (config_.create_app_volume) {
    QarAppVolumeInit vol = qar_app_volume_init_default();
    vol.display_name = config_.app_volume_name.c_str();
    // gesture_configuration stays NULL (qar_app_volume_init_default()'s own
    // default) -- the vendor's own advice: leaving it NULL gives
    // the volume the runtime's default configuration, and
    // qar_app_volume_gesture_configuration_default() returns that EXACT same
    // configuration, so a struct
    // we build and pass back would change no field while adding a real risk:
    // any rule we left unfilled, or any drift against the runtime's own
    // layout, silently zeros to QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_NONE
    // -- an invisible ray -- rather than to the vendor's SOLID/DOTTED
    // defaults. NULL removes that possibility outright.
    // pose is the cuboid CENTRE in room space, so lift the box off the floor rather
    // than leaving half of it sunk below y=0.
    vol.pose = qar_pose_default();
    vol.pose.position.y = config_.app_volume_center_y_m;
    vol.size.width_meters = config_.app_volume_width_m;
    vol.size.length_meters = config_.app_volume_length_m;
    vol.size.height_meters = config_.app_volume_height_m;
    vol.app_pose = qar_pose_default();
    vol.app_scale = config_.app_volume_scale;

    crumb("-> qar_app_volumes_add_volume");
    const QarResult av = qar_app_volumes_add_volume(session_, &vol, &app_volume_);
    crumb("<- qar_app_volumes_add_volume");
    if (qar_result_is_error(av)) {
      // Not fatal to the CONNECT -- the session is still usable and a later tick may
      // succeed -- but fatal to every render stream: since the 2026-09-21 SDK
      // the field is Required and NULL is rejected outright, so without a
      // volume create_sender() refuses; see the refusal there.
      std::fprintf(stderr,
        "[qar-adapter] %s - NO render stream can be created without an app volume: "
        "QarRenderSenderInit::app_volume_id is required and NULL is rejected with "
        "QAR_STATUS_APP_VOLUME_INVALID_ID.\n",
        report_result("qar_app_volumes_add_volume", av).c_str());
      std::fflush(stderr);
    } else {
      has_app_volume_ = true;
      if (adapter_trace_on()) {
        std::fprintf(stderr,
          "[qar-adapter] app volume '%s' created id=%s size=%.2fx%.2fx%.2f m "
          "centre_y=%.2f m app_scale=%.3f\n",
          config_.app_volume_name.c_str(), app_volume_id_to_hex(app_volume_).c_str(),
          static_cast<double>(config_.app_volume_width_m),
          static_cast<double>(config_.app_volume_length_m),
          static_cast<double>(config_.app_volume_height_m),
          static_cast<double>(config_.app_volume_center_y_m),
          static_cast<double>(config_.app_volume_scale));
        // Always NULL -- see the comment above vol.gesture_configuration.
        std::fprintf(stderr,
          "[qar-adapter] app volume gesture configuration (sent): NULL (runtime "
          "default)\n");
        std::fflush(stderr);
        if (config_.app_volume_pick_gestures) {
          // There is no get-handle-by-id call (see scene_placement() below for
          // the same limitation), so enumerate the session's app volumes and
          // match by id to read back what the runtime actually holds -- that
          // is what proves arbitration, not what we intended to send it.
          std::size_t volume_count = 0;
          if (!qar_result_is_error(qar_query_app_volumes_count(session_, &volume_count)) &&
            volume_count > 0)
          {
            std::vector<QarAppVolume *> volume_handles(volume_count, nullptr);
            std::size_t volumes_written = 0;
            if (!qar_result_is_error(qar_query_app_volumes(
                  session_, volume_handles.data(), volume_count, &volumes_written)))
            {
              bool read_back_logged = false;
              for (std::size_t i = 0; i < volumes_written && i < volume_handles.size(); ++i) {
                QarAppVolume * const handle = volume_handles[i];
                if (handle == nullptr) { continue; }
                if (!read_back_logged) {
                  QarAppVolumeId id{};
                  const bool is_ours =
                    !qar_result_is_error(qar_app_volume_get_id(handle, &id)) &&
                    std::memcmp(&id, &app_volume_, sizeof(QarAppVolumeId)) == 0;
                  QarAppVolumeGestureConfiguration read_back{};
                  if (is_ours && !qar_result_is_error(
                        qar_app_volume_get_gesture_configuration(handle, &read_back)))
                  {
                    log_gesture_configuration("read-back", read_back);
                    read_back_logged = true;
                  }
                }
                qar_app_volume_handle_destroy(handle);
              }
            }
          }
        }
      }
    }
  }

  // Discover peers that want a render stream through the peer-spec update subscription:
  // each joining visualizer surfaces via the callback, which records the peer id
  // (drained by poll_new_peer_requests()).
  crumb("-> qar_peer_subscribe_updates");
  r = qar_peer_subscribe_updates(session_, peer_update_trampoline, this, nullptr);
  crumb("<- qar_peer_subscribe_updates");
  if (qar_result_is_error(r)) {
    last_error_ = report_result("qar_peer_subscribe_updates", r);
    return;
  }

  // Optionally invite QAR Visualizer peers so the runtime spawns/
  // connects them (the host creates the session, then invites the visualizer
  // via invitePeer). An invited peer then surfaces through peer_subscribe_updates
  // and gets a render sender via create_sender(). The runtime spawns the invited
  // Visualizer itself: nothing starts qar-runtime-launcher.exe or the Visualizer
  // by hand.
  // DEFAULT 0: connecting opens the session and invites NOTHING. The
  // operator spawns a Visualizer on demand from the panel, because a real-device
  // session often wants no Visualizer at all, and because each invite is a blocking
  // call we should not force into every connect.
  if (config_.auto_invite_visualizers > 0) {
    {
      // Drop any internal peers that slipped into the queue before ANY invited id was
      // known, so the FIRST stream we open is a Visualizer's (matching app id).
      std::lock_guard<std::mutex> lk(mutex_);
      pending_requests_.clear();
    }
    // One Visualizer == one physical client head, so invite as many as were asked for.
    // Each spawns its own Visualizer and yields a distinct peer id.
    for (uint32_t i = 0; i < config_.auto_invite_visualizers; ++i) {
      QarPeerId invited = qar_peer_id_default();
      const std::string err =
        invite_one_peer_locked_free(PeerKind::kVisualizer, std::string(), invited);
      if (!err.empty()) {
        // Auto-inviting is an opt-in convenience, not the bootstrap path: a failed
        // invite leaves a perfectly usable session the operator can still invite into.
        // So warn and stop auto-inviting rather than failing the whole connect.
        std::fprintf(stderr, "[qar-adapter] *** auto visualizer invite %u/%u: %s\n",
          i + 1u, config_.auto_invite_visualizers, err.c_str());
        std::fflush(stderr);
        break;
      }
      // Queue each invited peer explicitly so its sender exists before it connects
      // (the runtime accepts its frames only against a sender built for its app id).
      on_render_request(invited);
    }
  }

  // Real devices, e.g. "hololens:HOLOLENS-A2B3,hololens:HOLOLENS-C4D5,quest".
  // Inviting them here is what puts them in the allow-list, so they are admitted by
  // exact id and the kInvitedOrNew timing heuristic is not needed for them at all.
  // One unreachable headset must never stop the session or the other devices.
  for (const std::string & entry : split_list(config_.invite_peers)) {
    const std::size_t colon = entry.find(':');
    const std::string kind_text = entry.substr(0, colon);
    const std::string hostname =
      (colon == std::string::npos) ? std::string() : entry.substr(colon + 1);
    PeerKind kind = PeerKind::kVisualizer;
    if (!parse_peer_kind(kind_text, kind)) {
      std::fprintf(stderr,
        "[qar-adapter] *** invite_peers: unknown device kind '%s' in '%s' "
        "(use visualizer|hololens|quest|zed)\n", kind_text.c_str(), entry.c_str());
      std::fflush(stderr);
      continue;
    }
    QarPeerId invited = qar_peer_id_default();
    const std::string err = invite_one_peer_locked_free(kind, hostname, invited);
    if (!err.empty()) {
      std::fprintf(stderr, "[qar-adapter] *** invite '%s' failed: %s\n",
        entry.c_str(), err.c_str());
      std::fflush(stderr);
      continue;
    }
    on_render_request(invited);
  }

  crumb("ctor ready");
  {
    // Starts the kInvitedOrNew settle window: peers surfacing from here until
    // baseline_settle_ms elapses are the runtime's own, anything later is a consumer.
    std::lock_guard<std::mutex> lk(mutex_);
    connected_at_ = std::chrono::steady_clock::now();
    connected_at_set_ = true;
  }
  ready_ = true;
}

QarCApiSenderFactory::~QarCApiSenderFactory()
{
  if (session_ != nullptr) { qar_session_destroy(session_); session_ = nullptr; }
  if (invite_ != nullptr) { qar_session_invite_handle_destroy(invite_); invite_ = nullptr; }
  if (runtime_ != nullptr) { qar_runtime_destroy(runtime_); runtime_ = nullptr; }
#ifdef QAR_ENABLE_DYNAMIC_LOADING
  // The constructor returns before qar_library_init exactly when qar_library_load failed.
  // The dispatch tables are empty then, and calling the wrapper would abort() the host.
  if (qar_is_library_loaded()) { qar_library_destroy(); }
#else
  qar_library_destroy();
#endif
#ifdef _WIN32
  // Destroy the per-sender contexts only after the library (and therefore every QAR
  // graphics thread) is gone, so we never delete a context still current elsewhere.
  {
    const WglApi & w = wgl();
    for (auto & ctx : gl_sender_contexts_) {
      if (ctx != nullptr && ctx->context != nullptr && w.DeleteContext != nullptr) {
        w.DeleteContext(static_cast<HGLRC>(ctx->context));
        ctx->context = nullptr;
      }
    }
  }
#endif
  gl_sender_contexts_.clear();
#ifdef QAR_ENABLE_DYNAMIC_LOADING
  // Release the dynamically loaded QAR DLL only if we were the ones who loaded it.
  if (loaded_library_) {
    qar_library_unload();
    loaded_library_ = false;
  }
#endif
}

bool QarCApiSenderFactory::scene_placement(qar_ar_core::ScenePlacement & out) const
{
  if (!has_app_volume_ || session_ == nullptr) {
    return false;
  }
  // There is no get-handle-by-id call, so enumerate and match. The session owns
  // one volume in practice; the loop exists because the API cannot promise it.
  std::size_t count = 0;
  if (qar_result_is_error(qar_query_app_volumes_count(session_, &count)) || count == 0) {
    return false;
  }
  std::vector<QarAppVolume *> handles(count, nullptr);
  std::size_t written = 0;
  if (qar_result_is_error(qar_query_app_volumes(session_, handles.data(), count, &written))) {
    return false;
  }
  bool found = false;
  qar_ar_core::ScenePlacement placement;
  for (std::size_t i = 0; i < written && i < handles.size(); ++i) {
    QarAppVolume * const handle = handles[i];
    if (handle == nullptr) {
      continue;
    }
    if (!found) {
      QarAppVolumeId id{};
      const bool is_ours = !qar_result_is_error(qar_app_volume_get_id(handle, &id)) &&
        std::memcmp(&id, &app_volume_, sizeof(QarAppVolumeId)) == 0;
      if (is_ours) {
        QarPose volume_pose{};
        QarPose app_pose{};
        float app_scale = 1.0f;
        // All three or none: a placement assembled from a partial read would be
        // a different transform, not a slightly wrong one.
        const bool ok =
          !qar_result_is_error(qar_app_volume_get_pose(handle, &volume_pose)) &&
          !qar_result_is_error(qar_app_volume_get_app_pose(handle, &app_pose)) &&
          !qar_result_is_error(qar_app_volume_get_app_scale(handle, &app_scale));
        if (ok) {
          placement.known = true;
          placement.volume_pose = volume_pose;
          placement.app_pose = app_pose;
          placement.app_scale = app_scale;
          found = true;
        }
      }
    }
    qar_app_volume_handle_destroy(handle);
  }
  if (!found) {
    return false;
  }
  out = placement;
  return true;
}

bool QarCApiSenderFactory::capture_gl_context()
{
#ifdef _WIN32
  const WglApi & w = wgl();
  if (!w.ok()) { return false; }

  HDC hdc = (w.GetCurrentDC != nullptr) ? w.GetCurrentDC() : nullptr;
  HGLRC hglrc = (w.GetCurrentContext != nullptr) ? w.GetCurrentContext() : nullptr;
  if (hdc != nullptr && hglrc != nullptr) {
    const bool changed = (gl_render_context_ != static_cast<void *>(hglrc)) ||
      (gl_device_context_ != static_cast<void *>(hdc));
    gl_device_context_ = static_cast<void *>(hdc);
    gl_render_context_ = static_cast<void *>(hglrc);
    if (changed && adapter_trace_on()) {
      std::fprintf(stderr, "[qar-adapter] latched WGL context HDC=%p HGLRC=%p\n",
        gl_device_context_, gl_render_context_);
      std::fflush(stderr);
    }
  }

#endif
  return gl_render_context_ != nullptr && gl_device_context_ != nullptr;
}

void * QarCApiSenderFactory::create_shared_gl_context()
{
#ifdef _WIN32
  const WglApi & w = wgl();
  if (gl_device_context_ == nullptr || gl_render_context_ == nullptr ||
    w.CreateContext == nullptr || w.ShareLists == nullptr)
  {
    return nullptr;
  }
  // Created here, while Ogre's context is current on this thread. It shares Ogre's
  // object namespace, so QAR sees our textures, but it is never made current HERE —
  // leaving QAR's internal graphics thread free to bind it.
  HGLRC shared = w.CreateContext(static_cast<HDC>(gl_device_context_));
  if (shared == nullptr) {
    std::fprintf(stderr, "[qar-adapter] *** wglCreateContext FAILED (err=%lu)\n",
      static_cast<unsigned long>(GetLastError()));
    std::fflush(stderr);
    return nullptr;
  }
  // wglShareLists(source, destination): the destination must be newly created and
  // still empty, which is exactly the context we just made.
  if (w.ShareLists(static_cast<HGLRC>(gl_render_context_), shared) == FALSE) {
    const DWORD err = GetLastError();
    if (w.DeleteContext != nullptr) { w.DeleteContext(shared); }
    std::fprintf(stderr,
      "[qar-adapter] *** wglShareLists FAILED (err=%lu) — QAR would not see our "
      "textures.\n", static_cast<unsigned long>(err));
    std::fflush(stderr);
    return nullptr;
  }
  if (adapter_trace_on()) {
    std::fprintf(stderr,
      "[qar-adapter] created GL context for QAR HGLRC=%p (shares Ogre's %p) "
      "[sender #%zu]\n",
      static_cast<void *>(shared), gl_render_context_, gl_sender_contexts_.size() + 1);
    std::fflush(stderr);
  }
  return static_cast<void *>(shared);
#else
  return nullptr;
#endif
}

// --- QarOpenGlContextCallbacks (run on QAR's internal graphics thread) -----------

void * QarCApiSenderFactory::gl_get_proc_address_cb(void * user_state, const char * name)
{
#ifdef _WIN32
  const WglApi & w = wgl();
  if (user_state == nullptr || name == nullptr) { return nullptr; }

  // wglGetProcAddress only works with a context current on THIS thread. QAR is
  // expected to call make_current first, but bind defensively if it did not —
  // otherwise every lookup silently returns null (the glGetStringi failure).
  if (w.GetCurrentContext != nullptr && w.GetCurrentContext() == nullptr) {
    gl_make_current_cb(user_state);
  }

  void * p = nullptr;
  if (w.GetProcAddress_ != nullptr) {
    p = reinterpret_cast<void *>(w.GetProcAddress_(name));
  }
  // wglGetProcAddress returns these sentinels, not just null, on failure — and never
  // resolves the OpenGL 1.1 core functions, which live in opengl32.dll itself.
  if (p == nullptr || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) ||
    p == reinterpret_cast<void *>(3) || p == reinterpret_cast<void *>(-1))
  {
    p = (w.module != nullptr)
      ? reinterpret_cast<void *>(::GetProcAddress(w.module, name)) : nullptr;
  }
  return p;
#else
  (void)user_state; (void)name;
  return nullptr;
#endif
}

bool QarCApiSenderFactory::gl_make_current_cb(void * user_state)
{
#ifdef _WIN32
  auto * state = static_cast<GlSenderContext *>(user_state);
  const WglApi & w = wgl();
  if (state == nullptr || state->factory == nullptr || w.MakeCurrent == nullptr) {
    return false;
  }
  QarCApiSenderFactory * self = state->factory;
  // This sender's OWN context. Falling back to Ogre's would put it current on a
  // second thread, which WGL forbids — better to fail loudly than to corrupt it.
  HGLRC ctx = static_cast<HGLRC>(
    state->context != nullptr ? state->context : self->gl_render_context_);
  if (ctx == nullptr || self->gl_device_context_ == nullptr) { return false; }
  if (w.MakeCurrent(static_cast<HDC>(self->gl_device_context_), ctx) != FALSE) {
    return true;
  }
  // Loud, because the multi-peer failure mode looks exactly like this: a context is
  // already current on another thread, so every later GL lookup returns null and the
  // runtime tears the stream down with an unrelated-sounding message.
  std::fprintf(stderr,
    "[qar-adapter] *** wglMakeCurrent FAILED (err=%lu) HDC=%p HGLRC=%p — is this "
    "context current on another thread?\n",
    static_cast<unsigned long>(GetLastError()), self->gl_device_context_,
    static_cast<void *>(ctx));
  std::fflush(stderr);
  return false;
#else
  (void)user_state;
  return false;
#endif
}

QarOpenGlContextThreadState QarCApiSenderFactory::gl_capture_current_cb(void * user_state)
{
  QarOpenGlContextThreadState state{};
#ifdef _WIN32
  (void)user_state;
  const WglApi & w = wgl();
  if (w.GetCurrentDC != nullptr) {
    state.display_or_device_context = static_cast<void *>(w.GetCurrentDC());
  }
  if (w.GetCurrentContext != nullptr) {
    state.context = static_cast<void *>(w.GetCurrentContext());
  }
  state.has_context = state.context != nullptr;
#else
  (void)user_state;
#endif
  return state;
}

bool QarCApiSenderFactory::gl_restore_current_cb(
  void * user_state, const QarOpenGlContextThreadState * state)
{
#ifdef _WIN32
  (void)user_state;
  const WglApi & w = wgl();
  if (w.MakeCurrent == nullptr || state == nullptr) { return false; }
  if (!state->has_context) { return w.MakeCurrent(nullptr, nullptr) != FALSE; }
  return w.MakeCurrent(
    static_cast<HDC>(state->display_or_device_context),
    static_cast<HGLRC>(state->context)) != FALSE;
#else
  (void)user_state; (void)state;
  return false;
#endif
}

std::shared_ptr<qar_ogre_streaming::IQarRenderSenderAdapter>
QarCApiSenderFactory::create_sender(const QarPeerId & peer_id)
{
  if (!ready_) { return nullptr; }

  // ---------------------------------------------------------------------------
  // ASYNC creation. Creating a sender negotiates a transfer with the
  // receiver and can take arbitrarily long — or never finish: an OpenGL sender
  // against the Vulkan-only mixer parks in "waiting for receiver borrow signal"
  // forever. Because create_sender() is driven from the
  // panel's QTimer tick, the synchronous call would freeze the whole of RViz.
  //
  // So this function never waits. First call for a peer kicks off
  // qar_render_sender_create_async and returns null; later calls poll the result.
  // Returning null means "not yet" — the panel re-queues the peer, so the render
  // tick keeps running at full rate meanwhile.
  for (auto it = pending_senders_.begin(); it != pending_senders_.end(); ++it) {
    std::shared_ptr<PendingSender> p = *it;
    if (!peer_eq(p->peer_id, peer_id)) { continue; }

    if (!p->done.load(std::memory_order_acquire)) {
      // Still in flight. Say so once when it looks stuck, so the next occurrence of
      // the mixer bug is visible in the log instead of being an unexplained blank.
      const double waited = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - p->started).count();
      if (!p->warned && waited > kPendingSenderWarnSeconds) {
        p->warned = true;
        std::fprintf(stderr,
          "[qar-adapter] create_sender: peer %s still creating after %.1f s — the "
          "receiver has not completed transfer negotiation (the render tick keeps running).\n",
          qar_ogre_streaming::peer_id_to_hex(peer_id).c_str(), waited);
        std::fflush(stderr);
      }
      return nullptr;
    }

    QarRenderSender * const stream = p->stream;
    const bool failed = p->failed.load(std::memory_order_acquire);
    pending_senders_.erase(it);
    if (failed || stream == nullptr) {
      std::fprintf(stderr, "[qar-adapter] create_sender: async creation FAILED for peer %s\n",
        qar_ogre_streaming::peer_id_to_hex(peer_id).c_str());
      std::fflush(stderr);
      return nullptr;
    }
    trace_msg("create_sender: <- qar_render_sender_create_async (ready)");
    auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
      stream, config_.near_far, config_.submit_mode, config_.copy_hook, config_.depth_scale,
      pointing_ray_slot_, peer_id, config_.log_folder_path);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      adapters_.push_back(PeerAdapter{peer_id, adapter});
    }
    return adapter;
  }

  trace_peer_id("create_sender", peer_id);
  QarRenderSenderInit init = qar_render_sender_init_default();
  init.peer_id = peer_id;

  // -------------------------------------------------------------------------
  // Vendor-required OpenGL stream extension.
  //
  // THIS is what makes an OpenGL producer work with an OUT-OF-PROCESS consumer.
  // Without it the runtime has no handle on our GL context, rejects every
  // cross-process backend ("backend feature not supported for OpenGL sender
  // transform"), falls back to VulkanGpuTransfer and takes both processes down.
  // With the context chained in, QAR reads our textures in-process on our context
  // and performs the cross-process transfer itself.
  //
  // The context must come from the render thread. create_sender() *is* on that
  // thread (the panel's QTimer tick) but runs BETWEEN Ogre frames, where a context
  // is not necessarily still current — so we use the handles latched by
  // capture_gl_context(), which the panel calls every tick. Try once more here in
  // case this is the very first tick.
  // One struct PER CONTEXT TYPE since the 2026-09-21 SDK: the Egl, Wgl and Glx variants
  // of QarStreamParamsOpenGl replace the single QarStreamParamsOpenGl (and its
  // QarOpenGlContextType discriminator, structure type 0x4001). We own a WGL context,
  // so this is the Wgl one and the context type is the struct's identity.
  QarStreamParamsOpenGlWgl gl_params = qar_stream_params_opengl_wgl_default();
#ifdef _WIN32
  if (capture_gl_context()) {
    // Mint a context for THIS sender. Never Ogre's live one (it is permanently current
    // on the render thread) and never one already handed to another sender: QAR binds
    // it on that sender's own graphics thread, and a WGL context cannot be current on
    // two threads at once. All of them share Ogre's object namespace, so every peer
    // still sees the same textures.
    auto sender_ctx = std::make_unique<GlSenderContext>();
    sender_ctx->factory = this;
    sender_ctx->context = create_shared_gl_context();
    if (sender_ctx->context == nullptr) {
      std::fprintf(stderr,
        "[qar-adapter] create_sender: could not create a GL context for this peer; "
        "refusing rather than sharing another sender's.\n");
      std::fflush(stderr);
      return nullptr;
    }
    GlSenderContext * ctx = sender_ctx.get();
    gl_sender_contexts_.push_back(std::move(sender_ctx));

    // The WGL struct names its fields for WGL: device_context is the HDC, and the
    // EGL/GLX-only draw/read surfaces simply do not exist on it.
    gl_params.device_context = gl_device_context_;
    gl_params.context = ctx->context;
    //
    // The callbacks are what make the WGL case work at all: QAR resolves GL entry
    // points on its internal graphics thread, where wglGetProcAddress returns null
    // unless a context is current there. Without them the runtime fails with
    // "Could not load OpenGL function: glGetStringi" and tears the stream down.
    // They live in a shared QarOpenGlContextCallbacks sub-struct, identical
    // across all three context types. There is no `unbind`: the library
    // restores the thread's previous binding through capture_current /
    // restore_current, and restore_current with has_context == false is
    // what "bind nothing" means.
    gl_params.callbacks.user_state = ctx;
    gl_params.callbacks.get_proc_address = &QarCApiSenderFactory::gl_get_proc_address_cb;
    gl_params.callbacks.make_current = &QarCApiSenderFactory::gl_make_current_cb;
    gl_params.callbacks.capture_current = &QarCApiSenderFactory::gl_capture_current_cb;
    gl_params.callbacks.restore_current = &QarCApiSenderFactory::gl_restore_current_cb;
    init.header.next = &gl_params.header;
    if (adapter_trace_on()) {
      std::fprintf(stderr,
        "[qar-adapter] create_sender: GL HDC=%p ogre_ctx=%p -> qar_ctx=%p (WGL, callbacks on)\n",
        gl_device_context_, gl_render_context_, gl_params.context);
      std::fflush(stderr);
    }
  } else {
    // No GL context has EVER been current on this thread => QAR cannot bind our
    // textures. Fail loudly rather than letting the runtime pick an incompatible
    // transfer backend (that fallback crashes both processes).
    // Returning null here is recoverable: the peer is retried on the next tick.
    std::fprintf(stderr,
      "[qar-adapter] create_sender: no WGL context latched yet - "
      "QarStreamParamsOpenGlWgl not chained; retrying on a later render tick.\n");
    std::fflush(stderr);
    return nullptr;
  }
#endif

  init.enable_auto_reconnects = true;
  // Key this stream to our app volume. REQUIRED since the 2026-09-21 SDK: the
  // field is documented "Required ... NULL is rejected with
  // QAR_STATUS_APP_VOLUME_INVALID_ID", and "a stream without one is not
  // positioned in the scene and no viewer can show it".
  //
  // There is no NULL fallback: the runtime rejects the call outright, so a
  // fallback would not degrade to "works without the mixer", it would degrade to
  // "no stream at all, with a confusing error". Refusing HERE turns it into a
  // legible message on the tick that would have failed, and create_sender is
  // retried every render tick, so a volume that appears later still works.
  if (!has_app_volume_) {
    std::fprintf(stderr,
      "[qar-adapter] create_sender: no app volume, and QarRenderSenderInit::app_volume_id "
      "is REQUIRED since the 2026-09-21 SDK (NULL => QAR_STATUS_APP_VOLUME_INVALID_ID). "
      "%s\n",
      config_.create_app_volume
        ? "Volume creation failed at connect - see the qar_app_volumes_add_volume error above."
        : "Config::create_app_volume is off; turn it on - there is no NULL-volume path.");
    std::fflush(stderr);
    return nullptr;
  }
  init.app_volume_id = &app_volume_;
  if (adapter_trace_on()) {
    std::fprintf(stderr, "[qar-adapter] create_sender: app_volume_id=%s\n",
      app_volume_id_to_hex(app_volume_).c_str());
    std::fflush(stderr);
  }
  // Real enumerator (QAR_GRAPHICS_API_OPENGL = 7) — no cast needed.
  init.graphics_api = QAR_GRAPHICS_API_OPENGL;
  init.color_format = QAR_PIXEL_FORMAT_R8G8B8A8;
  init.depth_format = QAR_PIXEL_FORMAT_R32_FLOAT;
  init.texture_layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;
  init.frame_views[0] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 0};
  init.frame_views[1] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 1};
  init.frame_views[2] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 2};
  init.frame_views[3] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 3};
  init.frame_views_count = 4;

  // Hand the whole init block to the heap: the SDK reads it (and the chained GL
  // params it points at) after this function has returned.
  auto pending = std::make_shared<PendingSender>();
  pending->peer_id = peer_id;
  pending->started = std::chrono::steady_clock::now();
  pending->init = init;
  pending->gl_params = gl_params;
  // Re-chain onto the HEAP copy — `init.header.next` still points at the stack
  // gl_params, which dies with this frame.
  if (init.header.next != nullptr) {
    pending->init.header.next = &pending->gl_params.header;
  }

  trace_msg("create_sender: -> qar_render_sender_create_async");
  const QarResult r = qar_render_sender_create_async(
    session_, &pending->init, &QarCApiSenderFactory::sender_created_trampoline,
    new std::shared_ptr<PendingSender>(pending), nullptr);
  trace_msg("create_sender: <- qar_render_sender_create_async (kicked off)");
  if (qar_result_is_error(r)) {
    std::fprintf(stderr, "[qar-adapter] %s\n",
      report_result("qar_render_sender_create_async", r).c_str());
    std::fflush(stderr);
    return nullptr;
  }
  pending_senders_.push_back(std::move(pending));
  return nullptr;  // not an error: poll again next tick
}

// Runs on a QAR-internal thread. Does the minimum: stash the result and publish it
// with a release store. The render tick picks it up on its next poll, so nothing
// here touches factory state that the GUI thread owns.
void QarCApiSenderFactory::sender_created_trampoline(
  QarResult status, QarRenderSender * stream, void * user_state)
{
  // Ownership of the heap shared_ptr passed to the SDK ends here.
  std::unique_ptr<std::shared_ptr<PendingSender>> holder(
    static_cast<std::shared_ptr<PendingSender> *>(user_state));
  if (!holder || !*holder) { return; }
  PendingSender & p = **holder;

  p.stream = stream;
  p.failed.store(qar_result_is_error(status) || stream == nullptr,
    std::memory_order_relaxed);
  p.done.store(true, std::memory_order_release);
}

std::string QarCApiSenderFactory::invite_one_peer_locked_free(
  PeerKind kind, const std::string & hostname, QarPeerId & out_peer)
{
  if (session_ == nullptr) {
    return "invite_peer: no active session";
  }
  if (peer_kind_needs_hostname(kind) && hostname.empty()) {
    return std::string("invite_peer: ") + to_string(kind) + " needs a device hostname";
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (invited_peers_.size() >= config_.max_peers) {
      return "invite_peer: already at the max_peers ceiling (" +
             std::to_string(config_.max_peers) + ")";
    }
  }
  // One connection-string builder per device type; only HoloLens is addressable, by
  // hostname. Inviting a real device here is what puts it in the allow-list, so the
  // timing heuristic (kInvitedOrNew) is not needed for anything invited this way.
  char conn[1024] = {0};
  QarResult r{};
  switch (kind) {
    case PeerKind::kHoloLens:
      trace_msg("-> qar_session_invite_connection_string_hololens");
      r = qar_session_invite_connection_string_hololens(hostname.c_str(), conn, sizeof(conn));
      break;
    case PeerKind::kQuest:
      trace_msg("-> qar_session_invite_connection_string_quest");
      r = qar_session_invite_connection_string_quest(conn, sizeof(conn));
      break;
    case PeerKind::kZed:
      trace_msg("-> qar_session_invite_connection_string_zed");
      r = qar_session_invite_connection_string_zed(conn, sizeof(conn));
      break;
    case PeerKind::kVisualizer:
    default:
      trace_msg("-> qar_session_invite_connection_string_visualizer");
      r = qar_session_invite_connection_string_visualizer(conn, sizeof(conn));
      if (!qar_result_is_error(r)) {
        // The builder emits a BARE "visualizer:". Every such invite resolves to the
        // same peer, so a second Visualizer cannot be opened. Naming a distinct GUID
        // in the payload slot is the vendor-suggested way to address a distinct
        // instance; QarOS already launches one mixer process per GUID.
        std::string suffix = hostname.empty() ? config_.visualizer_conn_suffix
                                              : hostname;
        if (suffix == "auto") {
          suffix = make_random_uuid();
        }
        if (!suffix.empty()) {
          const std::size_t len = std::strlen(conn);
          if (len + suffix.size() + 1 <= sizeof(conn)) {
            std::memcpy(conn + len, suffix.c_str(), suffix.size() + 1);
          } else {
            return "invite_peer: visualizer connection string too long";
          }
        }
      }
      break;
  }
  if (qar_result_is_error(r)) {
    return report_result("qar_session_invite_connection_string", r);
  }
  if (adapter_trace_on()) {
    // The launcher logs the invite it receives as `hostname='' device=…`, so seeing the
    // string we built is what makes a failed device invite diagnosable at all.
    std::fprintf(stderr, "[qar-adapter] invite kind=%s host='%s' conn='%s'\n",
      to_string(kind), hostname.c_str(), conn);
    std::fflush(stderr);
  }
  QarSessionInvitePeerInit invite_peer = qar_session_invite_peer_init_default();
  invite_peer.connection_string = conn;
  QarPeerId invited = qar_peer_id_default();
  trace_msg("-> qar_session_invite_peer");
  r = qar_session_invite_peer(session_, &invite_peer, &invited, nullptr);
  trace_msg("<- qar_session_invite_peer");
  if (qar_result_is_error(r)) {
    return report_result(
      (std::string("qar_session_invite_peer(") + to_string(kind) + ")").c_str(), r);
  }
  trace_peer_id("invite_peer->invited", invited);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    invited_peers_.push_back(invited);
  }
  out_peer = invited;
  return {};
}

std::string QarCApiSenderFactory::invite_additional_peer(
  PeerKind kind, const std::string & hostname, QarPeerId & out_peer)
{
  if (!ready_) {
    return "invite_peer: factory is not ready";
  }
  const std::string err = invite_one_peer_locked_free(kind, hostname, out_peer);
  if (!err.empty()) {
    return err;
  }
  // Queue it so its sender exists before it connects: the runtime accepts a peer's
  // frames only against a sender built for its app id.
  on_render_request(out_peer);
  return {};
}

std::string QarCApiSenderFactory::invite_additional_visualizer(QarPeerId & out_peer)
{
  return invite_additional_peer(PeerKind::kVisualizer, std::string(), out_peer);
}

std::vector<QarPeerId> QarCApiSenderFactory::invited_peers() const
{
  std::lock_guard<std::mutex> lk(mutex_);
  return invited_peers_;
}

std::size_t QarCApiSenderFactory::admitted_peer_count() const
{
  std::lock_guard<std::mutex> lk(mutex_);
  return admitted_peers_.size();
}

// ===========================================================================
//  Georeferencing the app volume (ROS geolocation -> QarGeoAnchorFrame)
// ===========================================================================

std::string QarCApiSenderFactory::set_geo_anchor_wgs84(
  double latitude_deg, double longitude_deg, double altitude_m)
{
  // The arithmetic lives in geo_anchor.cpp as a free function so it can be unit
  // tested without a session; see qar_qar_adapter/test/geo_anchor_tests.cpp.
  double origin[3], east[3], north[3], up[3];
  if (!geodetic_to_ecef_enu(
      latitude_deg, longitude_deg, altitude_m, origin, east, north, up))
  {
    return "set_geo_anchor: fix is NaN or out of range (receiver has no lock?)";
  }
  // ROS is +X East, +Y North, +Z Up (REP-103), so the app axes map straight on.
  return set_geo_anchor_ecef(origin, east, north, up);
}

std::string QarCApiSenderFactory::set_geo_anchor_ecef(
  const double origin_xyz[3],
  const double axis_x[3], const double axis_y[3], const double axis_z[3])
{
  if (session_ == nullptr) { return "set_geo_anchor: no session"; }
  if (!has_app_volume_) {
    // The anchor is a property OF a volume, so without one there is nothing to
    // georeference. Worth saying plainly: Config::create_app_volume = false silently
    // makes geolocation impossible rather than merely unhelpful.
    return "set_geo_anchor: no app volume (Config::create_app_volume is off?)";
  }

  QarGeoAnchorFrame frame{};
  frame.world_ref_system = QAR_WORLD_REF_SYSTEM_ECEF_WGS84_METERS;
  frame.origin_world = QarGeoVec3d{origin_xyz[0], origin_xyz[1], origin_xyz[2]};
  frame.axis_x_world = QarGeoVec3d{axis_x[0], axis_x[1], axis_x[2]};
  frame.axis_y_world = QarGeoVec3d{axis_y[0], axis_y[1], axis_y[2]};
  frame.axis_z_world = QarGeoVec3d{axis_z[0], axis_z[1], axis_z[2]};
  // ENU (East, North, Up) is right-handed, and so is the ROS frame it comes from.
  frame.handedness = QAR_HANDEDNESS_RIGHT;

  trace_msg("-> qar_app_volumes_change_app_world_anchor");
  const QarResult r =
    qar_app_volumes_change_app_world_anchor(session_, &app_volume_, &frame);
  trace_msg("<- qar_app_volumes_change_app_world_anchor");
  if (qar_result_is_error(r)) {
    has_geo_anchor_ = false;
    return report_result("qar_app_volumes_change_app_world_anchor", r);
  }
  has_geo_anchor_ = true;
  return {};
}

std::string QarCApiSenderFactory::clear_geo_anchor()
{
  if (session_ == nullptr || !has_app_volume_) { return "clear_geo_anchor: no volume"; }
  const QarResult r = qar_app_volumes_clear_app_world_anchor(session_, &app_volume_);
  if (qar_result_is_error(r)) {
    return report_result("qar_app_volumes_clear_app_world_anchor", r);
  }
  has_geo_anchor_ = false;
  return {};
}

bool QarCApiSenderFactory::baseline_frozen_locked()
{
  if (!connected_at_set_) {
    return false;  // session not up yet: everything seen so far is the runtime's own
  }
  const auto elapsed = std::chrono::steady_clock::now() - connected_at_;
  const bool frozen = elapsed >= std::chrono::milliseconds(config_.baseline_settle_ms);
  if (frozen && !baseline_logged_) {
    baseline_logged_ = true;
    std::fprintf(stderr,
      "[qar-adapter] peer baseline frozen: %zu pre-existing peer(s); any peer "
      "appearing from now on is treated as a consumer and admitted\n",
      baseline_peers_.size());
    for (const QarPeerId & p : baseline_peers_) {
      std::fprintf(stderr, "[qar-adapter]    baseline peer %s\n",
        qar_ogre_streaming::peer_id_to_hex(p).c_str());
    }
    std::fflush(stderr);
  }
  return frozen;
}

bool QarCApiSenderFactory::admit_locked(const QarPeerId & peer_id, const char * display_name)
{
  auto contains = [&peer_id](const std::vector<QarPeerId> & v) {
      return std::any_of(v.begin(), v.end(),
               [&peer_id](const QarPeerId & p) { return peer_eq(p, peer_id); });
    };

  // Never stream to ourselves, whatever the policy says.
  if (has_self_peer_ && peer_eq(peer_id, self_peer_)) {
    return false;
  }
  // Already admitted: let it through again (the producer de-dupes by stream) without
  // re-running the policy or re-counting it against max_peers.
  if (contains(admitted_peers_)) {
    return true;
  }

  // With invite_flow_admission off there is no invite flow, so the viewer-driven
  // peers flow through unchanged.
  // NOTE: this is deliberately NOT tied to whether we auto-invited anything. A session
  // that invites nothing at connect still filters, otherwise the runtime's internal
  // peers would all be admitted the moment the subscription opens.
  bool ok = !invite_flow_;
  if (!ok) {
    switch (config_.admission) {
      case PeerAdmission::kAny:
        ok = true;
        break;
      case PeerAdmission::kInvitedOrNamed:
        ok = contains(invited_peers_);
        if (!ok && display_name != nullptr && !config_.admission_name_filter.empty()) {
          ok = icontains(std::string(display_name), config_.admission_name_filter);
        }
        break;
      case PeerAdmission::kInvitedOrNew:
        ok = contains(invited_peers_);
        if (!ok) {
          // Everything visible during the settle window is the runtime's own; anything
          // that turns up later is a consumer that onboarded (a headset, or another
          // machine's Visualizer) and must be streamed to. See the enum's comment for
          // why timing is the only signal the SDK leaves us.
          if (!baseline_frozen_locked()) {
            if (!contains(baseline_peers_)) {
              baseline_peers_.push_back(peer_id);
            }
          } else {
            ok = !contains(baseline_peers_);
          }
        }
        break;
      case PeerAdmission::kInvitedOnly:
      default:
        ok = contains(invited_peers_);
        break;
    }
  }
  if (!ok) {
    return false;
  }

  // Every peer costs two full scene renders per frame; refuse loudly past the ceiling
  // rather than quietly turning the render tick into a slideshow.
  if (admitted_peers_.size() >= config_.max_peers) {
    std::fprintf(stderr,
      "[qar-adapter] *** peer %s refused: already streaming to %zu peers "
      "(max_peers=%u)\n",
      qar_ogre_streaming::peer_id_to_hex(peer_id).c_str(), admitted_peers_.size(),
      config_.max_peers);
    std::fflush(stderr);
    return false;
  }

  admitted_peers_.push_back(peer_id);
  if (adapter_trace_on()) {
    std::fprintf(stderr, "[qar-adapter] ADMITTED peer %s name='%s' (%zu streaming)\n",
      qar_ogre_streaming::peer_id_to_hex(peer_id).c_str(),
      display_name != nullptr ? display_name : "", admitted_peers_.size());
    std::fflush(stderr);
  }
  return true;
}

void QarCApiSenderFactory::on_render_request(const QarPeerId & peer_id, const char * display_name)
{
  std::lock_guard<std::mutex> lk(mutex_);
  // The peer subscription surfaces every peer in the session — the invited
  // Visualizer(s) AND the runtime's internal apps. Creating senders for the internal
  // ones mismatches app ids (the runtime then rejects the Visualizer's frames) and
  // multiplies GUI-thread render work, so admission is filtered. Multi-peer is
  // supported: the allow-list holds every Visualizer we invited, not just the first.
  if (!admit_locked(peer_id, display_name)) {
    return;
  }
  // Peer specs update repeatedly (there are more callbacks than peers), and the
  // producer re-queues a peer whose stream creation must be retried. Keep the queue
  // free of same-tick duplicates; it is drained every render tick anyway.
  for (const QarPeerId & queued : pending_requests_) {
    if (peer_eq(queued, peer_id)) {
      return;
    }
  }
  pending_requests_.push_back(peer_id);
}

void QarCApiSenderFactory::poll_new_peer_requests(std::vector<QarPeerId> & out)
{
  std::lock_guard<std::mutex> lk(mutex_);
  out.insert(out.end(), pending_requests_.begin(), pending_requests_.end());
  pending_requests_.clear();
}

void QarCApiSenderFactory::poll_closed_peers(std::vector<QarPeerId> & out)
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const PeerAdapter & pa : adapters_) {
    auto sp = pa.adapter.lock();
    if (sp && sp->is_closed()) { out.push_back(pa.peer_id); }
  }
}

void QarCApiSenderFactory::forget(const QarPeerId & peer_id)
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = adapters_.begin(); it != adapters_.end(); ++it) {
    if (peer_eq(it->peer_id, peer_id)) { adapters_.erase(it); break; }
  }
  // Free the peer's slot against max_peers, so a client that disconnects and comes
  // back (or a different one taking its place) is admitted again.
  admitted_peers_.erase(
    std::remove_if(admitted_peers_.begin(), admitted_peers_.end(),
      [&peer_id](const QarPeerId & p) { return peer_eq(p, peer_id); }),
    admitted_peers_.end());
}

}  // namespace qar_qar_adapter
