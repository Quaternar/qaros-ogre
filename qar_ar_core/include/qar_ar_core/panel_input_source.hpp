// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_INPUT_SOURCE_HPP_
#define QAR_AR_CORE__PANEL_INPUT_SOURCE_HPP_

/// @file panel_input_source.hpp
/// @brief The INPUT seam: where AR pointer samples enter the library. Qt-free.
///
/// The vocabulary itself (gestures, hands, phases, PanelPointerSample, IPanelInputSource and the
/// queue) is in pointer_input.hpp, which this header includes. What stays here is the mapping
/// FROM the vendor enums, with the compile-time pins on their values.
///
/// The input sink of a panel streaming library is the OUTPUT
/// seam — the library telling the host where an interaction landed. This is its mirror:
/// something telling the library that a pointer did something, without the
/// library knowing whether that something came from
///
///   * the app-volume gesture API (`qar_app_volumes_subscribe_gesture_events`
///     delivering an opaque `QarAppVolumeGesture*` whose
///     `qar_app_volume_gesture_get_points` accessor gives a room-space action
///     point),
///   * the vendor's per-panel gesture API —
///     `qar_gui_panels_subscribe_all_gesture_events`, bound by
///     `qar_qar_adapter`'s QarCApiPanelInputSource:
///     a panel id plus a (u, v), plus a hand, a tap count and a full
///     hover/tap/long-press/DRAG lifecycle, or
///   * a test, a simulator, or the host's own desktop mouse.
///
/// A sample therefore carries EITHER a room-space point (the library resolves
/// the panel itself, by a hit test) OR an already-resolved
/// panel key and (u, v). The phase mirrors `QarGesturePhase` one-to-one; the gesture is a
/// deliberate NARROWING of the two vendor kind enums onto what a widget can
/// act on (see @ref PointerGesture), so binding a real callback is a mechanical
/// field copy — @ref pointer_sample_from_gesture_fields is that copy, and the
/// enum mappers next to it are the whole vendor-facing vocabulary in one place.
///
/// ## Why the vendor handles are read in the ADAPTER, not here
///
/// Both events are opaque handles read through `qar_*_gesture_get_*()` accessors.
/// Those accessors are real C API calls, and
/// qar_ar_core links NO Qar runtime — it reuses Qar *types* only, which is what
/// lets its tests run with neither a headset nor a
/// runtime. So the pure field-copy and the enum mapping stay here, where they are
/// unit-tested without a runtime, and `qar_qar_adapter` reads the handles: its
/// QarCApiPanelInputSource reads a panel gesture in its callback, and an app-volume
/// gesture is read by `pointer_sample_from_gesture_event`, a function local to
/// qar_capi_spatial_gesture_source.cpp that hands what it read to
/// @ref pointer_sample_from_gesture_fields.
///
/// ## Threading
///
/// The seam is PULL-based: the panel stream manager drains
/// a source from the Qt GUI thread (on every tick, or whenever the host calls its
/// `pump_input()`), and everything downstream — the pointer state machine, Qt event
/// synthesis — runs there with no lock, exactly like the capture path. The runtime's
/// callback threads never reach the library core: a source that receives
/// samples on another thread queues them, and @ref QueuedPanelInputSource is
/// that queue, ready for the QarOS adapter to push into.
///
/// ## Why everything here is `inline`
///
/// The real source of these samples is `qar_qar_adapter`, which is deliberately
/// Qt-FREE: it is linked by console programs that must not drag Qt5 in. This
/// header is Qt-free as well, and its definitions are in the header itself, so
/// `qar_qar_adapter` and its consumers share one definition and the seam needs
/// no translation unit of its own.

#include "qar_ar_core/pointer_input.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace qar_ar_core
{

// ---------------------------------------------------------------------------
// The vendor enum values this seam maps FROM, pinned at compile time.
//
// The two vendor kind enums disagree on the meaning of ZERO:
//
//   QarGuiPanelGestureKind : HOVER=0, TAP=1, LONG_PRESS=2, DRAG=3
//   QarGestureKind         : CLICK=0, HOVER=1, ...
//
// A stale mapping across that seam turns every hover into a click, which
// publishes a robot goal for a hand that was merely pointing. There is no test
// that can catch a renumbered enum -- the old numbers simply stop existing --
// so it is caught HERE, at compile time, where a header that renumbers them
// fails loudly instead of silently committing picks. If one of these fires, read
// the installed header and fix the switch in pointer_gesture_from_panel_kind() /
// pointer_gesture_from_volume_kind() BEFORE changing the number below.
// ---------------------------------------------------------------------------
static_assert(
  static_cast<int>(QAR_GESTURE_CLICK) == 0 && static_cast<int>(QAR_GESTURE_HOVER) == 1,
  "QarGestureKind renumbered: the app-volume kind -> PointerGesture mapping must be re-read");
static_assert(
  static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_HOVER) == 0 &&
  static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_TAP) == 1 &&
  static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS) == 2 &&
  static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_DRAG) == 3,
  "QarGuiPanelGestureKind renumbered: the panel kind -> PointerGesture mapping must be re-read");
static_assert(
  static_cast<int>(QAR_GESTURE_CLICK) != static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_TAP) ||
  static_cast<int>(QAR_GESTURE_HOVER) != static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_HOVER),
  "the two vendor kind enums must not be assumed interchangeable");
static_assert(
  static_cast<int>(QAR_GESTURE_PHASE_STARTED) == 0 &&
  static_cast<int>(QAR_GESTURE_PHASE_UPDATED) == 1 &&
  static_cast<int>(QAR_GESTURE_PHASE_ENDED) == 2 &&
  static_cast<int>(QAR_GESTURE_PHASE_INSTANT) == 3 &&
  static_cast<int>(QAR_GESTURE_PHASE_CANCELED) == 4,
  "QarGesturePhase renumbered: PointerPhase mirrors it one-to-one and must be re-read");

/// @brief Convert a `QarTimePoint` (nanoseconds since the Unix epoch) to seconds.
double seconds_from_time_point(const QarTimePoint & time_point);

/// @brief `QarGesturePhase` -> @ref PointerPhase. False for a value outside the
/// enum, which a forward-compatible consumer must expect.
bool pointer_phase_from_qar(QarGesturePhase phase, PointerPhase & out);

/// @brief `QarGuiPanelGestureKind` -> @ref PointerGesture (the per-panel path).
/// False for a kind outside the enum. Note DRAG and TAP both map to
/// PointerGesture::Click; see @ref PointerGesture for why.
bool pointer_gesture_from_panel_kind(QarGuiPanelGestureKind kind, PointerGesture & out);

/// @brief `QarGestureKind` -> @ref PointerGesture (the app-volume path). False
/// for the manipulation kinds, which move the volume and are not pointer input.
bool pointer_gesture_from_volume_kind(QarGestureKind kind, PointerGesture & out);

/// @brief `QarHandedness` -> @ref PointerHand. Unknown for QAR_HANDEDNESS_UNSPECIFIED (no hand,
/// as for a gesture made with both hands) and for a value outside the enum.
PointerHand pointer_hand_from_qar(QarHandedness hand);

/// @brief The mechanical mapping from the app-volume gesture API, as FIELDS.
///
/// Takes the values the caller has already read off the opaque handle rather
/// than the handle itself, so it is exercised by a test that links no Qar
/// runtime — see the file comment. The caller that reads the handle is
/// `pointer_sample_from_gesture_event`, local to qar_qar_adapter's
/// qar_capi_spatial_gesture_source.cpp.
///
/// @param pointer_id The runtime keys gestures to a peer, not a pointer; the
///        binding layer chooses an id (e.g. one per `source_peer_id`).
/// @return false if the gesture is not a click/hover, or carries no action
///         point — a receiver "falls back to a context-specific default" then,
///         and for a panel there is none, so the sample is not produced.
bool pointer_sample_from_gesture_fields(
  QarGestureKind gesture_kind, QarGesturePhase gesture_phase, bool has_action_point,
  const QarVector3 & action_point, const QarTimePoint & timestamp, std::uint32_t pointer_id,
  PanelPointerSample & out, bool has_start_point = false,
  const QarVector3 & start_point = QarVector3{0.0f, 0.0f, 0.0f});

// ---------------------------------------------------------------------------
// Definitions. Header-inline so the Qt-free adapter shares them — see the file
// comment, "Why everything here is inline".
// ---------------------------------------------------------------------------

inline double
seconds_from_time_point(const QarTimePoint & time_point)
{
  return static_cast<double>(time_point.unix_ns) * 1e-9;
}

inline bool
pointer_phase_from_qar(QarGesturePhase phase, PointerPhase & out)
{
  switch (phase) {
    case QAR_GESTURE_PHASE_STARTED:
      out = PointerPhase::Started;
      return true;
    case QAR_GESTURE_PHASE_UPDATED:
      out = PointerPhase::Updated;
      return true;
    case QAR_GESTURE_PHASE_ENDED:
      out = PointerPhase::Ended;
      return true;
    case QAR_GESTURE_PHASE_INSTANT:
      out = PointerPhase::Instant;
      return true;
    case QAR_GESTURE_PHASE_CANCELED:
      out = PointerPhase::Canceled;
      return true;
  }
  return false;
}

inline bool
pointer_gesture_from_panel_kind(QarGuiPanelGestureKind kind, PointerGesture & out)
{
  switch (kind) {
    case QAR_GUI_PANEL_GESTURE_KIND_HOVER:
      out = PointerGesture::Hover;
      return true;
    case QAR_GUI_PANEL_GESTURE_KIND_TAP:
    case QAR_GUI_PANEL_GESTURE_KIND_DRAG:
      // Both are a press. A tap arrives Instant and the state machine expands
      // it into Press + Release + Click; a drag spells the same three out over
      // its lifecycle, which is what makes a QSlider handle draggable in AR.
      out = PointerGesture::Click;
      return true;
    case QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS:
      out = PointerGesture::LongPress;
      return true;
  }
  return false;
}

inline bool
pointer_gesture_from_volume_kind(QarGestureKind kind, PointerGesture & out)
{
  switch (kind) {
    case QAR_GESTURE_CLICK:
      out = PointerGesture::Click;
      return true;
    case QAR_GESTURE_HOVER:
      out = PointerGesture::Hover;
      return true;
    case QAR_GESTURE_SINGLE_POINTER_6DOF:
    case QAR_GESTURE_DUAL_POINTER_TRANSLATE_DISTANCE:
    case QAR_GESTURE_DUAL_POINTER_ROTATE:
      return false;  // manipulation gestures move the app volume, not a pointer
  }
  return false;
}

inline PointerHand
pointer_hand_from_qar(QarHandedness hand)
{
  switch (hand) {
    case QAR_HANDEDNESS_LEFT:
      return PointerHand::Left;
    case QAR_HANDEDNESS_RIGHT:
      return PointerHand::Right;
    case QAR_HANDEDNESS_UNSPECIFIED:
      return PointerHand::Unknown;
  }
  return PointerHand::Unknown;
}

inline bool
pointer_sample_from_gesture_fields(
  QarGestureKind gesture_kind, QarGesturePhase gesture_phase, bool has_action_point,
  const QarVector3 & action_point, const QarTimePoint & timestamp, std::uint32_t pointer_id,
  PanelPointerSample & out, bool has_start_point, const QarVector3 & start_point)
{
  PointerGesture gesture;
  if (!pointer_gesture_from_volume_kind(gesture_kind, gesture)) {
    return false;
  }
  PointerPhase phase;
  if (!pointer_phase_from_qar(gesture_phase, phase)) {
    return false;
  }

  // A cancel or an end may legitimately arrive without a point (the hand left
  // tracking); those still matter to the state machine. A press or a move
  // without a point cannot be placed anywhere, so it is not a sample.
  const bool needs_point = phase == PointerPhase::Started || phase == PointerPhase::Updated ||
    phase == PointerPhase::Instant;
  if (needs_point && !has_action_point) {
    return false;
  }

  out = PanelPointerSample{};
  out.pointer_id = pointer_id;
  out.gesture = gesture;
  out.phase = phase;
  out.timestamp_s = seconds_from_time_point(timestamp);
  out.has_room_point = has_action_point;
  if (has_action_point) {
    out.room_point = action_point;
    // The action point is the recogniser's own scene intersection -- EXCEPT
    // when it equals the start point, which the installed runtime's header defines
    // as "nothing was intersected". That case hands back the HAND, so it must not
    // be flagged as a hit: see PanelPointerSample::room_point_is_scene_hit.
    const bool intersected_nothing = has_start_point &&
      action_point.x == start_point.x && action_point.y == start_point.y &&
      action_point.z == start_point.z;
    out.room_point_is_scene_hit = !intersected_nothing;
  }
  out.has_room_start_point = has_start_point;
  if (has_start_point) {
    out.room_start_point = start_point;
  }
  // The app-volume path reports neither a hand nor a tap count; leaving them at
  // their defaults is what tells the state machine to infer a double click.
  return true;
}

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__PANEL_INPUT_SOURCE_HPP_
