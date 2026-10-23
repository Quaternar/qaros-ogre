// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The INPUT half of the QarOS panel path: pointer events on streamed QWidget
// panels.
//
// The 2026-09-21 SDK drop replaced the two discrete entry points
// (subscribe_click_events / subscribe_hover_events, delivering a flat
// QarGuiPanelPointerEvent) with ONE gesture stream:
//
//   qar_gui_panels_subscribe_all_gesture_events(session, id, cb, state, token)
//
// delivering an opaque QarGuiPanelGesture* read through accessors. What arrives
// is not "a completed click" but a classified gesture with a LIFECYCLE —
// HOVER, TAP, LONG_PRESS and, decisively, DRAG with started / updated / ended /
// canceled phases. This file binds that to the input seam that qar_qt_streaming
// consumes (qar_ar_core/panel_input_source.hpp), so a streamed QWidget reacts
// to being poked AND to being dragged in AR.
//
// It stays Qt-FREE, like every other adapter here: it implements the seam's
// pure-virtual IQarPanelInputSource and fills the seam's POD sample. That works
// because the seam lives in the Qt-free qar_ar_core and its definitions are inline
// (see the note at the top of panel_input_source.hpp).

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_PANEL_INPUT_SOURCE_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_PANEL_INPUT_SOURCE_HPP_

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"
#include "qar_qar_adapter/qar_capi_input_slots.hpp"

namespace qar_qar_adapter
{

/// @brief Fallback pointer id, used when the gesture names no peer or hand.
///
/// Deliberately NOT 0: qar_qt_streaming's QtPanelStreamManager::route_room_point uses 0
/// for its implicit room-space pointer, and two sources sharing an id would share the
/// state machine's grab and its double-click memory.
///
/// The runtime reports which hand made a contact (`qar_gui_panel_gesture_get_hand`,
/// since the 2026-09-21 SDK) and the lifecycle names its source peer, so
/// @ref panel_pointer_id_for mints one id per (peer, hand): two hands, or two
/// operators, do not fight over one grab and one double-click memory.
/// Still one id across ALL PANELS of a hand, not one per panel — a single
/// pointer moving from panel A to panel B must make the state machine emit
/// Leave-A then Enter-B, and a per-panel id would leave A hovered forever.
constexpr std::uint32_t kPanelPointerId = 1;

/// @brief Stable pointer id for one hand of one peer.
///
/// FNV-1a over the peer id bytes plus the hand, forced clear of the ids the
/// library reserves (0 = the manager's implicit room pointer, 1 =
/// @ref kPanelPointerId). Pure, so a test can predict it.
std::uint32_t panel_pointer_id_for(
  const QarPeerId & peer, qar_ar_core::PointerHand hand);

/// @brief Diagnostics for one pointer source. Read on the Qt GUI thread.
///
/// A callback that arrives AFTER its subscription was retired is deliberately
/// absent from these numbers: by then it has no owner to count itself on, which
/// is precisely what makes it harmless.
struct PanelInputStats
{
  /// Callbacks entered.
  std::uint64_t events_received = 0;
  /// Events dropped because an accessor could not read the handle. Since the
  /// 2026-09-21 SDK there is no invalid gesture (the flat event's `valid` flag
  /// is gone), only a handle we failed to read.
  std::uint64_t events_invalid = 0;
  /// Events whose pixel position was outside the content rectangle and had to
  /// be clamped. The renderer is documented to clamp already, so a non-zero
  /// count means our idea of the content size and its have drifted apart.
  std::uint64_t events_clamped = 0;
  /// Synthetic cancels produced by the liveness sweep — see
  /// @ref QarCApiPanelInputSource::set_lifecycle_timeout.
  std::uint64_t lifecycle_timeouts = 0;
  /// ... of which HOVER lifecycles, i.e. the "the finger left the panel"
  /// case.
  std::uint64_t hover_timeouts = 0;
  /// Gestures whose kind or phase was outside the enums this build knows. A
  /// future SDK adding a kind lands here instead of being misread as HOVER.
  std::uint64_t events_unknown_kind = 0;
  /// TAP gestures that arrived with a native tap_count >= 2, i.e. a double
  /// click the recogniser counted for us rather than one we inferred.
  std::uint64_t events_multi_tap = 0;
  /// DRAG gestures seen, any phase. The number that says whether AR drag is
  /// actually reaching the widget.
  std::uint64_t events_drag = 0;
  /// LONG_PRESS gestures seen. Carried through the seam, consumed by nothing.
  std::uint64_t events_long_press = 0;
};

/// @brief Feeds qar_qt_streaming's QtPanelStreamManager from the QarOS per-panel pointer events.
///
/// ## What the API delivers
///
/// ONE subscription per panel —
/// `qar_gui_panels_subscribe_all_gesture_events` — carrying every kind in
/// publish order. The header is explicit that this is the right call when a
/// consumer wants more than one kind: per-kind subscriptions are separate
/// readers with no order between them, and "a hover and the drag that grows out
/// of the same contact" must not overtake each other.
///
///   * **TAP** -> `PointerGesture::Click` / `PointerPhase::Instant`, which the
///     state machine expands into Press + Release + Click. Its native
///     `tap_count` rides along, so a double click is the recogniser's answer
///     rather than our timestamp guess;
///   * **DRAG** -> `PointerGesture::Click` with the real
///     Started / Updated / Ended / Canceled phases. A press that persists
///     across moves is exactly what the state machine's grab was built for, so
///     a QSlider handle, a scrollbar thumb, text selection, drag-and-drop and
///     rubber-band selection all work;
///   * **HOVER** -> `PointerGesture::Hover`, with a real lifecycle:
///     STARTED on entry, UPDATED as the contact moves, **ENDED on exit**. The
///     runtime says "the pointer left", so a hover-idle Leave synthesised by the
///     adapter is not the only signal — see
///     @ref set_lifecycle_timeout, which keeps a narrower version of it as a
///     backstop against a producer that dies mid-gesture;
///   * **LONG_PRESS** -> `PointerGesture::LongPress`, carried through the seam
///     and consumed by nothing yet;
///   * **hand identity** rides on every sample, so two hands are two pointers.
///
/// ## Lifecycle keys
///
/// A gesture id is unique only WITHIN a source peer id: "two users' first drags
/// both carry gesture id 1". Everything this class remembers per lifecycle is
/// therefore keyed on the (peer, gesture id, kind) triple the accessors return
/// together, never on the id alone.
///
/// ## Threading
///
/// `subscribe_panel`, `unsubscribe_panel` and `drain` are for the Qt GUI thread.
/// The QarOS callback runs on a library background thread and does the minimum
/// the header demands of it: convert, push, return. It takes exactly one lock
/// (its slot's) and one more inside the queue, touches no Qt, and never calls
/// back into a subscribe function.
class QarCApiPanelInputSource : public qar_ar_core::IQarPanelInputSource
{
public:
  /// @param session Borrowed, owned by QarCApiSenderFactory; must outlive this.
  explicit QarCApiPanelInputSource(QarSession * session);

  /// Retires every live subscription first, so no callback can be running by
  /// the time the queue underneath it goes away.
  ~QarCApiPanelInputSource() override;

  QarCApiPanelInputSource(const QarCApiPanelInputSource &) = delete;
  QarCApiPanelInputSource & operator=(const QarCApiPanelInputSource &) = delete;

  /// @brief Subscribe to every gesture kind on one panel.
  ///
  /// @param panel_key         The manager's key (PanelDesc::key); stamped on
  ///                          every sample this panel produces.
  /// @param id                The QarGuiPanelId `qar_gui_panels_add_panel` gave.
  /// @param content_width     Extent of the streamed image the widget actually
  /// @param content_height    covers, in pixels (PanelDesc::content_width) —
  ///                          the frame the incoming coordinates are in. NOT
  ///                          the encoder-aligned buffer size: the padding on
  ///                          its right and bottom edges is transparent slack
  ///                          the widget never paints, and dividing by it would
  ///                          pull every click a pixel towards the top-left.
  /// @param our_stream_id     The stream the adapter built this panel's sender with,
  ///                          canonical UUID text; empty if it has none. The runtime's record
  ///                          of the panel is compared with it (see @ref drain).
  /// @return false and records @ref last_error if the runtime refused. Every
  ///         panel reports gestures now, website panels included — "nothing
  ///         branches on content type on the publish path".
  bool subscribe_panel(
    const std::string & panel_key,
    const QarGuiPanelId & id,
    std::uint32_t content_width,
    std::uint32_t content_height,
    const std::string & our_stream_id = std::string());

  /// @brief Retire @p panel_key's subscriptions. Idempotent, and by the time it
  /// returns no callback for that panel is running or ever will be again.
  /// Any lifecycle still open on that panel is cancelled into the queue first,
  /// so a widget cannot be left holding a press nobody will release.
  void unsubscribe_panel(const std::string & panel_key);

  /// @brief Retire every subscription.
  void unsubscribe_all();

  /// @brief Append everything received since the previous call, oldest first.
  ///
  /// Also the place the lifecycle timeout fires (see @ref set_lifecycle_timeout) and where the
  /// lines about the runtime's own record of each panel are printed: one for every change of its
  /// state, of the stream its content is or of its size, with the stream compared to the one the
  /// adapter built the panel's sender with.
  void drain(std::vector<qar_ar_core::PanelPointerSample> & out) override;

  /// @brief Append the key of every panel the operator has CLOSED IN AR.
  ///
  /// Fed by `qar_gui_panels_subscribe_panel_updates`: the runtime republishes a
  /// panel whenever anything about it changes, and the change this cares about
  /// is its state reaching `QAR_GUI_PANEL_STATE_CLOSED`. The manager reacts by
  /// stopping capture for that panel, not by destroying its video sender: the
  /// operator closing a panel in AR does not end the stream, so the sender is
  /// released by the Qt-side close or at session shutdown
  /// (docs/QAROS_INTEGRATION_NOTES.md#teardown-and-destroy-order).
  ///
  /// Reported once per panel per close: the runtime republishes a closed panel
  /// as often as it likes, and the manager's reaction is idempotent anyway, but
  /// a stream of duplicates would make the counters unreadable.
  void drain_closed_panels(std::vector<std::string> & out) override;

  /// @brief Seconds of silence after which an OPEN lifecycle is treated as
  /// cancelled and a synthetic Canceled sample is produced for its pointer.
  ///
  /// Straight out of the header's liveness rule: "A live lifecycle is
  /// republished every 250 ms even when nothing changed. Treat one silent for
  /// three of those intervals as cancelled." The default is therefore 0.75 s,
  /// three heartbeats, and NOT a guess. It is what recovers a mixer that
  /// crashed mid-drag — the source peer id is the headset, which is still
  /// there, so the peer list would never notice.
  ///
  /// It is not the ONLY way a hover ends: HOVER carries
  /// QAR_GESTURE_PHASE_ENDED on exit, and that is the normal path. This is the
  /// backstop. Zero disables it.
  void set_lifecycle_timeout(double seconds) { lifecycle_timeout_s_ = seconds; }
  double lifecycle_timeout() const { return lifecycle_timeout_s_; }

  /// @brief Panels currently subscribed.
  std::size_t subscription_count() const;
  bool has_panel(const std::string & panel_key) const;

  PanelInputStats stats() const;
  const std::string & last_error() const { return last_error_; }

private:
  /// One panel: ONE gesture subscription (every kind, one reader, publish
  /// order preserved) plus the panel-update watch, each with its own token and
  /// its own slot, because the runtime keys a cancel to a subscription.
  struct PanelSubscription
  {
    detail::PointerSlot * gesture_slot = nullptr;
    /// Third subscription, and not an input one at all: panel UPDATES, watched
    /// so the library learns that the operator closed the panel in AR. It uses
    /// the same slot type because it needs the same two things — a panel key
    /// copied at subscribe time, and an owner pointer that can be disowned
    /// under a mutex so a late callback finds nothing to touch.
    detail::PointerSlot * state_slot = nullptr;
    QarCancelToken * gesture_token = nullptr;
    QarCancelToken * state_token = nullptr;
  };

  /// @brief The key of one gesture lifecycle.
  ///
  /// The gesture id is unique only within a source peer, and the same id is
  /// shared across kinds within that peer, so all three parts are needed to
  /// name one hover or one drag. Ordered so it can live in a std::map without
  /// a hash of its own.
  struct LifecycleKey
  {
    std::string peer;  ///< Raw bytes of the QarPeerId.
    std::uint64_t gesture_id = 0;
    int kind = 0;  ///< QarGuiPanelGestureKind.

    bool operator<(const LifecycleKey & other) const
    {
      if (peer != other.peer) { return peer < other.peer; }
      if (gesture_id != other.gesture_id) { return gesture_id < other.gesture_id; }
      return kind < other.kind;
    }
  };

  /// @brief What the liveness sweep needs to cancel a lifecycle that went
  /// silent: who to cancel it for, where it was last seen, and when.
  struct LiveLifecycle
  {
    std::uint32_t pointer_id = 0;
    std::string panel_key;
    qar_ar_core::PointerGesture gesture = qar_ar_core::PointerGesture::Hover;
    qar_ar_core::PointerHand hand = qar_ar_core::PointerHand::Unknown;
    float u = 0.0f;
    float v = 0.0f;
    double last_seen_s = 0.0;
  };

  /// @brief One received pointer event, remembered just long enough for the Qt
  /// thread to print it.
  ///
  /// The trace is recorded on the runtime's callback thread -- which must not do
  /// I/O -- and formatted in @ref drain, so a slow or blocked stderr can never
  /// stall the library's delivery thread. Rate-limited at the recording end, so
  /// a hover stream republished every frame cannot flood the log either.
  struct ReceivedEventLog
  {
    std::string panel_key;
    const char * kind = "?";
    const char * phase = "?";
    std::int32_t x_pixels = 0;
    std::int32_t y_pixels = 0;
    std::uint64_t gesture_id = 0;
    qar_ar_core::PointerHand hand = qar_ar_core::PointerHand::Unknown;
    std::uint8_t tap_count = 0;
    /// False when the event never became a (u, v): unreadable, a kind this
    /// build does not know, or a subscription whose content size is zero.
    bool placed = false;
    float u = 0.0f;
    float v = 0.0f;
    /// Why it was not placed; empty when it was.
    std::string drop_reason;
    /// 1-based @ref PanelInputStats::events_received at the time it arrived.
    std::uint64_t index = 0;
  };

  /// The runtime's entry point. Static, and deliberately tiny.
  static void on_gesture_event(QarGuiPanelGesture * gesture, void * user_state);

  /// The runtime's OTHER entry point: one panel was republished. Same shape and
  /// the same slot discipline as on_pointer_event; it reads the panel's state
  /// and, if it has reached closed, queues the key for the GUI thread.
  static void on_panel_update(QarGuiPanel * handle, void * user_state);

  /// Callback-thread half: read the handle, convert and queue. Never touches
  /// Qt, never blocks. Everything it needs is copied out of @p gesture before
  /// it returns: the handle is valid only for the duration of the callback.
  void offer(QarGuiPanelGesture * gesture, const detail::PointerSlot & slot);

  /// @brief Cancel every open lifecycle silent for longer than
  /// @ref lifecycle_timeout(), appending one synthetic Canceled sample each.
  /// Qt thread, from @ref drain.
  void sweep_lifecycles(
    double now_s, std::vector<qar_ar_core::PanelPointerSample> & out);

  /// @brief Cancel every open lifecycle on @p panel_key (empty = all of them).
  /// Queued, not appended, because this runs from unsubscribe rather than drain.
  void cancel_lifecycles_for_panel(const std::string & panel_key);

  /// Qt-thread half of the trace: print and forget whatever @ref offer recorded.
  void flush_event_trace();

  /// Qt-thread half of the record lines: print and forget whatever @ref on_panel_update queued.
  void flush_runtime_records();

  void retire(PanelSubscription & subscription);

  QarSession * session_ = nullptr;

  /// The seam's own queue — the single lock on the input path, reused rather
  /// than reinvented.
  qar_ar_core::QueuedPanelInputSource queue_;

  mutable std::mutex mutex_;  ///< Guards panels_, stats_, pending_log_ and the hover clock.
  std::map<std::string, PanelSubscription> panels_;
  PanelInputStats stats_{};

  /// Panel keys already reported closed, so the runtime republishing a closed
  /// panel does not queue the same key again and again. Guarded by `mutex_`.
  std::set<std::string> closed_panels_;

  /// Recorded by @ref offer, drained by @ref flush_event_trace. Bounded, so a
  /// manager that stopped ticking cannot turn the trace into a leak.
  std::vector<ReceivedEventLog> pending_log_;

  /// The runtime's records of the panels: queued by @ref on_panel_update, printed by
  /// @ref flush_runtime_records. Bounded like @ref pending_log_. Guarded by `mutex_`.
  RuntimeRecordLog record_log_;

  /// Open lifecycles, keyed on (peer, gesture id, kind) because a gesture id
  /// alone is ambiguous across peers. Guarded by `mutex_`. Instant kinds (tap,
  /// long press) never enter it — they have no lifecycle to keep alive.
  std::map<LifecycleKey, LiveLifecycle> live_;
  /// Three 250 ms heartbeats, the header's own rule. See set_lifecycle_timeout.
  double lifecycle_timeout_s_ = 0.75;

  std::string last_error_;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_PANEL_INPUT_SOURCE_HPP_
