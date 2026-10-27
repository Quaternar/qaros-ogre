// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_spatial_gesture_source.hpp"

#include "qar_capi_input_internal.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

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

/// Cheap, stable pointer id per peer: two operators pointing at the same app
/// volume must not share one pointer's grab or double-click memory. FNV-1a over
/// the id bytes, forced away from the ids the library reserves (0 = the
/// manager's implicit room pointer, 1 = kPanelPointerId).
std::uint32_t
pointer_id_for_peer(const QarPeerId & peer)
{
  std::uint32_t hash = 2166136261u;
  const auto * bytes = reinterpret_cast<const unsigned char *>(&peer);
  for (std::size_t i = 0; i < sizeof(QarPeerId); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return (hash % 0xFFFFF000u) + 16u;
}

bool
known_gesture_phase(QarGesturePhase phase)
{
  switch (phase) {
    case QAR_GESTURE_PHASE_STARTED:
    case QAR_GESTURE_PHASE_UPDATED:
    case QAR_GESTURE_PHASE_ENDED:
    case QAR_GESTURE_PHASE_INSTANT:
    case QAR_GESTURE_PHASE_CANCELED:
      return true;
  }
  return false;
}

/// The app-volume gesture mapping, reading one live `QarAppVolumeGesture` handle. The
/// accessors are real C API calls, so it lives here, in the adapter, and not in qar_ar_core
/// (which keeps the handle-free half, qar_ar_core::pointer_sample_from_gesture_fields). The
/// handle is valid only for the duration of the callback, which is why everything is
/// copied into @p out before returning.
bool
pointer_sample_from_gesture_event(
  QarAppVolumeGesture * gesture, std::uint32_t pointer_id, qar_ar_core::PanelPointerSample & out)
{
  if (gesture == nullptr) {
    return false;
  }
  QarGestureKind kind = QAR_GESTURE_CLICK;
  QarGesturePhase phase = QAR_GESTURE_PHASE_INSTANT;
  if (qar_result_is_error(
      QAR_CALL(qar_app_volume_gesture_get_classification, gesture, &kind, &phase)))
  {
    return false;
  }

  QarVector3 action_point{0.0f, 0.0f, 0.0f};
  QarVector3 start_point{0.0f, 0.0f, 0.0f};
  bool has_action_point = false;
  bool has_start_point = false;
  if (qar_result_is_error(
      QAR_CALL(
          qar_app_volume_gesture_get_points, gesture, &action_point, &has_action_point,
          &start_point, &has_start_point)))
  {
    return false;
  }

  QarTimePoint timestamp{};
  if (qar_result_is_error(QAR_CALL(qar_app_volume_gesture_get_timestamp, gesture, &timestamp))) {
    return false;
  }

  // NOTE: qar_app_volume_gesture_get_deltas() is deliberately NOT read here.
  // Its out_was_mapped_to_app_transform says the library already folded the
  // delta into the volume's app pose/scale, and applying it again is the
  // documented double-apply bug. A pointer sample carries a POSITION, never a
  // delta, so there is nothing here to double-apply.
  return qar_ar_core::pointer_sample_from_gesture_fields(
    kind, phase, has_action_point, action_point, timestamp, pointer_id, out, has_start_point,
    start_point);
}

}  // namespace

// ===========================================================================
//  QarCApiSpatialGestureSource
// ===========================================================================

QarCApiSpatialGestureSource::QarCApiSpatialGestureSource(QarSession * session)
: session_(session)
{
}

QarCApiSpatialGestureSource::~QarCApiSpatialGestureSource()
{
  unsubscribe();
}

bool
QarCApiSpatialGestureSource::subscribe_kind(
  const QarAppVolumeId & volume_id, QarGestureKind kind, detail::PointerSlot *& out_slot,
  QarCancelToken *& out_token)
{
  out_slot = detail::mint_slot(this);
  if (out_slot == nullptr) {
    last_error_ = "gesture slot arena is full (too many subscriptions this process)";
    return false;
  }
  const QarResult created = QAR_CALL(qar_cancel_token_create, &out_token);
  if (qar_result_is_error(created) || out_token == nullptr) {
    last_error_ = qar_result_is_error(created) ?
      input_result_error("qar_cancel_token_create", created) :
      std::string("qar_cancel_token_create gave no token");
    out_token = nullptr;
    return false;
  }
  // The token is required: it owns the subscription (a NULL one is refused by the runtime).
  const QarResult r = QAR_CALL(
      qar_app_volumes_subscribe_gesture_events, session_, &volume_id, kind,
      &QarCApiSpatialGestureSource::on_gesture_event, out_slot, out_token);
  if (qar_result_is_error(r)) {
    last_error_ = input_result_error("qar_app_volumes_subscribe_gesture_events", r);
    return false;
  }
  return true;
}

bool
QarCApiSpatialGestureSource::subscribe(const QarAppVolumeId & volume_id)
{
  if (session_ == nullptr) {
    last_error_ = "no QarSession (is QAR streaming started?)";
    return false;
  }
  unsubscribe();

  // ONE subscription for every kind, not one per kind -- mirrors the GUI
  // panel path (qar_gui_panels_subscribe_all_gesture_events, ~line 345) and
  // the header's own advice: "Prefer this when you take more than one kind.
  // Everything arrives on one reader ... several per-kind subscriptions are
  // separate readers" with no order between them. We dispatch on kind
  // ourselves in offer(), which already reads
  // qar_app_volume_gesture_get_classification() for every event: CLICK and
  // HOVER become pointer samples, and a manipulation kind is counted in
  // events_other_kind and never queued.
  detail::PointerSlot * all_slot = detail::mint_slot(this);
  if (all_slot == nullptr) {
    last_error_ = "gesture slot arena is full (too many subscriptions this process)";
    return false;
  }
  QarCancelToken * all_token = nullptr;
  QarResult r = QAR_CALL(qar_cancel_token_create, &all_token);
  if (qar_result_is_error(r) || all_token == nullptr) {
    last_error_ = qar_result_is_error(r) ?
      input_result_error("qar_cancel_token_create", r) :
      std::string("qar_cancel_token_create gave no token");
    detail::retire_slot(all_slot);
    return false;
  }
  r = QAR_CALL(
      qar_app_volumes_subscribe_all_gesture_events, session_, &volume_id,
      &QarCApiSpatialGestureSource::on_gesture_event, all_slot, all_token);
  if (!qar_result_is_error(r)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      all_slot_ = all_slot;
      all_token_ = all_token;
      volume_ = volume_id;
      has_volume_ = true;
    }
    std::fprintf(
      stderr,
      "[qar-spatial] subscribed via qar_app_volumes_subscribe_all_gesture_events "
      "(one reader; CLICK+HOVER dispatched on kind, manipulation kinds counted as "
      "other)\n");
    std::fflush(stderr);
    last_error_.clear();
    return true;
  }

  // FALLBACK: the unified call itself failed (e.g. an older runtime). Cancel
  // and free the slot/token minted for it, then fall back to the per-kind
  // CLICK + HOVER pair this class used before, so the source keeps working.
  const std::string all_error =
    input_result_error("qar_app_volumes_subscribe_all_gesture_events", r);
  QAR_CALL(qar_cancel_token_cancel, all_token);
  detail::retire_slot(all_slot);
  QAR_CALL(qar_cancel_token_handle_destroy, all_token);

  detail::PointerSlot * click_slot = nullptr;
  detail::PointerSlot * hover_slot = nullptr;
  QarCancelToken * click_token = nullptr;
  QarCancelToken * hover_token = nullptr;

  const bool ok_click =
    subscribe_kind(volume_id, QAR_GESTURE_CLICK, click_slot, click_token) &&
    subscribe_kind(volume_id, QAR_GESTURE_HOVER, hover_slot, hover_token);

  if (!ok_click) {
    for (QarCancelToken * token : {click_token, hover_token}) {
      if (token != nullptr) {
        QAR_CALL(qar_cancel_token_cancel, token);
      }
    }
    detail::retire_slot(click_slot);
    detail::retire_slot(hover_slot);
    for (QarCancelToken * token : {click_token, hover_token}) {
      if (token != nullptr) {
        QAR_CALL(qar_cancel_token_handle_destroy, token);
      }
    }
    last_error_ = all_error + "; fallback also failed: " + last_error_;
    return false;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    click_slot_ = click_slot;
    hover_slot_ = hover_slot;
    click_token_ = click_token;
    hover_token_ = hover_token;
    volume_ = volume_id;
    has_volume_ = true;
  }
  std::fprintf(
    stderr,
    "[qar-spatial] qar_app_volumes_subscribe_all_gesture_events failed (%s); fell "
    "back to per-kind qar_app_volumes_subscribe_gesture_events(CLICK) + (HOVER)\n",
    all_error.c_str());
  std::fflush(stderr);
  last_error_.clear();
  return true;
}

void
QarCApiSpatialGestureSource::unsubscribe()
{
  detail::PointerSlot * all_slot = nullptr;
  QarCancelToken * all_token = nullptr;
  detail::PointerSlot * click_slot = nullptr;
  detail::PointerSlot * hover_slot = nullptr;
  QarCancelToken * click_token = nullptr;
  QarCancelToken * hover_token = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::swap(all_slot, all_slot_);
    std::swap(all_token, all_token_);
    std::swap(click_slot, click_slot_);
    std::swap(hover_slot, hover_slot_);
    std::swap(click_token, click_token_);
    std::swap(hover_token, hover_token_);
  }
  for (QarCancelToken * token : {all_token, click_token, hover_token}) {
    if (token != nullptr) {
      QAR_CALL(qar_cancel_token_cancel, token);
    }
  }
  detail::retire_slot(all_slot);
  detail::retire_slot(click_slot);
  detail::retire_slot(hover_slot);
  for (QarCancelToken * token : {all_token, click_token, hover_token}) {
    if (token != nullptr) {
      QAR_CALL(qar_cancel_token_handle_destroy, token);
    }
  }
}

bool
QarCApiSpatialGestureSource::subscribed() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return all_slot_ != nullptr || (click_slot_ != nullptr && hover_slot_ != nullptr);
}

void
QarCApiSpatialGestureSource::on_gesture_event(
  QarAppVolumeGesture * gesture, void * user_state)
{
  auto * slot = static_cast<detail::PointerSlot *>(user_state);
  if (slot == nullptr || gesture == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(slot->mutex);
  auto * self = static_cast<QarCApiSpatialGestureSource *>(slot->owner);
  if (self == nullptr) {
    return;
  }
  self->offer(gesture);
}

void
QarCApiSpatialGestureSource::set_pointing_ray_slot(
  std::shared_ptr<qar_ar_core::PointingRaySlot> slot)
{
  pointing_ray_slot_ = std::move(slot);
}

void
QarCApiSpatialGestureSource::offer(QarAppVolumeGesture * gesture)
{
  // Read everything off the handle FIRST: it is valid only for the duration of
  // this call (the event is an opaque handle), so nothing below may touch it.
  QarPeerId peer{};
  QarAppVolumeId target{};
  QarGestureKind kind = QAR_GESTURE_CLICK;
  QarGesturePhase phase = QAR_GESTURE_PHASE_INSTANT;
  QarVector3 action_point{0.0f, 0.0f, 0.0f};
  QarVector3 start_point{0.0f, 0.0f, 0.0f};
  bool has_action_point = false;
  bool has_start_point = false;

  bool readable = !qar_result_is_error(
    QAR_CALL(qar_app_volume_gesture_get_lifecycle, gesture, &peer, &target));
  readable = readable && !qar_result_is_error(
    QAR_CALL(qar_app_volume_gesture_get_classification, gesture, &kind, &phase));
  readable = readable && !qar_result_is_error(
    QAR_CALL(
        qar_app_volume_gesture_get_points, gesture, &action_point, &has_action_point, &start_point,
        &has_start_point));

  // NOTE: qar_app_volume_gesture_get_deltas() is deliberately NOT read. Its
  // out_was_mapped_to_app_transform reports that the volume's
  // QarGestureAppTransformMapping already folded the delta into the app pose or
  // scale, and applying it again is the documented double-apply bug. A pointer
  // sample carries a POSITION and never a delta, so there is nothing here that
  // could be applied twice -- and the safe way to keep that true is not to read
  // the delta at all.

  // The field copy is the seam's own (qar_ar_core::pointer_sample_from_gesture_fields),
  // reached through this file's pointer_sample_from_gesture_event. It is reused
  // verbatim rather than re-derived, so the copy is fixed in one place. That function
  // reads the same handle through the same accessors, which is why the values above are
  // only for the stats and the trace.
  qar_ar_core::PanelPointerSample sample;
  const bool mapped = readable &&
    pointer_sample_from_gesture_event(
      gesture, pointer_id_for_peer(peer), sample);

  // WHY it was dropped, decided here rather than inferred later: the mapping
  // returns a bare bool and the ways it can fail are indistinguishable from
  // outside. The whole classification is a switch on a C enum, so it costs the
  // callback thread nothing and it does no I/O either way.
  // The aiming ray. FIRST CHOICE is the SDK's own, because the gesture carries
  // one: qar_app_volume_gesture_get_points
  // documents the start point as the hand, the action point as what the aim
  // intersected "or the start point when nothing was intersected", and says
  // outright that "from the start point to the action point is the ray the user
  // is pointing along". It costs no extra call -- both points are already read
  // above -- and it is the runtime's own geometry rather than our straight-line
  // guess at it. Equality of the two points is the no-intersection case, not a
  // sign that the pair is not a ray.
  qar_ar_core::PointingRay sdk_ray =
    qar_ar_core::pointing_ray_from_gesture_points(
    has_start_point, start_point, has_action_point, action_point,
    mapped ? sample.timestamp_s : 0.0);

  // SECOND CHOICE, and still needed: the eye-through-fingertip reconstruction
  // from the render thread's hand poll (see PointingRaySlot). An aim into empty
  // space legitimately intersects nothing, the runtime then hands back the start
  // point twice, and a plane or depth target still has to resolve SOMETHING --
  // so this path must keep working rather than degrade to a zero-length
  // direction.
  qar_ar_core::PointingRay hand_ray;
  ReceivedGestureLog::HandTrace hand_trace;
  if (pointing_ray_slot_) {
    // Read BOTH halves of one frame at once. The ray is what the sample needs;
    // the probe is what the diagnosis needs, and they must come from the same
    // publish or the distance below is measured between two different moments.
    hand_ray = pointing_ray_slot_->latest();
    const qar_ar_core::HandJointsProbe probe = pointing_ray_slot_->latest_probe();

    if (hand_ray.valid && has_action_point) {
      // aim_off: the error of the FALLBACK.
      // It is the perpendicular distance from our reconstruction to the
      // runtime's own action point, measured on the reconstruction whether or
      // not the reconstruction was the ray we used -- against the SDK ray it
      // would be zero by construction and would say nothing.
      //
      // So it answers one question and no other: how
      // far off would we have been on this aim if we had had to fall back?
      // It is NOT a frame proof (a shallow curve can mask a modest frame error)
      // and it is NOT a curvature measurement. Metres on every aim would say the
      // fallback is unusable, which matters because the fallback is what an aim
      // into empty space gets.
      hand_ray.aim_to_reported_m =
        qar_ar_core::ray_point_distance(
        hand_ray.origin, hand_ray.direction, action_point);
    }

    // The hand measurement. Filled for every event that carried a
    // point, queued or dropped, because a dropped click with the hand recorded
    // is still evidence.
    hand_trace.valid = probe.valid;
    hand_trace.has_eye = probe.has_eye;
    hand_trace.eye = probe.eye_position;
    const QarHandedness sides[2] = {QAR_HANDEDNESS_LEFT, QAR_HANDEDNESS_RIGHT};
    for (const QarHandedness which : sides) {
      const std::size_t i = static_cast<std::size_t>(qar_ar_core::hand_index(which));
      const qar_ar_core::HandJointsProbe::Hand & hand = probe.hand(which);
      hand_trace.tracked[i] = hand.tracked;
      const std::uint32_t tip = static_cast<std::uint32_t>(QAR_HAND_JOINT_INDEX_TIP_EXT);
      const std::uint32_t palm = static_cast<std::uint32_t>(QAR_HAND_JOINT_PALM_EXT);
      const std::uint32_t wrist = static_cast<std::uint32_t>(QAR_HAND_JOINT_WRIST_EXT);
      hand_trace.has_index_tip[i] = hand.has_joint[tip];
      hand_trace.index_tip[i] = hand.joint[tip];
      hand_trace.has_palm[i] = hand.has_joint[palm];
      hand_trace.palm[i] = hand.joint[palm];
      hand_trace.has_wrist[i] = hand.has_joint[wrist];
      hand_trace.wrist[i] = hand.joint[wrist];
    }
    if (has_action_point) {
      const qar_ar_core::NearestHandJoint nearest =
        qar_ar_core::nearest_hand_joint(probe, action_point);
      if (nearest.found) {
        hand_trace.nearest_joint_m = nearest.distance_m;
        hand_trace.nearest_joint_id = nearest.joint_id;
        hand_trace.nearest_hand = nearest.hand;
      }
      // Whichever hand's index tip is closer, so a left-handed operator is not
      // reported as "no index tip".
      for (const QarHandedness which : sides) {
        const float distance = qar_ar_core::hand_joint_distance(
          probe, which, QAR_HAND_JOINT_INDEX_TIP_EXT, action_point);
        if (distance < 0.0f) {
          continue;
        }
        if (hand_trace.index_tip_m < 0.0f || distance < hand_trace.index_tip_m) {
          hand_trace.index_tip_m = distance;
          hand_trace.index_tip_hand = which;
        }
      }
    }
  }

  // THE choice, in one place so the trace can name it: the SDK's own ray when
  // the runtime intersected something, our reconstruction when it did not.
  // PointingRay::kind carries which, all the way into the log line.
  qar_ar_core::PointingRay ray = sdk_ray.valid ? sdk_ray : hand_ray;
  // Carried across whichever way the choice went, because it is a property of
  // the RECONSTRUCTION and not of the ray in use -- see the comment where it is
  // computed.
  ray.aim_to_reported_m = hand_ray.aim_to_reported_m;
  if (!ray.valid) {
    // Neither source produced one. Keep BOTH refusals: "the runtime intersected
    // nothing" and "no hand is tracked" are different findings, and a trace that
    // shows only one of them sends the next session looking in the wrong place.
    if (sdk_ray.error.empty()) {
      ray.error = hand_ray.error;
    } else if (hand_ray.error.empty()) {
      ray.error = sdk_ray.error;
    } else {
      ray.error = sdk_ray.error + "; " + hand_ray.error;
    }
  }
  if (mapped && ray.valid) {
    sample.has_room_ray = true;
    sample.ray_origin = ray.origin;
    sample.ray_direction = ray.direction;
  }

  const char * drop_reason = nullptr;
  bool other_kind = false;
  bool no_point = false;
  bool unknown_phase = false;
  if (!mapped) {
    if (!readable) {
      unknown_phase = true;
      drop_reason = "an accessor refused to read the handle";
    } else if (kind != QAR_GESTURE_CLICK && kind != QAR_GESTURE_HOVER) {
      other_kind = true;
      drop_reason = "not a pointer gesture (a manipulation kind we did not subscribe to)";
    } else if (!known_gesture_phase(phase)) {
      unknown_phase = true;
      drop_reason = "gesture phase outside QarGesturePhase";
    } else {
      no_point = true;
      drop_reason = "has_action_point == false on a phase that needs a point";
    }
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.events_received;
    if (readable && kind == QAR_GESTURE_CLICK) {
      ++stats_.events_click;
    } else if (readable && kind == QAR_GESTURE_HOVER) {
      ++stats_.events_hover;
    }
    const bool volume_matched = !has_volume_ || !readable ||
      std::memcmp(&volume_, &target, sizeof(QarAppVolumeId)) == 0;
    if (!volume_matched) {
      ++stats_.events_other_volume;
    }
    if (mapped) {
      ++stats_.events_queued;
    } else {
      ++stats_.events_unmapped;
      stats_.events_other_kind += other_kind ? 1u : 0u;
      stats_.events_no_action_point += no_point ? 1u : 0u;
      stats_.events_unknown_phase += unknown_phase ? 1u : 0u;
      stats_.last_drop_reason = drop_reason;
    }

    if (should_trace_event(stats_.events_received) && pending_log_.size() < kMaxPendingTrace) {
      ReceivedGestureLog record;
      record.kind = kind;
      record.phase = phase;
      record.has_action_point = has_action_point;
      record.action_point = action_point;
      record.has_start_point = has_start_point;
      record.start_point = start_point;
      record.volume_matched = volume_matched;
      record.queued = mapped;
      record.ray = ray;
      record.hands = hand_trace;
      record.drop_reason = mapped ? std::string() : std::string(drop_reason);
      record.index = stats_.events_received;
      pending_log_.push_back(std::move(record));
    }

    if (!mapped) {
      return;
    }
  }
  queue_.push(sample);
}

void
QarCApiSpatialGestureSource::flush_event_trace()
{
  // |action - start| in app units, or -1 when there is no pair to compare.
  // THE number to read first in the log: non-zero means the runtime
  // intersected something and start->action is the wearer's aim; zero is the
  // documented no-intersection case (the runtime hands the start point back as
  // the action point) and the reason a line says ray_src=FALLBACK.
  const auto start_to_action_m = [](const ReceivedGestureLog & r) {
      if (!r.has_action_point || !r.has_start_point) {
        return -1.0f;
      }
      const float dx = r.action_point.x - r.start_point.x;
      const float dy = r.action_point.y - r.start_point.y;
      const float dz = r.action_point.z - r.start_point.z;
      return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    };
  std::vector<ReceivedGestureLog> records;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (pending_log_.empty()) {
      return;
    }
    records.swap(pending_log_);
  }
  for (const ReceivedGestureLog & r : records) {
    std::string outcome = r.queued ? std::string("queued") : ("DROPPED (" + r.drop_reason + ")");
    // The empty-space case, named on the line itself. Same exact-equality test
    // as pointer_sample_from_gesture_fields: the runtime copies the start point
    // verbatim when it intersected nothing. The pick layer then REFUSES an
    // Action Point click on it (SpatialPoint::refused_no_intersection) and the
    // panel heartbeat counts it as clicks-on-nothing=, so a click on empty
    // space is never a silent drop in the .err.log.
    if (r.has_action_point && r.has_start_point && r.action_point.x == r.start_point.x &&
      r.action_point.y == r.start_point.y && r.action_point.z == r.start_point.z)
    {
      outcome += " NO-HIT(action==start: the runtime intersected nothing; an Action Point "
        "pick publishes nothing)";
    }
    // action = what the aim intersected, or the start point when it intersected
    // nothing; start = the hand. `start_to_action` above says which of the two
    // this event was, and `ray_src` below says which source the ray then came
    // from, so a run can be read from this log alone:
    //
    //   ray_src=SDK(start->action)  the runtime intersected something and we
    //                               aim along its own line -- the usual one.
    //   ray_src=FALLBACK(<kind>/<joint>)  it intersected nothing, so the ray is
    //                               our eye-through-fingertip reconstruction
    //                               from the render thread's hand poll.
    //
    // aim_off is ALWAYS the reconstruction's perpendicular miss against the
    // runtime's action point, on both kinds of line, because on the SDK's own
    // line it would be zero by construction. It is the error of the FALLBACK
    // path -- how far off we would be on an aim that had to use it -- and it is
    // neither a frame proof nor a measurement of the runtime's ray curvature.
    std::string ray_text = " ray=none";
    if (r.ray.valid) {
      char source[96];
      if (r.ray.kind == qar_ar_core::PointingRayKind::GestureStartToAction) {
        std::snprintf(source, sizeof(source), "SDK(start->action)");
      } else {
        std::snprintf(
          source, sizeof(source), "FALLBACK(%s/%s)",
          qar_ar_core::to_string(r.ray.kind), qar_ar_core::to_string(r.ray.joint));
      }
      char buffer[320];
      std::snprintf(
        buffer, sizeof(buffer),
        " ray_src=%s origin=(%.3f, %.3f, %.3f) dir=(%.3f, %.3f, %.3f) "
        "aim_off=%.3f(reconstruction-vs-action_point: the fallback's error)",
        source,
        static_cast<double>(r.ray.origin.x), static_cast<double>(r.ray.origin.y),
        static_cast<double>(r.ray.origin.z), static_cast<double>(r.ray.direction.x),
        static_cast<double>(r.ray.direction.y), static_cast<double>(r.ray.direction.z),
        static_cast<double>(r.ray.aim_to_reported_m));
      ray_text = buffer;
    } else if (!r.ray.error.empty()) {
      // WHY there is no ray, so "ray=none" is never a dead end: the SDK's own
      // refusal reads "the action point IS the start point", the hand poll's
      // reads "no hand is tracked".
      ray_text = " ray=none(" + r.ray.error + ")";
    }
    // THE line that settles what action_point is. `to_hand` is the distance from
    // the runtime's reported point to the nearest tracked joint of either hand,
    // and `to_index_tip` the same against the fingertip the operator thinks they
    // are pointing with. Centimetres on every gesture means the API returns the
    // HAND -- which is not what the runtime documents. Metres means the runtime
    // is reporting a scene intersection and whatever error is left is on our
    // side of the seam. -1
    // means no hand was tracked on the frame the gesture arrived on, which is
    // neither answer and must not be read as one.
    std::string hand_text = " hand=unread";
    if (r.hands.valid) {
      char buffer[192];
      if (r.hands.nearest_joint_m < 0.0f) {
        std::snprintf(buffer, sizeof(buffer), " hand=untracked");
      } else {
        std::snprintf(
          buffer, sizeof(buffer), " to_hand=%.3f(%s %s) to_index_tip=%.3f",
          static_cast<double>(r.hands.nearest_joint_m),
          r.hands.nearest_hand == QAR_HANDEDNESS_LEFT ? "left" : "right",
          qar_ar_core::hand_joint_name(r.hands.nearest_joint_id),
          static_cast<double>(r.hands.index_tip_m));
      }
      hand_text = buffer;
    }
    std::fprintf(
      stderr,
      "[qar-spatial] #%llu kind=%s phase=%s has_point=%d action_room=(%.3f, %.3f, %.3f) "
      "has_start=%d start_room=(%.3f, %.3f, %.3f) start_to_action=%.3f%s%s%s -> %s\n",
      static_cast<unsigned long long>(r.index), gesture_kind_name(r.kind),
      gesture_phase_name(r.phase), r.has_action_point ? 1 : 0,
      static_cast<double>(r.action_point.x), static_cast<double>(r.action_point.y),
      static_cast<double>(r.action_point.z), r.has_start_point ? 1 : 0,
      static_cast<double>(r.start_point.x), static_cast<double>(r.start_point.y),
      static_cast<double>(r.start_point.z), static_cast<double>(start_to_action_m(r)),
      ray_text.c_str(), hand_text.c_str(), r.volume_matched ? "" : " OTHER-VOLUME",
      outcome.c_str());

    // The joints themselves, on their own line so the row above stays readable.
    // The eye is the frame's own view pose, printed here so all three -- eye,
    // hand, reported point -- can be compared in room metres without joining two
    // logs. "-" is a joint the runtime did not report as valid, never (0, 0, 0).
    if (r.hands.valid) {
      const auto joint_text =
        [](bool has, const QarVector3 & v, char * out, std::size_t n) {
          if (has) {
            std::snprintf(
              out, n, "(%.3f, %.3f, %.3f)", static_cast<double>(v.x),
              static_cast<double>(v.y), static_cast<double>(v.z));
          } else {
            std::snprintf(out, n, "-");
          }
        };
      char eye[64] = "-";
      joint_text(r.hands.has_eye, r.hands.eye, eye, sizeof(eye));
      char tip[2][64];
      char palm[2][64];
      char wrist[2][64];
      for (std::size_t i = 0; i < 2; ++i) {
        joint_text(r.hands.has_index_tip[i], r.hands.index_tip[i], tip[i], sizeof(tip[i]));
        joint_text(r.hands.has_palm[i], r.hands.palm[i], palm[i], sizeof(palm[i]));
        joint_text(r.hands.has_wrist[i], r.hands.wrist[i], wrist[i], sizeof(wrist[i]));
      }
      std::fprintf(
        stderr,
        "[qar-spatial] #%llu hands_room eye=%s L:tracked=%d tip=%s palm=%s wrist=%s "
        "R:tracked=%d tip=%s palm=%s wrist=%s\n",
        static_cast<unsigned long long>(r.index), eye, r.hands.tracked[0] ? 1 : 0, tip[0],
        palm[0], wrist[0], r.hands.tracked[1] ? 1 : 0, tip[1], palm[1], wrist[1]);
    }
  }
  std::fflush(stderr);
}

void
QarCApiSpatialGestureSource::drain(std::vector<qar_ar_core::PanelPointerSample> & out)
{
  queue_.drain(out);
  // Qt thread: safe to do I/O. Printing here rather than in the callback is the
  // whole point of pending_log_.
  flush_event_trace();
}

std::uint64_t
QarCApiSpatialGestureSource::events_received() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_.events_received;
}

std::uint64_t
QarCApiSpatialGestureSource::events_unmapped() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_.events_unmapped;
}

SpatialGestureStats
QarCApiSpatialGestureSource::stats() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace qar_qar_adapter
