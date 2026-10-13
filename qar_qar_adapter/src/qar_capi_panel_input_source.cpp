// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_panel_input_source.hpp"

#include "qar_capi_input_internal.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

namespace qar_qar_adapter
{
namespace
{

/// Monotonic seconds. Panel gestures DO carry a timestamp now
/// (qar_gui_panel_gesture_get_timestamp), but it is the recogniser's own clock
/// and the header is explicit that it is "comparable only within one source
/// peer id" — not across peers, and not against anything of ours. Downstream
/// compares timestamps for double-click detection and for the liveness sweep,
/// both of which must work across peers, so arrival time on OUR monotonic clock
/// stays the seam's clock. The gesture's own timestamp is traced, not used.
double
monotonic_seconds()
{
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

/// Raw bytes of a peer id as a map key. Opaque and order-stable, which is all a
/// std::map needs; never parsed.
std::string
peer_key(const QarPeerId & peer)
{
  return std::string(reinterpret_cast<const char *>(&peer), sizeof(QarPeerId));
}

const char *
panel_gesture_kind_name(QarGuiPanelGestureKind kind)
{
  switch (kind) {
    case QAR_GUI_PANEL_GESTURE_KIND_HOVER: return "hover";
    case QAR_GUI_PANEL_GESTURE_KIND_TAP: return "tap";
    case QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS: return "long-press";
    case QAR_GUI_PANEL_GESTURE_KIND_DRAG: return "drag";
  }
  return "unknown-kind";
}

/// Content pixel -> panel (u, v), and the whole coordinate contract in one place.
///
///  * The event is in CONTENT SPACE: "origin at the top-left corner of the
///    displayed image, x_pixels increasing to the right, y_pixels increasing
///    downward". qar_qt_streaming's PanelHit and PanelPointerSample use exactly the
///    same convention — "`u` grows right, `v` grows DOWN (image convention)" — and
///    so does Qt's widget space. All three agree, so there is NO axis flip here: the shipped
///    header puts the origin top-left (see
///    docs/QAROS_INTEGRATION_NOTES.md#panel-pixel-coordinates-and-content_scale).
///  * content_scale is NOT applied. The renderer is documented to map the
///    contact "through QarGuiPanelSize::content_scale into content space"
///    before publishing; applying it again here would scale every click twice.
///  * +0.5 addresses the CENTRE of the pixel. A pixel is an area, and its
///    centre is what maps back to the same pixel through qar_qt_streaming's
///    `floor(u * width)`, at any device-pixel ratio.
///
/// @return false when the content size is unknown (nothing can be placed).
bool
uv_from_content_pixels(
  std::int32_t x_pixels, std::int32_t y_pixels, std::uint32_t width, std::uint32_t height,
  float & out_u, float & out_v, bool & out_clamped)
{
  if (width == 0 || height == 0) {
    return false;
  }
  // Documented as already clamped to [0, w-1] x [0, h-1], and SIGNED "for
  // headroom" (2026-09-21 SDK). Clamping anyway is not distrust of the runtime,
  // it is the panel having been resized under a subscription that was made for
  // the old size: a stale coordinate must land on the panel's edge, never past
  // the end of a widget — and a negative one must land on the near edge, not
  // wrap through an unsigned conversion.
  const std::int32_t last_x = static_cast<std::int32_t>(width) - 1;
  const std::int32_t last_y = static_cast<std::int32_t>(height) - 1;
  out_clamped = x_pixels < 0 || y_pixels < 0 || x_pixels > last_x || y_pixels > last_y;
  const std::int32_t x = x_pixels < 0 ? 0 : (x_pixels > last_x ? last_x : x_pixels);
  const std::int32_t y = y_pixels < 0 ? 0 : (y_pixels > last_y ? last_y : y_pixels);

  out_u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
  out_v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
  return true;
}

}  // namespace

std::uint32_t
panel_pointer_id_for(const QarPeerId & peer, qar_ar_core::PointerHand hand)
{
  // Two hands of one peer are two pointers, and two peers' hands are four --
  // each with its own grab and its own double-click memory. The 2026-09-21 SDK
  // is the first to report a hand; without one every AR contact in the room
  // shares kPanelPointerId and a second operator could steal a first one's press.
  std::uint32_t hash = 2166136261u;
  const auto * bytes = reinterpret_cast<const unsigned char *>(&peer);
  for (std::size_t i = 0; i < sizeof(QarPeerId); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  hash ^= static_cast<unsigned char>(hand);
  hash *= 16777619u;
  // Clear of 0 (the manager's implicit room pointer) and 1 (kPanelPointerId).
  return (hash % 0xFFFFF000u) + 16u;
}

// ===========================================================================
//  QarCApiPanelInputSource
// ===========================================================================

QarCApiPanelInputSource::QarCApiPanelInputSource(QarSession * session)
: session_(session)
{
}

QarCApiPanelInputSource::~QarCApiPanelInputSource()
{
  unsubscribe_all();
}

bool
QarCApiPanelInputSource::subscribe_panel(
  const std::string & panel_key, const QarGuiPanelId & id, std::uint32_t content_width,
  std::uint32_t content_height)
{
  if (session_ == nullptr) {
    last_error_ = "no QarSession (is QAR streaming started?)";
    return false;
  }
  if (panel_key.empty()) {
    last_error_ = "panel input needs a panel key to report samples against";
    return false;
  }
  if (content_width == 0 || content_height == 0) {
    last_error_ = "panel input needs a non-zero content size to normalise against";
    return false;
  }

  // Re-subscribing the same key (a widget resized, so the panel was recreated)
  // retires the old pair first: two live subscriptions would double every click.
  unsubscribe_panel(panel_key);

  PanelSubscription subscription;
  subscription.gesture_slot = detail::mint_slot(this);
  subscription.state_slot = detail::mint_slot(this);
  if (subscription.gesture_slot == nullptr || subscription.state_slot == nullptr) {
    // A minted slot is never freed, so a half-made set is simply disowned.
    detail::retire_slot(subscription.gesture_slot);
    detail::retire_slot(subscription.state_slot);
    last_error_ = "panel input slot arena is full (too many subscriptions this process)";
    return false;
  }
  for (detail::PointerSlot * slot : {subscription.gesture_slot, subscription.state_slot}) {
    slot->panel_key = panel_key;
    slot->content_width = content_width;
    slot->content_height = content_height;
    slot->pointer_id = kPanelPointerId;
  }

  // One token per subscription: cancelling one must not silence the other.
  QarResult r = qar_cancel_token_create(&subscription.gesture_token);
  if (qar_result_is_error(r)) {
    last_error_ = input_result_error("qar_cancel_token_create", r);
    retire(subscription);
    return false;
  }

  // ONE subscription for every kind, not one per kind. The header: "Prefer this
  // when you take more than one kind. Everything arrives on one reader, so a
  // hover and the drag that grows out of the same contact keep the order the
  // producer published them in." Order between a hover Ended and a drag Started
  // from the SAME contact is exactly what the state machine's grab depends on.
  r = qar_gui_panels_subscribe_all_gesture_events(
    session_, &id, &QarCApiPanelInputSource::on_gesture_event, subscription.gesture_slot,
    subscription.gesture_token);
  if (qar_result_is_error(r)) {
    last_error_ = input_result_error("qar_gui_panels_subscribe_all_gesture_events", r);
    retire(subscription);
    return false;
  }

  // The panel's own updates, which is how we learn the operator closed it in AR.
  // Subscribed HERE rather than in the sender adapter so it shares this class's
  // slot discipline: the callback arrives on a runtime thread, and the only safe
  // way to survive a retirement racing it is the disown-under-the-slot-mutex
  // dance the pointer callbacks already do.
  r = qar_cancel_token_create(&subscription.state_token);
  if (qar_result_is_error(r)) {
    last_error_ = input_result_error("qar_cancel_token_create", r);
    retire(subscription);
    return false;
  }
  r = qar_gui_panels_subscribe_panel_updates(
    session_, &id, &QarCApiPanelInputSource::on_panel_update, subscription.state_slot,
    subscription.state_token);
  if (qar_result_is_error(r)) {
    last_error_ = input_result_error("qar_gui_panels_subscribe_panel_updates", r);
    retire(subscription);
    return false;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    panels_[panel_key] = subscription;
    // Re-subscribing a key that was closed in AR (the slot was revived onto a
    // new widget) starts its close history afresh.
    closed_panels_.erase(panel_key);
  }
  last_error_.clear();
  return true;
}

void
QarCApiPanelInputSource::unsubscribe_panel(const std::string & panel_key)
{
  PanelSubscription subscription;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_key);
    if (it == panels_.end()) {
      return;
    }
    subscription = it->second;
    panels_.erase(it);
  }
  // Anything still held down on that panel must be let go before the
  // subscription that would have ended it goes away: a widget left mid-press
  // never gets its release, and with DRAG that is a real possibility.
  cancel_lifecycles_for_panel(panel_key);
  // Retire OUTSIDE mutex_: retire() waits on the slot mutex, which a callback
  // may hold, and that callback wants mutex_ to bump the stats. Holding both in
  // the other order would be the deadlock.
  retire(subscription);
}

void
QarCApiPanelInputSource::unsubscribe_all()
{
  std::map<std::string, PanelSubscription> taken;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    taken.swap(panels_);
  }
  cancel_lifecycles_for_panel(std::string());
  for (auto & entry : taken) {
    retire(entry.second);
  }
}

void
QarCApiPanelInputSource::retire(PanelSubscription & subscription)
{
  // Cancel first so the runtime stops delivering, then disown the slots so
  // anything already in flight finds nothing to touch. Only then destroy the
  // token handles.
  if (subscription.gesture_token != nullptr) {
    qar_cancel_token_cancel(subscription.gesture_token);
  }
  if (subscription.state_token != nullptr) {
    qar_cancel_token_cancel(subscription.state_token);
  }
  detail::retire_slot(subscription.gesture_slot);
  detail::retire_slot(subscription.state_slot);
  if (subscription.gesture_token != nullptr) {
    qar_cancel_token_handle_destroy(subscription.gesture_token);
    subscription.gesture_token = nullptr;
  }
  if (subscription.state_token != nullptr) {
    qar_cancel_token_handle_destroy(subscription.state_token);
    subscription.state_token = nullptr;
  }
  subscription.gesture_slot = nullptr;
  subscription.state_slot = nullptr;
}

void
QarCApiPanelInputSource::on_panel_update(QarGuiPanel * handle, void * user_state)
{
  // Library background thread, same rules as on_gesture_event: short, no
  // blocking, no Qt, no calling back into a subscribe function.
  auto * slot = static_cast<detail::PointerSlot *>(user_state);
  if (slot == nullptr || handle == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(slot->mutex);
  auto * self = static_cast<QarCApiPanelInputSource *>(slot->owner);
  if (self == nullptr) {
    return;  // retired; the owner may be long gone
  }
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  const QarResult r = qar_gui_panel_get_state(handle, &state);
  if (qar_result_is_error(r) || state != QAR_GUI_PANEL_STATE_CLOSED) {
    // Every other state is a panel that is still there: minimized and hidden
    // are the operator putting it away, not finishing with it, and we keep
    // streaming in case they bring it back.
    return;
  }
  {
    const std::lock_guard<std::mutex> stats_lock(self->mutex_);
    if (!self->closed_panels_.insert(slot->panel_key).second) {
      return;  // already reported; the runtime republishes a closed panel freely
    }
  }
  self->queue_.push_closed_panel(slot->panel_key);
}

void
QarCApiPanelInputSource::drain_closed_panels(std::vector<std::string> & out)
{
  queue_.drain_closed_panels(out);
}

void
QarCApiPanelInputSource::on_gesture_event(QarGuiPanelGesture * gesture, void * user_state)
{
  // Library background thread. Everything this does must be short, must not
  // block, must not call back into a subscribe function, and must not touch Qt.
  auto * slot = static_cast<detail::PointerSlot *>(user_state);
  if (slot == nullptr || gesture == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(slot->mutex);
  auto * self = static_cast<QarCApiPanelInputSource *>(slot->owner);
  if (self == nullptr) {
    return;  // retired; the owner may be long gone
  }
  self->offer(gesture, *slot);
}

void
QarCApiPanelInputSource::offer(QarGuiPanelGesture * gesture, const detail::PointerSlot & slot)
{
  const double now_s = monotonic_seconds();

  // ---------------------------------------------------------------------
  // Read EVERYTHING off the handle first.
  //
  // "valid only for the duration of this call" is not advice: the storage
  // behind it is the library's, and the moment this function returns it may be
  // reused. So the handle is touched only here, at the top, and nothing below
  // this block refers to it. The class already copied-and-queued; this keeps
  // that discipline against an API where it now actually matters.
  // ---------------------------------------------------------------------
  QarPeerId peer{};
  std::uint64_t gesture_id = 0;
  QarGuiPanelGestureKind kind = QAR_GUI_PANEL_GESTURE_KIND_HOVER;
  QarGesturePhase phase = QAR_GESTURE_PHASE_INSTANT;
  QarGuiPanelPoint position{0, 0};
  QarGuiPanelPoint start_position{0, 0};
  QarHand raw_hand = QAR_HAND_RIGHT;
  bool has_hand = false;
  std::uint8_t tap_count = 0;

  bool readable = !qar_result_is_error(
    qar_gui_panel_gesture_get_lifecycle(gesture, &peer, &gesture_id));
  readable = readable && !qar_result_is_error(
    qar_gui_panel_gesture_get_classification(gesture, &kind, &phase));
  readable = readable && !qar_result_is_error(
    qar_gui_panel_gesture_get_positions(gesture, &position, &start_position));
  if (readable) {
    // Diagnostic only, and documented as such, so a runtime that cannot supply
    // it must not cost us the whole event.
    has_hand = !qar_result_is_error(qar_gui_panel_gesture_get_hand(gesture, &raw_hand));
    if (qar_result_is_error(qar_gui_panel_gesture_get_tap_count(gesture, &tap_count))) {
      tap_count = 0;
    }
  }

  const qar_ar_core::PointerHand hand =
    has_hand ? qar_ar_core::pointer_hand_from_qar(raw_hand)
             : qar_ar_core::PointerHand::Unknown;

  qar_ar_core::PointerGesture pointer_gesture = qar_ar_core::PointerGesture::Hover;
  qar_ar_core::PointerPhase pointer_phase = qar_ar_core::PointerPhase::Updated;
  const bool classified = readable &&
    qar_ar_core::pointer_gesture_from_panel_kind(kind, pointer_gesture) &&
    qar_ar_core::pointer_phase_from_qar(phase, pointer_phase);

  qar_ar_core::PanelPointerSample sample;
  bool queue_it = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.events_received;

    // Ground truth for a live debug session: what the runtime actually handed
    // us, before anything downstream can discard it. Recorded here (a push under
    // a lock already held) and PRINTED in drain(), because this is the runtime's
    // callback thread and it must not do I/O.
    const bool trace =
      should_trace_event(stats_.events_received) && pending_log_.size() < kMaxPendingTrace;
    ReceivedEventLog record;
    record.index = stats_.events_received;
    record.panel_key = slot.panel_key;
    record.kind = readable ? panel_gesture_kind_name(kind) : "unreadable";
    record.phase = readable ? gesture_phase_name(phase) : "unreadable";
    record.x_pixels = position.x_pixels;
    record.y_pixels = position.y_pixels;
    record.gesture_id = gesture_id;
    record.hand = hand;
    record.tap_count = tap_count;

    if (!readable) {
      // There is no "invalid" flag; an accessor that refuses IS the
      // invalid event, and skipping means skipping: not a Leave, not a guess.
      ++stats_.events_invalid;
      record.drop_reason = "an accessor refused to read the handle";
      if (trace) {
        pending_log_.push_back(std::move(record));
      }
      return;
    }
    if (!classified) {
      // A kind or phase this build does not know: a future SDK enumerator.
      // Counted separately so it reads as "newer runtime" rather than "broken".
      ++stats_.events_unknown_kind;
      record.drop_reason = "gesture kind or phase outside the enums this build knows";
      if (trace) {
        pending_log_.push_back(std::move(record));
      }
      return;
    }

    switch (kind) {
      case QAR_GUI_PANEL_GESTURE_KIND_DRAG:
        ++stats_.events_drag;
        break;
      case QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS:
        ++stats_.events_long_press;
        break;
      case QAR_GUI_PANEL_GESTURE_KIND_TAP:
        if (tap_count >= 2) {
          ++stats_.events_multi_tap;
        }
        break;
      case QAR_GUI_PANEL_GESTURE_KIND_HOVER:
        break;
    }

    float u = 0.0f;
    float v = 0.0f;
    bool clamped = false;
    if (!uv_from_content_pixels(
        position.x_pixels, position.y_pixels, slot.content_width, slot.content_height, u, v,
        clamped))
    {
      record.drop_reason = "subscription has no content size";
      if (trace) {
        pending_log_.push_back(std::move(record));
      }
      return;
    }
    if (clamped) {
      ++stats_.events_clamped;
    }
    record.placed = true;
    record.u = u;
    record.v = v;
    if (trace) {
      pending_log_.push_back(std::move(record));
    }

    // One pointer per (peer, hand): a second operator, or the operator's other
    // hand, gets its own grab and its own double-click memory.
    sample.pointer_id = panel_pointer_id_for(peer, hand);
    sample.gesture = pointer_gesture;
    sample.phase = pointer_phase;
    sample.hand = hand;
    // The recogniser's own count when it has one. 0 for every kind but TAP, and
    // 0 is exactly what tells the state machine to fall back on inference.
    sample.tap_count = tap_count;
    sample.timestamp_s = now_s;
    sample.has_panel_uv = true;
    sample.panel_key = slot.panel_key;
    sample.u = u;
    sample.v = v;

    // Liveness bookkeeping. Only the phased kinds have a lifecycle to keep
    // alive; tap and long press are INSTANT and are gone the moment they land.
    // The key is (peer, gesture id, kind) because "the gesture id is unique
    // only within a source peer id" and is shared across kinds within it.
    const bool lifecycle_kind = pointer_phase != qar_ar_core::PointerPhase::Instant;
    if (lifecycle_kind) {
      LifecycleKey key;
      key.peer = peer_key(peer);
      key.gesture_id = gesture_id;
      key.kind = static_cast<int>(kind);
      if (pointer_phase == qar_ar_core::PointerPhase::Ended ||
        pointer_phase == qar_ar_core::PointerPhase::Canceled)
      {
        live_.erase(key);
      } else {
        LiveLifecycle & entry = live_[key];
        entry.pointer_id = sample.pointer_id;
        entry.panel_key = slot.panel_key;
        entry.gesture = pointer_gesture;
        entry.hand = hand;
        entry.u = u;
        entry.v = v;
        entry.last_seen_s = now_s;
      }
    }
    queue_it = true;
  }

  if (queue_it) {
    queue_.push(sample);
  }
}

void
QarCApiPanelInputSource::sweep_lifecycles(
  double now_s, std::vector<qar_ar_core::PanelPointerSample> & out)
{
  const double timeout = lifecycle_timeout_s_;
  if (timeout <= 0.0) {
    return;
  }
  std::vector<qar_ar_core::PanelPointerSample> cancels;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = live_.begin(); it != live_.end();) {
      if (now_s - it->second.last_seen_s < timeout) {
        ++it;
        continue;
      }
      // Three heartbeats of silence: the producer is gone, not idle. A drag
      // that dies here must not leave the widget holding the press, and a hover
      // that dies must not leave a button lit.
      ++stats_.lifecycle_timeouts;
      if (it->second.gesture == qar_ar_core::PointerGesture::Hover) {
        ++stats_.hover_timeouts;
      }
      qar_ar_core::PanelPointerSample cancel;
      cancel.pointer_id = it->second.pointer_id;
      cancel.gesture = it->second.gesture;
      cancel.phase = qar_ar_core::PointerPhase::Canceled;
      cancel.hand = it->second.hand;
      cancel.timestamp_s = now_s;
      // The last place it was seen, so a Cancel lands on the grabbed panel
      // rather than on nothing. A Hover cancel resolves to hover_end -> Leave in
      // qar_qt_streaming's PanelPointerStateMachine.
      cancel.has_panel_uv = true;
      cancel.panel_key = it->second.panel_key;
      cancel.u = it->second.u;
      cancel.v = it->second.v;
      cancels.push_back(std::move(cancel));
      it = live_.erase(it);
    }
  }
  for (auto & cancel : cancels) {
    out.push_back(std::move(cancel));
  }
}

void
QarCApiPanelInputSource::cancel_lifecycles_for_panel(const std::string & panel_key)
{
  std::vector<qar_ar_core::PanelPointerSample> cancels;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = live_.begin(); it != live_.end();) {
      if (!panel_key.empty() && it->second.panel_key != panel_key) {
        ++it;
        continue;
      }
      qar_ar_core::PanelPointerSample cancel;
      cancel.pointer_id = it->second.pointer_id;
      cancel.gesture = it->second.gesture;
      cancel.phase = qar_ar_core::PointerPhase::Canceled;
      cancel.hand = it->second.hand;
      cancel.timestamp_s = monotonic_seconds();
      cancel.has_panel_uv = true;
      cancel.panel_key = it->second.panel_key;
      cancel.u = it->second.u;
      cancel.v = it->second.v;
      cancels.push_back(std::move(cancel));
      it = live_.erase(it);
    }
  }
  // Queued rather than appended: this runs from unsubscribe, not from drain,
  // so the Qt thread collects them on its next tick like any other sample.
  for (const auto & cancel : cancels) {
    queue_.push(cancel);
  }
}

void
QarCApiPanelInputSource::flush_event_trace()
{
  std::vector<ReceivedEventLog> records;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (pending_log_.empty()) {
      return;
    }
    records.swap(pending_log_);
  }
  for (const ReceivedEventLog & r : records) {
    if (r.placed) {
      std::fprintf(
        stderr,
        "[qar-panel-input] #%llu panel='%s' %s/%s id=%llu hand=%s taps=%u x=%d y=%d "
        "-> u=%.4f v=%.4f\n",
        static_cast<unsigned long long>(r.index), r.panel_key.c_str(), r.kind, r.phase,
        static_cast<unsigned long long>(r.gesture_id), qar_ar_core::to_string(r.hand),
        static_cast<unsigned>(r.tap_count), r.x_pixels, r.y_pixels, static_cast<double>(r.u),
        static_cast<double>(r.v));
    } else {
      std::fprintf(
        stderr,
        "[qar-panel-input] #%llu panel='%s' %s/%s id=%llu x=%d y=%d -> SKIPPED (%s)\n",
        static_cast<unsigned long long>(r.index), r.panel_key.c_str(), r.kind, r.phase,
        static_cast<unsigned long long>(r.gesture_id), r.x_pixels, r.y_pixels,
        r.drop_reason.c_str());
    }
  }
  std::fflush(stderr);
}

void
QarCApiPanelInputSource::drain(std::vector<qar_ar_core::PanelPointerSample> & out)
{
  queue_.drain(out);

  // Qt thread: safe to do I/O. Printing here rather than in the callback is the
  // whole point of pending_log_.
  flush_event_trace();

  // The liveness backstop. A hover that ENDS says so (HOVER has a real
  // lifecycle since the 2026-09-21 SDK), so this is not the ordinary way a
  // widget gets its Leave -- it is what happens when the producer stops talking
  // mid-gesture, which the header tells us to treat as a cancel after three
  // 250 ms heartbeats.
  sweep_lifecycles(monotonic_seconds(), out);
}

std::size_t
QarCApiPanelInputSource::subscription_count() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return panels_.size();
}

bool
QarCApiPanelInputSource::has_panel(const std::string & panel_key) const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return panels_.find(panel_key) != panels_.end();
}

PanelInputStats
QarCApiPanelInputSource::stats() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace qar_qar_adapter
