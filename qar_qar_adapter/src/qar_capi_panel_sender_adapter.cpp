// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"

#include "qar_qar_adapter/panel_state_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <utility>

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

QarCApiPanelSenderAdapter::QarCApiPanelSenderAdapter(
  QarSession * session, const qar_ar_core::PanelDesc & desc,
  std::shared_ptr<QarCApiPanelInputSource> input)
: session_(session),
  desc_(desc),
  input_(std::move(input))
{
  bootstrap();
}

QarCApiPanelSenderAdapter::~QarCApiPanelSenderAdapter()
{
  close();
}

void
QarCApiPanelSenderAdapter::bootstrap()
{
  if (session_ == nullptr) {
    last_error_ = "no QarSession (is QAR streaming started?)";
    return;
  }
  if (desc_.buffer_width == 0 || desc_.buffer_height == 0) {
    last_error_ = "panel buffer size must be non-zero";
    return;
  }

  QarVideoSenderInit init = qar_video_sender_init_default();
  init.graphics_api = QAR_GRAPHICS_API_CPU;
  init.layout = make_panel_layout(desc_);

  QarResult r = qar_video_sender_create(session_, &init, &sender_);
  if (qar_result_is_error(r) || sender_ == nullptr) {
    last_error_ = panel_result_error("qar_video_sender_create", r);
    sender_ = nullptr;
    return;
  }

  QarStreamId stream_id = qar_stream_id_default();
  r = qar_video_sender_stream_id(sender_, &stream_id);
  if (qar_result_is_error(r)) {
    last_error_ = panel_result_error("qar_video_sender_stream_id", r);
    return;
  }
  // Kept for the LIFETIME of this adapter, close() included: everything the
  // runtime ever says about this panel's content — the streamCatalog entry, the
  // streamRequest queries a receiver retries, the reconnect it enters when the
  // producer goes away — is keyed by this id and nothing else.
  stream_id_text_ = qar_id_uuid(stream_id.data);

  // The panel's content IS the stream: chain QarGuiPanelStreamIdExt onto the
  // init. WITHOUT this extension the runtime creates a website panel instead
  // (documented in qar_streaming.h), which would silently show a browser rather
  // than the widget.
  QarGuiPanelStreamIdExt stream_ext = qar_gui_panel_stream_id_ext_default();
  stream_ext.stream_id = stream_id;

  QarGuiPanelInit panel_init = qar_gui_panel_init_default();
  panel_init.header.next = &stream_ext;
  panel_init.display_name = desc_.display_name.empty() ? nullptr : desc_.display_name.c_str();
  panel_init.pose = desc_.placement.pose;
  panel_init.size = desc_.placement.size;

  r = qar_gui_panels_add_panel(session_, &panel_init, &panel_id_);
  if (qar_result_is_error(r)) {
    last_error_ = panel_result_error("qar_gui_panels_add_panel", r);
    return;
  }

  has_panel_ = true;
  ready_ = true;
  last_error_.clear();

  // The creation half of the pair the retirement line below completes. Printed
  // unconditionally, because it is the ONLY place the runtime's stream UUID and
  // the widget key are written next to each other: without it, a
  // `streamRequest/<uuid>` in the runtime trace names nothing we know.
  std::fprintf(
    stderr, "[qar-panel-adapter] %s panel '%s' created: id=%s stream=%s buffer=%ux%u px\n",
    local_time_hms_ms().c_str(), desc_.key.c_str(), panel_id_text(panel_id_).c_str(),
    stream_id_text_.c_str(), static_cast<unsigned>(desc_.buffer_width),
    static_cast<unsigned>(desc_.buffer_height));
  std::fflush(stderr);

  // The panel exists, so it can now be pointed at. Subscribing HERE rather
  // than in the RViz panel is what makes the subscription's lifetime exactly the
  // panel's — close() below is the only place it is torn down, and it runs
  // whether the panel went away, the widget resized, or the process is
  // shutting down. The content frame is the buffer we stream, which is what the
  // renderer decodes and therefore the frame the event's pixels are in.
  //
  // A failure here is NOT fatal: a panel that streams but takes no input is
  // strictly better than no panel, and it is the only outcome available on a
  // runtime without the gesture subscription API.
  if (input_) {
    // The content extent, NOT the buffer: the buffer is aligned up for the
    // encoder and the slack is transparent padding the widget never paints.
    const std::uint32_t content_w =
      desc_.content_width != 0 ? desc_.content_width : desc_.buffer_width;
    const std::uint32_t content_h =
      desc_.content_height != 0 ? desc_.content_height : desc_.buffer_height;
    if (!input_->subscribe_panel(desc_.key, panel_id_, content_w, content_h, stream_id_text_)) {
      std::fprintf(stderr,
        "[qar-panel-adapter] %s panel '%s' streams but takes no AR input: %s\n",
        local_time_hms_ms().c_str(), desc_.key.c_str(), input_->last_error().c_str());
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
        local_time_hms_ms().c_str(), desc_.key.c_str(), panel_id_text(panel_id_).c_str(),
        static_cast<unsigned>(content_w), static_cast<unsigned>(content_h));
      std::fflush(stderr);
    }
  }
}

bool
QarCApiPanelSenderAdapter::try_begin_frame(bool & out_ready)
{
  out_ready = false;
  not_ready_cause_ = qar_ar_core::PanelNotReadyCause::NoDestination;
  if (!ready_ || sender_ == nullptr) {
    if (last_error_.empty()) { last_error_ = "panel sender is not ready"; }
    return false;
  }

  bool sender_ready = false;
  const QarResult r = qar_video_sender_try_begin_frame(sender_, &sender_ready);
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
    if (no_transfers || reconnecting) {
      // Both are reported as not ready, and the caller counts them apart: nothing attached to
      // the stream yet is a different fault from a receiver that lost a stream it had.
      not_ready_cause_ = reconnecting ? qar_ar_core::PanelNotReadyCause::Reconnecting
                                      : qar_ar_core::PanelNotReadyCause::NoActiveTransfers;
      if (reconnecting && frames_shown_ > 0 && reconnect_noted_.first()) {
        std::fprintf(
          stderr, "%s\n",
          reconnecting_line(local_time_hms_ms(), desc_.key, stream_id_text_, frames_shown_)
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
  const QarResult r = qar_video_sender_frame_cpu(sender_, &frame);
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

  const QarResult shown = qar_video_sender_show_frame(sender_);
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
  if (!has_panel_ || session_ == nullptr) {
    last_error_ = "no live panel to place";
    return false;
  }

  const QarResult posed = qar_gui_panels_update_pose(session_, &panel_id_, &placement.pose);
  if (qar_result_is_error(posed)) {
    last_error_ = panel_result_error("qar_gui_panels_update_pose", posed);
    return false;
  }

  const QarResult sized = qar_gui_panels_change_size(session_, &panel_id_, &placement.size);
  if (qar_result_is_error(sized)) {
    last_error_ = panel_result_error("qar_gui_panels_change_size", sized);
    return false;
  }

  desc_.placement = placement;
  last_error_.clear();
  return true;
}

bool
QarCApiPanelSenderAdapter::set_state(QarGuiPanelState state)
{
  if (!has_panel_ || closed_ || session_ == nullptr) {
    last_error_ = "no live panel to change";
    return false;
  }

  const QarResult r = qar_gui_panels_set_state(session_, &panel_id_, state);
  if (qar_result_is_error(r)) {
    last_error_ = panel_result_error("qar_gui_panels_set_state", r);
    return false;
  }
  last_error_.clear();

  // The same shape as the creation line, so the change can be lined up against the host's own
  // log and against the runtime's trace. The input source reads a panel update as an AR-side
  // close only for the state CLOSED, so the HIDDEN or VISIBLE set here is not taken for one.
  std::fprintf(
    stderr, "[qar-panel-adapter] %s panel '%s' state -> %s\n", local_time_hms_ms().c_str(),
    desc_.key.c_str(), panel_state_text(state).c_str());
  std::fflush(stderr);
  return true;
}

qar_ar_core::PanelCloseReport
QarCApiPanelSenderAdapter::close(bool panel_closed_in_ar)
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

  const bool something_to_close = has_panel_ || sender_ != nullptr;
  const auto started = std::chrono::steady_clock::now();
  const auto step_line = [this](const char * what) {
    // Every line below carries the local wall-clock time, because the only way to
    // line a close up against a QarOS crash dump in %TEMP%\quaternar is by the
    // clock. Printed and flushed BEFORE each runtime call that follows it, so if
    // the process dies inside that call the last line in the log names it.
    std::fprintf(
      stderr, "[qar-panel-adapter] %s panel '%s' %s\n", local_time_hms_ms().c_str(),
      desc_.key.c_str(), what);
    std::fflush(stderr);
  };
  if (something_to_close) {
    char head[256] = {0};
    std::snprintf(
      head, sizeof(head) - 1, "closing: id=%s stream=%s (operator already closed it in AR: %s)",
      panel_id_text(panel_id_).c_str(), stream_id_text_.c_str(),
      panel_closed_in_ar ? "yes" : "no");
    step_line(head);
  }

  // STEP 1 — input FIRST, before the panel it is about. unsubscribe_panel does
  // not return until no click/hover callback for this panel is running, so from
  // here on nothing can arrive naming a panel that is being destroyed. It also
  // retires the panel-UPDATE subscription, which is what keeps the CLOSED state
  // the runtime republishes for OUR close_panel below from coming back to us as
  // "the operator closed this panel in AR".
  if (input_ && !desc_.key.empty()) {
    report.input_unsubscribed = input_->has_panel(desc_.key);
    input_->unsubscribe_panel(desc_.key);
    if (report.input_unsubscribed) {
      step_line("step 1/3: AR input + panel-update subscriptions cancelled");
    }
  }

  // STEP 2 — the panel, then (step 3) the stream it points at: closing the sender
  // first would leave a live panel referencing a retired stream. The runtime
  // confirms the order, also for dialog panels: the panel's State reaches 3
  // (closed) about a millisecond before the sender teardown begins.
  if (has_panel_ && session_ != nullptr) {
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
      const QarResult closed = qar_gui_panels_close_panel(session_, &panel_id_);
      if (qar_result_is_error(closed)) {
        last_error_ = panel_result_error("qar_gui_panels_close_panel", closed);
        report.panel = qar_ar_core::PanelCloseOutcome::Failed;
        report.panel_message = last_error_;
        step_line(
          ("step 2/3: " + last_error_ +
          " -- the video sender is destroyed regardless; the AR panel may still be on screen")
          .c_str());
      } else {
        report.panel = qar_ar_core::PanelCloseOutcome::Closed;
        step_line("step 2/3: qar_gui_panels_close_panel ran, ok");
      }
    }
    has_panel_ = false;
  }

  // STEP 3 — the video sender, unconditionally. Announced BEFORE the call (the
  // riskiest of the three: it is the one a crash dump has been about).
  if (sender_ != nullptr) {
    step_line("step 3/3: destroying the video sender");
    qar_video_sender_handle_destroy(sender_);
    sender_ = nullptr;
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
  if (!stream_id_text_.empty() && !retired_logged_) {
    retired_logged_ = true;
    const long long took_ms = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    const char * gui_panel = "none was open";
    switch (report.panel) {
      case qar_ar_core::PanelCloseOutcome::NoPanel:
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
      local_time_hms_ms().c_str(), desc_.key.c_str(), panel_id_text(panel_id_).c_str(),
      stream_id_text_.c_str(), took_ms, gui_panel,
      report.sender_destroyed ? "destroyed" : "none to destroy");
    std::fflush(stderr);
  }

  frame_reserved_ = false;
  ready_ = false;
  return report;
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
  QarSession * session = session_owner_->session();
  if (session == nullptr) {
    last_error_ = "QAR session handle is null";
    return nullptr;
  }

  auto adapter = std::make_shared<QarCApiPanelSenderAdapter>(session, desc, input_);
  if (!adapter->ready()) {
    last_error_ = adapter->last_error();
    return nullptr;
  }
  last_error_.clear();
  return adapter;
}

}  // namespace qar_qar_adapter
