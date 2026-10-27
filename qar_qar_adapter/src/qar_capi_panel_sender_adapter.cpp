// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"

#include "qar_ar_core/panel_attach.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/panel_state_guard.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace qar_qar_adapter
{
namespace
{

/// Local wall-clock time as `HH:MM:SS.mmm` — the same shape (and the same clock)
/// as the host panel's own `[qar-panel]` log lines, so a close can be lined up
/// against them and against the QarOS crash dumps in %TEMP%\quaternar.
std::string
local_time_hms_ms()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
    now.time_since_epoch()).count() % 1000;
  char buf[16] = {0};
  std::snprintf(
    buf, sizeof(buf) - 1, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
    static_cast<int>(millis));
  return std::string(buf);
}

/// Rate limit for the stderr echo, per DISTINCT message: all of the first few,
/// then one in @ref kErrorLogEvery.
///
/// A panel failure is almost never a single event — whatever is wrong is wrong
/// again on the next capture, so an unlimited echo turns one broken panel into
/// tens of thousands of identical lines a minute in a log that already reaches
/// hundreds of megabytes, and buries the line that says what changed. The first
/// occurrences are what diagnose the fault; the periodic repeat, carrying its
/// occurrence count, is what shows it is still happening.
constexpr std::uint64_t kErrorLogFirst = 3;
constexpr std::uint64_t kErrorLogEvery = 600;
/// Distinct messages remembered before the counters are dropped and the limit
/// starts over. Messages embed sizes, so the set is not closed.
constexpr std::size_t kErrorLogDistinct = 128;

/// @return true if this occurrence of @p message should be printed, and its
///         1-based occurrence count in @p out_count.
///
/// The panel adapter is Qt-GUI-thread-only (see the class docs), so the counters
/// need no lock; the static itself is initialised thread-safely regardless.
bool
should_log_panel_error(const std::string & message, std::uint64_t & out_count)
{
  static std::map<std::string, std::uint64_t> seen;
  if (seen.size() >= kErrorLogDistinct && seen.find(message) == seen.end()) {
    seen.clear();
  }
  out_count = ++seen[message];
  return out_count <= kErrorLogFirst || (out_count % kErrorLogEvery) == 0;
}

/// Same shape as the render adapter's helper: a readable "<call> failed: ..."
/// line, also echoed to stderr (rate limited) because the panel UI only shows
/// the last error and a bootstrap can fail several calls deep.
std::string
panel_result_error(const char * what, const QarResult & r)
{
  char buf[512] = {0};
  qar_result_message(r, buf, sizeof(buf) - 1);
  std::string out = std::string(what) + " failed: code=" +
    std::to_string(static_cast<long long>(r.code));
  if (buf[0] != '\0') {
    out += " msg=";
    out += buf;
  }
  std::uint64_t occurrence = 0;
  if (should_log_panel_error(out, occurrence)) {
    if (occurrence <= kErrorLogFirst) {
      std::fprintf(stderr, "[qar-panel-adapter] *** %s\n", out.c_str());
    } else {
      std::fprintf(
        stderr, "[qar-panel-adapter] *** %s (x%llu, still failing)\n", out.c_str(),
        static_cast<unsigned long long>(occurrence));
    }
    std::fflush(stderr);
  }
  // The RETURNED string is unchanged and un-throttled: it is the panel's
  // last_error, which is read, not appended to.
  return out;
}

/// Canonical 8-4-4-4-12 UUID text for one of the SDK's 16-byte ids.
///
/// The header calls QarGuiPanelId::data / QarStreamId::data an "identifier
/// string", but the bytes are a UUID in network order, and the runtime names
/// the SAME id in its own log as a canonical UUID. Printing the bytes as text
/// (an unprintable byte as '.') gives `.Xh...@....C.Eb?` where the runtime logs
/// `f75868aa-9802-40b3-92c1-10438545623f` — the same sixteen bytes — and
/// matching a stalled stream to its widget means byte-comparing the two.
/// Printed as a canonical UUID the two are the same string and the match is a
/// grep.
std::string
qar_id_uuid(const std::uint8_t (& data)[QAR_MAX_ID_LENGTH])
{
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

std::string
panel_id_text(const QarGuiPanelId & id)
{
  return qar_id_uuid(id.data);
}

/// The name of a panel state as the log prints it.
std::string
panel_state_text(QarGuiPanelState state)
{
  switch (state) {
    case QAR_GUI_PANEL_STATE_VISIBLE:
      return "VISIBLE";
    case QAR_GUI_PANEL_STATE_MINIMIZED:
      return "MINIMIZED";
    case QAR_GUI_PANEL_STATE_HIDDEN:
      return "HIDDEN";
    case QAR_GUI_PANEL_STATE_CLOSED:
      return "CLOSED";
  }
  return "state " + std::to_string(static_cast<int>(state));
}

/// True when two panel sizes are the same to a tenth of a millimetre and a ten-thousandth of a
/// content scale.
bool
same_panel_size(const QarGuiPanelSize & a, const QarGuiPanelSize & b)
{
  return std::fabs(a.width_meters - b.width_meters) < 1e-4f &&
         std::fabs(a.height_meters - b.height_meters) < 1e-4f &&
         std::fabs(a.content_scale - b.content_scale) < 1e-4f;
}

/// Handles of the first panel list query, and the largest list read at all.
constexpr std::size_t kPanelListFirst = 64;
constexpr std::size_t kPanelListMax = 1024;

/// Reads the session's panel list with one `qar_query_gui_panels` call into a buffer that grows
/// while it comes back full (or while the runtime reports more panels than the buffer holds).
/// The handles are owned by the caller. False when the query fails.
bool
query_panel_list(QarSession * session, std::vector<QarGuiPanel *> & out)
{
  out.clear();
  std::size_t capacity = kPanelListFirst;
  for (;;) {
    std::vector<QarGuiPanel *> handles(capacity, nullptr);
    std::size_t written = 0;
    if (qar_result_is_error(
        QAR_CALL(qar_query_gui_panels, session, handles.data(), handles.size(), &written)))
    {
      return false;
    }
    const bool more = (written >= capacity) && capacity < kPanelListMax;
    if (!more) {
      handles.resize(std::min(written, handles.size()));
      out = std::move(handles);
      return true;
    }
    for (QarGuiPanel * handle : handles) {
      if (handle != nullptr) {
        QAR_CALL(qar_gui_panel_handle_destroy, handle);
      }
    }
    capacity = std::min(std::max(capacity * 4, written), kPanelListMax);
  }
}

/// What the probe read about the panel `get_or_create` returned.
struct PanelProbeReading
{
  qar_ar_core::PanelProbe probe;
};

/// Reads the panel @p id out of the session's panel list: its state, its content type, its
/// content stream and its size. The handles of the list are owned here and destroyed. A value
/// the runtime does not answer takes the safe default: HIDDEN (the panel is shown), a content
/// that differs (it is replaced) and a size that differs (it is set).
PanelProbeReading
probe_panel(
  QarSession * session, const QarGuiPanelId & id, const QarStreamId & ours,
  const QarGuiPanelSize & wanted)
{
  PanelProbeReading reading;
  std::vector<QarGuiPanel *> handles;
  if (!query_panel_list(session, handles)) {
    return reading;
  }
  for (QarGuiPanel * handle : handles) {
    if (handle == nullptr) {
      continue;
    }
    QarGuiPanelId listed{};
    if (!reading.probe.found &&
      !qar_result_is_error(QAR_CALL(qar_gui_panel_get_id, handle, &listed)) &&
      std::memcmp(listed.data, id.data, sizeof(id.data)) == 0)
    {
      reading.probe.found = true;
      QarGuiPanelState state = QAR_GUI_PANEL_STATE_HIDDEN;
      if (qar_result_is_error(QAR_CALL(qar_gui_panel_get_state, handle, &state))) {
        state = QAR_GUI_PANEL_STATE_HIDDEN;
      }
      reading.probe.state = state;
      QarGuiPanelContentType type = QAR_GUI_PANEL_CONTENT_TYPE_STREAM_ID;
      if (qar_result_is_error(QAR_CALL(qar_gui_panel_get_content_type, handle, &type))) {
        type = QAR_GUI_PANEL_CONTENT_TYPE_STREAM_ID;
      }
      reading.probe.content_mismatch = type == QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI;
      reading.probe.content_differs = true;
      if (!reading.probe.content_mismatch) {
        QarStreamId stream = qar_stream_id_default();
        if (!qar_result_is_error(QAR_CALL(qar_gui_panel_get_content_stream_id, handle, &stream))) {
          reading.probe.content_differs =
            std::memcmp(stream.data, ours.data, sizeof(ours.data)) != 0;
        }
      }
      QarGuiPanelSize size{};
      reading.probe.size_differs =
        qar_result_is_error(QAR_CALL(qar_gui_panel_get_size, handle, &size)) ||
        !same_panel_size(size, wanted);
    }
    QAR_CALL(qar_gui_panel_handle_destroy, handle);
  }
  return reading;
}

/// Build the single-texture, single-view CPU layout one GUI panel needs.
///
/// A panel is one plain 2D image: one texture, one full-extent COLOR view with
/// no eye assignment — deliberately unlike the stereo render path's two-eye
/// separated-texture layout.
QarVideoFrameLayout
make_panel_layout(const qar_ar_core::PanelDesc & desc)
{
  QarVideoFrameLayout layout = qar_video_frame_layout_default();

  layout.textures_count = 1;
  layout.textures[0] = qar_texture_size_default();
  layout.textures[0].format = desc.pixel_format;
  layout.textures[0].width = desc.buffer_width;
  layout.textures[0].height = desc.buffer_height;
  layout.textures[0].array_layers = 1;

  layout.views_count = 1;
  layout.views[0] = qar_video_frame_view_default();
  layout.views[0].start_x = 0;
  layout.views[0].start_y = 0;
  layout.views[0].end_x = desc.buffer_width;   // end values are exclusive
  layout.views[0].end_y = desc.buffer_height;
  layout.views[0].texture_index = 0;
  layout.views[0].array_layer_index = 0;
  layout.views[0].texture_format = desc.pixel_format;
  layout.views[0].data_type = QAR_VIDEO_FRAME_VIEW_TYPE_COLOR;
  layout.views[0].eye = QAR_VIDEO_FRAME_VIEW_EYE_NONE;

  return layout;
}

}  // namespace

// What the panel's tasks and the GUI thread share. The tasks hold it by shared_ptr, so it outlives
// an adapter that was released while its tasks are queued. The video sender and the panel are
// made and destroyed on the thread that owns the session; the GUI thread uses the sender for the
// frame calls only between the moment `frames_open` is set (the panel exists and shows the
// stream) or `phase` reads Ready, and the moment it closes the adapter. After that moment the
// thread that owns the session touches the sender in the teardown only, which a close of the
// adapter queues behind the create task.
struct QarCApiPanelSenderAdapter::Core
{
  enum class Phase { kCreating, kReady, kFailed, kClosed };

  std::atomic<Phase> phase{Phase::kCreating};
  /// Set by the create task as soon as the panel exists and shows the stream of the sender: from
  /// then on the GUI thread may send frames, although the attach probe, a reattach and the input
  /// subscription are still to run. The receiver shows the panel from this moment, so a frame
  /// that waits for the end of the create task is a black panel.
  std::atomic<bool> frames_open{false};
  qar_ar_core::PanelDesc desc;
  /// Null when the host wants a stream-only panel (input off).
  std::shared_ptr<QarCApiPanelInputSource> input;

  /// Guards everything below.
  mutable std::mutex mutex;
  QarVideoSender * sender = nullptr;
  QarGuiPanelId panel_id{};
  /// Set once by the create task, never cleared: see stream_id_text().
  std::string stream_id_text;
  /// The common name of the panel; see QarCApiPanelSenderAdapter::common_name.
  std::string common_name;
  std::string error;
  bool has_panel = false;
  bool reattached = false;
  /// A "panel-place" task is queued; the newest placement asked for is in `wanted`.
  bool place_queued = false;
  bool has_wanted = false;
  qar_ar_core::PanelPlacement wanted;
  std::vector<qar_ar_core::PanelOpResult> results;
  /// close() is idempotent and its task can be repeated by nobody; this keeps the retirement line
  /// to one per panel.
  bool retired_logged = false;
  /// Keeps the panel in the state the adapter asked for; see panel_state_guard.hpp. Never null.
  std::shared_ptr<PanelStateKeeper> keeper;
};

QarCApiPanelSenderAdapter::QarCApiPanelSenderAdapter(
  IQarSessionOwner * owner, const qar_ar_core::PanelDesc & desc,
  std::shared_ptr<QarCApiPanelInputSource> input)
: owner_(owner),
  desc_(desc),
  input_(input),
  core_(std::make_shared<Core>())
{
  core_->desc = desc;
  core_->input = std::move(input);
  core_->common_name = desc.common_name;
  core_->keeper = std::make_shared<PanelStateKeeper>(owner, "qar-panel-adapter", desc.key);

  // What can be checked without the runtime is refused at once.
  if (owner_ == nullptr) {
    refuse("no QarSession (is QAR streaming started?)");
    return;
  }
  if (desc_.buffer_width == 0 || desc_.buffer_height == 0) {
    refuse("panel buffer size must be non-zero");
    return;
  }
  if (desc_.common_name.empty()) {
    refuse("no common name for panel '" + desc_.key + "'");
    return;
  }
  const std::shared_ptr<Core> core = core_;
  const bool posted = owner_->post_session_task(
    ("panel-create:" + desc_.key).c_str(),
    [core](QarSession * session) { run_create(session, core); },
    [core] {
      qar_ar_core::PanelOpResult result;
      result.op = qar_ar_core::PanelOpResult::Op::Create;
      result.ok = false;
      result.error = "the QarOS session ended before the panel was made";
      std::lock_guard<std::mutex> lock(core->mutex);
      core->error = result.error;
      core->results.push_back(result);
      Core::Phase expected = Core::Phase::kCreating;
      core->phase.compare_exchange_strong(expected, Core::Phase::kFailed);
    });
  if (!posted) {
    refuse("QAR session is not accepting calls");
  }
}

QarCApiPanelSenderAdapter::~QarCApiPanelSenderAdapter()
{
  close();
}

void
QarCApiPanelSenderAdapter::refuse(const std::string & reason)
{
  refused_ = true;
  last_error_ = reason;
  std::lock_guard<std::mutex> lock(core_->mutex);
  core_->error = reason;
  core_->phase.store(Core::Phase::kFailed, std::memory_order_release);
}

bool
QarCApiPanelSenderAdapter::ready() const
{
  return core_->phase.load(std::memory_order_acquire) == Core::Phase::kReady;
}

bool
QarCApiPanelSenderAdapter::pending() const
{
  return core_->phase.load(std::memory_order_acquire) == Core::Phase::kCreating &&
         !core_->frames_open.load(std::memory_order_acquire);
}

QarGuiPanelId
QarCApiPanelSenderAdapter::panel_id() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->panel_id;
}

bool
QarCApiPanelSenderAdapter::reattached() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->reattached;
}

std::string
QarCApiPanelSenderAdapter::common_name() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->common_name;
}

std::string
QarCApiPanelSenderAdapter::stream_id_text() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->stream_id_text;
}

void
QarCApiPanelSenderAdapter::drain_results(std::vector<qar_ar_core::PanelOpResult> & out)
{
  // The consumer calls this on every tick, which is when a panel that QarOS hid again inside the
  // hold of an ask and then left alone is asked about once more.
  core_->keeper->poll();
  std::vector<qar_ar_core::PanelOpResult> taken;
  {
    std::lock_guard<std::mutex> lock(core_->mutex);
    taken.swap(core_->results);
  }
  for (qar_ar_core::PanelOpResult & result : taken) {
    if (!result.ok && !result.error.empty()) {
      last_error_ = result.error;
    }
    out.push_back(std::move(result));
  }
}

// The create chain: video_sender_create -> stream_id -> get_or_create -> probe, and the reattach
// steps when the panel is an earlier one. Runs on the thread that owns the session. The failing
// step is recorded in Core::error and the phase becomes Failed. The frames open right after the
// first get_or_create, so the probe and the reattach do not hold them.
void
QarCApiPanelSenderAdapter::run_create(QarSession * session, const std::shared_ptr<Core> & core)
{
  // An adapter closed or detached before its turn never made a panel: nothing is created, so the
  // hide and the detach queued behind this task find nothing to change and cost no call. One that
  // is closed while this task runs leaves the panel it made for those tasks (hide, detach).
  if (core->phase.load(std::memory_order_acquire) == Core::Phase::kClosed) {
    return;
  }
  const qar_ar_core::PanelDesc desc = core->desc;
  const auto fail = [&core](const std::string & error) {
      qar_ar_core::PanelOpResult result;
      result.op = qar_ar_core::PanelOpResult::Op::Create;
      result.ok = false;
      result.error = error;
      std::lock_guard<std::mutex> lock(core->mutex);
      core->error = error;
      core->results.push_back(result);
      Core::Phase expected = Core::Phase::kCreating;
      core->phase.compare_exchange_strong(expected, Core::Phase::kFailed);
    };

  QarVideoSenderInit init = qar_video_sender_init_default();
  init.graphics_api = QAR_GRAPHICS_API_CPU;
  init.layout = make_panel_layout(desc);

  QarVideoSender * sender = nullptr;
  QarResult r = QAR_CALL(qar_video_sender_create, session, &init, &sender);
  if (qar_result_is_error(r) || sender == nullptr) {
    fail(panel_result_error("qar_video_sender_create", r));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->sender = sender;
  }

  QarStreamId stream_id = qar_stream_id_default();
  r = QAR_CALL(qar_video_sender_stream_id, sender, &stream_id);
  if (qar_result_is_error(r)) {
    fail(panel_result_error("qar_video_sender_stream_id", r));
    return;
  }
  // Kept for the LIFETIME of this adapter, close() included: everything the
  // runtime ever says about this panel's content — the streamCatalog entry, the
  // streamRequest queries a receiver retries, the reconnect it enters when the
  // producer goes away — is keyed by this id and nothing else.
  const std::string stream_id_text = qar_id_uuid(stream_id.data);
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->stream_id_text = stream_id_text;
  }

  // The panel's content IS the stream: chain QarGuiPanelStreamIdExt onto the
  // init. WITHOUT this extension the runtime creates a website panel instead
  // (documented in qar_streaming.h), which would silently show a browser rather
  // than the widget.
  QarGuiPanelStreamIdExt stream_ext = qar_gui_panel_stream_id_ext_default();
  stream_ext.stream_id = stream_id;

  // The panel of this common name: created, or found again when an earlier session left it
  // behind. A name that is taken by a panel that cannot be reused is tried again with a suffix
  // appended to the base name: "-v2" for a website panel, "-r1" .. "-r8" for closed panels.
  std::string common_name = desc.common_name;
  QarGuiPanelId panel_id{};
  qar_ar_core::PanelAttachDecision decision;
  PanelProbeReading reading;
  for (unsigned retries = 0;; ++retries) {
    QarGuiPanelInit panel_init = qar_gui_panel_init_default();
    panel_init.header.next = &stream_ext;
    panel_init.common_name = common_name.c_str();
    panel_init.display_name = desc.display_name.empty() ? nullptr : desc.display_name.c_str();
    panel_init.pose = desc.placement.pose;
    panel_init.size = desc.placement.size;

    r = QAR_CALL(qar_gui_panels_get_or_create, session, &panel_init, &panel_id);
    if (qar_result_is_error(r)) {
      fail(panel_result_error("qar_gui_panels_get_or_create", r));
      return;
    }
    // The panel exists, and the receiver may attach to the stream at once. Frames sent before the
    // probe ends are harmless: a stream nobody shows has no transfer and the sender answers "no
    // active transfers". The sender stays the same through a retry with a suffix and through the
    // reattach (which names the stream, not the sender); a failure below sets Failed and the GUI
    // thread stops sending at its next tick. Nothing on this thread touches the sender again
    // before the teardown, which runs only after the adapter was closed.
    core->frames_open.store(true, std::memory_order_release);

    reading = probe_panel(session, panel_id, stream_id, desc.placement.size);
    decision = qar_ar_core::decide_panel_attach(reading.probe, retries);
    if (decision.kind != qar_ar_core::PanelAttachKind::RetryWithSuffix) {
      break;
    }
    std::fprintf(
      stderr, "[qar-panel-adapter] %s panel '%s' name=%s is taken by %s panel, trying %s%s\n",
      local_time_hms_ms().c_str(), desc.key.c_str(), common_name.c_str(),
      decision.reason == "closed" ? "a closed" : "a web", desc.common_name.c_str(),
      decision.retry_suffix.c_str());
    std::fflush(stderr);
    common_name = desc.common_name + decision.retry_suffix;
  }
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->common_name = common_name;
    core->panel_id = panel_id;
  }
  if (decision.kind == qar_ar_core::PanelAttachKind::Refused) {
    const std::string refusal = decision.reason == "closed" ?
      "closed panels of the names " + desc.common_name + " to " + common_name +
      " exist; no free name is left" :
      "a web panel of the name " + common_name + " exists";
    std::fprintf(
      stderr, "[qar-panel-adapter] %s panel '%s' refused: %s\n", local_time_hms_ms().c_str(),
      desc.key.c_str(), refusal.c_str());
    std::fflush(stderr);
    fail(refusal);
    return;
  }

  const bool reattached = decision.kind == qar_ar_core::PanelAttachKind::Reattached;
  if (reattached) {
    // The panel of an earlier session shows the stream of a sender that is gone. Our
    // stream goes on it, then size and pose, then it is shown again.
    r = QAR_CALL(qar_gui_panels_set_stream, session, &panel_id, &stream_id);
    if (qar_result_is_error(r)) {
      fail(panel_result_error("qar_gui_panels_set_stream", r));
      return;
    }
    if (decision.change_size) {
      r = QAR_CALL(qar_gui_panels_change_size, session, &panel_id, &desc.placement.size);
      if (qar_result_is_error(r)) {
        fail(panel_result_error("qar_gui_panels_change_size", r));
        return;
      }
    }
    r = QAR_CALL(qar_gui_panels_update_pose, session, &panel_id, &desc.placement.pose);
    if (qar_result_is_error(r)) {
      fail(panel_result_error("qar_gui_panels_update_pose", r));
      return;
    }
    if (decision.show) {
      r = QAR_CALL(qar_gui_panels_set_state, session, &panel_id, QAR_GUI_PANEL_STATE_VISIBLE);
      if (qar_result_is_error(r)) {
        fail(panel_result_error("qar_gui_panels_set_state", r));
        return;
      }
    }
  }
  // Marked only now: a step of the reattach that failed leaves the panel of the earlier session
  // alone, because the consumer drops a failed adapter and its teardown closes the panel it owns.
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->has_panel = true;
    core->reattached = reattached;
  }

  // How the panel came to be, in the one format the log checks read.
  qar_ar_core::PanelAttachLogFields fields;
  fields.time = local_time_hms_ms();
  fields.key = desc.key;
  fields.reattached = reattached;
  fields.common_name = common_name;
  fields.panel_id = panel_id_text(panel_id);
  if (reading.probe.found) {
    fields.state_was = qar_ar_core::panel_state_name(reading.probe.state);
  }
  fields.stream_swapped = reattached && decision.swap_content;
  fields.size_changed = reattached && decision.change_size;
  std::fprintf(stderr, "%s\n", qar_ar_core::panel_attach_log_line(fields).c_str());

  // The creation half of the pair the retirement line below completes. Printed
  // unconditionally, because it is the ONLY place the runtime's stream UUID and
  // the widget key are written next to each other: without it, a
  // `streamRequest/<uuid>` in the runtime trace names nothing we know.
  std::fprintf(
    stderr, "[qar-panel-adapter] %s panel '%s' %s: id=%s stream=%s buffer=%ux%u px\n",
    local_time_hms_ms().c_str(), desc.key.c_str(), reattached ? "reattached" : "created",
    panel_id_text(panel_id).c_str(), stream_id_text.c_str(),
    static_cast<unsigned>(desc.buffer_width), static_cast<unsigned>(desc.buffer_height));
  std::fflush(stderr);

  // The panel exists, so it can now be pointed at. Subscribing HERE rather
  // than in the RViz panel is what makes the subscription's lifetime exactly the
  // panel's - the teardown is the only place it is ended, and it runs
  // whether the panel went away, the widget resized, or the process is
  // shutting down. The content frame is the buffer we stream, which is what the
  // renderer decodes and therefore the frame the event's pixels are in.
  //
  // A failure here is NOT fatal: a panel that streams but takes no input is
  // strictly better than no panel, and it is the only outcome available on a
  // runtime without the gesture subscription API.
  if (core->input) {
    // The content extent, NOT the buffer: the buffer is aligned up for the
    // encoder and the slack is transparent padding the widget never paints.
    const std::uint32_t content_w =
      desc.content_width != 0 ? desc.content_width : desc.buffer_width;
    const std::uint32_t content_h =
      desc.content_height != 0 ? desc.content_height : desc.buffer_height;
    if (!core->input->subscribe_panel(desc.key, panel_id, content_w, content_h, stream_id_text)) {
      std::fprintf(stderr,
        "[qar-panel-adapter] %s panel '%s' streams but takes no AR input: %s\n",
        local_time_hms_ms().c_str(), desc.key.c_str(), core->input->last_error().c_str());
      std::fflush(stderr);
    } else {
      // The POSITIVE half, printed once per panel creation. Without it, silence
      // is ambiguous: neither line in a log means we never reached here at all,
      // whereas this line means the runtime accepted the subscription and owes us
      // click/hover events for THIS id at THIS extent -- which is also the
      // extent every incoming pixel coordinate will be normalised against, so a
      // mismatch with the panel you are poking is visible right here.
      std::fprintf(stderr,
        "[qar-panel-adapter] %s panel '%s' subscribed for AR click+hover: id='%s' content=%ux%u px\n",
        local_time_hms_ms().c_str(), desc.key.c_str(), panel_id_text(panel_id).c_str(),
        static_cast<unsigned>(content_w), static_cast<unsigned>(content_h));
      std::fflush(stderr);
    }
  }

  // The panel shows the stream, so VISIBLE is the state to keep. A runtime that reports the panel
  // hidden right after the adapter showed it is asked again. A watch that cannot start is
  // reported and the panel works without it.
  core->keeper->start(session, panel_id);

  qar_ar_core::PanelOpResult done;
  done.op = qar_ar_core::PanelOpResult::Op::Create;
  done.ok = true;
  done.reattached = reattached;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->error.clear();
    core->results.push_back(done);
  }
  // Published last: the GUI thread reads the sender after it saw Ready. A close that came in
  // meanwhile keeps the phase Closed, and its task cleans up behind this one.
  Core::Phase expected = Core::Phase::kCreating;
  core->phase.compare_exchange_strong(
    expected, Core::Phase::kReady, std::memory_order_release, std::memory_order_relaxed);
}

void
QarCApiPanelSenderAdapter::run_place(QarSession * session, const std::shared_ptr<Core> & core)
{
  qar_ar_core::PanelPlacement placement;
  QarGuiPanelId panel_id{};
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->place_queued = false;
    if (!core->has_wanted) {
      return;
    }
    core->has_wanted = false;
    placement = core->wanted;
    panel_id = core->panel_id;
    if (!core->has_panel || core->phase.load() == Core::Phase::kFailed) {
      return;
    }
  }
  qar_ar_core::PanelOpResult result;
  result.op = qar_ar_core::PanelOpResult::Op::Placement;
  const QarResult posed = QAR_CALL(qar_gui_panels_update_pose, session, &panel_id, &placement.pose);
  if (qar_result_is_error(posed)) {
    result.ok = false;
    result.error = panel_result_error("qar_gui_panels_update_pose", posed);
  } else {
    const QarResult sized = QAR_CALL(
        qar_gui_panels_change_size, session, &panel_id, &placement.size);
    if (qar_result_is_error(sized)) {
      result.ok = false;
      result.error = panel_result_error("qar_gui_panels_change_size", sized);
    }
  }
  std::lock_guard<std::mutex> lock(core->mutex);
  if (result.ok) {
    core->desc.placement = placement;
  }
  core->results.push_back(std::move(result));
}

void
QarCApiPanelSenderAdapter::run_state(
  QarSession * session, const std::shared_ptr<Core> & core, QarGuiPanelState state)
{
  // Whatever this task finds to do, the keeper learns that the request has been served.
  struct Served
  {
    PanelStateKeeper & keeper;
    ~Served() { keeper.request_done(); }
  } served{*core->keeper};
  QarGuiPanelId panel_id{};
  std::string key;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    if (!core->has_panel || core->phase.load() == Core::Phase::kFailed) {
      return;  // the panel was never made, or its creation failed: nothing to change
    }
    panel_id = core->panel_id;
    key = core->desc.key;
  }
  qar_ar_core::PanelOpResult result;
  result.op = qar_ar_core::PanelOpResult::Op::State;
  result.state = state;
  const QarResult r = QAR_CALL(qar_gui_panels_set_state, session, &panel_id, state);
  if (qar_result_is_error(r)) {
    result.ok = false;
    result.error = panel_result_error("qar_gui_panels_set_state", r);
  } else {
    // The same shape as the creation line, so the change can be lined up against the host's own
    // log and against the runtime's trace. The input source reads a panel update as an AR-side
    // close only for the state CLOSED, so the HIDDEN or VISIBLE set here is not taken for one.
    std::fprintf(
      stderr, "[qar-panel-adapter] %s panel '%s' state -> %s\n", local_time_hms_ms().c_str(),
      key.c_str(), panel_state_text(state).c_str());
    std::fflush(stderr);
  }
  std::lock_guard<std::mutex> lock(core->mutex);
  core->results.push_back(std::move(result));
}

bool
QarCApiPanelSenderAdapter::try_begin_frame(bool & out_ready)
{
  out_ready = false;
  not_ready_cause_ = qar_ar_core::PanelNotReadyCause::NoDestination;
  const Core::Phase phase = core_->phase.load(std::memory_order_acquire);
  if (phase == Core::Phase::kCreating && !core_->frames_open.load(std::memory_order_acquire)) {
    not_ready_cause_ = qar_ar_core::PanelNotReadyCause::Creating;
    return true;
  }
  if ((phase != Core::Phase::kReady && phase != Core::Phase::kCreating) || closed_) {
    if (phase == Core::Phase::kFailed) {
      std::lock_guard<std::mutex> lock(core_->mutex);
      last_error_ = core_->error;
    }
    if (last_error_.empty()) { last_error_ = "panel sender is not ready"; }
    return false;
  }
  if (sender_ == nullptr) {
    std::lock_guard<std::mutex> lock(core_->mutex);
    sender_ = core_->sender;
  }
  if (sender_ == nullptr) {
    if (last_error_.empty()) { last_error_ = "panel sender is not ready"; }
    return false;
  }

  bool sender_ready = false;
  const QarResult r = QAR_CALL(qar_video_sender_try_begin_frame, sender_, &sender_ready);
  if (qar_result_is_error(r)) {
    // NOT a failure — a NOT-READY state, and the difference matters because the
    // caller marks the panel broken on a failure and simply polls again on a
    // not-ready.
    //
    // The header documents this call as "non-blocking attempt to begin
    // producing a new frame. Sets out_ready to true when a send cycle was
    // reserved. When false, no frame cycle exists and the caller should poll
    // again later." A panel sender legitimately exists before anything in the
    // headset has attached to its stream, and that is exactly what
    // MEDIA_STREAMING_NO_ACTIVE_TRANSFERS (604) says: there is no transfer to
    // reserve a destination on. STREAM_IS_RECONNECTING (603) is the same shape
    // — transient, retried by polling, nothing the caller can fix. Both are
    // therefore reported the documented way: success, out_ready == false.
    const bool no_transfers =
      qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_NO_ACTIVE_TRANSFERS);
    const bool reconnecting =
      qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_RECONNECTING);
    // STREAM_IS_PAUSED (602): the receiver paused the stream. Nothing is wanted until it
    // resumes, which is a state of the receiver and no fault of this sender.
    const bool paused = qar_result_has_code(r, QAR_STATUS_MEDIA_STREAMING_STREAM_IS_PAUSED);
    if (no_transfers || reconnecting || paused) {
      // All three are reported as not ready, and the caller counts them apart: nothing attached
      // to the stream yet is a different fault from a receiver that lost a stream it had, and
      // from one that paused it.
      not_ready_cause_ = paused ? qar_ar_core::PanelNotReadyCause::Paused :
        reconnecting ? qar_ar_core::PanelNotReadyCause::Reconnecting :
        qar_ar_core::PanelNotReadyCause::NoActiveTransfers;
      if (paused && paused_noted_.first()) {
        std::fprintf(
          stderr, "[qar-panel-adapter] %s panel '%s' stream %s: paused by the receiver\n",
          local_time_hms_ms().c_str(), desc_.key.c_str(), stream_id_text().c_str());
        std::fflush(stderr);
      }
      if (reconnecting && frames_shown_ > 0 && reconnect_noted_.first()) {
        std::fprintf(
          stderr, "%s\n",
          reconnecting_line(local_time_hms_ms(), desc_.key, stream_id_text(), frames_shown_)
          .c_str());
        std::fflush(stderr);
      }
      frame_reserved_ = false;
      out_ready = false;
      return true;
    }
    last_error_ = panel_result_error("qar_video_sender_try_begin_frame", r);
    return false;
  }
  frame_reserved_ = sender_ready;
  out_ready = sender_ready;
  return true;
}

bool
QarCApiPanelSenderAdapter::submit_frame(const qar_ar_core::PanelPixels & pixels)
{
  if (!frame_reserved_) {
    last_error_ = "submit_frame without a reserved cycle";
    return false;
  }
  // Whatever happens below, the reservation is consumed: leaving it set would
  // make the next tick believe a cycle is still open.
  frame_reserved_ = false;

  if (pixels.data == nullptr) {
    last_error_ = "submit_frame got a null pixel buffer";
    return false;
  }

  QarVideoFrameCpu frame = qar_video_frame_cpu_default();
  const QarResult r = QAR_CALL(qar_video_sender_frame_cpu, sender_, &frame);
  if (qar_result_is_error(r)) {
    last_error_ = panel_result_error("qar_video_sender_frame_cpu", r);
    return false;
  }
  if (frame.textures_count == 0 || frame.textures[0].texture_data == nullptr) {
    last_error_ = "qar_video_sender_frame_cpu returned no writable texture";
    return false;
  }

  const QarVideoTextureCpu & dst = frame.textures[0];
  // The sender owns the destination and its layout is fixed at creation, so a
  // mismatch means our geometry and the sender have drifted apart — copying
  // anyway would write past the destination.
  if (dst.size.width != desc_.buffer_width || dst.size.height != desc_.buffer_height) {
    last_error_ = "panel frame size mismatch: sender has " +
      std::to_string(dst.size.width) + "x" + std::to_string(dst.size.height) + ", we captured " +
      std::to_string(desc_.buffer_width) + "x" + std::to_string(desc_.buffer_height);
    return false;
  }

  const std::uint32_t row_bytes = desc_.buffer_width * 4u;
  const std::uint32_t dst_pitch = dst.pitch != 0 ? dst.pitch : row_bytes;
  if (dst_pitch < row_bytes || pixels.stride_bytes < row_bytes) {
    last_error_ = "panel frame pitch is smaller than a row of pixels";
    return false;
  }
  const std::size_t needed = static_cast<std::size_t>(dst_pitch) * (desc_.buffer_height - 1) +
    row_bytes;
  if (dst.texture_data_size < needed) {
    last_error_ = "panel destination buffer is too small for the captured frame";
    return false;
  }

  // Row-by-row: Qt's scanline stride and QAR's pitch are independently chosen,
  // so a single flat memcpy would shear the image whenever they differ.
  for (std::uint32_t y = 0; y < desc_.buffer_height; ++y) {
    std::memcpy(
      dst.texture_data + static_cast<std::size_t>(y) * dst_pitch,
      pixels.data + static_cast<std::size_t>(y) * pixels.stride_bytes,
      row_bytes);
  }

  const QarResult shown = QAR_CALL(qar_video_sender_show_frame, sender_);
  if (qar_result_is_error(shown)) {
    last_error_ = panel_result_error("qar_video_sender_show_frame", shown);
    return false;
  }
  ++frames_shown_;
  if (first_show_noted_.first()) {
    // The thread that shows frames, to be read next to the threads of the 3D stream's lines.
    std::fprintf(
      stderr, "%s\n",
      first_panel_frame_line(local_time_hms_ms(), desc_.key, thread_number()).c_str());
    std::fflush(stderr);
  }
  last_error_.clear();
  return true;
}

bool
QarCApiPanelSenderAdapter::set_placement(const qar_ar_core::PanelPlacement & placement)
{
  const Core::Phase phase = core_->phase.load(std::memory_order_acquire);
  if (closed_ || (phase != Core::Phase::kCreating && phase != Core::Phase::kReady)) {
    last_error_ = "no live panel to place";
    return false;
  }
  bool queue = false;
  {
    // The newest placement wins: one task is queued at most and applies what was asked last.
    std::lock_guard<std::mutex> lock(core_->mutex);
    core_->wanted = placement;
    core_->has_wanted = true;
    queue = !core_->place_queued;
    core_->place_queued = true;
  }
  if (queue) {
    const std::shared_ptr<Core> core = core_;
    const bool posted = owner_->post_session_task(
      ("panel-place:" + desc_.key).c_str(),
      [core](QarSession * session) { run_place(session, core); },
      [core] {
        std::lock_guard<std::mutex> lock(core->mutex);
        core->place_queued = false;
        core->has_wanted = false;
      });
    if (!posted) {
      std::lock_guard<std::mutex> lock(core_->mutex);
      core_->place_queued = false;
      core_->has_wanted = false;
      last_error_ = "QAR session is not accepting calls";
      return false;
    }
  }
  last_error_.clear();
  return true;
}

bool
QarCApiPanelSenderAdapter::set_state(QarGuiPanelState state)
{
  const Core::Phase phase = core_->phase.load(std::memory_order_acquire);
  if (closed_ || (phase != Core::Phase::kCreating && phase != Core::Phase::kReady)) {
    last_error_ = "no live panel to change";
    return false;
  }
  const std::shared_ptr<Core> core = core_;
  core->keeper->request(state);
  const bool posted = owner_->post_session_task(
    ("panel-state:" + desc_.key).c_str(),
    [core, state](QarSession * session) { run_state(session, core, state); },
    [core] { core->keeper->request_dropped(); });
  if (!posted) {
    core->keeper->request_dropped();
    last_error_ = "QAR session is not accepting calls";
    return false;
  }
  last_error_.clear();
  return true;
}

void
QarCApiPanelSenderAdapter::set_state_guard_clock(PanelStateKeeper::ClockFn now)
{
  core_->keeper->set_clock(std::move(now));
}

qar_ar_core::PanelCloseReport
QarCApiPanelSenderAdapter::close(bool panel_closed_in_ar)
{
  return teardown(panel_closed_in_ar, /*keep_panel=*/false);
}

void
QarCApiPanelSenderAdapter::detach()
{
  teardown(/*panel_closed_in_ar=*/false, /*keep_panel=*/true);
}

qar_ar_core::PanelCloseReport
QarCApiPanelSenderAdapter::teardown(bool panel_closed_in_ar, bool keep_panel)
{
  qar_ar_core::PanelCloseReport report;
  // The second close — the destructor after the stream's own — has nothing left
  // to do, and must not do the first step again: unsubscribe_panel() is keyed by
  // PANEL KEY, so a late one would cancel the subscriptions of a newer adapter
  // that has since taken the same key (a dialog opened again, a dock restreamed).
  if (closed_) {
    return report;
  }
  closed_ = true;
  frame_reserved_ = false;
  sender_ = nullptr;
  // From here no frame call is made on the sender, so the thread that owns the session may
  // destroy it.
  core_->phase.store(Core::Phase::kClosed, std::memory_order_release);
  core_->keeper->retire();

  if (input_ && !desc_.key.empty()) {
    report.input_unsubscribed = input_->has_panel(desc_.key);
  }
  if (owner_ == nullptr) {
    return report;
  }
  const std::shared_ptr<Core> core = core_;
  const bool posted = owner_->post_session_task(
    ((keep_panel ? "panel-detach:" : "panel-close:") + desc_.key).c_str(),
    [core, panel_closed_in_ar, keep_panel](QarSession * session) {
      run_teardown(session, core, panel_closed_in_ar, keep_panel);
    },
    [core] { core->keeper->abandon(); });
  if (posted) {
    report.panel = qar_ar_core::PanelCloseOutcome::Queued;
  }
  return report;
}

// The one teardown, in the one safe order, on the thread that owns the session. Idempotent per
// Core.
void
QarCApiPanelSenderAdapter::run_teardown(
  QarSession * session, const std::shared_ptr<Core> & core, bool panel_closed_in_ar,
  bool keep_panel)
{
  QarVideoSender * sender = nullptr;
  QarGuiPanelId panel_id{};
  std::string stream_id_text;
  bool has_panel = false;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    sender = core->sender;
    panel_id = core->panel_id;
    stream_id_text = core->stream_id_text;
    has_panel = core->has_panel;
    core->sender = nullptr;
    core->has_panel = false;
  }
  const std::string & key = core->desc.key;
  const bool something_to_close = has_panel || sender != nullptr;
  const auto started = std::chrono::steady_clock::now();
  const auto step_line = [&key](const char * what) {
    // Every line below carries the local wall-clock time, because the only way to
    // line a close up against a QarOS crash dump in %TEMP%\quaternar is by the
    // clock. Printed and flushed BEFORE each runtime call that follows it, so if
    // the process dies inside that call the last line in the log names it.
    std::fprintf(
      stderr, "[qar-panel-adapter] %s panel '%s' %s\n", local_time_hms_ms().c_str(),
      key.c_str(), what);
    std::fflush(stderr);
  };
  if (something_to_close) {
    char head[256] = {0};
    if (keep_panel) {
      std::snprintf(
        head, sizeof(head) - 1, "detaching: id=%s stream=%s (the GUI panel is kept)",
        panel_id_text(panel_id).c_str(), stream_id_text.c_str());
    } else {
      std::snprintf(
        head, sizeof(head) - 1,
        "closing: id=%s stream=%s (operator already closed it in AR: %s)",
        panel_id_text(panel_id).c_str(), stream_id_text.c_str(),
        panel_closed_in_ar ? "yes" : "no");
    }
    step_line(head);
  }

  // The watch of the panel's state ends with the input subscriptions: from here on the runtime's
  // republishing of the panel, our own close included, asks for nothing.
  core->keeper->stop();

  // STEP 1 — input FIRST, before the panel it is about. unsubscribe_panel does
  // not return until no click/hover callback for this panel is running, so from
  // here on nothing can arrive naming a panel that is being destroyed. It also
  // retires the panel-UPDATE subscription, which is what keeps the CLOSED state
  // the runtime republishes for OUR close_panel below from coming back to us as
  // "the operator closed this panel in AR".
  if (core->input && !key.empty()) {
    const bool had_input = core->input->has_panel(key);
    core->input->unsubscribe_panel(key);
    if (had_input) {
      step_line("step 1/3: AR input + panel-update subscriptions cancelled");
    }
  }

  // STEP 2 — the panel, then (step 3) the stream it points at: closing the sender
  // first would leave a live panel referencing a retired stream. The runtime
  // confirms the order, also for dialog panels: the panel's State reaches 3
  // (closed) about a millisecond before the sender teardown begins.
  qar_ar_core::PanelCloseReport report;
  if (has_panel && keep_panel) {
    // Detach: the QarOS panel stays as it is (the caller hid it), and the next session finds it
    // again under its common name.
    step_line("step 2/3: GUI panel kept for the next session");
  } else if (has_panel) {
    if (panel_closed_in_ar) {
      // The operator already closed it: the runtime has removed the panel, and a
      // second close of the same id is a call we have no reason to make and no
      // documented behaviour for. Skipped, and said so.
      report.panel = qar_ar_core::PanelCloseOutcome::AlreadyClosedInAr;
      step_line(
        "step 2/3: qar_gui_panels_close_panel SKIPPED -- the operator already closed this "
        "panel in AR");
    } else {
      // The result is CHECKED, not discarded. A close that failed would leave a
      // panel in the session pointing at the sender destroyed immediately below —
      // an orphan, and an invisible one if this call's QarResult were thrown
      // away. panel_result_error() also echoes it to stderr.
      const QarResult closed = QAR_CALL(qar_gui_panels_close_panel, session, &panel_id);
      if (qar_result_is_error(closed)) {
        const std::string error = panel_result_error("qar_gui_panels_close_panel", closed);
        report.panel = qar_ar_core::PanelCloseOutcome::Failed;
        report.panel_message = error;
        step_line(
          ("step 2/3: " + error +
          " -- the video sender is destroyed regardless; the AR panel may still be on screen")
          .c_str());
      } else {
        report.panel = qar_ar_core::PanelCloseOutcome::Closed;
        step_line("step 2/3: qar_gui_panels_close_panel ran, ok");
      }
    }
  }

  // STEP 3 — the video sender, unconditionally. Announced BEFORE the call (the
  // riskiest of the three: it is the one a crash dump has been about).
  if (sender != nullptr) {
    step_line("step 3/3: destroying the video sender");
    QAR_CALL(qar_video_sender_handle_destroy, sender);
    report.sender_destroyed = true;
  }

  // The retirement line, and the counterpart to the creation line above: what this
  // close actually did, as the outcome of the steps just logged, so one grep on
  // "retired:" answers "was the AR panel closed, and is the sender gone" for a stream id.
  //
  // The line states the outcome and nothing else. The C API has no call to withdraw a
  // stream (qar_video_sender_handle_destroy returns void, qar_gui_panels_close_panel
  // closes the PANEL, and neither tells a receiver already attached to the stream that
  // it has gone: the receiver logs "connection lost for stream ..., entering reconnecting
  // state" and retries every ten seconds, see
  // docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations). The line is printed on EVERY
  // close and the sender IS destroyed on every one, so a standing warning about a stream
  // "kept" would say the opposite of what happened.
  bool print_retired = false;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    print_retired = !stream_id_text.empty() && !core->retired_logged;
    core->retired_logged = true;
  }
  if (print_retired) {
    const long long took_ms = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    const char * gui_panel = keep_panel ? "kept" : "none was open";
    switch (report.panel) {
      case qar_ar_core::PanelCloseOutcome::NoPanel:
      case qar_ar_core::PanelCloseOutcome::Queued:
        break;
      case qar_ar_core::PanelCloseOutcome::Closed:
        gui_panel = "closed";
        break;
      case qar_ar_core::PanelCloseOutcome::AlreadyClosedInAr:
        gui_panel = "close skipped (the operator had already closed it in AR)";
        break;
      case qar_ar_core::PanelCloseOutcome::Failed:
        gui_panel = "close FAILED (see step 2/3 above)";
        break;
    }
    std::fprintf(
      stderr,
      "[qar-panel-adapter] %s panel '%s' retired: id=%s stream=%s took=%lldms -- GUI panel %s, "
      "video sender %s\n",
      local_time_hms_ms().c_str(), key.c_str(), panel_id_text(panel_id).c_str(),
      stream_id_text.c_str(), took_ms, gui_panel,
      report.sender_destroyed ? "destroyed" : "none to destroy");
    std::fflush(stderr);
  }
}

QarCApiPanelSenderFactory::QarCApiPanelSenderFactory(
  IQarSessionOwner * session_owner, std::shared_ptr<QarCApiPanelInputSource> input)
: session_owner_(session_owner),
  input_(std::move(input))
{
}

std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter>
QarCApiPanelSenderFactory::create_panel_sender(const qar_ar_core::PanelDesc & desc)
{
  if (session_owner_ == nullptr) {
    last_error_ = "no QAR session owner";
    return nullptr;
  }
  if (!session_owner_->ready()) {
    last_error_ = "QAR session is not ready yet";
    return nullptr;
  }

  // The panel and its video sender are made on the thread that owns the session; the adapter
  // is handed back at once, still "creating".
  auto adapter = std::make_shared<QarCApiPanelSenderAdapter>(session_owner_, desc, input_);
  if (adapter->refused()) {
    last_error_ = adapter->last_error();
    return nullptr;
  }
  last_error_.clear();
  return adapter;
}

}  // namespace qar_qar_adapter
