// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"

#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/geo_anchor.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/render_timing.hpp"
#include "qar_ar_core/timing_log.hpp"

#include <algorithm>
#include <map>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>
#include <string>
#include <thread>
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
// the subscription surfaces and which we create the render sender with — the way
// to diagnose a "target app id do not match" rejection.
void trace_peer_id(const char * what, const QarPeerId & id)
{
  if (!environment_is_set("QAR_ADAPTER_TRACE")) { return; }
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
  static const bool on = environment_is_set("QAR_ADAPTER_TRACE");
  return on;
}
/// Trace every frame up to this count, where a startup hang would show up.
constexpr unsigned long kTraceFullFrames = 150;
/// After that, keep one sampled frame per this many as a liveness breadcrumb.
constexpr unsigned long kTraceSampleEvery = 300;

/// Set QAR_ADAPTER_TRACE_ALL to defeat the sampling and trace every frame.
bool adapter_trace_all()
{
  static const bool on = environment_is_set("QAR_ADAPTER_TRACE_ALL");
  return on;
}

// QAR_ADAPTER_PRIME: one tick per frame instead of two (default on).
bool prime_enabled_from_environment()
{
  const std::string value = read_environment("QAR_ADAPTER_PRIME");
  return qar_qar_adapter::parse_bool_env(
    environment_is_set("QAR_ADAPTER_PRIME") ? value.c_str() : nullptr, true);
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

/// A step of the render thread's GL work that takes this long or longer is reported: the render
/// thread can do nothing else meanwhile, and a stall of the GPU shows here first.
constexpr double kSlowGlStepMs = 250.0;

/// Reports the step @p step (copy-into-qar, glFinish, fence or readback-probe) that began at
/// @p started when it took kSlowGlStepMs or longer: the first five of each step, then one in a
/// hundred. @p on_worker names the thread: a frame worker's step is not a stall of the GUI thread.
void note_gl_step(
  const char * step, std::chrono::steady_clock::time_point started, bool on_worker = false)
{
  const double ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - started).count();
  if (ms < kSlowGlStepMs) { return; }
  static std::atomic<unsigned> reported[4];
  const std::size_t slot = std::strcmp(step, "copy-into-qar") == 0 ? 0 :
    std::strcmp(step, "glFinish") == 0 ? 1 : std::strcmp(step, "fence") == 0 ? 3 : 2;
  const unsigned n = ++reported[slot];
  if (n > 5 && (n % 100) != 0) { return; }
  std::fprintf(
    stderr, "[qar-gl-call] %s WARNING %s took %.0f ms on the %s (limit %.0f ms)\n",
    wall_clock_hms_ms().c_str(), step, ms, on_worker ? "frame worker" : "GUI thread",
    kSlowGlStepMs);
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

/// gl_finish_current_thread(), timed.
void gl_finish_timed()
{
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  gl_finish_current_thread();
  note_gl_step("glFinish", started);
}
#endif  // _WIN32


bool peer_eq(const QarPeerId & a, const QarPeerId & b)
{
  return std::memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

}  // namespace

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
    environment_is_set("QAR_ADAPTER_FRAME_TRACE") ? "1" : nullptr);
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
  std::string tmp = read_environment("TEMP");
  if (tmp.empty()) { tmp = read_environment("TMP"); }
  return !tmp.empty() ? (tmp + "\\quaternar") : std::string("quaternar");
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
    std::error_code folder_error;
    std::filesystem::create_directories(std::filesystem::u8path(folder), folder_error);
    path_ = folder + "\\qar_frame_trace_" + timestamp_yyyyMMdd_HHmmss() + "_" +
      make_stream_tag(peer_id) + ".csv";
#ifdef _WIN32
    if (_wfopen_s(&file_, std::filesystem::u8path(path_).c_str(), L"w") != 0) { file_ = nullptr; }
#else
    file_ = std::fopen(path_.c_str(), "w");
#endif
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
      const QarResult rl = QAR_CALL(qar_render_frame_info_get_view_pose, handle, 0, &wait_.a_left);
      const QarResult rr = QAR_CALL(qar_render_frame_info_get_view_pose, handle, 1, &wait_.a_right);
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
      QAR_CALL(qar_render_frame_info_handle_destroy, row_.handle);
      row_.handle = nullptr;
    }
    row_.valid = false;
  }

  /// @brief Hands the handle of an in-flight row to the caller, who destroys it, and closes the
  /// row without a CSV line. Null when no row is open.
  QarRenderFrameInfo * take_row_handle()
  {
    QarRenderFrameInfo * const handle = row_.handle;
    row_.handle = nullptr;
    row_.valid = false;
    return handle;
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
    const QarResult rl = QAR_CALL(qar_render_frame_info_get_view_pose, row_.handle, 0, &c_left);
    const QarResult rr = QAR_CALL(qar_render_frame_info_get_view_pose, row_.handle, 1, &c_right);
    row_.have_c = qar_result_is_success(rl) && qar_result_is_success(rr);
    if (row_.have_c) {
      row_.c_left = c_left;
      row_.c_right = c_right;
    } else {
      std::fprintf(stderr, "[qar-adapter] frame trace: (c) pose re-read failed\n");
      std::fflush(stderr);
    }
    QAR_CALL(qar_render_frame_info_handle_destroy, row_.handle);
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

namespace
{

void publish_ray(
  qar_ar_core::PointingRaySlot & slot, QarRenderSender * stream, const QarPose & eye_pose,
  FrameBreaker & breaker, bool & logged_hands_tracked, bool & logged_hands_missing)
{
  // Drain the device's hand stream. Documented as "returns the last sample, or
  // both hands untracked when nothing arrived since the previous call" -- so a
  // failure and an empty drain are the same thing to us, an invalid ray, and
  // publishing that invalid ray is how "the hands went away" reaches the
  // picker instead of a stale aim.
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  const std::chrono::steady_clock::time_point hands_start = std::chrono::steady_clock::now();
  const QarResult hands_result = QAR_CALL(qar_render_sender_last_hands, stream, &hands);
  breaker.note("last_hands", hands_start);

  qar_ar_core::PointingRayInputs inputs;
  if (!qar_result_is_error(hands_result)) {
    inputs = qar_ar_core::pointing_ray_inputs_from_hands(
      hands, QAR_HANDEDNESS_RIGHT, qar_ar_core::HandRayJoint::IndexTip);
  }
  inputs.has_eye = true;
  inputs.eye_position = eye_pose.position;
  inputs.timestamp_s = static_cast<double>(hands.device_timestamp.unix_ns) * 1e-9;

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
  slot.publish(ray, probe);

  // One line each way, ever. The question the log has to answer is simply
  // "does this device send hands?", and that is a one-shot fact.
  if (ray.valid && !logged_hands_tracked) {
    logged_hands_tracked = true;
    std::fprintf(
      stderr,
      "[qar-adapter] hands: the device IS sending hand joints - AR pointing ray is live "
      "(%s, %s of the %s hand). origin=(%.3f, %.3f, %.3f) dir=(%.3f, %.3f, %.3f) room\n",
      qar_ar_core::to_string(ray.kind), qar_ar_core::to_string(ray.joint),
      ray.hand == QAR_HANDEDNESS_LEFT ? "left" : "right",
      static_cast<double>(ray.origin.x), static_cast<double>(ray.origin.y),
      static_cast<double>(ray.origin.z), static_cast<double>(ray.direction.x),
      static_cast<double>(ray.direction.y), static_cast<double>(ray.direction.z));
    std::fflush(stderr);
  } else if (!ray.valid && !logged_hands_missing) {
    logged_hands_missing = true;
    std::fprintf(
      stderr,
      "[qar-adapter] hands: no AR pointing ray yet - %s (qar_render_sender_last_hands "
      "code=%d). Plane pick targets fall back to projecting the reported action "
      "point onto the plane.\n",
      ray.error.c_str(), static_cast<int>(hands_result.code));
    std::fflush(stderr);
  }
}

}  // namespace

// ===========================================================================
//  The frame calls on a worker thread
// ===========================================================================
//
// The state that the render thread and the worker thread of one sender share. The worker is the
// only thread that makes a QarOS frame call of this sender; the render thread reads and writes
// the slot under `m`, which no thread holds across a QarOS call. The slot goes
//
//   kIdle -> kOpen       the worker reserved a frame and published its poses and textures
//   kOpen -> kRendering  the render thread took it (begin_frame)
//   kRendering -> kOpen  the render thread did not submit it (a render error, a frame the
//                        scheduler shed): the next begin_frame offers the same frame again
//   kRendering -> kShowing  the render thread rendered, copied and finished the GL work, and
//                        handed the frame over (submit_opengl_stereo_frame)
//   kShowing -> kIdle    the worker showed it
//
// A frame cycle lasts from a successful try-begin to its show-frame; the QarOS header has no call
// that ends one in any other way, and it does not say what a second try-begin inside an open cycle
// does. So the worker calls try-begin only while the slot is idle, and every frame it began ends
// in a show-frame: the rendered one, or, when the frame cannot be rendered (a view getter failed,
// the textures could not be read), one of whatever the textures hold.
// A paused or blocked worker leaves the slot as it is; the render thread never waits for it.
struct RenderFrameCore
{
  enum class Slot { kIdle, kOpen, kRendering, kShowing };

  /// One frame the worker reserved.
  struct Cycle
  {
    QarPose left_pose{};
    QarFov left_fov{};
    QarPose right_pose{};
    QarFov right_fov{};
    /// The frame as the runtime filled it: the ids of its destination textures.
    QarVideoFrameOpenGl gl{};
    /// The runtime gave no textures (the layout of the frames was not known yet): the render
    /// thread learns the layout from its frame and drops the cycle.
    bool no_ids = false;
    std::uint64_t frame_index = 0;
    /// The frame follows a stall of the frames shown; see FrameStallDetector.
    bool stalled = false;
    StallReport stall;
    /// The fence behind the copy into the textures of the runtime; the worker waits for it before
    /// it shows the frame. Null: the render thread waited for the GPU itself.
    void * fence_sync = nullptr;
  };

  // Fixed before the worker starts.
  std::string peer_label;
  QarNearFar near_far{};
  float depth_scale = 1.0f;
  std::shared_ptr<qar_ar_core::PointingRaySlot> ray_slot;
  std::shared_ptr<IReleaseLane> lane;
  std::shared_ptr<std::atomic<bool>> context_released;
  std::shared_ptr<FrameBreaker> breaker;
  std::string trace_path;
  /// The hook that reads the colour back after a stall when the cycle carries a fence.
  std::shared_ptr<IQarGlTextureCopyHook> copy_hook;

  // Shared, guarded by `m`.
  std::mutex m;
  Slot slot = Slot::kIdle;
  Cycle cycle;
  /// The textures the runtime fills are described by `layout`: the layout the sender settled on
  /// (read before the first frame), then the layout of the last frame the render thread submitted.
  bool layout_known = false;
  QarVideoFrameOpenGl layout{};
  /// Why the last poll reserved no frame, when it is more than "not ready yet". An error stays
  /// until a frame is published.
  std::string detail;
  bool detail_is_error = false;
  bool detail_reported = false;
  /// The result of the last show that failed; empty after a show that succeeded.
  std::string show_failure;
  /// Our frame of the cycle that is being shown, kept for the read-back after a stall when that
  /// read-back runs on the worker (the cycle carries a fence).
  bool stall_frame_valid = false;
  qar_ogre_streaming::QarOpenGlStereoFrame stall_frame;

  // The GL fence the render thread ends its copy with, and the worker waits for. The setup is
  // fixed before the worker starts; the worker binds its context before the first poll.
  FrameFenceSetup fence;
  /// 0 not in use (no setup, or not bound yet), 1 the worker bound its context, 2 the bind failed.
  std::atomic<int> fence_state{0};
  /// The worker is waiting for the GPU for the notice time or longer.
  std::atomic<bool> gpu_waiting{false};

  // Read by the render thread without the lock.
  std::atomic<bool> closed{false};
  std::atomic<bool> leaving{false};
  std::atomic<bool> prime{true};
  std::atomic<double> tick_ms{0.1};
  std::atomic<std::uint64_t> trace_rows{0};
  std::atomic<std::uint64_t> trace_changed_rows{0};

  // The worker's alone.
  QarRenderSender * stream = nullptr;
  std::uint64_t frame_index = 0;
  FrameStallDetector stall_detector;
  LogOnce first_show_noted;
  bool logged_hands_tracked = false;
  bool logged_hands_missing = false;
  std::unique_ptr<FrameTraceState> trace;
  std::unique_ptr<RenderTimingState> timing;
  std::uint64_t error_count = 0;
  /// Does the runtime ask this sender for frames; see RenderRecycleWatch.
  RenderRecycleWatch recycle_watch;
  /// Set by the worker when it closed the sender for want of render requests.
  std::atomic<bool> recycled{false};

  /// The last holder of the state may be any thread, the render thread among them. The handle the
  /// trace kept for a frame the worker never finished is left to the runtime: destroying it here
  /// would be a QarOS call on whichever thread drops the last reference.
  ~RenderFrameCore()
  {
    if (trace) { (void)trace->take_row_handle(); }
  }
};

namespace
{

/// The one line about the first frame shown: the thread that showed it and its texture ids.
void print_first_show_line(const std::string & peer_label, const QarVideoFrameOpenGl & gl)
{
  // The texture ids as the frame was shown with them: in render-into mode the runtime's own
  // destination textures, which is what the receiving side reads.
  SenderTextureIds ids;
  const std::size_t count = gl.textures_count;
  ids.colour[0] = count > 0 ? gl.textures[0].texture : 0u;
  ids.colour[1] = count > 1 ? gl.textures[1].texture : 0u;
  ids.depth[0] = count > 2 ? gl.textures[2].texture : 0u;
  ids.depth[1] = count > 3 ? gl.textures[3].texture : 0u;
  if (count > 0) {
    ids.width = gl.textures[0].size.width;
    ids.height = gl.textures[0].size.height;
  }
  std::fprintf(
    stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
    format_first_show_line(peer_label, thread_number(), ids).c_str());
  std::fflush(stderr);
}

void set_worker_detail(RenderFrameCore & c, std::string text, bool error)
{
  // The owner stopped waiting for this worker: what its calls answer from now on is no answer.
  if (frame_worker_calls_blocked()) { return; }
  if (error) {
    const std::uint64_t count = ++c.error_count;
    if (count <= 5 || (count % 100) == 0) {
      std::fprintf(
        stderr, "[qar-adapter] %s peer %s frame call failed (%llu): %s\n",
        wall_clock_hms_ms().c_str(), c.peer_label.c_str(),
        static_cast<unsigned long long>(count), text.c_str());
      std::fflush(stderr);
    }
  }
  std::lock_guard<std::mutex> lock(c.m);
  // A poll that merely finds no frame ready does not wipe the last error: it stays the skip
  // detail until a frame is published.
  if (!error && text.empty() && c.detail_is_error) { return; }
  c.detail = std::move(text);
  c.detail_is_error = error;
  c.detail_reported = false;
}

void destroy_frame_info(QarRenderFrameInfo * info)
{
  if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
}

constexpr const char * kFrameCallsStopped = "frame calls stopped: a frame call blocked";

/// The one line about a sender that the runtime asks for no frames and that is made again.
void print_recycle_line(
  const std::string & peer_label, const RenderRecycleWatch & watch,
  std::chrono::steady_clock::time_point now)
{
  std::fprintf(
    stderr,
    "[qar-adapter] %s peer %s no render request for %lld s after the stream reconnected "
    "(the runtime answers %d); the 3D sender is made again\n",
    wall_clock_hms_ms().c_str(), peer_label.c_str(), watch.seconds_without_frame(now),
    watch.last_code());
  std::fflush(stderr);
}

/// What a failed try-begin answers, for the judgement of RenderRecycleWatch.
RenderRecycleWatch::Answer recycle_answer_of(const QarResult & r)
{
  if (qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING)) {
    return RenderRecycleWatch::Answer::kReconnecting;
  }
  if (qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED) ||
    qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED))
  {
    return RenderRecycleWatch::Answer::kNotYet;
  }
  return RenderRecycleWatch::Answer::kError;
}

/// Worker thread: ends a frame cycle that was begun and cannot be rendered. The only call that
/// ends a cycle is show-frame, and a cycle left open is not a state the QarOS header describes,
/// so the frame is shown with whatever its textures hold. The call is made also while the breaker
/// pauses the frame calls: the pause holds back the next try-begin, not the end of this cycle.
void end_unrendered_cycle(RenderFrameCore & c, std::string detail, bool error)
{
  if (c.trace) { c.trace->abandon_row(); }
  QarRenderFrameShow show = qar_render_frame_show_default();
  show.rendered_near_far = c.near_far;
  show.depth_scale = c.depth_scale;
  const std::chrono::steady_clock::time_point show_start = std::chrono::steady_clock::now();
  const QarResult r = QAR_CALL(qar_render_sender_show_frame, c.stream, &show);
  c.breaker->note("show_frame", show_start);
  {
    std::lock_guard<std::mutex> lock(c.m);
    c.slot = RenderFrameCore::Slot::kIdle;
  }
  if (qar_result_is_error(r)) {
    set_worker_detail(c, result_text("show_frame", r), true);
  } else {
    set_worker_detail(c, std::move(detail), error);
  }
}

/// Worker thread, before the first frame: the layout the sender settled on describes the textures
/// the runtime fills, so the first frame already has its texture ids.
void learn_sender_layout(RenderFrameCore & c)
{
  QarVideoFrameLayout sender_layout{};
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const QarResult r = QAR_CALL(qar_render_sender_layout, c.stream, &sender_layout);
  c.breaker->note("sender_layout", start);
  if (qar_result_is_error(r) || sender_layout.textures_count == 0 ||
    sender_layout.textures_count > QAR_MAX_FRAME_TEXTURES ||
    sender_layout.views_count > QAR_MAX_FRAME_VIEWS)
  {
    return;
  }
  QarVideoFrameOpenGl gl{};
  gl.textures_count = sender_layout.textures_count;
  for (std::size_t i = 0; i < sender_layout.textures_count; ++i) {
    gl.textures[i].size = sender_layout.textures[i];
    gl.textures[i].texture = 0u;
  }
  gl.texture_views_count = sender_layout.views_count;
  for (std::size_t i = 0; i < sender_layout.views_count; ++i) {
    gl.texture_views[i] = sender_layout.views[i];
  }
  std::lock_guard<std::mutex> lock(c.m);
  if (!c.layout_known) {
    c.layout = gl;
    c.layout_known = true;
  }
}

/// Worker thread, before its first poll: binds the context of the fence setup for good. A bind
/// that fails leaves the sender to wait for the GPU on the render thread, and says so.
void bind_fence_context_once(RenderFrameCore & c)
{
  if (c.fence_state.load() != 0 || !c.fence.usable()) { return; }
  const int err = c.fence.context->bind();
  if (err == 0) {
    c.fence_state.store(1);
    return;
  }
  c.fence_state.store(2);
  std::fprintf(
    stderr,
    "[qar-adapter] %s peer %s the frame worker could not bind its GL context (err=%d); this "
    "sender waits for the GPU on the GUI thread\n",
    wall_clock_hms_ms().c_str(), c.peer_label.c_str(), err);
  std::fflush(stderr);
}

/// Worker thread: waits until the GPU has finished the copy of @p cycle. The wait is made in
/// slices, between which the stop flag is looked at. After the notice time it is reported once and
/// the sender's skip detail names the frame and the time.
/// @return true when the fence is signalled (or cannot be waited for), false when the sender is
/// leaving: the frame is then not shown and the fence is not deleted.
bool wait_for_gpu(RenderFrameCore & c, const RenderFrameCore::Cycle & cycle)
{
  const GlFenceOps & ops = c.fence.ops;
  const std::uint64_t slice_ns = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(c.fence.timings.slice).count());
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  bool noticed = false;
  for (;;) {
    if (c.leaving.load() || frame_worker_calls_blocked()) {
      c.gpu_waiting.store(false);
      // The fence is this sender's: it goes with the sender, on the thread whose context is bound.
      ops.remove(cycle.fence_sync);
      return false;
    }
    const int waited_result = ops.wait(cycle.fence_sync, slice_ns);
    if (waited_result > 0) {
      break;
    }
    if (waited_result < 0) {
      // The wait itself failed (for example a fence of another share group): it cannot be
      // repeated. The frame is shown without the guarantee, and the next ones are finished by the
      // GUI thread.
      c.fence_state.store(2);
      std::fprintf(
        stderr,
        "[qar-adapter] %s peer %s the frame worker could not wait for its fence; this sender "
        "waits for the GPU on the GUI thread\n",
        wall_clock_hms_ms().c_str(), c.peer_label.c_str());
      std::fflush(stderr);
      break;
    }
    const std::chrono::steady_clock::duration waited = std::chrono::steady_clock::now() - started;
    if (waited >= c.fence.timings.notice) {
      const double seconds = std::chrono::duration<double>(waited).count();
      char text[96];
      std::snprintf(
        text, sizeof(text), "waiting for the GPU: frame %llu for %.1f s",
        static_cast<unsigned long long>(cycle.frame_index), seconds);
      if (!noticed) {
        noticed = true;
        c.gpu_waiting.store(true);
        std::fprintf(
          stderr,
          "[qar-adapter] %s peer %s frame worker waiting for the GPU (frame %llu) for %.1f s\n",
          wall_clock_hms_ms().c_str(), c.peer_label.c_str(),
          static_cast<unsigned long long>(cycle.frame_index), seconds);
        std::fflush(stderr);
      }
      set_worker_detail(c, text, false);
    }
  }
  ops.remove(cycle.fence_sync);
  if (noticed) {
    c.gpu_waiting.store(false);
    std::fprintf(
      stderr, "[qar-adapter] %s peer %s the GPU finished frame %llu after %.1f s\n",
      wall_clock_hms_ms().c_str(), c.peer_label.c_str(),
      static_cast<unsigned long long>(cycle.frame_index),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    std::fflush(stderr);
    set_worker_detail(c, std::string(), false);
  }
  return true;
}

/// Worker thread: reserves the next frame when the slot is idle. Try-begin and, when a frame is
/// ready, its poses, fields of view and hands, then the textures of the runtime; the result is
/// published in the slot. A frame that is begun is always either published or ended with a
/// show-frame; a second try-begin never follows before that.
void worker_poll(RenderFrameCore & c, bool from_prime)
{
  // The first poll runs before any frame can be handed over, so the render thread knows how its
  // sender waits for the GPU before its first submit.
  bind_fence_context_once(c);
  {
    std::lock_guard<std::mutex> lock(c.m);
    if (c.slot != RenderFrameCore::Slot::kIdle) { return; }
  }
  if (c.leaving.load() || c.stream == nullptr) { return; }
  if (c.breaker->blocked()) {
    set_worker_detail(c, kFrameCallsStopped, false);
    return;
  }

  QarRenderFrameInfo * info = nullptr;
  bool ready = false;
  if (c.trace) { c.trace->note_try_begin_frame_call(from_prime); }
  const std::int64_t poll_start_us = c.timing ? qar_ar_core::timing_epoch_us() : 0;
  const std::chrono::steady_clock::time_point call_start = std::chrono::steady_clock::now();
  const QarResult r = QAR_CALL(qar_render_sender_try_begin_frame, c.stream, &info, &ready);
  c.breaker->note("try_begin_frame", call_start);
  if (c.timing) { c.timing->note_poll_call(poll_start_us); }
  const bool began = qar_result_is_success(r) && ready && info != nullptr;
  if (c.breaker->tripped() && !began) {
    destroy_frame_info(info);
    set_worker_detail(c, kFrameCallsStopped, false);
    return;
  }
  // A sender that the runtime asks for no frames is closed, and the producer makes a new one.
  {
    const std::chrono::steady_clock::time_point answered = std::chrono::steady_clock::now();
    const RenderRecycleWatch::Answer answer = qar_result_is_error(r)
      ? recycle_answer_of(r)
      : (began ? RenderRecycleWatch::Answer::kBegan : RenderRecycleWatch::Answer::kNotReady);
    if (!c.closed.load() &&
      c.recycle_watch.note(answer, static_cast<int>(r.code), answered))
    {
      print_recycle_line(c.peer_label, c.recycle_watch, answered);
      c.recycled.store(true);
      c.closed.store(true);
    }
  }
  if (qar_result_is_error(r)) {
    destroy_frame_info(info);
    // Two answers mean "not yet", not a failure of the sender: the app volume is not resolved in
    // the Hub yet, or the receiver paused the stream because its display is not visible.
    if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED)) {
      set_worker_detail(c, "app volume not resolved yet", false);
    } else if (qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED)) {
      set_worker_detail(c, "paused by the receiver (its display is not visible)", false);
    } else if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_STREAM_IS_CLOSED)) {
      c.closed.store(true);
      set_worker_detail(c, "try_begin_frame: producer stream is closed", true);
    } else {
      set_worker_detail(c, result_text("try_begin_frame", r), true);
    }
    return;
  }
  if (c.timing) {
    if (began) { c.timing->note_ready(from_prime); }
    else { c.timing->note_pending(); }
  }
  if (!began) {
    destroy_frame_info(info);
    set_worker_detail(c, std::string(), false);
    return;
  }
  // From here on a frame cycle is open.
  if (c.trace) { c.trace->note_ready(/*is_prime_call=*/false, info); }
  if (c.breaker->tripped()) {
    destroy_frame_info(info);
    end_unrendered_cycle(c, kFrameCallsStopped, false);
    return;
  }

  // A frame whose pose or field of view cannot be read is not rendered: what a failed getter
  // leaves behind is a zero quaternion, which is no orientation.
  RenderFrameCore::Cycle cycle;
  struct ViewRead { const char * what; QarResult result; };
  const ViewRead view_reads[] = {
    {"qar_render_frame_info_get_view_pose(0)",
      QAR_CALL(qar_render_frame_info_get_view_pose, info, 0, &cycle.left_pose)},
    {"qar_render_frame_info_get_view_fov(0)",
      QAR_CALL(qar_render_frame_info_get_view_fov, info, 0, &cycle.left_fov)},
    {"qar_render_frame_info_get_view_pose(1)",
      QAR_CALL(qar_render_frame_info_get_view_pose, info, 1, &cycle.right_pose)},
    {"qar_render_frame_info_get_view_fov(1)",
      QAR_CALL(qar_render_frame_info_get_view_fov, info, 1, &cycle.right_fov)},
  };
  for (const ViewRead & read : view_reads) {
    if (qar_result_is_error(read.result)) {
      destroy_frame_info(info);
      end_unrendered_cycle(c, result_text(read.what, read.result), true);
      return;
    }
  }

  // The AR pointing ray, while both halves are in hand.
  if (c.ray_slot) {
    publish_ray(
      *c.ray_slot, c.stream, cycle.left_pose, *c.breaker, c.logged_hands_tracked,
      c.logged_hands_missing);
    if (c.breaker->tripped()) {
      destroy_frame_info(info);
      end_unrendered_cycle(c, kFrameCallsStopped, false);
      return;
    }
  }

  c.stall_detector.frame_ready(
    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),
    StallPoint{cycle.left_pose.position.x, cycle.left_pose.position.y,
      cycle.left_pose.position.z},
    StallPoint{cycle.right_pose.position.x, cycle.right_pose.position.y,
      cycle.right_pose.position.z});
  cycle.frame_index = c.frame_index++;
  if (c.timing) { c.timing->note_consume(cycle.frame_index); }
  if (c.trace) {
    // The trace keeps the handle until just before the frame is shown (the pose re-read).
    c.trace->begin_row(
      cycle.frame_index, c.prime.load(), c.tick_ms.load(), cycle.left_pose, cycle.right_pose,
      info);
  } else {
    destroy_frame_info(info);
  }

  // The textures of the runtime for this frame, described by the layout the sender settled on
  // (read before the first frame) or, later, by the layout of the last frame the render thread
  // submitted.
  bool layout_known = false;
  {
    std::lock_guard<std::mutex> lock(c.m);
    layout_known = c.layout_known;
  }
  if (!layout_known) {
    learn_sender_layout(c);
    if (c.breaker->tripped()) {
      end_unrendered_cycle(c, kFrameCallsStopped, false);
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(c.m);
    layout_known = c.layout_known;
    cycle.gl = c.layout;
  }
  const std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();
  const QarResult fr = QAR_CALL(qar_render_sender_frame_opengl, c.stream, &cycle.gl);
  c.breaker->note("frame_opengl", frame_start);
  if (c.breaker->tripped()) {
    end_unrendered_cycle(c, kFrameCallsStopped, false);
    return;
  }
  if (qar_result_is_error(fr)) {
    if (layout_known) {
      end_unrendered_cycle(c, result_text("frame_opengl", fr), true);
      return;
    }
    cycle.gl = QarVideoFrameOpenGl{};
    cycle.no_ids = true;
  } else if (!layout_known && cycle.gl.textures_count == 0) {
    cycle.no_ids = true;
  }
  cycle.stalled = c.stall_detector.take(cycle.stall);

  std::lock_guard<std::mutex> lock(c.m);
  c.cycle = std::move(cycle);
  c.slot = RenderFrameCore::Slot::kOpen;
  c.detail.clear();
  c.detail_is_error = false;
}

/// Worker thread: shows the frame the render thread handed over, then reserves the next one.
void worker_show(RenderFrameCore & c)
{
  RenderFrameCore::Cycle cycle;
  {
    std::lock_guard<std::mutex> lock(c.m);
    if (c.slot != RenderFrameCore::Slot::kShowing) { return; }
    cycle = c.cycle;
  }
  if (c.stream == nullptr) {
    if (cycle.fence_sync != nullptr) { c.fence.ops.remove(cycle.fence_sync); }
    std::lock_guard<std::mutex> lock(c.m);
    c.slot = RenderFrameCore::Slot::kIdle;
    return;
  }
  // The copy into the textures of the runtime ends with a fence: the frame is shown when the GPU
  // has finished it. A sender that is told to leave while it waits shows nothing; the destroy of
  // its stream ends the cycle.
  if (cycle.fence_sync != nullptr && !wait_for_gpu(c, cycle)) {
    return;
  }

  bool show_ok = false;
  QarResult shown{};
  {
    SubmitTimingGuard submit_timing(c.timing.get());
    if (c.trace) {
      c.trace->note_render_done();
      // The pose re-read, right before show_frame, while the frame's handle is still alive.
      c.trace->note_pre_show();
    }
    QarRenderFrameShow show = qar_render_frame_show_default();
    show.rendered_near_far = c.near_far;
    // Metric metres by default; scaled to whatever encoding the consumer expects.
    show.depth_scale = c.depth_scale;
    const std::chrono::steady_clock::time_point show_start = std::chrono::steady_clock::now();
    shown = QAR_CALL(qar_render_sender_show_frame, c.stream, &show);
    c.breaker->note("show_frame", show_start);
    show_ok = qar_result_is_success(shown);
    submit_timing.show_returned(show_ok);
    if (c.trace) {
      c.trace->note_show_done(show_ok);
      c.trace_rows.store(c.trace->rows_written());
      c.trace_changed_rows.store(c.trace->changed_rows());
    }
  }
  if (show_ok) {
    if (c.first_show_noted.first()) { print_first_show_line(c.peer_label, cycle.gl); }
    c.stall_detector.frame_shown(
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  // The first frame after a stall: its colour is read back from our own texture. With a fence
  // the render thread leaves the read-back to this thread, whose context is bound; the GPU has
  // finished the frame, and the next one is not rendered before the slot is idle.
  if (cycle.fence_sync != nullptr && cycle.stalled) {
    qar_ogre_streaming::QarOpenGlStereoFrame frame;
    bool have_frame = false;
    {
      std::lock_guard<std::mutex> lock(c.m);
      have_frame = c.stall_frame_valid;
      if (have_frame) {
        frame = c.stall_frame;
        c.stall_frame_valid = false;
      }
    }
    if (have_frame) {
      float rgb[3] = {0.0f, 0.0f, 0.0f};
      const std::chrono::steady_clock::time_point probe_start = std::chrono::steady_clock::now();
      const bool have_colour = c.copy_hook && c.copy_hook->mean_left_colour(frame, rgb);
      note_gl_step("readback-probe", probe_start, /*on_worker=*/true);
      std::fprintf(
        stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
        format_stall_line(cycle.stall, have_colour, rgb).c_str());
      std::fflush(stderr);
    }
  }
  {
    std::lock_guard<std::mutex> lock(c.m);
    c.slot = RenderFrameCore::Slot::kIdle;
    if (!frame_worker_calls_blocked()) {
      c.show_failure = show_ok ? std::string() : result_text("show_frame", shown);
    }
  }
  // The frame is out; start the wait for the next one at once so that it runs during the gap
  // before the next tick (QAR_ADAPTER_PRIME).
  if (show_ok && c.prime.load() && !c.leaving.load() && !c.breaker->tripped()) {
    worker_poll(c, /*from_prime=*/true);
  }
}

/// Worker thread, once, when the sender is released: the handles go to the lane, which destroys
/// them on the thread that owns the session. A frame cycle that is still open ends with the
/// destroy of the stream; a show-frame on a stream that is going away would only be one more
/// call that may wait.
void worker_leave(RenderFrameCore & c)
{
  // The names of the textures of this stream are free for the next one.
  if (c.copy_hook) { c.copy_hook->forget_texture_targets(); }
  QarRenderFrameInfo * const traced = c.trace ? c.trace->take_row_handle() : nullptr;
  QarRenderSender * const stream = c.stream;
  c.stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(c.m);
    c.slot = RenderFrameCore::Slot::kIdle;
  }
  // The fence context goes before the flag of the stream is set: the context is current on this
  // thread alone, so it is released and deleted here and nowhere else.
  if (c.fence.context != nullptr) {
    c.fence.context->release_and_delete();
  }
  if (traced == nullptr && stream == nullptr) { return; }
  const std::shared_ptr<std::atomic<bool>> released = c.context_released;
  const std::string label = c.peer_label;
  const auto destroy = [traced, stream, released, label] {
      if (traced != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, traced); }
      if (stream != nullptr) {
        const auto started = std::chrono::steady_clock::now();
        QAR_CALL(qar_render_stream_handle_destroy, stream);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
        std::fprintf(
          stderr, "[qar-adapter] %s peer %s 3D stream released (qar_render_stream_handle_destroy "
          "took %lld ms)\n", wall_clock_hms_ms().c_str(), label.c_str(),
          static_cast<long long>(took));
        std::fflush(stderr);
      }
      // The stream that used the GL context of this sender is gone.
      if (released != nullptr) { released->store(true); }
    };
  if (!c.lane->post_release("render-stream-release", destroy)) {
    std::fprintf(
      stderr, "[qar-adapter] %s peer %s stream handle left unreleased (the owner thread is gone)\n",
      wall_clock_hms_ms().c_str(), c.peer_label.c_str());
    std::fflush(stderr);
  }
}

}  // namespace

bool QarCApiRenderSenderAdapter::begin_frame_on_worker(
  qar_ogre_streaming::QarFrameInfo & out_info)
{
  out_info = qar_ogre_streaming::QarFrameInfo{};
  out_info.should_render = false;
  last_skip_detail_.clear();
  RenderFrameCore & c = *core_;

  // The worker is inside a QarOS call that does not return: say which, and ask for nothing.
  FrameWorker::Wait waited;
  if (worker_->waiting(&waited)) {
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", waited.seconds);
    last_skip_detail_ = "waiting for QarOS: " + waited.call + " for " + seconds + " s";
    return true;
  }
  if (breaker_->tripped()) {
    // The worker looks at the breaker when it is kicked, and lets the probe frame through once the
    // pause is over.
    worker_->kick();
    last_skip_detail_ = kFrameCallsStopped;
    return true;
  }

  bool kick = false;
  bool report_error = false;
  std::string detail;
  {
    std::lock_guard<std::mutex> lock(c.m);
    switch (c.slot) {
      case RenderFrameCore::Slot::kOpen:
      case RenderFrameCore::Slot::kRendering:
        {
          // A frame taken earlier and never submitted (a render error, a frame the scheduler shed
          // after it had begun) is offered again: its cycle is still open in QarOS, and the
          // worker reserves no other before it is shown.
          const RenderFrameCore::Cycle & cycle = c.cycle;
          out_info.frame_index = cycle.frame_index;
          out_info.predicted_display_time = QarTimePoint{static_cast<std::int64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())};
          out_info.left_eye.pose = cycle.left_pose;
          out_info.left_eye.fov = cycle.left_fov;
          out_info.left_eye.near_far = near_far_;
          out_info.right_eye.pose = cycle.right_pose;
          out_info.right_eye.fov = cycle.right_fov;
          out_info.right_eye.near_far = near_far_;
          out_info.should_render = true;
          c.slot = RenderFrameCore::Slot::kRendering;
          consecutive_pending_ = 0;
          return true;
        }
      case RenderFrameCore::Slot::kIdle:
        kick = true;
        break;
      case RenderFrameCore::Slot::kShowing:
        break;
    }
    detail = c.detail.empty() ? c.show_failure : c.detail;
    if (c.detail_is_error && !c.detail_reported) {
      c.detail_reported = true;
      report_error = true;
    }
  }
  if (kick) { worker_->kick(); }
  ++consecutive_pending_;
  if (!detail.empty()) {
    last_skip_detail_ = detail;
    return !report_error;
  }
  // The begin-frame wait has not completed. In the steady state this is just the viewer pacing us
  // below our tick rate; the count tells it from a producer that is wedged.
  last_skip_detail_ =
    "begin-frame pending (no render request from the viewer); consecutive=" +
    std::to_string(consecutive_pending_);
  return true;
}

bool QarCApiRenderSenderAdapter::submit_on_worker(
  const qar_ogre_streaming::QarOpenGlStereoFrame & frame)
{
  RenderFrameCore & c = *core_;
  RenderFrameCore::Cycle cycle;
  {
    std::lock_guard<std::mutex> lock(c.m);
    if (c.slot != RenderFrameCore::Slot::kRendering) {
      last_skip_detail_ = "no frame cycle is open";
      return false;
    }
    // The runtime fills the textures of the next frame according to this layout.
    fill_frame_layout(c.layout, frame, /*supply_ids=*/false);
    c.layout_known = true;
    cycle = c.cycle;
  }

  // The frame cycle is open in QarOS: whatever happens below, it ends with a show-frame, which the
  // worker makes when the frame is handed over. A frame that could not be written into the
  // textures of the runtime is shown as they are.
  const char * unwritten = nullptr;
  // The fence that the worker waits for before it shows the frame; null when this thread waits
  // for the GPU itself.
  void * fence_sync = nullptr;
  // RENDER-INTO: copy our rendered Ogre color/depth INTO the textures the runtime handed out
  // before the frame is shown (see GlSubmitMode).
  if (cycle.no_ids) {
    unwritten = "the textures of the runtime were not known for this frame";
  } else if (copy_hook_) {
    const std::chrono::steady_clock::time_point copy_start = std::chrono::steady_clock::now();
    // The copy of a worker sender reads nothing back from the textures of the runtime.
    const bool copied = copy_hook_->copy_frame_into_quiet(frame, cycle.gl);
    note_gl_step("copy-into-qar", copy_start);
    if (!copied) {
      unwritten = "the frame could not be copied into the textures of the runtime";
    }
    // The copy must be COMPLETE before QAR consumes these textures from its own context:
    // shared-object writes become visible to another context only after the writing context
    // flushes (GL spec, Appendix D). The worker shows the frame only after this. A sender whose
    // worker bound a context ends the copy with a fence and a flush, and the worker waits for
    // the fence; any other sender waits for the GPU here.
    if (copied && c.fence_state.load() == 1) {
      const std::chrono::steady_clock::time_point fence_start = std::chrono::steady_clock::now();
      fence_sync = c.fence.ops.insert();
      note_gl_step("fence", fence_start);
    }
#ifdef _WIN32
    if (copied && fence_sync == nullptr) { gl_finish_timed(); }
#endif
  } else if (!warned_no_copy_hook_) {
    warned_no_copy_hook_ = true;
    std::fprintf(stderr,
      "[qar-adapter] render-into WITHOUT a copy hook: QAR's textures are never "
      "written, so the consumer will show a BLACK scene. Wire make_gl_copy_image_hook().\n");
    std::fflush(stderr);
  }

  // With a fence the worker reads the colour back after a stall, from our own texture, on its
  // context; it needs the frame description.
  const bool read_back_on_worker = fence_sync != nullptr && cycle.stalled && unwritten == nullptr;
  {
    std::lock_guard<std::mutex> lock(c.m);
    c.cycle.fence_sync = fence_sync;
    if (read_back_on_worker) {
      c.stall_frame = frame;
      c.stall_frame_valid = true;
    }
    c.slot = RenderFrameCore::Slot::kShowing;
  }
  worker_->hand_over();
  if (unwritten != nullptr) {
    last_skip_detail_ = std::string(unwritten) + "; the frame was shown as they are";
    return false;
  }

  // The first frame after a stall: its colour is read back from our own texture, which this
  // thread can reach; the frame is on its way already, so the read-back cannot delay it. (With a
  // fence the worker does this.)
  if (cycle.stalled && !read_back_on_worker) {
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    const std::chrono::steady_clock::time_point probe_start = std::chrono::steady_clock::now();
    const bool have_colour = copy_hook_ && copy_hook_->mean_left_colour(frame, rgb);
    note_gl_step("readback-probe", probe_start);
    std::fprintf(
      stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
      format_stall_line(cycle.stall, have_colour, rgb).c_str());
    std::fflush(stderr);
  }
  return true;
}

// ===========================================================================
//  QarCApiRenderSenderAdapter
// ===========================================================================
QarCApiRenderSenderAdapter::QarCApiRenderSenderAdapter(
  QarRenderSender * stream, const QarNearFar & near_far,
  GlSubmitMode submit_mode, std::shared_ptr<IQarGlTextureCopyHook> copy_hook,
  float depth_scale, std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot,
  QarPeerId peer_id, std::string log_folder_path, std::shared_ptr<IReleaseLane> release_lane,
  std::shared_ptr<std::atomic<bool>> context_released, bool frame_calls_on_worker,
  const FrameWorkerTimings & worker_timings, const FrameFenceSetup & fence,
  const RenderRecycleTimings & recycle, std::shared_ptr<RenderRecycleCount> recycle_count)
: stream_(stream), near_far_(near_far), depth_scale_(depth_scale),
  submit_mode_(submit_mode), copy_hook_(std::move(copy_hook)),
  recycle_timings_(recycle), recycle_count_(std::move(recycle_count)),
  pointing_ray_slot_(std::move(pointing_ray_slot)),
  release_lane_(std::move(release_lane)),
  context_released_(std::move(context_released)),
  // QAR_ADAPTER_PRIME sets the INITIAL value only; the manager overwrites this via
  // set_prime_enabled() right after construction (see OgreQarStreamingManager::
  // create_stream), and the panel checkbox changes it live from there. Read here too
  // so an adapter built outside that flow (a test, a standalone sample) still honors
  // the env var on its own.
  prime_enabled_(prime_enabled_from_environment())
{
  recycle_watch_ = RenderRecycleWatch(
    recycle_timings_, recycle_count_, std::chrono::steady_clock::now());
  peer_label_ = qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8);
  // The runtime names the textures of this sender, and GL may reuse the name of a texture of an
  // earlier sender that was of another kind.
  if (copy_hook_) { copy_hook_->forget_texture_targets(); }
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
  breaker_ = std::make_shared<FrameBreaker>(peer_label_);
  // The frame calls run on a worker thread of their own when the sender can release its stream on
  // the lane and renders into the runtime's textures (the supply-own mode needs the frame to know
  // the textures, so it keeps the frame calls on the render thread). A lane that refuses the
  // worker leaves them there too.
  if (frame_calls_on_worker && release_lane_ != nullptr && stream_ != nullptr &&
    submit_mode_ == GlSubmitMode::kRenderIntoQarTextures)
  {
    auto core = std::make_shared<RenderFrameCore>();
    core->peer_label = peer_label_;
    core->near_far = near_far_;
    core->depth_scale = depth_scale_;
    core->ray_slot = pointing_ray_slot_;
    core->lane = release_lane_;
    core->context_released = context_released_;
    core->breaker = breaker_;
    core->stream = stream_;
    core->copy_hook = copy_hook_;
    core->recycle_watch = recycle_watch_;
    // Only a sender that has a copy to wait for uses the fence.
    if (fence.usable() && copy_hook_ != nullptr) { core->fence = fence; }
    core->prime.store(prime_enabled_);
    core->tick_ms.store(tick_ms_);
    core->trace_path = frame_trace_ ? frame_trace_->path() : std::string();
    core->trace = std::move(frame_trace_);
    core->timing = std::move(timing_);
    FrameWorker::Jobs jobs;
    jobs.poll = [core] { worker_poll(*core, /*from_prime=*/false); };
    jobs.show = [core] { worker_show(*core); };
    jobs.leave = [core] { worker_leave(*core); };
    worker_ = FrameWorker::start("peer " + peer_label_, release_lane_, jobs, worker_timings);
    if (worker_ != nullptr) {
      core_ = core;
      stream_ = nullptr;  // the worker owns the handle from here on
    } else {
      frame_trace_ = std::move(core->trace);
      timing_ = std::move(core->timing);
      std::fprintf(
        stderr, "[qar-adapter] %s peer %s frame calls run on the GUI thread "
        "(no frame worker could start)\n", wall_clock_hms_ms().c_str(), peer_label_.c_str());
      std::fflush(stderr);
    }
  }
}

QarCApiRenderSenderAdapter::~QarCApiRenderSenderAdapter()
{
  if (worker_ != nullptr) {
    // The worker posts the release of the stream when its current call returns; this thread
    // neither waits for it nor joins it.
    core_->leaving.store(true);
    worker_->stop();
  }
  release_handles();
}

bool QarCApiRenderSenderAdapter::is_closed() const
{
  return closed_.load() || (core_ != nullptr && core_->closed.load());
}

bool QarCApiRenderSenderAdapter::recycled() const
{
  return recycled_.load() || (core_ != nullptr && core_->recycled.load());
}

bool QarCApiRenderSenderAdapter::frame_calls_stopped() const
{
  return breaker_->tripped() || (worker_ != nullptr && worker_->waiting()) ||
         (core_ != nullptr && core_->gpu_waiting.load());
}

bool QarCApiRenderSenderAdapter::frame_fence_active() const
{
  return core_ != nullptr && core_->fence_state.load() == 1;
}

void QarCApiRenderSenderAdapter::set_prime_enabled(bool enabled)
{
  prime_enabled_ = enabled;
  if (core_ != nullptr) { core_->prime.store(enabled); }
}

void QarCApiRenderSenderAdapter::set_tick_ms(double tick_ms)
{
  tick_ms_ = tick_ms;
  if (core_ != nullptr) { core_->tick_ms.store(tick_ms); }
}

void QarCApiRenderSenderAdapter::release_handles()
{
  if (copy_hook_) { copy_hook_->forget_texture_targets(); }
  // A primed-but-unconsumed frame is released BEFORE the stream that owns it. Same ordering
  // concern for a trace-only kept-alive handle (the pre-show pose re-read): a frame caught
  // mid-flight (between consume and show) when the stream is torn down must not leak or be
  // destroyed twice. frame_trace_'s own destructor (flush + close the CSV) runs later, which is
  // fine: by then it holds no QAR handle of any kind.
  QarRenderFrameInfo * const primed = primed_info_;
  QarRenderFrameInfo * const traced = frame_trace_ ? frame_trace_->take_row_handle() : nullptr;
  QarRenderSender * const stream = stream_;
  primed_info_ = nullptr;
  stream_ = nullptr;
  if (primed == nullptr && traced == nullptr && stream == nullptr) {
    return;
  }
  const std::shared_ptr<std::atomic<bool>> released = context_released_;
  const std::string label = peer_label_;
  const auto destroy = [primed, traced, stream, released, label] {
      if (primed != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, primed); }
      if (traced != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, traced); }
      if (stream != nullptr) {
        // How long the destroy takes is logged: with the Hub gone it may wait.
        const auto started = std::chrono::steady_clock::now();
        QAR_CALL(qar_render_stream_handle_destroy, stream);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
        std::fprintf(
          stderr, "[qar-adapter] %s peer %s 3D stream released (qar_render_stream_handle_destroy "
          "took %lld ms)\n", wall_clock_hms_ms().c_str(), label.c_str(),
          static_cast<long long>(took));
        std::fflush(stderr);
      }
      // The stream that used the GL context of this sender is gone.
      if (released != nullptr) { released->store(true); }
    };
  if (release_lane_ == nullptr) {
    destroy();
    return;
  }
  // The destroy waits for the Hub when the Hub does not answer; it runs on the thread that owns
  // the session, so this thread only posts. A lane that refuses has no thread left to run it:
  // the handles stay as they are and no call is made on them.
  if (!release_lane_->post_release("render-stream-release", destroy)) {
    std::fprintf(
      stderr, "[qar-adapter] %s peer %s stream handle left unreleased (the owner thread is gone)\n",
      wall_clock_hms_ms().c_str(), peer_label_.c_str());
    std::fflush(stderr);
  }
}

RenderRecycleTimings default_render_recycle_timings()
{
  static const RenderRecycleTimings timings = [] {
      RenderRecycleTimings t;
      const std::string value = read_environment("QAR_RENDER_RECYCLE_S");
      if (environment_is_set("QAR_RENDER_RECYCLE_S")) {
        const double seconds = parse_render_recycle_seconds(
          value.c_str(), static_cast<double>(t.after.count()) / 1000.0);
        t.after = std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
      }
      return t;
    }();
  return timings;
}

RenderRecycleWatch::RenderRecycleWatch(
  const RenderRecycleTimings & timings, std::shared_ptr<RenderRecycleCount> count,
  std::chrono::steady_clock::time_point created)
: timings_(timings), count_(std::move(count)), began_at_(created)
{
}

std::chrono::steady_clock::time_point RenderRecycleWatch::clock_start() const
{
  return have_reconnected_ && last_reconnecting_at_ > began_at_
    ? last_reconnecting_at_ : began_at_;
}

std::chrono::milliseconds RenderRecycleWatch::limit() const
{
  std::chrono::milliseconds limit = timings_.after;
  const std::chrono::milliseconds cap =
    timings_.cap > timings_.after ? timings_.cap : timings_.after;
  const unsigned recycles = count_ ? count_->load() : 0u;
  for (unsigned i = 0; i < recycles && limit < cap; ++i) { limit *= 2; }
  return limit < cap ? limit : cap;
}

long long RenderRecycleWatch::seconds_without_frame(
  std::chrono::steady_clock::time_point now) const
{
  return static_cast<long long>(
    std::chrono::duration_cast<std::chrono::seconds>(now - clock_start()).count());
}

bool RenderRecycleWatch::note(
  Answer answer, int code, std::chrono::steady_clock::time_point now)
{
  if (timings_.after.count() <= 0 || recycled_) { return false; }
  // A run of "reconnecting" answers goes on through "not ready" and "not yet" answers and ends
  // with a frame, with another error, or when the runtime has not said so for error_window.
  if (reconnecting_ && now - last_reconnecting_at_ > timings_.error_window) {
    reconnecting_ = false;
  }
  switch (answer) {
    case Answer::kBegan:
      began_at_ = now;
      have_error_ = false;
      reconnecting_ = false;
      have_reconnected_ = false;
      if (count_) { count_->store(0); }
      return false;
    case Answer::kReconnecting:
      if (!reconnecting_) {
        reconnecting_ = true;
        reconnecting_since_ = now;
      }
      last_reconnecting_at_ = now;
      have_reconnected_ = true;
      last_code_ = code;
      have_error_ = false;
      break;
    case Answer::kError:
      reconnecting_ = false;
      // The errors last while no gap between two of them is longer than error_window.
      if (!have_error_ || now - last_error_at_ > timings_.error_window) {
        have_error_ = true;
        error_since_ = now;
      }
      last_error_at_ = now;
      last_code_ = code;
      break;
    case Answer::kNotYet:
    case Answer::kNotReady:
      break;
  }
  const bool reconnecting_too_long =
    reconnecting_ && now - reconnecting_since_ > timings_.reconnecting_limit;
  const bool no_request = have_error_ && now - last_error_at_ <= timings_.error_window &&
    now - error_since_ >= timings_.error_persist && now - clock_start() >= limit();
  if (!reconnecting_too_long && !no_request) { return false; }
  // The runtime answers "reconnecting" in a run that is too long; its code names the cause.
  if (reconnecting_too_long) {
    last_code_ = static_cast<int>(QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING);
  }
  recycled_ = true;
  if (count_) { count_->fetch_add(1); }
  return true;
}

bool FrameBreaker::blocked()
{
  if (!tripped_.load() || probing_) {
    return false;
  }
  if (std::chrono::steady_clock::now() < resume_at_) {
    return true;
  }
  // The pause is over: the next frame call is the probe, note() judges it.
  probing_ = true;
  return false;
}

void FrameBreaker::note(const char * call, std::chrono::steady_clock::time_point started)
{
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  const auto as_ms = [](std::chrono::steady_clock::duration d) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(d);
    };
  const std::chrono::milliseconds took = as_ms(now - started);
  const bool slow = took >= timings_.slow_call;
  if (probing_) {
    probing_ = false;
    if (slow) {
      // The Hub still stalls the frame calls: pause longer.
      last_slow_call_ = now;
      pause_ = std::min(pause_ * 2, timings_.pause_cap);
      resume_at_ = now + pause_;
      return;
    }
    tripped_.store(false);
    // The slow calls before the pause still count: a slow call soon after the resume pauses the
    // frame calls again.
    std::fprintf(
      stderr, "[qar-adapter] %s peer %s frame calls resumed after %.1f s\n",
      wall_clock_hms_ms().c_str(), peer_label_.c_str(),
      std::chrono::duration<double>(now - first_trip_at_).count());
    std::fflush(stderr);
    return;
  }
  if (tripped_.load()) {
    return;
  }
  if (pause_ != timings_.pause_first && now - last_slow_call_ >= timings_.calm_reset) {
    pause_ = timings_.pause_first;
  }
  if (!slow) {
    return;
  }
  const bool second = have_slow_call_ && as_ms(now - last_slow_call_) <= timings_.window;
  last_slow_call_ = now;
  have_slow_call_ = true;
  if (!second) {
    return;
  }
  first_trip_at_ = now;
  resume_at_ = now + pause_;
  tripped_.store(true);
  std::fprintf(
    stderr, "[qar-adapter] %s peer %s frame calls stopped: %s took %lld ms twice\n",
    wall_clock_hms_ms().c_str(), peer_label_.c_str(), call, static_cast<long long>(took.count()));
  std::fflush(stderr);
}

bool QarCApiRenderSenderAdapter::frame_calls_blocked()
{
  return breaker_->blocked();
}

void QarCApiRenderSenderAdapter::note_frame_call(
  const char * call, std::chrono::steady_clock::time_point started)
{
  breaker_->note(call, started);
}

std::string QarCApiRenderSenderAdapter::frame_trace_path() const
{
  if (core_ != nullptr) { return core_->trace_path; }
  return frame_trace_ ? frame_trace_->path() : std::string();
}

std::uint64_t QarCApiRenderSenderAdapter::frame_trace_rows() const
{
  if (core_ != nullptr) { return core_->trace_rows.load(); }
  return frame_trace_ ? frame_trace_->rows_written() : 0;
}

std::uint64_t QarCApiRenderSenderAdapter::frame_trace_changed_rows() const
{
  if (core_ != nullptr) { return core_->trace_changed_rows.load(); }
  return frame_trace_ ? frame_trace_->changed_rows() : 0;
}

bool QarCApiRenderSenderAdapter::begin_frame(qar_ogre_streaming::QarFrameInfo & out_info)
{
  if (core_ != nullptr) {
    return begin_frame_on_worker(out_info);
  }
  out_info = qar_ogre_streaming::QarFrameInfo{};
  out_info.should_render = false;
  last_skip_detail_.clear();
  if (stream_ == nullptr) {
    last_skip_detail_ = "no QAR render-stream handle";
    return false;
  }
  if (frame_calls_blocked()) {
    last_skip_detail_ = "frame calls stopped: a frame call blocked";
    return true;
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
    const std::chrono::steady_clock::time_point call_start = std::chrono::steady_clock::now();
    const QarResult r = QAR_CALL(qar_render_sender_try_begin_frame, stream_, &info, &ready);
    note_frame_call("try_begin_frame", call_start);
    if (timing_) { timing_->note_poll_call(poll_start_us); }
    if (breaker_->tripped()) {
      // This call paused the frame calls (again): the rest of the frame waits for the next probe.
      if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
      last_skip_detail_ = "frame calls stopped: a frame call blocked";
      return true;
    }
    if (tbf_trace) { trace_msg(ready ? "begin_frame: <- try_begin_frame (ready)"
                                      : "begin_frame: <- try_begin_frame (pending)"); }
    {
      const std::chrono::steady_clock::time_point answered = std::chrono::steady_clock::now();
      const RenderRecycleWatch::Answer answer = qar_result_is_error(r)
        ? recycle_answer_of(r)
        : (ready && info != nullptr ? RenderRecycleWatch::Answer::kBegan
                                    : RenderRecycleWatch::Answer::kNotReady);
      if (!closed_.load() &&
        recycle_watch_.note(answer, static_cast<int>(r.code), answered))
      {
        print_recycle_line(peer_label_, recycle_watch_, answered);
        recycled_.store(true);
        closed_.store(true);
      }
    }
    if (qar_result_is_error(r)) {
      if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
      // Two answers mean "not yet", not a failure of the sender: the app volume is not resolved
      // in the Hub yet (the stream hands out nothing meanwhile), or the receiver paused the
      // stream because its display is not visible. The tick declines and polls again.
      if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_APP_VOLUME_UNRESOLVED)) {
        last_skip_detail_ = "app volume not resolved yet";
        return true;
      }
      if (qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED)) {
        last_skip_detail_ = "paused by the receiver (its display is not visible)";
        return true;
      }
      if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_STREAM_IS_CLOSED)) {
        closed_.store(true);
        last_skip_detail_ = "try_begin_frame: producer stream is closed";
      } else {
        last_skip_detail_ = result_text("try_begin_frame", r);
      }
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
    if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
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
      QAR_CALL(qar_render_frame_info_get_view_pose, info, 0, &left_pose)},
    {"qar_render_frame_info_get_view_fov(0)",
      QAR_CALL(qar_render_frame_info_get_view_fov, info, 0, &left_fov)},
    {"qar_render_frame_info_get_view_pose(1)",
      QAR_CALL(qar_render_frame_info_get_view_pose, info, 1, &right_pose)},
    {"qar_render_frame_info_get_view_fov(1)",
      QAR_CALL(qar_render_frame_info_get_view_fov, info, 1, &right_fov)},
  };
  for (const ViewRead & read : view_reads) {
    if (qar_result_is_error(read.result)) {
      QAR_CALL(qar_render_frame_info_handle_destroy, info);
      last_skip_detail_ = result_text(read.what, read.result);
      return false;
    }
  }

  // The AR pointing ray, while both halves are in hand. See the declaration for
  // why this must happen here and not on the gesture thread. Only needs left_pose,
  // so it runs regardless of whether `info` is about to be destroyed or kept alive.
  publish_pointing_ray(left_pose);

  // Does this frame follow a stall of the frames shown? Answered here, where both eye
  // positions are in hand; the line itself is printed once the frame has been shown.
  stall_detector_.frame_ready(
    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),
    StallPoint{left_pose.position.x, left_pose.position.y, left_pose.position.z},
    StallPoint{right_pose.position.x, right_pose.position.y, right_pose.position.z});

  out_info.frame_index = frame_index_++;
  out_info.predicted_display_time = QarTimePoint{static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count())};
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
    QAR_CALL(qar_render_frame_info_handle_destroy, info);
  }
  return true;
}

void QarCApiRenderSenderAdapter::publish_pointing_ray(const QarPose & eye_pose)
{
  if (!pointing_ray_slot_ || stream_ == nullptr || frame_calls_blocked()) {
    return;
  }
  publish_ray(
    *pointing_ray_slot_, stream_, eye_pose, *breaker_, logged_hands_tracked_,
    logged_hands_missing_);
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
  if (core_ != nullptr) {
    return submit_on_worker(frame);
  }
  if (stream_ == nullptr || frame_calls_blocked()) { return false; }

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
    gl_finish_timed();
#endif
    trace_submit(n, "-> frame_opengl (supply-own)");
    const std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();
    const QarResult fr = QAR_CALL(qar_render_sender_frame_opengl, stream_, &gl);
    note_frame_call("frame_opengl", frame_start);
    trace_submit(n, "<- frame_opengl (supply-own)");
    if (qar_result_is_error(fr)) {
      // Abandoned before show_frame: release the trace's kept-alive handle (if any)
      // without writing a row for it — see FrameTraceState::abandon_row().
      if (frame_trace_) { frame_trace_->abandon_row(); }
      return false;
    }
    if (breaker_->tripped()) {
      if (frame_trace_) { frame_trace_->abandon_row(); }
      return false;
    }
  } else {
    // RENDER-INTO: describe the layout, ask QAR to hand us its owned destination
    // texture ids via frame_opengl(out_frame), then copy our rendered Ogre
    // color/depth INTO those ids before show_frame (see GlSubmitMode).
    fill_frame_layout(gl, frame, /*supply_ids=*/false);
    trace_submit(n, "-> frame_opengl (render-into)");
    const std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();
    const QarResult fr = QAR_CALL(qar_render_sender_frame_opengl, stream_, &gl);
    note_frame_call("frame_opengl", frame_start);
    trace_submit(n, "<- frame_opengl (render-into)");
    if (qar_result_is_error(fr) || breaker_->tripped()) {
      if (frame_trace_) { frame_trace_->abandon_row(); }
      return false;
    }
    // gl.textures[i].texture now hold QAR-owned destination ids: copy our rendered
    // Ogre color/depth into them. WITHOUT this the frames are delivered perfectly
    // but arrive black — QAR's textures were simply never written.
    if (copy_hook_) {
      const std::chrono::steady_clock::time_point copy_start = std::chrono::steady_clock::now();
      const bool copied = copy_hook_->copy_frame_into(frame, gl);
      note_gl_step("copy-into-qar", copy_start);
      if (!copied) {
        if (frame_trace_) { frame_trace_->abandon_row(); }
        return false;
      }
#ifdef _WIN32
      // The copy must be COMPLETE before QAR consumes these textures from its own
      // context: shared-object writes become visible to another context only after
      // the writing context flushes (GL spec, Appendix D).
      gl_finish_timed();
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
  const std::chrono::steady_clock::time_point show_start = std::chrono::steady_clock::now();
  const QarResult sr = QAR_CALL(qar_render_sender_show_frame, stream_, &show);
  note_frame_call("show_frame", show_start);
  trace_submit(n, "<- show_frame");
  const bool show_ok = qar_result_is_success(sr);
  submit_timing.show_returned(show_ok);

  if (frame_trace_) { frame_trace_->note_show_done(show_ok); }

  if (show_ok && first_show_noted_.first()) {
    note_first_show(gl);
  }

  if (show_ok) {
    // After the frame is out, so a read-back cannot delay it. The detector is told afterwards:
    // the stall is measured up to the frame that was shown, and ends with it.
    probe_after_stall(frame);
    stall_detector_.frame_shown(
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  if (show_ok && prime_enabled_) {
    // The frame is out; immediately start the wait for the next one so it runs
    // during the gap before the next tick instead of costing that tick. Skipped
    // when QAR_ADAPTER_PRIME=0 (or the live checkbox is off): the wait then starts
    // at the NEXT tick's begin_frame() instead, and that tick pays for it.
    prime_next_frame();
  }
  return show_ok;
}

void QarCApiRenderSenderAdapter::note_first_show(const QarVideoFrameOpenGl & gl)
{
  print_first_show_line(peer_label_, gl);
}

void QarCApiRenderSenderAdapter::probe_after_stall(
  const qar_ogre_streaming::QarOpenGlStereoFrame & frame)
{
  StallReport report;
  if (!stall_detector_.take(report)) {
    return;  // the usual frame: nothing to read back, nothing to say
  }
  // The one read-back of the colour of the first frame after a stall; the hook owns the GL
  // calls. A stream without a hook (or one that cannot read textures back) says so in the line.
  float rgb[3] = {0.0f, 0.0f, 0.0f};
  const std::chrono::steady_clock::time_point probe_start = std::chrono::steady_clock::now();
  const bool have_colour = copy_hook_ && copy_hook_->mean_left_colour(frame, rgb);
  note_gl_step("readback-probe", probe_start);
  std::fprintf(
    stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
    format_stall_line(report, have_colour, rgb).c_str());
  std::fflush(stderr);
}

void QarCApiRenderSenderAdapter::prime_next_frame()
{
  if (stream_ == nullptr || primed_info_ != nullptr || frame_calls_blocked()) {
    return;
  }
  QarRenderFrameInfo * info = nullptr;
  bool ready = false;
  // This call is what STARTS the next begin-frame wait; whether it also completes
  // immediately is a bonus. Non-blocking either way, so it is safe on this thread.
  if (frame_trace_) { frame_trace_->note_try_begin_frame_call(/*from_prime=*/true); }
  const std::int64_t poll_start_us = timing_ ? qar_ar_core::timing_epoch_us() : 0;
  const std::chrono::steady_clock::time_point call_start = std::chrono::steady_clock::now();
  const QarResult r = QAR_CALL(qar_render_sender_try_begin_frame, stream_, &info, &ready);
  note_frame_call("try_begin_frame", call_start);
  if (timing_) { timing_->note_poll_call(poll_start_us); }
  if (breaker_->tripped()) {
    if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
    return;
  }
  if (qar_result_is_error(r)) {
    if (qar_result_has_code(r, QAR_STATUS_RENDERING_PRODUCER_STREAM_IS_CLOSED)) {
      closed_.store(true);
    }
    if (info != nullptr) { QAR_CALL(qar_render_frame_info_handle_destroy, info); }
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
    // A frame that begins here is a frame the runtime asked for, as one that begin_frame() polls.
    if (!closed_.load()) {
      recycle_watch_.note(
        RenderRecycleWatch::Answer::kBegan, 0, std::chrono::steady_clock::now());
    }
  } else if (info != nullptr) {
    QAR_CALL(qar_render_frame_info_handle_destroy, info);
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
  GlBlitCopyHook() = default;

  /// A hook on the GL entry points of @p api instead of those of the process.
  explicit GlBlitCopyHook(const GlBlitApi & api) : injected_(api), has_injected_(true) {}

  bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame & src,
    const QarVideoFrameOpenGl & dst) override
  {
    return copy(src, dst, /*worker_path=*/false);
  }

  bool copy_frame_into_quiet(
    const qar_ogre_streaming::QarOpenGlStereoFrame & src,
    const QarVideoFrameOpenGl & dst) override
  {
    return copy(src, dst, /*worker_path=*/true);
  }

  bool copy(
    const qar_ogre_streaming::QarOpenGlStereoFrame & src,
    const QarVideoFrameOpenGl & dst, bool worker_path)
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
      if ((sw != dw || sh != dh) && !warned_size_.exchange(true)) {
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
        if (!reported_[i].exchange(true)) {  // per-view: a depth-only failure must not be masked
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
      // What the attach left in the error queue must not read as a failure of the blit.
      drain_errors();
      blit_framebuffer_(
        0, 0, static_cast<int>(sw), static_cast<int>(sh),
        0, dy0, static_cast<int>(dw), dy1,
        kColorBufferBit, is_depth ? kNearest : kLinear);

      const unsigned int err = (get_error_ != nullptr) ? get_error_() : 0u;
      if (err != 0u) {
        all_ok = false;
        if (!reported_[i].exchange(true)) {  // per-view, once: colour must not mask depth
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
    const bool probe_now = copy_probe_due(
      worker_path, probe_frames_, probe_every_, probe_every_set_, probes_done_, kMaxProbes);
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

      // The same read on the colour QAR holds after the blit. A scene that is lit in the
      // control above and dark here was lost on the way into QAR's texture; one that is lit in
      // both reached it, so a black picture is made after the copy.
      probe_float_texture("DST left-COLOR (QAR, post-blit, red channel 0..1)",
        dst.textures[0].texture, dst.textures[0].size.width, dst.textures[0].size.height,
        dst.textures[0].size.array_layers);

      // The depth question, answered directly: what floats are actually in the
      // left-depth texture on each side of the copy?
      if (src.has_depth && dst.textures_count > 2) {
        probe_float_texture("SRC left-depth (ours, pre-blit)",
          src.left_depth.texture_id, src.left_depth.size.width, src.left_depth.size.height);
        probe_float_texture("DST left-depth (QAR, post-blit)",
          dst.textures[2].texture, dst.textures[2].size.width, dst.textures[2].size.height,
          dst.textures[2].size.array_layers);
      }
    }
    return all_ok;
  }

  /// The mean colour of our left colour texture, read back through the same glGetTexImage the
  /// periodic probes use (one read of the whole texture as 8-bit RGBA). The binding of the
  /// texture unit that is active is put back as it was, so Ogre's idea of what is bound stays
  /// true.
  bool mean_left_colour(
    const qar_ogre_streaming::QarOpenGlStereoFrame & src, float (& rgb)[3]) override
  {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (!resolve() || get_tex_image_ == nullptr || bind_texture_ == nullptr ||
      get_error_ == nullptr)
    {
      return false;
    }
    const unsigned int tex = src.left_color.texture_id;
    const unsigned int w = src.left_color.size.width;
    const unsigned int h = src.left_color.size.height;
    if (tex == 0u || w == 0u || h == 0u) { return false; }

    // Only a plain 2D texture: an array or multisample texture would not fit the one image
    // buffer below, and our own render target is a plain 2D texture.
    const unsigned int target = detect_target(tex);
    if (target != kTexture2D) { return false; }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u, 0u);
    int previous = 0;
    if (get_integerv_ != nullptr) {
      get_integerv_(kTextureBinding2D, &previous);
    }
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
    bind_texture_(target, tex);
    get_tex_image_(target, 0, kRgba, kUnsignedByte, pixels.data());
    const unsigned int err = get_error_();
    bind_texture_(target, static_cast<unsigned int>(previous));
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
    if (err != 0u) { return false; }
    return mean_rgba8(pixels.data(), w, h, kColourReadStep, rgb);
  }

  /// The textures of a new sender get names that GL may have given to textures of another kind
  /// before: the cache of the kinds is dropped, and the one-shot lines are due again.
  void forget_texture_targets() override
  {
    {
      std::lock_guard<std::mutex> lock(target_mutex_);
      target_cache_count_ = 0;
    }
    for (std::atomic<bool> & reported : reported_) { reported.store(false); }
    warned_size_.store(false);
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
  static constexpr unsigned int kFramebufferMissingAttachment = 0x8CD7;
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
  static constexpr unsigned int kRgba = 0x1908;           // GL_RGBA
  static constexpr unsigned int kUnsignedByte = 0x1401;   // GL_UNSIGNED_BYTE
  static constexpr unsigned int kTextureBinding2D = 0x8069;  // GL_TEXTURE_BINDING_2D
  /// Every 4th pixel in x and y is averaged: the read-back is the cost, not the sum.
  static constexpr std::uint32_t kColourReadStep = 4;

  /// Read a texture's actual float contents back and report min/max/mean plus a few
  /// samples: the decisive depth diagnostic ("did our depth RTT get real metric values,
  /// or did the transport lose them?"). Six probes at most, the first at the first copied
  /// frame and then every probe_every_ frames, whatever the trace setting; it stalls the pipeline.
  /// @p layers is the layer count of an array texture: glGetTexImage returns every layer, so the
  /// buffer holds them all and only the first is looked at.
  void probe_float_texture(
    const char * label, unsigned int tex, unsigned int w, unsigned int h,
    unsigned int layers = 1u)
  {
    if (get_tex_image_ == nullptr || bind_texture_ == nullptr || get_error_ == nullptr) {
      return;
    }
    if (tex == 0u || w == 0u || h == 0u) { return; }
    const unsigned int target = detect_target(tex);
    std::vector<float> px(
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * (layers > 1u ? layers : 1u),
      0.0f);

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
    px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));

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
    {
      // The render thread copies and, after a stall, a frame worker reads a colour back.
      std::lock_guard<std::mutex> lock(target_mutex_);
      for (std::size_t i = 0; i < target_cache_count_; ++i) {
        if (target_cache_[i].id == id) { return target_cache_[i].target; }
      }
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
    std::lock_guard<std::mutex> lock(target_mutex_);
    if (target_cache_count_ < kTargetCacheMax) {
      target_cache_[target_cache_count_++] = {id, found};
    }
    return found;
  }

  /// Empties the error queue of GL, so that what is read afterwards is the error of the next call.
  void drain_errors()
  {
    if (get_error_ == nullptr) { return; }
    for (int guard = 0; guard < 32 && get_error_() != 0u; ++guard) {}
  }

  /// Stores @p target as the kind of the texture @p id, replacing what the cache held.
  void remember_target(unsigned int id, unsigned int target)
  {
    std::lock_guard<std::mutex> lock(target_mutex_);
    for (std::size_t i = 0; i < target_cache_count_; ++i) {
      if (target_cache_[i].id == id) {
        target_cache_[i].target = target;
        return;
      }
    }
    if (target_cache_count_ < kTargetCacheMax) {
      target_cache_[target_cache_count_++] = {id, target};
    }
  }

  /// Attach @p tex to @p fb_target's colour attachment using the form its REAL target
  /// requires, and return the resulting framebuffer status. Array textures must be
  /// attached one layer at a time (non-layered) for glBlitFramebuffer to accept them.
  ///
  /// The kind of a texture is cached per name, and GL reuses the names of destroyed textures, so
  /// the cached kind may be wrong. A form that does not fit raises GL_INVALID_OPERATION and leaves
  /// the attachment as it was, so the attachment is cleared first: a rejected form must leave the
  /// framebuffer incomplete and never the texture of the view before. The error is read after every
  /// attempt and drained (it would otherwise be read as the error of the blit); a rejected form
  /// does not count as attached, and the form that completes the framebuffer replaces the cached
  /// kind.
  unsigned int attach(unsigned int fb_target, unsigned int tex, unsigned int layer_index)
  {
    const unsigned int target = detect_target(tex);
    const bool layered_first = target == kTexture2DArray && framebuffer_texture_layer_ != nullptr;
    framebuffer_texture_2d_(fb_target, kColorAttachment0, kTexture2D, 0u, 0);
    drain_errors();
    // The status of the framebuffer after one attempt; a rejected form is reported as incomplete.
    const auto attempted = [&]() {
        const bool rejected = get_error_ != nullptr && get_error_() != 0u;
        drain_errors();
        const unsigned int st = check_framebuffer_status_(fb_target);
        return (rejected && st == kFramebufferComplete) ? kFramebufferMissingAttachment : st;
      };
    if (layered_first) {
      framebuffer_texture_layer_(fb_target, kColorAttachment0, tex, 0,
        static_cast<int>(layer_index));
    } else {
      framebuffer_texture_2d_(fb_target, kColorAttachment0, target, tex, 0);
    }
    unsigned int st = attempted();
    if (st == kFramebufferComplete) { return st; }

    // Fallbacks, in case the cached kind is not the real one.
    if (!layered_first && framebuffer_texture_layer_ != nullptr) {
      framebuffer_texture_layer_(fb_target, kColorAttachment0, tex, 0,
        static_cast<int>(layer_index));
      st = attempted();
      if (st == kFramebufferComplete) {
        remember_target(tex, kTexture2DArray);
        return st;
      }
    }
    if (target != kTexture2D) {
      framebuffer_texture_2d_(fb_target, kColorAttachment0, kTexture2D, tex, 0);
      st = attempted();
      if (st == kFramebufferComplete) { remember_target(tex, kTexture2D); }
    }
    return st;
  }

  bool resolve()
  {
    if (resolved_) { return ready_; }
    resolved_ = true;
    if (has_injected_) {
      gen_framebuffers_ = injected_.gen_framebuffers;
      bind_framebuffer_ = injected_.bind_framebuffer;
      framebuffer_texture_2d_ = injected_.framebuffer_texture_2d;
      framebuffer_texture_layer_ = injected_.framebuffer_texture_layer;
      blit_framebuffer_ = injected_.blit_framebuffer;
      check_framebuffer_status_ = injected_.check_framebuffer_status;
      get_error_ = injected_.get_error;
      get_integerv_ = injected_.get_integerv;
      bind_texture_ = injected_.bind_texture;
      return finish_resolve();
    }
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
    return finish_resolve();
  }

  /// The last step of resolve(): checks the entry points and makes the two framebuffers.
  bool finish_resolve()
  {
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
    flip_y_ = environment_is_set("QAR_ADAPTER_FLIP_Y");
    const std::string probe_every = read_environment("QAR_ADAPTER_PROBE_EVERY");
    if (environment_is_set("QAR_ADAPTER_PROBE_EVERY")) {
      const long v = std::strtol(probe_every.c_str(), nullptr, 10);
      probe_every_ = (v > 0) ? static_cast<unsigned int>(v) : 0u;
      probe_every_set_ = true;
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
  std::mutex target_mutex_;

  unsigned int src_fbo_ = 0u;
  unsigned int dst_fbo_ = 0u;
  bool flip_y_ = false;
  bool ready_ = false;
  bool resolved_ = false;
  /// Per-view one-shot error guard (colour must not mask depth). forget_texture_targets() clears
  /// it from any thread.
  std::atomic<bool> reported_[4]{};
  std::atomic<bool> warned_size_{false};
  GlBlitApi injected_;
  bool has_injected_ = false;

  // Periodic probing. Frame 1 is always sampled; after that every probe_every_ frames
  // (QAR_ADAPTER_PROBE_EVERY, default 600 ~= 10 s at 60 fps), up to kMaxProbes.
  static constexpr unsigned int kMaxProbes = 6;
  unsigned long probe_frames_ = 0;
  unsigned int probes_done_ = 0;
  unsigned int probe_every_ = 600;
  /// QAR_ADAPTER_PROBE_EVERY was set: the copy of a worker sender probes too.
  bool probe_every_set_ = false;
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

std::shared_ptr<IQarGlTextureCopyHook> make_gl_copy_image_hook_with_api(const GlBlitApi & api)
{
#ifdef _WIN32
  return std::make_shared<GlBlitCopyHook>(api);
#else
  (void)api;
  return nullptr;
#endif
}

// ===========================================================================
//  QarCApiSenderFactory
// ===========================================================================
bool default_stream_release_on_owner()
{
  static const bool on_owner = [] {
      std::string value = read_environment("QAR_ADAPTER_STREAM_RELEASE");
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value != "gui";
    }();
  return on_owner;
}

bool default_frame_calls_on_worker()
{
  static const bool on_worker = [] {
      std::string value = read_environment("QAR_FRAME_CALLS");
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (value.empty() || value == "worker") {
        return true;
      }
      if (value != "gui") {
        std::fprintf(
          stderr, "[qar-adapter] QAR_FRAME_CALLS='%s' is neither worker nor gui; the frame calls "
          "run on one worker thread per sender\n", value.c_str());
        std::fflush(stderr);
        return true;
      }
      return false;
    }();
  return on_worker;
}

bool default_frame_fence_on_worker()
{
  static const bool on_worker = [] {
      std::string value = read_environment("QAR_FRAME_FENCE");
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (value.empty() || value == "worker") {
        return true;
      }
      if (value != "gui") {
        std::fprintf(
          stderr, "[qar-adapter] QAR_FRAME_FENCE='%s' is neither worker nor gui; the GL fence "
          "waits run on the frame worker of each sender\n", value.c_str());
        std::fflush(stderr);
        return true;
      }
      return false;
    }();
  return on_worker;
}

bool copy_probe_due(
  bool worker_path, unsigned long frame, unsigned int every, bool every_was_set,
  unsigned int probes_done, unsigned int max_probes)
{
  if (worker_path && !every_was_set) {
    return false;
  }
  return frame == 1 || (every > 0 && probes_done < max_probes && (frame % every) == 0);
}

namespace
{

using GlContextList = std::vector<std::unique_ptr<QarCApiSenderFactory::GlSenderContext>>;

// The WGL contexts of a factory that ended. The senders of the factory use them until the thread
// that owns the QarOS session has destroyed their streams, so they are kept here with a flag and
// are deleted by delete_released_gl_contexts() once it is set. A context with a stream of its own
// has its own flag, which the release of that stream sets; the others (no stream was made for
// them) share one flag that a lane item sets behind everything the lane was given. An entry whose
// flag is never set (the lane refused, or a frame worker never returned) is never deleted.
struct GlGraveyardEntry
{
  std::shared_ptr<std::atomic<bool>> released;
  GlContextList contexts;
};

std::mutex & graveyard_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<GlGraveyardEntry> & graveyard()
{
  static std::vector<GlGraveyardEntry> entries;
  return entries;
}

void (*& gl_context_deleter())(void *)
{
  static void (* deleter)(void *) = nullptr;
  return deleter;
}

void delete_gl_context(void * context)
{
#ifdef _WIN32
  const WglApi & w = wgl();
  if (context != nullptr && w.DeleteContext != nullptr) {
    w.DeleteContext(static_cast<HGLRC>(context));
  }
#else
  (void)context;
#endif
}

}  // namespace

QarCApiSenderFactory::QarCApiSenderFactory(
  std::shared_ptr<QarSessionOwner> owner, const Config & config)
: config_(config), owner_(owner), shared_(std::make_shared<CrossThread>())
{
  std::fprintf(
    stderr, config_.frame_calls_on_worker ?
    "[qar-adapter] frame calls run on one worker thread per sender (QAR_FRAME_CALLS=worker)\n" :
    "[qar-adapter] frame calls run on the GUI thread (QAR_FRAME_CALLS=gui)\n");
  std::fprintf(
    stderr, config_.frame_fence_on_worker ?
    "[qar-adapter] GL fence waits run on the frame worker of each sender "
    "(QAR_FRAME_FENCE=worker)\n" :
    "[qar-adapter] GL fence waits run on the GUI thread (QAR_FRAME_FENCE=gui)\n");
  std::fprintf(stderr, "%s\n", gpu_gate_banner(config_.gpu_gate, config_.gpu_gate_timings).c_str());
  std::fflush(stderr);
  gpu_watch_ = std::make_shared<GpuProgressWatch>(
    config_.gl_fence_ops, config_.gpu_gate, config_.gpu_gate_timings,
    config_.gpu_gate_test_seconds);
}

std::shared_ptr<IReleaseLane> QarCApiSenderFactory::release_lane() const
{
  const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
  return owner != nullptr ? owner->release_lane() : nullptr;
}

std::shared_ptr<IReleaseLane> QarCApiSenderFactory::stream_lane()
{
  if (!config_.stream_release_on_owner) {
    return nullptr;
  }
  if (!lane_fetched_) {
    lane_ = release_lane();
    lane_fetched_ = true;
  }
  return lane_;
}

int QarCApiSenderFactory::create_attempt(const QarPeerId & peer_id) const
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const CreateRetry & retry : retries_) {
    if (peer_eq(retry.peer_id, peer_id)) {
      return retry.started;
    }
  }
  return 0;
}

bool QarCApiSenderFactory::create_given_up(const QarPeerId & peer_id) const
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const CreateRetry & retry : retries_) {
    if (peer_eq(retry.peer_id, peer_id)) {
      return retry.given_up;
    }
  }
  return false;
}

bool QarCApiSenderFactory::frame_calls_stopped(const QarPeerId & peer_id) const
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const PeerAdapter & entry : adapters_) {
    if (peer_eq(entry.peer_id, peer_id)) {
      const std::shared_ptr<QarCApiRenderSenderAdapter> adapter = entry.adapter.lock();
      return adapter != nullptr && adapter->frame_calls_stopped();
    }
  }
  return false;
}

void QarCApiSenderFactory::set_gl_context_deleter(void (* deleter)(void * context))
{
  std::lock_guard<std::mutex> lk(graveyard_mutex());
  gl_context_deleter() = deleter;
}

void QarCApiSenderFactory::delete_released_gl_contexts()
{
  std::vector<GlContextList> ready;
  void (* deleter)(void *) = nullptr;
  {
    std::lock_guard<std::mutex> lk(graveyard_mutex());
    std::vector<GlGraveyardEntry> & entries = graveyard();
    for (auto it = entries.begin(); it != entries.end();) {
      if (it->released->load()) {
        ready.push_back(std::move(it->contexts));
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
    deleter = gl_context_deleter() != nullptr ? gl_context_deleter() : &delete_gl_context;
  }
  for (GlContextList & contexts : ready) {
    for (auto & ctx : contexts) {
      if (ctx != nullptr && ctx->context != nullptr) {
        deleter(ctx->context);
        ctx->context = nullptr;
      }
    }
  }
}

// What the thread that owns the session and the consumer thread share. The consumer thread reads
// it through shared_; the tasks hold their own reference.
struct QarCApiSenderFactory::CrossThread
{
  using Clock = std::chrono::steady_clock;

  // Guards last_error, volume and the anchor requests.
  mutable std::mutex mutex;
  std::string last_error;
  QarAppVolumeId volume{};
  std::atomic<bool> has_volume{false};
  std::atomic<bool> attached{false};
  std::atomic<AttachState> attach_state{AttachState::kIdle};
  /// Set by detach() and by the next attach: the tasks of this state undo themselves.
  std::atomic<bool> retired{false};
  /// Held while the attach decides to register the listener and while detach() retires, so
  /// that the listener is never registered after detach() removed it.
  std::mutex listener_mutex;

  std::atomic<bool> has_geo_anchor{false};
  bool geo_result_ready = false;
  std::string geo_result;
  bool geo_pending = false;
  bool geo_clear = false;
  QarGeoAnchorFrame geo_frame{};
  bool geo_task_queued = false;

  // The placement cache. Guarded by placement_mutex, which no QarOS call is made under.
  std::mutex placement_mutex;
  qar_ar_core::ScenePlacement placement;
  Clock::time_point read_at{};
  /// The next refresh is not asked for before this (after a failure, or a slow refresh).
  Clock::time_point refresh_not_before{};
  std::chrono::milliseconds refresh_interval{1000};
  Clock::time_point full_read_at{};
  /// When a change of the cached placement was last logged (the log is rate-limited).
  Clock::time_point change_logged_at{};
  bool subscribed = false;
  std::uint64_t updates_seen = 0;
  bool latest_failure_noted = false;
  std::atomic<bool> refresh_in_flight{false};

  // The update subscription. Owner thread only.
  QarCancelToken * token = nullptr;
  PlacementSubscription * subscription = nullptr;

  // The render request subscription (diagnostics). Token and subscription: owner thread only.
  QarCancelToken * request_token = nullptr;
  RequestSubscription * request_subscription = nullptr;
  /// What the callback saw and what our own senders are; guarded by request_mutex, which no
  /// QarOS call and no I/O is made under.
  struct SeenRequest
  {
    std::string peer8;
    std::string stream8;
    Clock::time_point seen_at{};
  };
  std::mutex request_mutex;
  std::vector<SeenRequest> requests_unlogged;
  std::vector<std::string> requests_logged;  ///< "<peer8>/<stream8>" of each line printed.
  /// The pairs printed as "unknown" (our own id was not read yet): printed again with the verdict
  /// once it is.
  std::vector<std::string> requests_unknown;
  std::map<std::string, std::string> our_streams;  ///< peer8 -> stream8 of our sender.
};

/// The first eight hex digits of an id: the start of the UUID text that the runtime logs.
static std::string id_first8(const std::uint8_t (& data)[QAR_MAX_ID_LENGTH])
{
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

/// What the runtime's update callback needs. Heap-allocated for the life of the subscription: the
/// runtime may call back until the token is cancelled.
struct QarCApiSenderFactory::PlacementSubscription
{
  std::shared_ptr<CrossThread> shared;
  QarAppVolumeId volume{};
};

/// What the render request callback needs; lives until the token is cancelled.
struct QarCApiSenderFactory::RequestSubscription
{
  std::shared_ptr<CrossThread> shared;
};

struct QarCApiSenderFactory::AttachJob
{
  Config config;
  std::shared_ptr<CrossThread> shared;
  /// Alive while any task runs: the tasks run on the owner's own thread, which the owner joins
  /// before it is destroyed.
  QarSessionOwner * owner = nullptr;
  std::weak_ptr<ITargetListener> listener;
};

void QarCApiSenderFactory::set_error(const std::string & text)
{
  std::lock_guard<std::mutex> lk(shared_->mutex);
  shared_->last_error = text;
}

std::string QarCApiSenderFactory::last_error() const
{
  std::lock_guard<std::mutex> lk(shared_->mutex);
  return shared_->last_error;
}

bool QarCApiSenderFactory::attached() const
{
  return shared_->attached.load();
}

bool QarCApiSenderFactory::has_geo_anchor() const
{
  return shared_->has_geo_anchor.load();
}

bool QarCApiSenderFactory::app_volume(QarAppVolumeId & out_id) const
{
  if (!shared_->has_volume.load()) {
    return false;
  }
  std::lock_guard<std::mutex> lk(shared_->mutex);
  out_id = shared_->volume;
  return true;
}

bool QarCApiSenderFactory::post_session_task(
  const char * name, SessionTask task, std::function<void()> dropped)
{
  const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
  if (owner == nullptr) {
    return false;
  }
  return owner->post_session_task(name, std::move(task), std::move(dropped));
}

QarCApiSenderFactory::AttachState QarCApiSenderFactory::attach_state() const
{
  return shared_->attach_state.load();
}

bool QarCApiSenderFactory::begin_attach()
{
  if (shared_->attached.load()) {
    return true;
  }
  if (shared_->attach_state.load() == AttachState::kPending) {
    return true;
  }
  // A new attempt starts from a state of its own; a task of an earlier attempt that is still
  // queued sees `retired` and undoes itself.
  shared_->retired.store(true);
  shared_ = std::make_shared<CrossThread>();

  const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
  if (owner == nullptr || !owner->ready() || owner->session() == nullptr) {
    set_error("attach: the QarOS session is not running");
    return false;
  }
  const std::weak_ptr<QarCApiSenderFactory> self = weak_from_this();
  if (self.expired()) {
    set_error("attach: the factory must be owned by a std::shared_ptr");
    return false;
  }
  auto job = std::make_shared<AttachJob>();
  job->config = config_;
  job->shared = shared_;
  job->owner = owner.get();
  job->listener = self;
  const std::shared_ptr<CrossThread> shared = shared_;
  shared->attach_state.store(AttachState::kPending);
  const bool posted = owner->post_session_task(
    "attach", [job](QarSession * session) { run_attach(session, *job); },
    [shared] {
      {
        std::lock_guard<std::mutex> lk(shared->mutex);
        shared->last_error = "attach: the QarOS session ended before the attach ran";
      }
      shared->attach_state.store(AttachState::kFailed);
    });
  if (!posted) {
    shared->attach_state.store(AttachState::kIdle);
    set_error("attach: the QarOS session is not accepting calls");
    return false;
  }
  return true;
}

bool QarCApiSenderFactory::attach()
{
  if (!begin_attach()) {
    return false;
  }
  while (attach_state() == AttachState::kPending) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return attach_state() == AttachState::kAttached;
}

bool QarCApiSenderFactory::read_placement_by_query(
  QarSession * session, const QarAppVolumeId & volume, qar_ar_core::ScenePlacement & out)
{
  // There is no get-handle-by-id call, so enumerate and match. The session owns
  // one volume in practice; the loop exists because the API cannot promise it.
  std::size_t count = 0;
  if (qar_result_is_error(QAR_CALL(qar_query_app_volumes_count, session, &count)) || count == 0) {
    return false;
  }
  std::vector<QarAppVolume *> handles(count, nullptr);
  std::size_t written = 0;
  if (qar_result_is_error(
      QAR_CALL(qar_query_app_volumes, session, handles.data(), count, &written)))
  {
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
      const bool is_ours = !qar_result_is_error(QAR_CALL(qar_app_volume_get_id, handle, &id)) &&
        std::memcmp(&id, &volume, sizeof(QarAppVolumeId)) == 0;
      if (is_ours) {
        QarPose volume_pose{};
        QarPose app_pose{};
        float app_scale = 1.0f;
        // All three or none: a placement assembled from a partial read would be
        // a different transform, not a slightly wrong one.
        const bool ok =
          !qar_result_is_error(QAR_CALL(qar_app_volume_get_pose, handle, &volume_pose)) &&
          !qar_result_is_error(QAR_CALL(qar_app_volume_get_app_pose, handle, &app_pose)) &&
          !qar_result_is_error(QAR_CALL(qar_app_volume_get_app_scale, handle, &app_scale));
        if (ok) {
          placement.known = true;
          placement.volume_pose = volume_pose;
          placement.app_pose = app_pose;
          placement.app_scale = app_scale;
          found = true;
        }
      }
    }
    QAR_CALL(qar_app_volume_handle_destroy, handle);
  }
  if (!found) {
    return false;
  }
  out = placement;
  return true;
}

void QarCApiSenderFactory::placement_update_trampoline(QarAppVolume * handle, void * user_state)
{
  // A library thread. The handle is borrowed: its accessors are read here and it is not kept. No
  // I/O, and only the cache lock is taken.
  const auto * subscription = static_cast<const PlacementSubscription *>(user_state);
  if (subscription == nullptr || handle == nullptr) {
    return;
  }
  QarAppVolumeId id{};
  if (qar_result_is_error(QAR_CALL(qar_app_volume_get_id, handle, &id)) ||
    std::memcmp(&id, &subscription->volume, sizeof(QarAppVolumeId)) != 0)
  {
    return;
  }
  QarPose volume_pose{};
  QarPose app_pose{};
  float app_scale = 1.0f;
  if (qar_result_is_error(QAR_CALL(qar_app_volume_get_pose, handle, &volume_pose)) ||
    qar_result_is_error(QAR_CALL(qar_app_volume_get_app_pose, handle, &app_pose)) ||
    qar_result_is_error(QAR_CALL(qar_app_volume_get_app_scale, handle, &app_scale)))
  {
    return;
  }
  CrossThread & state = *subscription->shared;
  std::lock_guard<std::mutex> lk(state.placement_mutex);
  state.placement.known = true;
  state.placement.volume_pose = volume_pose;
  state.placement.app_pose = app_pose;
  state.placement.app_scale = app_scale;
  state.read_at = CrossThread::Clock::now();
  ++state.updates_seen;
}

void QarCApiSenderFactory::render_request_trampoline(
  QarRenderStreamRequest * request, void * user_state)
{
  // A library thread: it records and returns, no I/O. The request handle is released here.
  const auto * subscription = static_cast<const RequestSubscription *>(user_state);
  if (request == nullptr) {
    return;
  }
  QarPeerId peer{};
  QarStreamId stream{};
  const bool read =
    !qar_result_is_error(QAR_CALL(qar_render_request_get_target_peer_id, request, &peer)) &&
    !qar_result_is_error(QAR_CALL(qar_render_request_get_stream_id, request, &stream));
  QAR_CALL(qar_render_request_handle_destroy, request);
  if (!read || subscription == nullptr || subscription->shared == nullptr) {
    return;
  }
  CrossThread & state = *subscription->shared;
  const std::string peer8 = id_first8(peer.data);
  const std::string stream8 = id_first8(stream.data);
  std::lock_guard<std::mutex> lk(state.request_mutex);
  constexpr std::size_t kMostPairs = 64;
  const std::string pair = peer8 + "/" + stream8;
  if (state.requests_logged.size() + state.requests_unknown.size() +
    state.requests_unlogged.size() >= kMostPairs ||
    std::find(state.requests_logged.begin(), state.requests_logged.end(), pair) !=
    state.requests_logged.end() ||
    std::find(state.requests_unknown.begin(), state.requests_unknown.end(), pair) !=
    state.requests_unknown.end())
  {
    return;
  }
  for (const CrossThread::SeenRequest & seen : state.requests_unlogged) {
    if (seen.peer8 == peer8 && seen.stream8 == stream8) {
      return;
    }
  }
  state.requests_unlogged.push_back(
    CrossThread::SeenRequest{peer8, stream8, CrossThread::Clock::now()});
}

int QarCApiSenderFactory::log_render_requests()
{
  const std::shared_ptr<CrossThread> shared = shared_;
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lk(shared->request_mutex);
    const auto now = CrossThread::Clock::now();
    // A pair that was printed as "unknown" is printed again once our own id is read.
    for (auto it = shared->requests_unknown.begin(); it != shared->requests_unknown.end();) {
      const std::size_t slash = it->find('/');
      const std::string peer8 = it->substr(0, slash);
      if (shared->our_streams.find(peer8) == shared->our_streams.end()) {
        ++it;
        continue;
      }
      shared->requests_unlogged.push_back(
        CrossThread::SeenRequest{peer8, it->substr(slash + 1), now});
      it = shared->requests_unknown.erase(it);
    }
    for (auto it = shared->requests_unlogged.begin(); it != shared->requests_unlogged.end();) {
      const auto ours = shared->our_streams.find(it->peer8);
      const bool known = ours != shared->our_streams.end();
      if (!known && now - it->seen_at < std::chrono::seconds(3)) {
        ++it;
        continue;
      }
      const char * verdict =
        !known ? "unknown" : (ours->second == it->stream8 ? "match" : "MISMATCH");
      lines.push_back(
        "render request for peer " + it->peer8 + " on stream " + it->stream8 + " (ours " +
        (known ? ours->second : std::string("unknown")) + ": " + verdict + ")");
      (known ? shared->requests_logged : shared->requests_unknown)
        .push_back(it->peer8 + "/" + it->stream8);
      it = shared->requests_unlogged.erase(it);
    }
  }
  for (const std::string & line : lines) {
    std::fprintf(stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(), line.c_str());
  }
  if (!lines.empty()) {
    std::fflush(stderr);
  }
  return static_cast<int>(lines.size());
}

void QarCApiSenderFactory::run_request_unsubscribe(const std::shared_ptr<CrossThread> & shared)
{
  CrossThread & state = *shared;
  QarCancelToken * const token = state.request_token;
  RequestSubscription * const subscription = state.request_subscription;
  state.request_token = nullptr;
  state.request_subscription = nullptr;
  if (token != nullptr) {
    // Returns after a callback that is running has ended, so the context can go right after.
    QAR_CALL(qar_cancel_token_cancel, token);
    QAR_CALL(qar_cancel_token_handle_destroy, token);
  }
  delete subscription;
}

void QarCApiSenderFactory::run_attach(QarSession * session, const AttachJob & job)
{
  CrossThread & state = *job.shared;
  const Config & config = job.config;
  if (state.retired.load()) {
    return;
  }

  // Get the app volume BEFORE any peer is served, so it is known by the time create_sender()
  // runs. The app volume is the mixer's unit of work: it composites each incoming stream into
  // the volume named by that stream's QarRenderSenderInit::app_volume_id. With the mixer ON, a
  // stream created with the default NULL id has nowhere to land: the Visualizer renders an empty
  // room. The common name is stable, so the next start finds the same volume again.
  if (config.create_app_volume) {
    const std::string common_name = qar_ar_core::make_volume_common_name(config.name_scope);
    QarAppVolumeInit vol = qar_app_volume_init_default();
    vol.common_name = common_name.c_str();
    vol.display_name = config.app_volume_name.c_str();
    // gesture_configuration stays NULL (qar_app_volume_init_default()'s own default): the
    // volume then gets the runtime's default configuration, which is exactly what
    // qar_app_volume_gesture_configuration_default() returns, so a struct built here would
    // change no field while adding a real risk: any rule left unfilled, or any drift against
    // the runtime's own layout, silently zeros to QAR_APP_VOLUME_CONTROLLER_RAY_LINE_STYLE_NONE
    // (an invisible ray) rather than to the vendor's SOLID or DOTTED defaults.
    // The pose is the cuboid CENTRE in room space, so the box is lifted rather than leaving
    // half of it below y = 0.
    vol.pose = qar_pose_default();
    vol.pose.position.y = config.app_volume_center_y_m;
    vol.size.width_meters = config.app_volume_width_m;
    vol.size.length_meters = config.app_volume_length_m;
    vol.size.height_meters = config.app_volume_height_m;
    vol.app_pose = qar_pose_default();
    vol.app_scale = config.app_volume_scale;

    QarAppVolumeId volume_id{};
    trace_msg("-> qar_app_volumes_get_or_create");
    const QarResult av = QAR_CALL(qar_app_volumes_get_or_create, session, &vol, &volume_id);
    trace_msg("<- qar_app_volumes_get_or_create");
    if (qar_result_is_error(av)) {
      // Fatal to every render stream: the field is Required and NULL is rejected outright,
      // so without a volume create_sender() refuses; see the refusal there.
      {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.last_error = report_result("qar_app_volumes_get_or_create", av);
      }
      std::fprintf(stderr,
        "[qar-adapter] NO render stream can be created without an app volume: "
        "QarRenderSenderInit::app_volume_id is required and NULL is rejected with "
        "QAR_STATUS_APP_VOLUME_INVALID_ID.\n");
      std::fflush(stderr);
      state.attach_state.store(AttachState::kFailed);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(state.mutex);
      state.volume = volume_id;
    }
    state.has_volume.store(true);

    // What the Hub holds for the volume at this moment. A volume that existed already keeps the
    // placement the operator gave it, so this line says whether the placement was restored. It
    // is the one query of the placement: after it the cache is kept by the update subscription.
    qar_ar_core::ScenePlacement placement;
    if (read_placement_by_query(session, volume_id, placement)) {
      {
        std::lock_guard<std::mutex> lk(state.placement_mutex);
        state.placement = placement;
        state.read_at = CrossThread::Clock::now();
        state.full_read_at = state.read_at;
      }
      std::fprintf(stderr,
        "[qar-adapter] app volume '%s' id=%s placement read back: volume pose=(%.2f, %.2f, %.2f) "
        "app_scale=%.3f (asked: centre_y=%.2f m, size=%.2fx%.2fx%.2f m, app_scale=%.3f)\n",
        common_name.c_str(), app_volume_id_to_hex(volume_id).c_str(),
        static_cast<double>(placement.volume_pose.position.x),
        static_cast<double>(placement.volume_pose.position.y),
        static_cast<double>(placement.volume_pose.position.z),
        static_cast<double>(placement.app_scale),
        static_cast<double>(config.app_volume_center_y_m),
        static_cast<double>(config.app_volume_width_m),
        static_cast<double>(config.app_volume_length_m),
        static_cast<double>(config.app_volume_height_m),
        static_cast<double>(config.app_volume_scale));
    } else {
      std::fprintf(stderr,
        "[qar-adapter] app volume '%s' id=%s: the placement could not be read back\n",
        common_name.c_str(), app_volume_id_to_hex(volume_id).c_str());
    }
    std::fflush(stderr);

    if (adapter_trace_on() && config.app_volume_pick_gestures) {
      // There is no get-handle-by-id call, so enumerate the session's app volumes and match
      // by id to read back what the runtime actually holds: that is what proves arbitration,
      // not what we intended to send it.
      std::size_t volume_count = 0;
      if (!qar_result_is_error(QAR_CALL(qar_query_app_volumes_count, session, &volume_count)) &&
        volume_count > 0)
      {
        std::vector<QarAppVolume *> volume_handles(volume_count, nullptr);
        std::size_t volumes_written = 0;
        if (!qar_result_is_error(QAR_CALL(
              qar_query_app_volumes, session, volume_handles.data(), volume_count,
              &volumes_written)))
        {
          bool read_back_logged = false;
          for (std::size_t i = 0; i < volumes_written && i < volume_handles.size(); ++i) {
            QarAppVolume * const handle = volume_handles[i];
            if (handle == nullptr) { continue; }
            if (!read_back_logged) {
              QarAppVolumeId id{};
              const bool is_ours =
                !qar_result_is_error(QAR_CALL(qar_app_volume_get_id, handle, &id)) &&
                std::memcmp(&id, &volume_id, sizeof(QarAppVolumeId)) == 0;
              QarAppVolumeGestureConfiguration read_back{};
              if (is_ours && !qar_result_is_error(
                    QAR_CALL(qar_app_volume_get_gesture_configuration, handle, &read_back)))
              {
                log_gesture_configuration("read-back", read_back);
                read_back_logged = true;
              }
            }
            QAR_CALL(qar_app_volume_handle_destroy, handle);
          }
        }
      }
    }

    // Changes made by the operator or by another peer reach the cache through the runtime's own
    // update subscription; the getters of the refresh task back it up.
    QarCancelToken * token = nullptr;
    if (!qar_result_is_error(QAR_CALL(qar_cancel_token_create, &token)) && token != nullptr) {
      auto * subscription = new PlacementSubscription();
      subscription->shared = job.shared;
      subscription->volume = volume_id;
      const QarResult subscribed = QAR_CALL(
          qar_app_volumes_subscribe_updates, session,
          &QarCApiSenderFactory::placement_update_trampoline, subscription, token);
      if (qar_result_is_error(subscribed)) {
        QAR_CALL(qar_cancel_token_handle_destroy, token);
        delete subscription;
        std::fprintf(stderr, "[qar-adapter] %s; the placement is refreshed with the getters\n",
          result_text("qar_app_volumes_subscribe_updates", subscribed).c_str());
        std::fflush(stderr);
      } else {
        state.token = token;
        state.subscription = subscription;
        std::lock_guard<std::mutex> lk(state.placement_mutex);
        state.subscribed = true;
      }
    }
  }

  // The requests that receivers make, for the log only: nothing is created from them.
  {
    QarCancelToken * token = nullptr;
    if (!qar_result_is_error(QAR_CALL(qar_cancel_token_create, &token)) && token != nullptr) {
      auto * subscription = new RequestSubscription();
      subscription->shared = job.shared;
      const QarResult subscribed = QAR_CALL(
          qar_render_sender_subscribe_requests, session,
          &QarCApiSenderFactory::render_request_trampoline, subscription, token);
      if (qar_result_is_error(subscribed)) {
        QAR_CALL(qar_cancel_token_handle_destroy, token);
        delete subscription;
        std::fprintf(stderr, "[qar-adapter] %s; the render requests are not logged\n",
          result_text("qar_render_sender_subscribe_requests", subscribed).c_str());
        std::fflush(stderr);
      } else {
        state.request_token = token;
        state.request_subscription = subscription;
      }
    }
  }

  {
    // Held with detach(): either the listener is registered before detach() retires this state
    // and removes it, or it is not registered.
    std::lock_guard<std::mutex> lk(state.listener_mutex);
    if (state.retired.load()) {
      run_placement_unsubscribe(job.shared);
      return;
    }
    if (job.owner != nullptr) {
      job.owner->set_target_listener(job.listener);
    }
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.last_error.clear();
    }
    state.attached.store(true);
    state.attach_state.store(AttachState::kAttached);
  }
}

void QarCApiSenderFactory::run_placement_unsubscribe(const std::shared_ptr<CrossThread> & shared)
{
  CrossThread & state = *shared;
  QarCancelToken * const token = state.token;
  PlacementSubscription * const subscription = state.subscription;
  state.token = nullptr;
  state.subscription = nullptr;
  {
    std::lock_guard<std::mutex> lk(state.placement_mutex);
    state.subscribed = false;
  }
  if (token != nullptr) {
    // Returns after a callback that is running has ended, so the context can go right after.
    QAR_CALL(qar_cancel_token_cancel, token);
    QAR_CALL(qar_cancel_token_handle_destroy, token);
  }
  delete subscription;
  run_request_unsubscribe(shared);
}

// True when the two placements differ by more than a millimetre or a thousandth of a scale (an
// unknown placement differs from every read).
static bool placement_differs(
  const qar_ar_core::ScenePlacement & a, const qar_ar_core::ScenePlacement & b)
{
  if (!a.known || !b.known) {
    return a.known != b.known;
  }
  const auto far_apart = [](float x, float y) { return std::fabs(x - y) > 0.001f; };
  const auto pose_far = [&far_apart](const QarPose & p, const QarPose & q) {
      return far_apart(p.position.x, q.position.x) || far_apart(p.position.y, q.position.y) ||
             far_apart(p.position.z, q.position.z) ||
             far_apart(p.orientation.x, q.orientation.x) ||
             far_apart(p.orientation.y, q.orientation.y) ||
             far_apart(p.orientation.z, q.orientation.z) ||
             far_apart(p.orientation.w, q.orientation.w);
    };
  return pose_far(a.volume_pose, b.volume_pose) || pose_far(a.app_pose, b.app_pose) ||
         far_apart(a.app_scale, b.app_scale);
}

void QarCApiSenderFactory::run_placement_refresh(
  QarSession * session, const std::shared_ptr<CrossThread> & shared)
{
  CrossThread & state = *shared;
  struct Done
  {
    CrossThread & state;
    ~Done() { state.refresh_in_flight.store(false); }
  } done{state};
  if (state.retired.load() || !state.has_volume.load()) {
    return;
  }
  QarAppVolumeId volume{};
  {
    std::lock_guard<std::mutex> lk(state.mutex);
    volume = state.volume;
  }
  const CrossThread::Clock::time_point start = CrossThread::Clock::now();
  QarPose volume_pose{};
  QarPose app_pose{};
  float app_scale = 1.0f;
  QarResult failure{};
  bool ok = true;
  const QarResult pose_result = QAR_CALL(
      qar_app_volume_get_latest_pose, session, &volume, &volume_pose);
  if (qar_result_is_error(pose_result)) {
    ok = false;
    failure = pose_result;
  }
  if (ok) {
    const QarResult result = QAR_CALL(
        qar_app_volume_get_latest_app_pose, session, &volume, &app_pose);
    if (qar_result_is_error(result)) {
      ok = false;
      failure = result;
    }
  }
  if (ok) {
    const QarResult result = QAR_CALL(
        qar_app_volume_get_latest_app_scale, session, &volume, &app_scale);
    if (qar_result_is_error(result)) {
      ok = false;
      failure = result;
    }
  }
  // Stores a value that was read and logs it, rate-limited, when it changes what was cached:
  // the values before and after and the source that answered. A value that only looks valid
  // (a default the runtime answers for a volume without a fast-path change) shows up here.
  const auto store = [&state](const qar_ar_core::ScenePlacement & after, const char * source) {
      qar_ar_core::ScenePlacement before;
      bool log_it = false;
      {
        std::lock_guard<std::mutex> lk(state.placement_mutex);
        before = state.placement;
        state.placement = after;
        state.placement.known = true;
        const CrossThread::Clock::time_point now = CrossThread::Clock::now();
        log_it = placement_differs(before, state.placement) &&
          (now - state.change_logged_at) >= std::chrono::seconds(5);
        if (log_it) {
          state.change_logged_at = now;
        }
      }
      if (log_it) {
        std::fprintf(stderr,
          "[qar-adapter] placement changed (%s)%s: volume pose (%.3f, %.3f, %.3f) -> "
          "(%.3f, %.3f, %.3f), app pose (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f), "
          "app_scale %.3f -> %.3f\n",
          source, before.known ? "" : " [first value]",
          static_cast<double>(before.volume_pose.position.x),
          static_cast<double>(before.volume_pose.position.y),
          static_cast<double>(before.volume_pose.position.z),
          static_cast<double>(after.volume_pose.position.x),
          static_cast<double>(after.volume_pose.position.y),
          static_cast<double>(after.volume_pose.position.z),
          static_cast<double>(before.app_pose.position.x),
          static_cast<double>(before.app_pose.position.y),
          static_cast<double>(before.app_pose.position.z),
          static_cast<double>(after.app_pose.position.x),
          static_cast<double>(after.app_pose.position.y),
          static_cast<double>(after.app_pose.position.z),
          static_cast<double>(before.app_scale), static_cast<double>(after.app_scale));
        std::fflush(stderr);
      }
    };
  bool note_failure = false;
  bool query_instead = false;
  if (ok) {
    qar_ar_core::ScenePlacement after;
    after.volume_pose = volume_pose;
    after.app_pose = app_pose;
    after.app_scale = app_scale;
    store(after, "qar_app_volume_get_latest_pose, _app_pose, _app_scale");
  } else {
    std::lock_guard<std::mutex> lk(state.placement_mutex);
    note_failure = !state.latest_failure_noted;
    state.latest_failure_noted = true;
    // Without the update subscription the getters are the only source: a query then keeps the
    // cache from going stale, at most every 10 s. A placement that is not known yet is read by
    // a query as well, because the getters may have nothing to answer with.
    query_instead = (!state.subscribed || !state.placement.known) &&
      (CrossThread::Clock::now() - state.full_read_at) >= std::chrono::seconds(10);
  }
  if (note_failure) {
    std::fprintf(stderr, "[qar-adapter] %s; the placement relies on the update subscription\n",
      result_text("qar_app_volume_get_latest_*", failure).c_str());
    std::fflush(stderr);
  }
  if (!ok && query_instead) {
    qar_ar_core::ScenePlacement placement;
    if (read_placement_by_query(session, volume, placement)) {
      store(placement, "qar_query_app_volumes");
      {
        std::lock_guard<std::mutex> lk(state.placement_mutex);
        state.full_read_at = CrossThread::Clock::now();
      }
      ok = true;
    }
  }
  const CrossThread::Clock::time_point end = CrossThread::Clock::now();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  std::lock_guard<std::mutex> lk(state.placement_mutex);
  state.read_at = end;
  // A refresh that is slow is asked for less often, so it cannot keep the thread busy: it
  // may take at most a twentieth of the time.
  state.refresh_interval = std::min(
    std::max(std::chrono::milliseconds(1000), took * 20), std::chrono::milliseconds(30000));
  state.refresh_not_before = ok ? end : end + std::chrono::seconds(10);
}

void QarCApiSenderFactory::detach()
{
  // The tasks of the state that is left behind see `retired` and undo themselves. The next
  // attach starts from a fresh state.
  const std::shared_ptr<CrossThread> left = shared_;
  {
    std::lock_guard<std::mutex> lk(left->listener_mutex);
    left->retired.store(true);
  }
  shared_ = std::make_shared<CrossThread>();

  // Removing the listener returns after a delivery in progress ended, so the factory is not used
  // by a QAR thread afterwards and may be released by the caller.
  if (const std::shared_ptr<QarSessionOwner> owner = owner_.lock()) {
    owner->set_target_listener(std::weak_ptr<ITargetListener>());
    // The update subscription ends on the thread that owns the session, behind an attach that
    // may still be running. When the post is refused (the session ended on its own) the
    // subscription is not cancelled here: the destruction of the session ends it, and the
    // senders with it.
    owner->post_session_task(
      "placement-unsubscribe", [left](QarSession *) { run_placement_unsubscribe(left); });
  }
  left->attached.store(false);
  for (const std::shared_ptr<PendingSender> & pending : pending_senders_) {
    abandon_pending(*pending);
  }
  pending_senders_.clear();
  std::lock_guard<std::mutex> lk(mutex_);
  pending_requests_.clear();
  served_peers_.clear();
  closed_peers_.clear();
  adapters_.clear();
  retries_.clear();
}

QarCApiSenderFactory::~QarCApiSenderFactory()
{
  // The gate opens for good; the watch thread ends by itself and is joined by a later reap().
  gpu_watch_->stop();
  detach();
  // The per-sender contexts go last: every sender of this factory is gone by now (the producer
  // destroys its manager before it releases the factory), but the streams of those senders are
  // destroyed by the thread that owns the session, behind the lane item posted here, and a QAR
  // graphics thread may bind a context until then. The contexts wait in the graveyard for that
  // item. Without a lane every stream was destroyed already, so they are deleted at once.
  GlContextList without_stream;
  for (auto & ctx : gl_sender_contexts_) {
    if (ctx == nullptr) {
      continue;
    }
    if (ctx->stream_released != nullptr) {
      // The release of its own stream sets the flag, whenever the frame worker posts it.
      std::lock_guard<std::mutex> lk(graveyard_mutex());
      GlContextList one;
      const std::shared_ptr<std::atomic<bool>> flag = ctx->stream_released;
      one.push_back(std::move(ctx));
      graveyard().push_back(GlGraveyardEntry{flag, std::move(one)});
    } else {
      without_stream.push_back(std::move(ctx));
    }
  }
  gl_sender_contexts_.clear();
  if (!without_stream.empty()) {
    const auto released = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lk(graveyard_mutex());
      graveyard().push_back(GlGraveyardEntry{released, std::move(without_stream)});
    }
    mark_context_released(stream_lane(), released);
  }
  delete_released_gl_contexts();
}

bool QarCApiSenderFactory::scene_placement(qar_ar_core::ScenePlacement & out) const
{
  const std::shared_ptr<CrossThread> shared = shared_;
  if (!shared->has_volume.load()) {
    return false;
  }
  bool refresh = false;
  bool known = false;
  {
    std::lock_guard<std::mutex> lk(shared->placement_mutex);
    known = shared->placement.known;
    if (known) {
      out = shared->placement;
    }
    // Asked for also while nothing is known: a read that failed at the attach is tried again,
    // paced like every refresh.
    const CrossThread::Clock::time_point now = CrossThread::Clock::now();
    refresh = (now - shared->read_at) >= shared->refresh_interval &&
      now >= shared->refresh_not_before;
  }
  if (refresh && shared->attached.load() && !shared->retired.load() &&
    !shared->refresh_in_flight.exchange(true))
  {
    const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
    const bool posted = owner != nullptr && owner->post_session_task(
      "placement-refresh",
      [shared](QarSession * session) { run_placement_refresh(session, shared); },
      [shared] { shared->refresh_in_flight.store(false); });
    if (!posted) {
      shared->refresh_in_flight.store(false);
    }
  }
  return known;
}

void QarCApiSenderFactory::delete_contexts_of_released_streams()
{
  void (* deleter)(void *) = nullptr;
  {
    std::lock_guard<std::mutex> lk(graveyard_mutex());
    deleter = gl_context_deleter() != nullptr ? gl_context_deleter() : &delete_gl_context;
  }
  for (auto & ctx : gl_sender_contexts_) {
    if (ctx != nullptr && ctx->context != nullptr && ctx->stream_released != nullptr &&
      ctx->stream_released->load())
    {
      // No stream uses the context any more: the lane destroyed it, and no graphics thread of
      // the runtime binds the context after that.
      deleter(ctx->context);
      ctx->context = nullptr;
    }
  }
}

bool QarCApiSenderFactory::capture_gl_context()
{
  // On the render thread: the contexts of an earlier factory are deleted once their streams are,
  // and the frame workers of released senders that have ended are joined. While the GPU progress
  // gate is closed no context is deleted: a context that GPU work still refers to may make the
  // delete wait, so it is left for the next tick that finds the gate open.
  const bool gate_closed = gpu_watch_->any_gate_closed();
  if (!gate_closed) { delete_released_gl_contexts(); }
  FrameWorker::reap();
  GpuProgressWatch::reap();
  if (!gate_closed) { delete_contexts_of_released_streams(); }
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
    // The progress watch follows the frames of this context from a thread of its own; it starts
    // once, with the first context that is latched.
    gpu_watch_->start(gl_device_context_, gl_render_context_);
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
  if (state == nullptr || w.MakeCurrent == nullptr) {
    return false;
  }
  // This sender's OWN context on the device context it was created on, both stored in the
  // sender's state when it was created. The factory's latched handles are not read: the render
  // thread rewrites them every tick and this runs on the runtime's graphics thread. Falling
  // back to Ogre's context would put it current on a second thread, which WGL forbids, so a
  // sender without its own pair binds nothing.
  void * hdc = nullptr;
  void * ctx = nullptr;
  if (!state->own_handles(hdc, ctx)) { return false; }
  if (w.MakeCurrent(static_cast<HDC>(hdc), static_cast<HGLRC>(ctx)) != FALSE) {
    state->bound_by.store(thread_number(), std::memory_order_relaxed);
    // Once per sender: which thread of the runtime binds which context. A short write on that
    // thread, the same as the failure line below.
    if (state->bound.first()) {
      std::fprintf(
        stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
        format_sdk_bound_line(state->peer, ctx, hdc, thread_number()).c_str());
      std::fflush(stderr);
    }
    return true;
  }
  // Loud, because the multi-peer failure mode looks exactly like this: a context is
  // already current on another thread, so every later GL lookup returns null and the
  // runtime tears the stream down with an unrelated-sounding message.
  const unsigned long error_code = GetLastError();
  std::fprintf(stderr,
    "[qar-adapter] %s *** wglMakeCurrent FAILED (err=%lu) tid=%llu ctx held by tid=%llu "
    "HDC=%p HGLRC=%p — is this context current on another thread?\n",
    wall_clock_hms_ms().c_str(), error_code, thread_number(),
    state->bound_by.load(std::memory_order_relaxed), hdc, ctx);
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
  if (!ready() || !shared_->attached.load()) { return nullptr; }
  QarSession * const open_session = session();
  const std::shared_ptr<IReleaseLane> lane = stream_lane();

  // ---------------------------------------------------------------------------
  // ASYNC creation. Creating a sender negotiates a transfer with the
  // receiver and is not bounded in time: it can take arbitrarily long - or never
  // finish: an OpenGL sender against the Vulkan-only mixer parks in "waiting for
  // receiver borrow signal" forever. Because create_sender()
  // is driven from the panel's QTimer tick, the synchronous call would freeze the whole of RViz.
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
      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      const double waited = std::chrono::duration<double>(now - p->started).count();
      if (!p->warned && waited > kPendingSenderWarnSeconds) {
        p->warned = true;
        std::fprintf(stderr,
          "[qar-adapter] create_sender: peer %s still creating after %.1f s — the "
          "receiver has not completed transfer negotiation (the render tick keeps running).\n",
          qar_ogre_streaming::peer_id_to_hex(peer_id).c_str(), waited);
        std::fflush(stderr);
      }
      if (p->deadline != std::chrono::steady_clock::time_point{} && now >= p->deadline) {
        give_up_waiting(it, waited);
      }
      return nullptr;
    }

    QarRenderSender * const stream = p->stream.exchange(nullptr);
    const bool failed = p->failed.load(std::memory_order_acquire);
    pending_senders_.erase(it);
    release_token(*p, /*cancel=*/false);
    if (failed || stream == nullptr) {
      // No stream will use the context of this creation.
      if (stream == nullptr) {
        mark_context_released(lane, p->context_released);
      }
      forget_retry(peer_id);
      std::fprintf(stderr, "[qar-adapter] create_sender: async creation FAILED for peer %s\n",
        qar_ogre_streaming::peer_id_to_hex(peer_id).c_str());
      std::fflush(stderr);
      return nullptr;
    }
    trace_msg("create_sender: <- qar_render_sender_create_async (ready)");
    // Which thread of the runtime delivered the sender, and how long after the request. The
    // callback only recorded its thread; the line is printed here, on the render thread.
    std::fprintf(
      stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
      format_created_line(
        qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8),
        p->callback_thread.load(std::memory_order_acquire),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - p->started).count())
      .c_str());
    std::fflush(stderr);
    if (lane != nullptr) {
      // The id of this sender is read on the owner's thread. The release of the stream is posted
      // later and runs behind this task, so the stream is alive when it runs.
      const std::shared_ptr<CrossThread> shared = shared_;
      const std::string peer8 = qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8);
      lane->post_release("render-stream-id", [shared, stream, peer8] {
          QarStreamId id{};
          if (qar_result_is_error(QAR_CALL(qar_render_sender_stream_id, stream, &id))) {
            return;
          }
          std::lock_guard<std::mutex> lk(shared->request_mutex);
          shared->our_streams[peer8] = id_first8(id.data);
        });
    }
    auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
      stream, config_.near_far, config_.submit_mode, config_.copy_hook, config_.depth_scale,
      pointing_ray_slot_, peer_id, config_.log_folder_path, lane, p->context_released,
      config_.frame_calls_on_worker, config_.frame_worker_timings,
      FrameFenceSetup{config_.gl_fence_ops, p->fence_context, config_.frame_fence_timings},
      config_.render_recycle, recycle_count_of(peer_id));
    {
      std::lock_guard<std::mutex> lk(mutex_);
      adapters_.push_back(PeerAdapter{peer_id, adapter});
    }
    forget_retry(peer_id);
    return adapter;
  }

  // A creation that was cancelled for want of an answer is tried again after a back-off, and not
  // at all once it was cancelled too often, until the target goes down and up again.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const CreateRetry & retry : retries_) {
      if (peer_eq(retry.peer_id, peer_id) &&
        (retry.given_up || std::chrono::steady_clock::now() < retry.retry_at))
      {
        return nullptr;
      }
    }
  }

  // No GL context is made while the GPU progress gate is closed (wglCreateContext and
  // wglShareLists may wait for a stalled GPU): the creation is tried again on a later tick.
  if (gpu_watch_->any_gate_closed()) {
    const unsigned n = ++gate_waits_;
    if (n <= 5 || (n % 100) == 0) {
      std::fprintf(
        stderr,
        "[qar-adapter] create_sender: the GPU progress gate is closed; the stream of this peer "
        "is created when it opens\n");
      std::fflush(stderr);
    }
    return nullptr;
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
  // One struct PER CONTEXT TYPE: the Egl, Wgl and Glx variants of
  // QarStreamParamsOpenGl, with no context-type discriminator. We own a WGL context,
  // so this is the Wgl one and the context type is the struct's identity.
  QarStreamParamsOpenGlWgl gl_params = qar_stream_params_opengl_wgl_default();
  GlSenderContext * created_context = nullptr;
#ifdef _WIN32
  if (!chain_gl_context_) {
    // Nothing is chained: the creation goes to the runtime as it is.
  } else if (capture_gl_context()) {
    // Mint a context for THIS sender. Never Ogre's live one (it is permanently current
    // on the render thread) and never one already handed to another sender: QAR binds
    // it on that sender's own graphics thread, and a WGL context cannot be current on
    // two threads at once. All of them share Ogre's object namespace, so every peer
    // still sees the same textures.
    auto sender_ctx = std::make_unique<GlSenderContext>();
    sender_ctx->factory = this;
    // The device context the new context is created on (create_shared_gl_context() reads the
    // same member on this thread): kept with it, because the runtime's thread binds the pair.
    sender_ctx->hdc = gl_device_context_;
    sender_ctx->peer = qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8);
    sender_ctx->context = create_shared_gl_context();
    if (sender_ctx->context == nullptr) {
      std::fprintf(stderr,
        "[qar-adapter] create_sender: could not create a GL context for this peer; "
        "refusing rather than sharing another sender's.\n");
      std::fflush(stderr);
      return nullptr;
    }
    GlSenderContext * ctx = sender_ctx.get();
    created_context = ctx;
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

  // The context that the frame worker of this sender binds for the fences of its copies. It is
  // made here, on the render thread, from the latched context like the one of the runtime; the
  // worker binds it once and deletes it when it leaves. Without it the sender waits for the GPU
  // on the render thread.
  std::shared_ptr<FenceContext> fence_context;
  if (config_.frame_fence_on_worker && config_.frame_calls_on_worker && lane != nullptr &&
    config_.gl_fence_ops.complete())
  {
    void * const made = config_.gl_fence_ops.create_context(
      gl_device_context_, gl_render_context_);
    if (made != nullptr) {
      fence_context = std::make_shared<FenceContext>(
        config_.gl_fence_ops, gl_device_context_, made);
    } else if (gl_device_context_ != nullptr) {
      std::fprintf(
        stderr, "[qar-adapter] create_sender: could not create a GL context for the fence waits "
        "of this peer (err=%d); the sender waits for the GPU on the GUI thread\n",
        config_.gl_fence_ops.last_error());
      std::fflush(stderr);
    }
  }

  init.enable_auto_reconnects = true;
  // Key this stream to our app volume. REQUIRED by the installed runtime: the
  // field is documented "Required ... NULL is rejected with
  // QAR_STATUS_APP_VOLUME_INVALID_ID", and "a stream without one is not
  // positioned in the scene and no viewer can show it".
  //
  // There is no NULL fallback: the runtime rejects the call outright, so a
  // fallback would not degrade to "works without the mixer", it would degrade to
  // "no stream at all, with a confusing error". Refusing HERE turns it into a
  // legible message on the tick that would have failed, and create_sender is
  // retried every render tick, so a volume that appears later still works.
  QarAppVolumeId stream_volume{};
  if (!app_volume(stream_volume)) {
    std::fprintf(stderr,
      "[qar-adapter] create_sender: no app volume, and QarRenderSenderInit::app_volume_id "
      "is REQUIRED by the QAROS runtime (NULL => QAR_STATUS_APP_VOLUME_INVALID_ID). "
      "%s\n",
      config_.create_app_volume
        ? "Volume creation failed at connect - see the qar_app_volumes_add_volume error above."
        : "Config::create_app_volume is off; turn it on - there is no NULL-volume path.");
    std::fflush(stderr);
    return nullptr;
  }
  app_volume_ = stream_volume;
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
  pending->deadline = pending->started + config_.create_timeout;
  pending->lane = lane;
  pending->fence_context = fence_context;
  if (created_context != nullptr) {
    // The release of the stream of this creation, whoever ends up holding it, frees the context.
    created_context->stream_released = std::make_shared<std::atomic<bool>>(false);
    pending->context_released = created_context->stream_released;
  }
  if (lane != nullptr) {
    // The token lets a creation that never answers be cancelled. Making one is a call on an
    // object of this process; the cancel and the destroy go to the lane.
    QarCancelToken * token = nullptr;
    const QarResult made = QAR_CALL(qar_cancel_token_create, &token);
    pending->token.store(qar_result_is_error(made) ? nullptr : token);
  }
  pending->init = init;
  pending->gl_params = gl_params;
  // Re-chain onto the HEAP copy — `init.header.next` still points at the stack
  // gl_params, which dies with this frame.
  if (init.header.next != nullptr) {
    pending->init.header.next = &pending->gl_params.header;
  }

  trace_msg("create_sender: -> qar_render_sender_create_async");
  const QarResult r = QAR_CALL(
      qar_render_sender_create_async, open_session, &pending->init,
      &QarCApiSenderFactory::sender_created_trampoline, new std::shared_ptr<PendingSender>(pending),
      pending->token.load());
  trace_msg("create_sender: <- qar_render_sender_create_async (kicked off)");
  if (qar_result_is_error(r)) {
    std::fprintf(stderr, "[qar-adapter] %s\n",
      report_result("qar_render_sender_create_async", r).c_str());
    std::fflush(stderr);
    release_token(*pending, /*cancel=*/false);
    mark_context_released(lane, pending->context_released);
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    CreateRetry * entry = nullptr;
    for (CreateRetry & retry : retries_) {
      if (peer_eq(retry.peer_id, peer_id)) {
        entry = &retry;
        break;
      }
    }
    if (entry == nullptr) {
      retries_.push_back(CreateRetry{});
      entry = &retries_.back();
      entry->peer_id = peer_id;
    }
    ++entry->started;
  }
  // The request is in: say which thread made it. The runtime answers on a thread of its own, and
  // the line printed when it does names that one (see the poll above).
  std::fprintf(
    stderr, "[qar-adapter] %s %s\n", wall_clock_hms_ms().c_str(),
    format_create_async_line(
      qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8), thread_number()).c_str());
  std::fflush(stderr);
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

  p.stream.store(stream);
  p.failed.store(qar_result_is_error(status) || stream == nullptr,
    std::memory_order_relaxed);
  // Only the number is kept: the render tick prints it, a callback of the runtime does no I/O.
  p.callback_thread.store(thread_number(), std::memory_order_relaxed);
  p.done.store(true);
  // A creation that was given up has no reader: the stream is handed to the release lane (or
  // destroyed here when the creation has none). Both sides set their flag first and look at the
  // other's second, and whoever takes the stream owns it.
  if (p.abandoned.load()) {
    if (QarRenderSender * const late = p.stream.exchange(nullptr)) {
      release_stream(p, late);
    } else if (stream == nullptr) {
      mark_context_released(p.lane, p.context_released);
    }
  }
}

void QarCApiSenderFactory::abandon_pending(PendingSender & pending, bool cancel)
{
  pending.abandoned.store(true);
  const bool answered = pending.done.load();
  if (answered) {
    if (QarRenderSender * const stream = pending.stream.exchange(nullptr)) {
      release_stream(pending, stream);
    } else {
      mark_context_released(pending.lane, pending.context_released);
    }
  }
  release_token(pending, cancel && !answered);
}

void QarCApiSenderFactory::release_stream(PendingSender & pending, QarRenderSender * stream)
{
  if (pending.lane == nullptr) {
    QAR_CALL(qar_render_stream_handle_destroy, stream);
    if (pending.context_released != nullptr) { pending.context_released->store(true); }
    return;
  }
  const std::shared_ptr<std::atomic<bool>> released = pending.context_released;
  if (!pending.lane->post_release("render-stream-late", [stream, released] {
        QAR_CALL(qar_render_stream_handle_destroy, stream);
        if (released != nullptr) { released->store(true); }
      }))
  {
    std::fprintf(
      stderr, "[qar-adapter] %s peer %s stream handle left unreleased (the owner thread is gone)\n",
      wall_clock_hms_ms().c_str(),
      qar_ogre_streaming::peer_id_to_hex(pending.peer_id).substr(0, 8).c_str());
    std::fflush(stderr);
  }
}

void QarCApiSenderFactory::mark_context_released(
  const std::shared_ptr<IReleaseLane> & lane, const std::shared_ptr<std::atomic<bool>> & flag)
{
  if (flag == nullptr) {
    return;
  }
  if (lane == nullptr) {
    flag->store(true);
    return;
  }
  // A refused post leaves the flag unset: nothing is known about the streams, so the context is
  // never deleted.
  lane->post_release("gl-context-released", [flag] { flag->store(true); });
}

void QarCApiSenderFactory::release_token(PendingSender & pending, bool cancel)
{
  QarCancelToken * const token = pending.token.exchange(nullptr);
  if (token == nullptr) {
    return;
  }
  const auto release = [token, cancel] {
      if (cancel) { QAR_CALL(qar_cancel_token_cancel, token); }
      QAR_CALL(qar_cancel_token_handle_destroy, token);
    };
  if (pending.lane == nullptr) {
    release();
    return;
  }
  // A refused post leaves the token as it is: no thread is left to cancel it.
  pending.lane->post_release("render-create-token", release);
}

void QarCApiSenderFactory::give_up_waiting(
  std::vector<std::shared_ptr<PendingSender>>::iterator it, double waited_s)
{
  const std::shared_ptr<PendingSender> pending = *it;
  const QarPeerId peer_id = pending->peer_id;
  pending_senders_.erase(it);
  abandon_pending(*pending, /*cancel=*/true);

  int attempt = 0;
  bool giving_up = false;
  long long next_ms = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    CreateRetry * entry = nullptr;
    for (CreateRetry & retry : retries_) {
      if (peer_eq(retry.peer_id, peer_id)) {
        entry = &retry;
        break;
      }
    }
    if (entry == nullptr) {
      retries_.push_back(CreateRetry{});
      entry = &retries_.back();
      entry->peer_id = peer_id;
      entry->started = 1;
    }
    ++entry->cancelled;
    attempt = entry->started;
    if (entry->cancelled > config_.create_give_up_after) {
      entry->given_up = true;
      giving_up = true;
    } else {
      std::chrono::milliseconds wait = config_.create_backoff_first;
      for (int i = 1; i < entry->cancelled && wait < config_.create_backoff_cap; ++i) {
        wait *= 2;
      }
      wait = std::min(wait, config_.create_backoff_cap);
      next_ms = wait.count();
      entry->retry_at = std::chrono::steady_clock::now() + wait;
    }
  }
  std::string line = "[qar-adapter] " + wall_clock_hms_ms() + " peer " +
    qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8) + " create_async gave no answer in ";
  char number[32];
  std::snprintf(number, sizeof(number), "%.1f", waited_s);
  line += number;
  line += " s; abandoned (attempt " + std::to_string(attempt) + "), ";
  line += giving_up ? std::string("giving up until the target returns")
    : "next try in " + std::to_string(next_ms) + " ms";
  std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
}

void QarCApiSenderFactory::forget_retry(const QarPeerId & peer_id)
{
  std::lock_guard<std::mutex> lk(mutex_);
  retries_.erase(
    std::remove_if(retries_.begin(), retries_.end(),
      [&peer_id](const CreateRetry & r) { return peer_eq(r.peer_id, peer_id); }),
    retries_.end());
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

bool QarCApiSenderFactory::take_geo_anchor_result(std::string & out_error)
{
  std::lock_guard<std::mutex> lk(shared_->mutex);
  if (!shared_->geo_result_ready) {
    return false;
  }
  out_error = std::move(shared_->geo_result);
  shared_->geo_result.clear();
  shared_->geo_result_ready = false;
  return true;
}

std::string QarCApiSenderFactory::set_geo_anchor_ecef(
  const double origin_xyz[3],
  const double axis_x[3], const double axis_y[3], const double axis_z[3])
{
  if (!ready()) { return "set_geo_anchor: no session"; }
  if (!shared_->has_volume.load()) {
    // The anchor is a property OF a volume, so without one there is nothing to
    // georeference. Worth saying plainly: Config::create_app_volume = false silently
    // makes geolocation impossible rather than merely unhelpful.
    return "set_geo_anchor: no app volume (Config::create_app_volume is off?)";
  }

  QarGeoAnchorFrame frame{};
  frame.world_ref_system = QAR_WORLD_REF_SYSTEM_ECEF_WGS84_METERS;
  frame.origin_world = QarVector3d{origin_xyz[0], origin_xyz[1], origin_xyz[2]};
  frame.axis_x_world = QarVector3d{axis_x[0], axis_x[1], axis_x[2]};
  frame.axis_y_world = QarVector3d{axis_y[0], axis_y[1], axis_y[2]};
  frame.axis_z_world = QarVector3d{axis_z[0], axis_z[1], axis_z[2]};
  // ENU (East, North, Up) is right-handed, and so is the ROS frame it comes from.
  frame.handedness = QAR_HANDEDNESS_RIGHT;
  return queue_geo_request(false, &frame);
}

std::string QarCApiSenderFactory::clear_geo_anchor()
{
  if (!ready() || !shared_->has_volume.load()) { return "clear_geo_anchor: no volume"; }
  return queue_geo_request(true, nullptr);
}

std::string QarCApiSenderFactory::queue_geo_request(bool clear, const QarGeoAnchorFrame * frame)
{
  const std::shared_ptr<CrossThread> shared = shared_;
  bool post = false;
  {
    // A request that has not run yet is replaced by the newer one; one task is queued at most.
    std::lock_guard<std::mutex> lk(shared->mutex);
    shared->geo_pending = true;
    shared->geo_clear = clear;
    if (frame != nullptr) {
      shared->geo_frame = *frame;
    }
    post = !shared->geo_task_queued;
    shared->geo_task_queued = true;
  }
  if (!post) {
    return {};
  }
  const bool posted = post_session_task(
    "geo-anchor", [shared](QarSession * session) { run_geo_anchor(session, shared); },
    [shared] {
      std::lock_guard<std::mutex> lk(shared->mutex);
      shared->geo_task_queued = false;
      shared->geo_pending = false;
      shared->geo_result = "the QarOS session ended before the anchor change ran";
      shared->geo_result_ready = true;
    });
  if (!posted) {
    std::lock_guard<std::mutex> lk(shared->mutex);
    shared->geo_task_queued = false;
    shared->geo_pending = false;
    return clear ? "clear_geo_anchor: no session" : "set_geo_anchor: no session";
  }
  return {};
}

void QarCApiSenderFactory::run_geo_anchor(
  QarSession * session, const std::shared_ptr<CrossThread> & shared)
{
  CrossThread & state = *shared;
  bool clear = false;
  QarGeoAnchorFrame frame{};
  QarAppVolumeId volume{};
  {
    std::lock_guard<std::mutex> lk(state.mutex);
    state.geo_task_queued = false;
    if (!state.geo_pending) {
      return;
    }
    state.geo_pending = false;
    clear = state.geo_clear;
    frame = state.geo_frame;
    volume = state.volume;
  }
  std::string outcome;
  if (clear) {
    const QarResult r = QAR_CALL(qar_app_volumes_clear_app_world_anchor, session, &volume);
    if (qar_result_is_error(r)) {
      outcome = report_result("qar_app_volumes_clear_app_world_anchor", r);
    } else {
      state.has_geo_anchor.store(false);
    }
  } else {
    trace_msg("-> qar_app_volumes_change_app_world_anchor");
    const QarResult r = QAR_CALL(qar_app_volumes_change_app_world_anchor, session, &volume, &frame);
    trace_msg("<- qar_app_volumes_change_app_world_anchor");
    if (qar_result_is_error(r)) {
      state.has_geo_anchor.store(false);
      outcome = report_result("qar_app_volumes_change_app_world_anchor", r);
    } else {
      state.has_geo_anchor.store(true);
    }
  }
  std::lock_guard<std::mutex> lk(state.mutex);
  state.geo_result = std::move(outcome);
  state.geo_result_ready = true;
}

void QarCApiSenderFactory::on_render_request(const QarPeerId & peer_id)
{
  std::lock_guard<std::mutex> lk(mutex_);
  // Targets surface repeatedly and the producer re-queues a peer whose stream creation must be
  // retried. Keep the queue free of duplicates; it is drained every render tick anyway.
  for (const QarPeerId & queued : pending_requests_) {
    if (peer_eq(queued, peer_id)) {
      return;
    }
  }
  pending_requests_.push_back(peer_id);
}

void QarCApiSenderFactory::on_target_up(const TargetInfo & target)
{
  std::lock_guard<std::mutex> lk(mutex_);
  const bool known = std::any_of(served_peers_.begin(), served_peers_.end(),
      [&target](const QarPeerId & p) { return peer_eq(p, target.id); });
  if (!known) {
    // Every peer costs four scene renders per frame (color and depth for each of two eyes), so
    // refuse loudly past the ceiling rather than quietly turning the render tick into a
    // slideshow.
    if (served_peers_.size() >= config_.max_peers) {
      std::fprintf(stderr,
        "[qar-adapter] *** peer %s refused: already streaming to %zu peers (max_peers=%u)\n",
        qar_ogre_streaming::peer_id_to_hex(target.id).c_str(), served_peers_.size(),
        config_.max_peers);
      std::fflush(stderr);
      return;
    }
    served_peers_.push_back(target.id);
  }
  for (const QarPeerId & queued : pending_requests_) {
    if (peer_eq(queued, target.id)) {
      return;
    }
  }
  pending_requests_.push_back(target.id);
}

std::shared_ptr<RenderRecycleCount> QarCApiSenderFactory::recycle_count_of(
  const QarPeerId & peer_id)
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const RecycleEntry & entry : recycle_counts_) {
    if (peer_eq(entry.peer_id, peer_id)) { return entry.count; }
  }
  recycle_counts_.push_back(RecycleEntry{peer_id, std::make_shared<RenderRecycleCount>(0u)});
  return recycle_counts_.back().count;
}

void QarCApiSenderFactory::on_target_down(const QarPeerId & id, QarPeerPresence presence)
{
  (void)presence;
  std::lock_guard<std::mutex> lk(mutex_);
  // A target that comes back starts with the first limit again.
  recycle_counts_.erase(
    std::remove_if(recycle_counts_.begin(), recycle_counts_.end(),
      [&id](const RecycleEntry & e) { return peer_eq(e.peer_id, id); }),
    recycle_counts_.end());
  // The tries for a target that goes down start afresh when it comes up.
  retries_.erase(
    std::remove_if(retries_.begin(), retries_.end(),
      [&id](const CreateRetry & r) { return peer_eq(r.peer_id, id); }),
    retries_.end());
  pending_requests_.erase(
    std::remove_if(pending_requests_.begin(), pending_requests_.end(),
      [&id](const QarPeerId & p) { return peer_eq(p, id); }),
    pending_requests_.end());
  const bool known = std::any_of(served_peers_.begin(), served_peers_.end(),
      [&id](const QarPeerId & p) { return peer_eq(p, id); });
  if (!known) {
    return;
  }
  // The producer closes the stream through poll_closed_peers() and then calls forget().
  const bool listed = std::any_of(closed_peers_.begin(), closed_peers_.end(),
      [&id](const QarPeerId & p) { return peer_eq(p, id); });
  if (!listed) {
    closed_peers_.push_back(id);
  }
}

std::size_t QarCApiSenderFactory::served_peer_count() const
{
  std::lock_guard<std::mutex> lk(mutex_);
  return served_peers_.size();
}

std::vector<std::string> QarCApiSenderFactory::note_hub_process(const HubProcessInfo & info)
{
  std::vector<std::string> lines;
  const std::shared_ptr<QarSessionOwner> owner = owner_.lock();
  const bool running = owner != nullptr && owner->state() == SessionState::kRunning;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!hub_seeded_) {
    hub_seeded_ = true;
    hub_gone_seen_ = info.gone_count;
    return lines;
  }
  if (info.gone_count < hub_gone_seen_) {
    // The owner made a fresh implementation: its counter starts at zero again, and nothing ended.
    hub_gone_seen_ = info.gone_count;
  }
  if (info.gone_count != hub_gone_seen_) {
    hub_gone_seen_ = info.gone_count;
    std::string creating;
    for (const std::shared_ptr<PendingSender> & pending : pending_senders_) {
      creating += (creating.empty() ? "" : ", ") +
        qar_ogre_streaming::peer_id_to_hex(pending->peer_id).substr(0, 8);
    }
    if (!creating.empty()) {
      lines.push_back(
        "the QAROS Hub process ended (pid " + std::to_string(info.last_gone_pid) +
        ") while the 3D sender of " + creating + " was being created: that creation is not "
        "held");
    }
    if (!adapters_.empty()) {
      hub_hold_ = true;
      hub_old_pid_ = info.last_gone_pid;
      std::string peers;
      for (const PeerAdapter & pa : adapters_) {
        const bool listed = std::any_of(closed_peers_.begin(), closed_peers_.end(),
            [&pa](const QarPeerId & p) { return peer_eq(p, pa.peer_id); });
        if (!listed) {
          closed_peers_.push_back(pa.peer_id);
        }
        peers += (peers.empty() ? "" : ", ") +
          qar_ogre_streaming::peer_id_to_hex(pa.peer_id).substr(0, 8);
      }
      lines.push_back(
        "the QAROS Hub process ended (pid " + std::to_string(info.last_gone_pid) +
        "): the 3D sender of " + peers + " is released now and made again when the target is "
        "online");
    }
  }
  if (hub_hold_ && info.pid != 0 && info.pid != hub_old_pid_ && running) {
    hub_hold_ = false;
    lines.push_back(
      "the QAROS Hub restarted (pid " + std::to_string(hub_old_pid_) + " -> " +
      std::to_string(info.pid) + "): the 3D sender is made again when the target is online");
  }
  return lines;
}

bool QarCApiSenderFactory::senders_held_for_hub() const
{
  std::lock_guard<std::mutex> lk(mutex_);
  return hub_hold_;
}

void QarCApiSenderFactory::poll_new_peer_requests(std::vector<QarPeerId> & out)
{
  std::lock_guard<std::mutex> lk(mutex_);
  if (hub_hold_) {
    return;  // the requests wait for the new Hub process
  }
  out.insert(out.end(), pending_requests_.begin(), pending_requests_.end());
  pending_requests_.clear();
}

void QarCApiSenderFactory::poll_closed_peers(std::vector<QarPeerId> & out)
{
  std::lock_guard<std::mutex> lk(mutex_);
  for (const QarPeerId & id : closed_peers_) {
    out.push_back(id);
  }
  for (const PeerAdapter & pa : adapters_) {
    auto sp = pa.adapter.lock();
    if (sp && sp->is_closed()) {
      const bool listed = std::any_of(out.begin(), out.end(),
          [&pa](const QarPeerId & p) { return peer_eq(p, pa.peer_id); });
      if (!listed) {
        out.push_back(pa.peer_id);
      }
    }
  }
}

void QarCApiSenderFactory::forget(const QarPeerId & peer_id)
{
  {
    // The sender of the peer is gone: its stream id is not "ours" any more, so a request for the
    // old id before the next sender exists is not printed as a match.
    std::lock_guard<std::mutex> request_lock(shared_->request_mutex);
    shared_->our_streams.erase(qar_ogre_streaming::peer_id_to_hex(peer_id).substr(0, 8));
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = adapters_.begin(); it != adapters_.end(); ++it) {
      if (peer_eq(it->peer_id, peer_id)) { adapters_.erase(it); break; }
    }
    closed_peers_.erase(
      std::remove_if(closed_peers_.begin(), closed_peers_.end(),
        [&peer_id](const QarPeerId & p) { return peer_eq(p, peer_id); }),
      closed_peers_.end());
    // A request that the producer queued again for a peer that went down in the same tick
    // would be asked for every tick for good: nothing serves the peer any more, and a later
    // on_target_down ignores a peer that is not served. A target that is still here is
    // requested again below.
    pending_requests_.erase(
      std::remove_if(pending_requests_.begin(), pending_requests_.end(),
        [&peer_id](const QarPeerId & p) { return peer_eq(p, peer_id); }),
      pending_requests_.end());
    // Free the peer's slot against max_peers, so a client that disconnects and comes back (or
    // a different one taking its place) is served again.
    served_peers_.erase(
      std::remove_if(served_peers_.begin(), served_peers_.end(),
        [&peer_id](const QarPeerId & p) { return peer_eq(p, peer_id); }),
      served_peers_.end());
  }
  // A target that is still here lost only its stream (QAR closed it): ask for a new one. The
  // owner's table is read with no lock of this factory held.
  if (const std::shared_ptr<QarSessionOwner> owner = owner_.lock()) {
    for (const TargetInfo & target : owner->targets()) {
      if (peer_eq(target.id, peer_id) && target.served) {
        on_target_up(target);
        break;
      }
    }
  }
}

}  // namespace qar_qar_adapter
