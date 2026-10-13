// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// The app-volume (spatial, 3D scene) gesture source: QarOS app-volume gesture events behind
// the IQarPanelInputSource seam. Component `gestures`. Separate from
// qar_capi_panel_input_source.hpp (component `panels`), with which it shares only the
// callback-slot arena of the base component `capi`.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_SPATIAL_GESTURE_SOURCE_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_SPATIAL_GESTURE_SOURCE_HPP_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_qar_adapter/qar_capi_input_slots.hpp"

namespace qar_qar_adapter
{

/// @brief Diagnostics for the SPATIAL (app-volume gesture) pointer source.
///
/// ## Why this struct exists at all
///
/// "Is anything arriving?" is the first question when a pointer path stays
/// silent: a counter that says whether events arrive AT ALL separates "the
/// runtime sends nothing" from "we receive and discard". The subscription can
/// succeed while no app-volume gesture is ever delivered. @ref events_received is
/// the decisive number — it is incremented at the TOP of the callback, before
/// any filter, including for gestures we then throw away.
struct SpatialGestureStats
{
  /// Callbacks entered. Counted before every filter; this is the number that
  /// answers "did the runtime call us at all".
  std::uint64_t events_received = 0;
  std::uint64_t events_click = 0;   ///< ... of which QAR_GESTURE_CLICK.
  std::uint64_t events_hover = 0;   ///< ... of which QAR_GESTURE_HOVER.
  /// Manipulation kinds (not pointer input): expected traffic on the unified
  /// all-kinds subscription, which we dispatch on kind ourselves, or, on the
  /// per-kind CLICK/HOVER fallback, a kind we did not subscribe to at all.
  std::uint64_t events_other_kind = 0;
  /// Phase needed a point and `has_action_point` was false.
  std::uint64_t events_no_action_point = 0;
  /// `state` was outside QarGesturePhase.
  std::uint64_t events_unknown_phase = 0;
  /// `target_app_volume_id` was not the volume we subscribed to. Counted, never
  /// dropped: the runtime filters by volume, so a mismatch means our idea of
  /// which volume is ours is wrong.
  std::uint64_t events_other_volume = 0;
  /// Total rejected by the seam's mapping (the sum of the three reasons above
  /// that actually drop).
  std::uint64_t events_unmapped = 0;
  /// Handed to the queue, i.e. reached the Qt thread as a pointer sample.
  std::uint64_t events_queued = 0;
  /// The most recent drop, in words. Empty until something is dropped.
  std::string last_drop_reason;
};

/// @brief Feeds the spatial (3D) interaction path from the app-volume gestures.
///
/// qar_rviz_plugin's `QarStreamingPanel::submitSpatialSample` consumes the samples; this
/// class is the connection to QarOS:
///
///   qar_app_volumes_subscribe_all_gesture_events(CLICK, HOVER, manipulation...)
///     -> offer() dispatches on qar_app_volume_gesture_get_classification()
///       -> pointer_sample_from_gesture_event  (local to qar_capi_spatial_gesture_source.cpp:
///          reads the handle, then calls qar_ar_core::pointer_sample_from_gesture_fields,
///          the seam's own mapping)
///         -> this queue -> drained on the Qt thread -> submitSpatialSample
///
/// The 2026-09-21 drop renamed `_subscribe_gesture_updates` to
/// `_subscribe_gesture_events` and made the event an opaque
/// `QarAppVolumeGesture*` read through accessors. This class takes every kind on
/// ONE reader — mirroring the GUI panel path's
/// `qar_gui_panels_subscribe_all_gesture_events` — rather than one per-kind
/// subscription for CLICK and one for HOVER: the header recommends this
/// whenever a consumer wants more than one kind, because several per-kind
/// subscriptions are separate readers with no order between them. Only CLICK
/// and HOVER become pointer samples; a manipulation kind
/// (SINGLE_POINTER_6DOF, DUAL_POINTER_TRANSLATE_DISTANCE, DUAL_POINTER_ROTATE)
/// moves or scales the app volume itself, so `offer()` counts it in
/// `events_other_kind` and never queues it. If the unified subscribe call
/// itself fails, `subscribe()` falls back to the per-kind CLICK + HOVER pair so
/// the class keeps working.
///
/// These gestures carry a phase, so the full Started / Updated / Ended /
/// Canceled stream survives the trip — and since the 2026-09-21 drop the PANEL
/// path carries one too, which is what makes AR drag possible.
///
/// Same threading and lifetime rules, same slot arena, for the same reason.
class QarCApiSpatialGestureSource : public qar_ar_core::IQarPanelInputSource
{
public:
  explicit QarCApiSpatialGestureSource(QarSession * session);
  ~QarCApiSpatialGestureSource() override;

  QarCApiSpatialGestureSource(const QarCApiSpatialGestureSource &) = delete;
  QarCApiSpatialGestureSource & operator=(const QarCApiSpatialGestureSource &) = delete;

  /// @brief Subscribe to CLICK and HOVER gestures on @p volume_id.
  ///
  /// Registers ONE `qar_app_volumes_subscribe_all_gesture_events` reader and
  /// dispatches on kind in @ref offer; falls back to the per-kind
  /// CLICK + HOVER pair if that call itself fails. Either way, which path is
  /// active is logged at subscribe time.
  /// @return false and records @ref last_error if the runtime refused both.
  bool subscribe(const QarAppVolumeId & volume_id);

  /// @brief Retire the subscriptions. Idempotent; safe during teardown.
  void unsubscribe();

  /// @brief Where to read the FALLBACK AR pointing ray from.
  ///
  /// Since the drop after the minimum drop the gesture itself carries an aim - its
  /// start point (the hand) through its action point (what the aim intersected) - and
  /// that is what a sample normally gets; see
  /// `qar_ar_core::pointing_ray_from_gesture_points`. The runtime reports
  /// the two as one and the same point when it intersected NOTHING, which is
  /// legal and expected for an aim into empty space, and two of the three pick
  /// targets ("Ground plane (z = 0)" and "Fixed depth") are ray operations that
  /// still need a direction then. That is what this slot supplies: a ray derived
  /// on the render thread from `qar_render_sender_last_hands` plus the frame's
  /// eye pose. Unset (or an invalid ray in the slot) means such a gesture gets
  /// no ray at all, and a plane target then degrades to projecting the reported
  /// point onto its plane.
  void set_pointing_ray_slot(std::shared_ptr<qar_ar_core::PointingRaySlot> slot);

  bool subscribed() const;

  void drain(std::vector<qar_ar_core::PanelPointerSample> & out) override;

  /// Gesture events entered / rejected by the seam's mapping (a manipulation
  /// gesture, or a press with no action point, is not a pointer sample).
  std::uint64_t events_received() const;
  std::uint64_t events_unmapped() const;
  /// @brief The full breakdown. @ref stats().events_received is THE number:
  /// non-zero means the runtime is calling us, zero means it is not.
  SpatialGestureStats stats() const;
  const std::string & last_error() const { return last_error_; }

private:
  /// @brief One received gesture, remembered just long enough for the Qt thread
  /// to print it — same discipline as @ref QarCApiPanelInputSource's trace: the
  /// runtime's delivery thread must never do I/O.
  struct ReceivedGestureLog
  {
    QarGestureKind kind = QAR_GESTURE_CLICK;
    QarGesturePhase phase = QAR_GESTURE_PHASE_INSTANT;
    bool has_action_point = false;
    QarVector3 action_point{0.0f, 0.0f, 0.0f};
    /// `qar_app_volume_gesture_get_points`' start point -- the HAND. Traced
    /// alongside the action point because the two together ARE the aim: the
    /// trace's `start_to_action` is zero exactly when the runtime intersected
    /// nothing, which is the one case that falls back to the reconstruction.
    bool has_start_point = false;
    QarVector3 start_point{0.0f, 0.0f, 0.0f};
    bool volume_matched = true;
    bool queued = false;
    /// The aiming ray attached to this event.
    /// @ref qar_ar_core::PointingRay::kind names its SOURCE -- the SDK's
    /// own start->action line, or our eye-through-fingertip reconstruction --
    /// and the trace prints it as `ray_src=`, so a run can be read from the log
    /// alone.
    /// @ref qar_ar_core::PointingRay::aim_to_reported_m rides along as the
    /// RECONSTRUCTION's error against the runtime's action point, on both kinds
    /// of line; it is not a frame proof and not a measurement of the runtime's
    /// ray curvature.
    qar_ar_core::PointingRay ray;
    /// @brief The operator's own HAND at the moment of this gesture, reduced to
    /// what one trace line prints.
    ///
    /// The measurement that says what `action_point` IS. `aim_to_reported_m`
    /// above cannot, because a straight ray and a curved one disagree by an
    /// unknown amount. A distance to a hand JOINT can: the reported point sits
    /// farther from the eye than the hand does, so the API
    /// returns a genuine scene intersection, not the hand (gesture points need
    /// no placement inverse, see `qar_ar_core::ScenePlacement` and
    /// docs/ARCHITECTURE.md#gesture-points-are-in-app-content-space).
    ///
    /// It stays as the cheapest possible regression check on that answer:
    /// centimetres on every gesture would mean the runtime returns the hand
    /// after all, and nothing else in the log would say so.
    ///
    /// A fixed-size reduction rather than the whole
    /// @ref qar_ar_core::HandJointsProbe: up to @ref kMaxPendingTrace of
    /// these are held on the callback thread between Qt ticks, and 52 joint
    /// positions each would be a hundred times the memory for no extra finding.
    struct HandTrace
    {
      /// A hand sample was read at all. False means every field below is
      /// unknown, NOT zero.
      bool valid = false;
      bool has_eye = false;
      QarVector3 eye{0.0f, 0.0f, 0.0f};
      /// Indexed by `QarHand` (0 = left, 1 = right).
      bool tracked[2] = {false, false};
      bool has_index_tip[2] = {false, false};
      QarVector3 index_tip[2] = {};
      bool has_palm[2] = {false, false};
      QarVector3 palm[2] = {};
      bool has_wrist[2] = {false, false};
      QarVector3 wrist[2] = {};
      /// Metres from the action point to the closest tracked joint of EITHER
      /// hand, with which joint it was. -1 when nothing was tracked.
      float nearest_joint_m = -1.0f;
      std::uint32_t nearest_joint_id = 0;
      QarHand nearest_hand = QAR_HAND_RIGHT;
      /// Metres from the action point to the INDEX TIP, reported separately
      /// because that is the joint the wearer believes they are pointing with.
      /// -1 when no index tip was tracked.
      float index_tip_m = -1.0f;
      QarHand index_tip_hand = QAR_HAND_RIGHT;
    };
    HandTrace hands;
    /// Why it was dropped; empty when it was queued.
    std::string drop_reason;
    /// 1-based @ref SpatialGestureStats::events_received at arrival.
    std::uint64_t index = 0;
  };

  static void on_gesture_event(QarAppVolumeGesture * gesture, void * user_state);
  void offer(QarAppVolumeGesture * gesture);
  /// Qt-thread half of the trace: print and forget whatever @ref offer recorded.
  void flush_event_trace();

  bool subscribe_kind(
    const QarAppVolumeId & volume_id, QarGestureKind kind, detail::PointerSlot *& out_slot,
    QarCancelToken *& out_token);

  /// Written once at wire-up (Qt thread) and read on the QarOS callback
  /// thread; the slot itself is what synchronises the ray value.
  std::shared_ptr<qar_ar_core::PointingRaySlot> pointing_ray_slot_;

  QarSession * session_ = nullptr;
  qar_ar_core::QueuedPanelInputSource queue_;

  mutable std::mutex mutex_;
  /// The primary path: one reader for every kind. Non-null exactly when
  /// @ref subscribe registered it successfully.
  detail::PointerSlot * all_slot_ = nullptr;
  QarCancelToken * all_token_ = nullptr;
  /// The fallback pair, used only when the unified call above failed. Non-null
  /// exactly when the fallback is the active registration.
  detail::PointerSlot * click_slot_ = nullptr;
  detail::PointerSlot * hover_slot_ = nullptr;
  QarCancelToken * click_token_ = nullptr;
  QarCancelToken * hover_token_ = nullptr;
  SpatialGestureStats stats_{};
  /// Recorded by @ref offer, drained by @ref flush_event_trace. Bounded, so a
  /// panel that stopped ticking cannot turn the trace into a leak.
  std::vector<ReceivedGestureLog> pending_log_;
  /// The volume we subscribed to, so an event aimed at a DIFFERENT volume is
  /// visible rather than silently indistinguishable from one of ours.
  QarAppVolumeId volume_{};
  bool has_volume_ = false;

  std::string last_error_;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_SPATIAL_GESTURE_SOURCE_HPP_
