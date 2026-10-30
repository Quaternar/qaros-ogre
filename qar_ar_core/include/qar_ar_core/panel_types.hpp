// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_TYPES_HPP_
#define QAR_AR_CORE__PANEL_TYPES_HPP_

/// @file panel_types.hpp
/// @brief Value types of the QarOS panel seam, shared by several hosts. Qt-FREE on purpose.
///
/// Everything a QarOS-side adapter needs to publish a panel lives here, in POD
/// form, so `qar_qar_adapter` can implement the seam (IQarPanelSenderAdapter in
/// qar_panel_sender.hpp) without gaining a Qt dependency — exactly as it implements
/// qar_ogre_streaming's Ogre-free IQarRenderSenderAdapter. The hosts that use these
/// types are `qar_qar_adapter` itself, qar_qt_streaming (its panel manager and streams)
/// and qar_rviz_plugin (its panel implements the diagnostics sink and reads the records).

#include "qar_ar_core/qar_streaming_c.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qar_ar_core
{

/// @brief Coarse result code returned by library operations.
enum class Status
{
  Ok = 0,
  InvalidArgument,  ///< A caller-supplied argument was null/out-of-range.
  InvalidState,     ///< Operation not valid in the current object state.
  AlreadyExists,    ///< A panel stream with this key already exists.
  NotFound,         ///< No panel stream for the requested key.
  GuiThreadRequired,///< Called off the Qt GUI thread (widgets are GUI-thread-only).
  SenderFailed      ///< The QarOS-side adapter refused the operation.
};

/// @brief Human-readable name for a @ref Status value (for logs/diagnostics).
const char * to_string(Status status);

/// @brief A @ref Status paired with an explanatory message.
struct Result
{
  Status status = Status::Ok;
  std::string message;

  bool ok() const { return status == Status::Ok; }
  explicit operator bool() const { return ok(); }

  static Result success() { return {Status::Ok, {}}; }
  static Result error(Status s, std::string msg) { return {s, std::move(msg)}; }
};

/// @brief Where and how big a GUI panel sits in the AR room.
///
/// Both members are real Qar ABI types, so this struct feeds
/// `qar_gui_panels_get_or_create` / `_update_pose` / `_change_size` unchanged.
struct PanelPlacement
{
  /// The panel's pose in room space, in QarOS PANEL SPACE: the origin is the
  /// midpoint of the TOP EDGE (the surface hangs below it), +X runs along that
  /// edge to the viewer's LEFT, +Y is up and -Z is the front-face normal. It is
  /// handed verbatim to `QarGuiPanelInit::pose`, so it cannot be anything else;
  /// qar_qt_streaming's panel_hit_test.hpp carries the full derivation.
  QarPose pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};  // orientation, then position
  QarGuiPanelSize size{0.0f, 0.0f, 1.0f};
};

/// @brief Everything a factory needs to mint one panel stream.
struct PanelDesc
{
  /// The manager's key for this panel: the same string that
  /// qar_qt_streaming's QtPanelStreamManager was given when the panel was added, and the one a
  /// `PanelPointerSample::panel_key` must carry to come back to
  /// it. An adapter that subscribes to QarOS pointer events needs it to
  /// translate a `QarGuiPanelId` back into something the library knows; nothing
  /// else reads it, and the display name is NOT a substitute (it is a title,
  /// may be empty, and need not be unique).
  std::string key;

  /// Shown by QarOS as the panel's title. Not an identifier.
  std::string display_name;

  /// The stable ASCII name QarOS derives the panel's id from (`QarGuiPanelInit::common_name`):
  /// the same name finds the same panel again in the next session. Built by the host from the
  /// key with `make_panel_common_name` (common_name.hpp). The adapters refuse an empty one.
  std::string common_name;

  /// Pixel dimensions of the CPU buffer we will hand over every frame. Fixed
  /// for the sender's lifetime — QarOS allocates the receiver from this layout,
  /// so a resize means a new sender (see qar_qt_streaming's QtPanelStream::resize).
  std::uint32_t buffer_width = 0;
  std::uint32_t buffer_height = 0;

  /// Part of that buffer the widget covers — see qar_qt_streaming's PanelGeometry::content_width.
  /// The frame an incoming pointer position must be normalised against. Zero
  /// (the default) means "the whole buffer", which is right whenever the buffer
  /// needed no alignment padding.
  std::uint32_t content_width = 0;
  std::uint32_t content_height = 0;

  /// Byte order of the buffer. R8G8B8A8 matches QImage::Format_RGBA8888, which
  /// is byte-ordered on every endianness (unlike Format_ARGB32).
  QarPixelFormat pixel_format = QAR_PIXEL_FORMAT_R8G8B8A8;

  PanelPlacement placement{};
};

/// @brief One captured widget frame, as a BORROWED CPU pixel buffer.
///
/// Valid only for the duration of the `submit_frame` call — the adapter must
/// copy, never retain. Rows are @ref stride_bytes apart, which may exceed
/// `width * 4` when Qt pads its scanlines.
struct PanelPixels
{
  const std::uint8_t * data = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride_bytes = 0;
};

/// @brief Why a panel sender reported no destination to write into when it was asked (a tick on
/// which `try_begin_frame` gave `out_ready == false`).
enum class PanelNotReadyCause
{
  /// The runtime answered with success and no frame cycle: nothing is reserved yet.
  NoDestination,
  /// The runtime answered MEDIA_STREAMING_NO_ACTIVE_TRANSFERS (604): no receiver is attached to
  /// the stream.
  NoActiveTransfers,
  /// The runtime answered STREAM_IS_RECONNECTING (603): a receiver was attached, lost the stream
  /// and is connecting again.
  Reconnecting,
  /// The runtime answered STREAM_IS_PAUSED (602): the receiver paused the stream, so nothing
  /// is wanted until it resumes. The sender is fine; "paused by the receiver".
  Paused,
  /// The panel and its video sender are still being made on the session's thread. Nothing is
  /// wrong and nobody is waiting for a frame yet.
  Creating
};

/// @brief The outcome of one queued call on a panel, handed back by drain_results() of
/// IQarPanelSenderAdapter and IQarUrlPanelAdapter.
struct PanelOpResult
{
  enum class Op { Create, Placement, State, Navigate, VisibleTo, Close, Detach };
  Op op = Op::Create;
  bool ok = true;
  /// Op::State: the state that was asked for.
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  /// Op::Create: the panel of an earlier session was found again.
  bool reattached = false;
  /// The runtime's words when !ok.
  std::string error;
};

/// @brief Rolling accounting for one panel stream.
struct PanelStreamDiagnostics
{
  std::string key;
  std::string display_name;
  std::uint32_t buffer_width = 0;
  std::uint32_t buffer_height = 0;
  float width_meters = 0.0f;
  float height_meters = 0.0f;
  /// What was last sent to QarOS as QarGuiPanelSize::content_scale, which is a
  /// ZOOM (see third_party/qar/include/qar_streaming.h). Always 1.0: a zoom
  /// above 1.0 pushes the content past the panel edge (hover and click land
  /// offset), so the HiDPI/supersample ratio is never sent as content_scale.
  float content_scale = 1.0f;
  /// The dpr x render_scale supersample ratio behind buffer_width/height.
  /// Diagnostics-only: never sent as content_scale (see above).
  double device_pixel_ratio = 1.0;
  /// @brief One-line geometry summary for the panel sender most recently
  /// built (see @ref adapter_rebuilds): logical size, device pixel ratio,
  /// render (supersample) scale, the effective ratio they combine to, the
  /// content_scale actually sent (always 1.0, see above), content/buffer
  /// pixel size and physical size. Refreshed on every successful geometry
  /// read; qar_qt_streaming's QtPanelStreamManager::tick() logs it to the .err.log exactly once
  /// per rebuild, diffing @ref adapter_rebuilds the same way it already
  /// diffs @ref last_error.
  std::string last_geometry_summary;
  bool active = false;
  /// False means the source widget is hidden, so the streamed frame carries only
  /// its background — the panel looks blank in AR. See qar_qt_streaming's qt_widget_capture.hpp.
  bool widget_visible = false;

  /// @name Tombstone state — the panel outlives its source window
  ///
  /// Only in PanelCloseMode::Notice of qar_qt_streaming (the default, Destroy, never produces one).
  /// Notice is the escape hatch for a QarOS runtime that does not survive
  /// destroying a video sender mid-session (the current runtime does), so it
  /// keeps the video sender for the whole session: on such a runtime a sender
  /// destroyed mid-session leaves the AR side's receiver reconnecting, which
  /// starves the other streams of that peer
  /// (docs/QAROS_INTEGRATION_NOTES.md#teardown-and-destroy-order).
  /// When the streamed window closes, the
  /// stream keeps running and switches to an owned notice widget
  /// (qar_qt_streaming's panel_tombstone.hpp) telling the operator the source is gone
  /// and they may close the panel in AR. These three fields are how an operator tells
  /// that state apart from a live panel at a glance.
  /// @{

  /// True once the source window is gone and this stream is showing the notice.
  /// It keeps capturing and submitting, but nothing the RViz side does can
  /// change what it shows.
  bool tombstoned = false;

  /// Why, in one phrase, for the status line: "the window was closed in RViz",
  /// "the dialog was dismissed", ... Empty while live.
  std::string tombstone_reason;

  /// The notice does not fit the panel's width even at the ISO reading floor,
  /// so the operator is seeing a cut-off window name. See
  /// qar_qt_streaming's panel_tombstone.hpp — the panel was too narrow for its own name.
  bool tombstone_clipped = false;

  /// The operator closed this panel in AR (observed through
  /// `qar_gui_panels_subscribe_updates`, state reaching closed), so capture has
  /// stopped. **The sender is still alive** and stays so until the Qt window
  /// closes (Destroy mode) or the session shuts down; whichever tears it down
  /// then skips `qar_gui_panels_close_panel`, because the operator already did.
  /// See qar_qt_streaming's QtPanelStream::suspend_capture.
  bool ar_panel_closed = false;

  /// @}

  /// @name AR visibility
  ///
  /// A panel can be hidden in AR without being closed (IQarPanelSenderAdapter::set_state):
  /// the QarOS panel, its video sender and its subscriptions stay, capture is paused and input
  /// is dropped until the panel is shown again. See qar_qt_streaming's
  /// QtPanelStream::request_ar_visible.
  /// @{

  /// True while the panel is hidden in AR.
  bool ar_hidden = false;
  /// Ticks that sat out because the panel was hidden.
  std::uint64_t frames_skipped_hidden = 0;
  /// Hide and show requests the QarOS side carried out.
  std::uint64_t visibility_changes = 0;
  /// Hide and show requests it refused, or could not carry out (no adapter support).
  std::uint64_t visibility_failures = 0;
  /// Why the most recent request failed; kept until the next one succeeds.
  std::string last_visibility_error;

  /// @}

  /// @name Park state
  ///
  /// In qar_qt_streaming's PanelCloseMode::Park a Qt-side close does not close anything: the
  /// panel is hidden in AR (see above) and its QarOS panel, video sender and subscriptions are
  /// kept until the session ends. The source window may be gone; showing a window on the stream
  /// again revives it.
  /// @{

  /// True from the Qt-side close that parked the stream until a window is shown on it again.
  bool parked = false;

  /// @}
  std::uint64_t frames_captured = 0;
  std::uint64_t frames_submitted = 0;
  /// Ticks where the captured pixels were byte-identical to the previous frame,
  /// so nothing was sent. A static control panel should sit almost entirely here.
  std::uint64_t frames_skipped_unchanged = 0;
  /// Ticks where QarOS had no ready destination (`try_begin_frame` -> false).
  std::uint64_t frames_skipped_not_ready = 0;
  /// Ticks shed by the per-panel FPS cap.
  std::uint64_t frames_skipped_paced = 0;
  /// Ticks a frame-driven panel (video) sat out because its source had published
  /// nothing new — qar_qt_streaming's capture_gate.hpp. Per TICK, like @ref frames_skipped_paced.
  std::uint64_t frames_skipped_no_new_frame = 0;
  /// True while this panel's content has a live LatestVideoFrame (qar_qt_streaming) source and is
  /// therefore captured when a frame arrives instead of on the FPS timer.
  bool frame_driven = false;

  /// @name Interval between consecutive SUBMITTED frames
  /// What the operator actually experiences as (un)smooth, in ms, measured on
  /// the clock `tick` is given. A WINDOW: the host reads it and then calls
  /// qar_qt_streaming's QtPanelStreamManager::reset_submit_gap_windows(), so the
  /// numbers describe the period since the previous read. The first submit after a sender
  /// (re)build has no predecessor and adds nothing.
  /// @{
  double submit_gap_max_ms = 0.0;
  double submit_gap_sum_ms = 0.0;
  std::uint64_t submit_gap_count = 0;
  /// @}
  /// Consecutive ticks that ended in `try_begin_frame` reporting "not ready",
  /// reset by the next successful submit. A number that only ever grows means
  /// nothing is attached to this panel's video stream — which is a STATE, not an
  /// error (see qar_panel_sender.hpp), and this is where it is visible.
  std::uint64_t not_ready_streak = 0;
  /// @ref not_ready_streak by cause (see PanelNotReadyCause): the three add up to it and are
  /// reset with it. Which of them grows says whether nothing is reserved (the first), nothing is
  /// attached (the second) or the receiver lost the stream (the third).
  std::uint64_t not_ready_streak_no_destination = 0;
  std::uint64_t not_ready_streak_no_transfers = 0;
  std::uint64_t not_ready_streak_reconnecting = 0;

  /// @brief Identity of the logical video stream QarOS knows this panel's
  /// content by — see IQarPanelSenderAdapter::stream_id_text. Empty until a
  /// sender exists, and kept after the sender is retired so the retirement can
  /// still name the stream a receiver may go on asking for.
  std::string stream_id;

  /// @brief Seconds of wall time this panel has had a live sender and not
  /// managed to submit a single frame, measured on the same clock @ref tick is
  /// given. Reset by every successful submit.
  ///
  /// The companion to @ref not_ready_streak, and the one a human can act on: a
  /// streak is a count of ticks whose rate you have to know, whereas "we have
  /// had a sender for this panel for 9 seconds and have never answered"
  /// describes the fault on its own. QarOS re-asks for an unanswered stream on
  /// its own schedule, so a panel sitting here is a panel the headset is
  /// waiting on.
  double unserved_seconds = 0.0;

  /// @brief True once @ref unserved_seconds has crossed the manager's
  /// threshold and the condition has been reported. Latched so the warning is
  /// emitted once per unserved run, not once per tick.
  bool unserved_reported = false;

  /// QarOS senders built for this panel, first one included. The sender's buffer
  /// layout is fixed at creation, so every entry here is a full teardown and
  /// rebuild of the sender, the QarOS panel and its input subscription. It should
  /// be 1 for a panel that has simply been streaming; a climbing number is
  /// resize churn (see qar_qt_streaming's PanelStreamOptions::resize_* for the hysteresis).
  std::uint64_t adapter_rebuilds = 0;
  /// Widget size changes too small to be worth a rebuild, absorbed by streaming
  /// on at the live sender's layout.
  std::uint64_t resizes_ignored_small = 0;
  /// True while a significant size change is being timed out before it is
  /// adopted; the panel keeps streaming at the committed size meanwhile.
  bool resize_pending = false;
  /// The size being timed, when @ref resize_pending.
  std::uint32_t pending_buffer_width = 0;
  std::uint32_t pending_buffer_height = 0;

  double last_capture_ms = 0.0;
  double avg_capture_ms = 0.0;

  /// @name Brightness of the frame most recently measured
  ///
  /// What a panel SENDS, in one number each: a panel that shows black in AR while its frames
  /// carry content is a receiver problem, one whose frames are black is a capture problem. The
  /// stream measures the frame it has just submitted at most once a second, from a sparse
  /// sample of its pixels (qar_qt_streaming's frame_luminance.hpp). Luminance and alpha run from
  /// 0 to 255; a negative value means nothing has been measured yet.
  /// @{

  /// Mean luminance over every sampled row.
  double luminance_all = -1.0;
  /// ... over the sampled rows in the top 15 % of the frame (a title or tab strip, say), and
  /// over the sampled rows below them (the page): a frame whose top is bright and whose rest is
  /// black has lost its page.
  double luminance_top = -1.0;
  double luminance_rest = -1.0;
  /// Mean alpha over every sampled pixel. A frame that is transparent shows nothing in AR.
  double alpha_mean = -1.0;
  /// The @ref frames_submitted value of the frame these were measured on; 0 before any.
  std::uint64_t luminance_frame = 0;

  /// @}

  /// Pointer samples the INPUT SOURCE produced for this panel, counted the
  /// moment they are drained and before anything can discard them. This is the
  /// number that separates "the runtime is sending us nothing" from "events
  /// arrive and we drop them": zero here with a panel that is visibly being
  /// poked means the problem is upstream of this library, and no counter below
  /// it can tell you that.
  std::uint64_t input_samples_received = 0;

  /// Qt input events synthesized into the widget tree from AR pointer actions
  /// (see qar_qt_streaming's qt_panel_input_injector.hpp), and the actions that could not become
  /// one — with the most recent reason: off the GUI thread, widget destroyed,
  /// no widget under the pointer accepts touch, a second pointer while the
  /// first holds the (single) mouse, ...
  std::uint64_t input_events_delivered = 0;
  std::uint64_t input_events_dropped = 0;
  std::string last_input_drop_reason;

  std::string last_error;
};

/// @brief The not-ready streak of a panel by cause, as the heartbeat prints it:
/// `<no destination>/<604>/<603>`, for example `6/0/0`.
inline std::string
not_ready_split_text(const PanelStreamDiagnostics & panel)
{
  const auto text = [](std::uint64_t n) {
      return std::to_string(static_cast<unsigned long long>(n));
    };
  return text(panel.not_ready_streak_no_destination) + "/" +
         text(panel.not_ready_streak_no_transfers) + "/" +
         text(panel.not_ready_streak_reconnecting);
}

/// @brief Whole-manager snapshot (plain value type, copyable without a lock).
struct PanelDiagnosticsSnapshot
{
  bool streaming_active = false;
  std::uint32_t panel_count = 0;
  std::uint64_t ticks = 0;

  /// @name Tombstone accounting
  ///
  /// A tombstoned stream holds a video sender and a QarOS GUI panel for the
  /// rest of the session by design (see PanelStreamDiagnostics::tombstoned), so
  /// the number of them is a resource the operator has to be able to see. These
  /// are what the status line and the heartbeat show.
  /// @{

  /// Panels still showing their real window. `panel_count - live_panels` is the
  /// number that are notices.
  std::uint32_t live_panels = 0;
  /// Panels showing the "source window closed" notice.
  std::uint32_t tombstoned_panels = 0;
  /// Of those, the ones the operator has also closed in AR: capture stopped,
  /// sender still held until session shutdown.
  std::uint32_t ar_closed_panels = 0;
  /// Tombstones created over the whole session — a rate, where
  /// @ref tombstoned_panels is a level.
  std::uint64_t tombstones_created = 0;
  /// Tombstoned slots re-adopted by a new widget instead of minting a sender.
  /// This is the number that keeps a repeated open/close cycle bounded.
  std::uint64_t tombstones_revived = 0;
  /// Times a panel was NOT created because the tombstone cap was already
  /// reached (see qar_qt_streaming's QtPanelStreamingConfig::max_tombstoned_panels).
  std::uint64_t tombstone_cap_refusals = 0;

  /// @}

  /// Panels hidden in AR (PanelStreamDiagnostics::ar_hidden): capture paused, nothing closed.
  /// A hidden panel still counts in @ref live_panels or @ref tombstoned_panels.
  std::uint32_t hidden_panels = 0;

  /// @name Park accounting (qar_qt_streaming's PanelCloseMode::Park)
  /// @{

  /// Panels parked by a Qt-side close (PanelStreamDiagnostics::parked). A parked panel is not a
  /// notice, so it counts in @ref live_panels, and in @ref hidden_panels once QarOS has accepted
  /// the hide.
  std::uint32_t parked_panels = 0;
  /// Qt-side closes that parked a stream, over the whole session: a rate, where
  /// @ref parked_panels is a level.
  std::uint64_t parks = 0;
  /// Parked streams that a window was shown on again.
  std::uint64_t park_revivals = 0;

  /// @}

  /// @name Close accounting (qar_qt_streaming's PanelCloseMode::Destroy)
  ///
  /// A panel closed in Destroy mode leaves no stream entry behind to be counted,
  /// so these are SESSION TOTALS — what the heartbeat's `closed=` reports next to
  /// the `live=` and `notice=` levels.
  /// @{

  /// Panels closed through the close path: AR panel closed (or skipped, below),
  /// video sender destroyed, stream entry dropped.
  std::uint64_t panels_closed = 0;
  /// ... of which `qar_gui_panels_close_panel` was NOT called because the
  /// operator had already closed the panel in AR.
  std::uint64_t close_panel_skipped = 0;
  /// ... of which `qar_gui_panels_close_panel` returned an error (the sender was
  /// destroyed and the entry dropped regardless).
  std::uint64_t close_panel_failures = 0;
  /// Closes that had to wait for a pointer delivery into the panel's own widget
  /// to return before the stream could be destroyed.
  std::uint64_t closes_deferred = 0;

  /// @}

  /// Pointer samples taken from the input source; how many landed on no panel
  /// (pointing at nothing); how many made no sense in the pointer's state
  /// (a release without a press). See qar_qt_streaming's PointerStateDiagnostics.
  std::uint64_t input_samples = 0;
  std::uint64_t input_samples_missed = 0;
  std::uint64_t input_samples_dropped = 0;
  std::string last_input_drop_reason;

  std::string last_error;
  std::vector<PanelStreamDiagnostics> panels;
};

/// @brief Severity for a diagnostics message.
enum class DiagnosticSeverity
{
  Info,
  Warning,
  Error
};

const char * to_string(DiagnosticSeverity severity);

/// @brief Sink for structured diagnostics emitted by qar_qt_streaming's panel manager.
///
/// The consumer (RViz panel, a standalone Qt app, a test harness) implements
/// this so the reusable library never depends on a specific logging/UI
/// framework. Called on the Qt GUI thread; must not block.
class PanelDiagnosticsSink
{
public:
  virtual ~PanelDiagnosticsSink() = default;

  /// @brief Emit one diagnostic line. @p panel_key may be empty for
  /// library-global messages.
  virtual void on_message(
    DiagnosticSeverity severity,
    const std::string & category,
    const std::string & message,
    const std::string & panel_key) = 0;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__PANEL_TYPES_HPP_
