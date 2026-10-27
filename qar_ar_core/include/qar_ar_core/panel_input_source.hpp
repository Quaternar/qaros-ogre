// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_INPUT_SOURCE_HPP_
#define QAR_AR_CORE__PANEL_INPUT_SOURCE_HPP_

/// @file panel_input_source.hpp
/// @brief The INPUT seam: where AR pointer samples enter the library. Qt-free.
///
/// qar_qt_streaming's IQarPanelInputSink (qt_panel_stream_manager.hpp) is the OUTPUT
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
/// the panel itself, see qar_qt_streaming's panel_hit_test.hpp) OR an already-resolved
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
/// lets its tests, like those of qar_qt_streaming, run with neither a headset nor a
/// runtime. So the pure field-copy and the enum mapping stay here, where they are
/// unit-tested without a runtime, and `qar_qar_adapter` reads the handles: its
/// QarCApiPanelInputSource reads a panel gesture in its callback, and an app-volume
/// gesture is read by `pointer_sample_from_gesture_event`, a function local to
/// qar_capi_spatial_gesture_source.cpp that hands what it read to
/// @ref pointer_sample_from_gesture_fields.
///
/// ## Threading
///
/// The seam is PULL-based: the manager (qar_qt_streaming's QtPanelStreamManager) drains
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
/// `qar_qar_adapter` and qar_qt_streaming share one definition and the seam needs
/// no translation unit of its own.

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

/// @brief The gestures a pointer can perform on a panel.
///
/// A NARROWING of the two vendor kind enums, not a mirror of either — they
/// disagree (panel kinds are HOVER=0, TAP=1, LONG_PRESS=2, DRAG=3; app-volume
/// kinds are CLICK=0, HOVER=1), so value 0 means a different gesture on each
/// side. These values are the library's own; the mapping from either vendor
/// enum is done by pointer_gesture_from_panel_kind() and
/// pointer_gesture_from_volume_kind(), so a downstream consumer never depends
/// on a vendor number:
///
/// | vendor kind                              | PointerGesture | phases seen              |
/// |------------------------------------------|----------------|--------------------------|
/// | `QAR_GUI_PANEL_GESTURE_KIND_TAP`         | Click          | Instant                  |
/// | `QAR_GUI_PANEL_GESTURE_KIND_DRAG`        | Click          | Started/Updated/Ended/Canceled |
/// | `QAR_GUI_PANEL_GESTURE_KIND_HOVER`       | Hover          | Started/Updated/Ended    |
/// | `QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS`  | LongPress      | Instant                  |
/// | `QAR_GESTURE_CLICK` (app volume)         | Click          | any                      |
/// | `QAR_GESTURE_HOVER` (app volume)         | Hover          | any                      |
///
/// TAP and DRAG both arrive as @ref PointerGesture::Click on purpose: to a
/// widget they are the SAME gesture at different lengths — a press, zero or
/// more moves, a release — and the state machine expresses exactly that. A tap
/// collapses it into one Instant sample; a drag spells it out.
/// Keeping a separate `Drag` kind here would have duplicated every press
/// transition downstream for no gain.
enum class PointerGesture : std::uint8_t
{
  /// A contact that can press: a TAP (Instant) or a DRAG (Started … Ended).
  Click = 0,
  /// A contact resting on the glass without committing to anything.
  Hover = 1,
  /// `QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS`: touched and held in place, once
  /// per contact, always Instant. Carried through the seam so a consumer can be
  /// added without another migration; nothing downstream acts on it yet (Qt has
  /// no long-press event — the natural use is a context menu). The contact may
  /// still become a drag afterwards, which arrives separately as Click.
  LongPress = 2
};

/// @brief May this gesture COMMIT something, or may it only preview?
///
/// The one place the answer lives, for panels and for the 3D scene alike.
/// @ref PointerGesture::Click -- a TAP, a DRAG, or an app-volume
/// `QAR_GESTURE_CLICK` -- is a deliberate act and commits. @ref
/// PointerGesture::Hover is a hand passing through the air: it may move a
/// preview or light a highlight, and must never publish. @ref
/// PointerGesture::LongPress has no consumer yet and is treated as a preview
/// until one exists, because "nothing happens" is the safe half of that guess.
///
/// This exists because the app-volume runtime emits hover as
/// `QAR_GESTURE_PHASE_INSTANT`, the same phase a tap uses, so a pick state
/// machine that dispatches on the PHASE alone commits on every hover, and
/// hovers outnumber taps by orders of magnitude on that path.
constexpr bool gesture_commits_pick(PointerGesture gesture)
{
  return gesture == PointerGesture::Click;
}

/// @brief Which hand made the contact, when the source knows.
///
/// `qar_gui_panel_gesture_get_handedness()`; without it every AR contact would have to share
/// one pointer id. The hand is
/// carried per sample rather than folded into
/// `pointer_id` because the id must stay stable for a whole lifecycle while the
/// hand is an attribute of it, and because the app-volume path still has none.
enum class PointerHand : std::uint8_t
{
  Unknown = 0,  ///< No hand reported (app-volume path, tests, QAR_HANDEDNESS_UNSPECIFIED).
  Left = 1,     ///< QAR_HANDEDNESS_LEFT
  Right = 2     ///< QAR_HANDEDNESS_RIGHT
};

/// @brief Where a gesture is in its lifecycle. Mirrors `QarGesturePhase`
/// exactly — same names, same values, same meaning — so nothing is lost or
/// invented between the runtime and the state machine.
enum class PointerPhase : std::uint8_t
{
  Started = 0,   ///< QAR_GESTURE_PHASE_STARTED: the gesture became active (a press).
  Updated = 1,   ///< QAR_GESTURE_PHASE_UPDATED: position update while active.
  Ended = 2,     ///< QAR_GESTURE_PHASE_ENDED: ended normally (a release).
  Instant = 3,   ///< QAR_GESTURE_PHASE_INSTANT: whole gesture in one event (a tap).
  Canceled = 4   ///< QAR_GESTURE_PHASE_CANCELED: abandoned; nothing must be committed.
};

const char * to_string(PointerGesture gesture);
const char * to_string(PointerPhase phase);
const char * to_string(PointerHand hand);

/// @brief One raw pointer observation, as the source saw it.
struct PanelPointerSample
{
  /// Distinguishes simultaneous pointers (two hands, two peers). Stable for
  /// the lifetime of one gesture; the library never interprets its value.
  std::uint32_t pointer_id = 0;

  PointerGesture gesture = PointerGesture::Hover;
  PointerPhase phase = PointerPhase::Updated;

  /// Seconds on any monotonic clock shared by all samples of a source. Only
  /// differences are used (double-click detection).
  double timestamp_s = 0.0;

  /// Which hand made the contact, when the source knows it
  /// (`qar_gui_panel_gesture_get_handedness`). Unknown on every other path.
  PointerHand hand = PointerHand::Unknown;

  /// Taps in this run, NATIVELY counted by the recogniser
  /// (`qar_gui_panel_gesture_get_tap_count`): 1, 2, 3 … for taps close enough
  /// together in time and space. 0 means "the source does not count taps", and
  /// the state machine then infers a double click from the timestamps as it
  /// always has. Preferred over inference where it is available: the recogniser
  /// sees the hand, we see only the pixels it landed on.
  std::uint8_t tap_count = 0;

  /// App-space point (`qar_app_volume_gesture_get_points`' action point).
  bool has_room_point = false;
  QarVector3 room_point{0.0f, 0.0f, 0.0f};

  /// True when @ref room_point is the RUNTIME'S OWN intersection of the
  /// pointing ray with the scene, rather than a bare position that still has to
  /// be resolved against something.
  ///
  /// The app-volume path sets it: the action point is the intersection QarOS
  /// computes between the wearer's aim and the app volume's scene, from a ray
  /// that is **slightly curved, not a straight line**. The SDK header agrees
  /// ("when a point is absent the recogniser had no INTERSECTION to report"),
  /// and so do recorded action points: they span several metres vertically and
  /// cluster in tight bands at distinct heights. A hand occupies neither that
  /// volume nor those bands; ray hits on real surfaces do.
  ///
  /// Because the runtime's ray is curved, NOTHING on our side can reproduce
  /// this point by casting a straight ray. That is why
  /// `PickTargetKind::ActionPoint` publishes it verbatim. It is not the default
  /// target: `PickTarget::kind` starts as `GroundPlane`.
  ///
  /// **It is NOT always a hit.** The installed runtime's header says the action
  /// point is "the intersection the gesture acts on, **or the start point when
  /// nothing was intersected**". So an action point that equals the start point
  /// is the HAND and not a scene hit, and this flag is false for it — which is
  /// the whole reason the flag exists, because that is precisely the case where
  /// publishing the point verbatim would put a marker on the operator's hand.
  ///
  /// It matters because a point that is ALREADY an intersection must not be
  /// orthogonally projected onto a target plane a second time: projecting a hand
  /// position is what produced "the published point is the hand's XY with z
  /// zeroed", and projecting a real hit throws away the depth the runtime
  /// resolved for us. See `resolve_pick_point` in spatial_pick.hpp.
  bool room_point_is_scene_hit = false;

  /// App-space point where this gesture's lifecycle BEGAN
  /// (`qar_app_volume_gesture_get_points`' start point).
  ///
  /// **It is the HAND** — "where it was when the gesture began, and for a hover
  /// where it is now", midway between the hands for a two-hand gesture
  /// (as the installed runtime's header states). Taken with @ref room_point it is
  /// therefore an aiming ray, start → action, which is what @ref has_room_ray
  /// normally carries; see `pointing_ray_from_gesture_points`.
  bool has_room_start_point = false;
  QarVector3 room_start_point{0.0f, 0.0f, 0.0f};

  /// App-space aiming ray. Normally the SDK's own, @ref room_start_point →
  /// @ref room_point; our eye-through-fingertip reconstruction from
  /// `QarHandJoints` when the runtime intersected nothing and the two points
  /// coincide. The direction need not be normalized.
  bool has_room_ray = false;
  QarVector3 ray_origin{0.0f, 0.0f, 0.0f};
  QarVector3 ray_direction{0.0f, 0.0f, 0.0f};

  /// Already-resolved panel coordinates: the vendor's per-panel path. `u` grows
  /// right, `v` grows DOWN (image convention), 0..1 inside the panel — the same
  /// axes as the runtime's CONTENT space, which is what the adapter converts
  /// from. Content space, not PANEL space: the two run backwards from each
  /// other along both axes (see qar_qt_streaming's panel_hit_test.hpp).
  bool has_panel_uv = false;
  std::string panel_key;
  float u = 0.0f;
  float v = 0.0f;
};

/// @brief Supplies pointer samples to the manager.
class IQarPanelInputSource
{
public:
  virtual ~IQarPanelInputSource() = default;

  /// @brief Append every sample received since the previous call to @p out,
  /// oldest first, and forget them. Called on the Qt GUI thread; must not block.
  virtual void drain(std::vector<PanelPointerSample> & out) = 0;

  /// @brief Append the key of every panel the operator has CLOSED IN AR since
  /// the previous call, and forget them.
  ///
  /// The second thing the runtime can tell us about a panel, and the one the
  /// lifetime rule needs: `qar_gui_panels_subscribe_updates` reports a panel
  /// whose state has reached closed, which means the person wearing the headset
  /// dismissed it. The manager (qar_qt_streaming's QtPanelStreamManager) reacts by
  /// stopping capture for that panel and by doing nothing else — emphatically NOT by
  /// destroying its video sender, for the reason on qar_qt_streaming's QtPanelStream::close.
  ///
  /// Defaulted to "nothing to report" so a source that only produces pointer
  /// samples (a test, a gesture-only path) needs no change.
  virtual void drain_closed_panels(std::vector<std::string> & out) { (void)out; }
};

/// @brief A thread-safe queue that is also the programmatic source.
///
/// Push from anywhere — a test, a simulator, or the runtime's callback thread
/// once the real adapter exists — and the manager drains on the GUI thread.
/// The mutex here is the ONE lock on the whole input path, and it lives at the
/// seam rather than in the library core, mirroring how the render path keeps
/// the runtime's threads out.
class QueuedPanelInputSource : public IQarPanelInputSource
{
public:
  void push(const PanelPointerSample & sample);

  void drain(std::vector<PanelPointerSample> & out) override;

  /// @brief Report that the operator closed @p panel_key's panel in AR.
  /// Pushed from the runtime's callback thread, drained on the GUI thread —
  /// the same one-lock-at-the-seam rule as the samples above.
  void push_closed_panel(const std::string & panel_key);

  void drain_closed_panels(std::vector<std::string> & out) override;

  /// @brief Samples pushed but not yet drained.
  std::size_t pending() const;

private:
  mutable std::mutex mutex_;
  std::vector<PanelPointerSample> queue_;
  std::vector<std::string> closed_queue_;
};

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

inline const char *
to_string(PointerGesture gesture)
{
  switch (gesture) {
    case PointerGesture::Click:
      return "Click";
    case PointerGesture::Hover:
      return "Hover";
    case PointerGesture::LongPress:
      return "LongPress";
  }
  return "Unknown";
}

inline const char *
to_string(PointerHand hand)
{
  switch (hand) {
    case PointerHand::Unknown:
      return "Unknown";
    case PointerHand::Left:
      return "Left";
    case PointerHand::Right:
      return "Right";
  }
  return "Unknown";
}

inline const char *
to_string(PointerPhase phase)
{
  switch (phase) {
    case PointerPhase::Started:
      return "Started";
    case PointerPhase::Updated:
      return "Updated";
    case PointerPhase::Ended:
      return "Ended";
    case PointerPhase::Instant:
      return "Instant";
    case PointerPhase::Canceled:
      return "Canceled";
  }
  return "Unknown";
}

inline void
QueuedPanelInputSource::push(const PanelPointerSample & sample)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(sample);
}

inline void
QueuedPanelInputSource::drain(std::vector<PanelPointerSample> & out)
{
  std::vector<PanelPointerSample> pending;
  {
    // Swap under the lock, append outside it: the GUI thread must never hold a
    // lock the runtime's callback thread is waiting on for longer than a swap.
    const std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(queue_);
  }
  for (auto & sample : pending) {
    out.push_back(std::move(sample));
  }
}

inline void
QueuedPanelInputSource::push_closed_panel(const std::string & panel_key)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  closed_queue_.push_back(panel_key);
}

inline void
QueuedPanelInputSource::drain_closed_panels(std::vector<std::string> & out)
{
  std::vector<std::string> pending;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(closed_queue_);
  }
  for (auto & key : pending) {
    out.push_back(std::move(key));
  }
}

inline std::size_t
QueuedPanelInputSource::pending() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

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
