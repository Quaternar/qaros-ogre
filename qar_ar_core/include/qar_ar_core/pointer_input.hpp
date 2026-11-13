// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__POINTER_INPUT_HPP_
#define QAR_AR_CORE__POINTER_INPUT_HPP_

/// @file pointer_input.hpp
/// @brief The vocabulary of AR pointer input: the gestures, hands and phases, one raw sample of
/// a pointer, the interface that supplies samples and a thread-safe queue that implements it.
/// Qt-free, and no call into the QarOS C API.
///
/// A sample carries EITHER a room-space point OR an already-resolved panel key and (u, v). The
/// phase mirrors `QarGesturePhase` one-to-one; the gesture is a deliberate NARROWING of the two
/// vendor kind enums onto what a widget can act on (see @ref PointerGesture).
///
/// ## Threading
///
/// The interface is PULL-based: the panel manager drains a source from the Qt GUI thread, and
/// everything downstream runs there with no lock, exactly like the capture path. The runtime's
/// callback threads never reach the library core: a source that receives samples on another thread
/// queues them, and @ref QueuedPanelInputSource is that queue.
///
/// ## Why everything here is `inline`
///
/// The members are defined in the header itself, so the code that fills a source and the code that
/// drains it share one definition and the vocabulary needs no translation unit of its own.

#include "qar_ar_core/qar_streaming_c.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qar_ar_core
{

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
  /// other along both axes.
  bool has_panel_uv = false;
  std::string panel_key;
  float u = 0.0f;
  float v = 0.0f;
};

/// @brief Supplies pointer samples to the manager.
class IPanelInputSource
{
public:
  virtual ~IPanelInputSource() = default;

  /// @brief Append every sample received since the previous call to @p out,
  /// oldest first, and forget them. Called on the Qt GUI thread; must not block.
  virtual void drain(std::vector<PanelPointerSample> & out) = 0;

  /// @brief Append the key of every panel the operator has CLOSED IN AR since
  /// the previous call, and forget them.
  ///
  /// The second thing the runtime can tell us about a panel, and the one the
  /// lifetime rule needs: `qar_gui_panels_subscribe_updates` reports a panel
  /// whose state has reached closed, which means the person wearing the headset
  /// dismissed it. The panel stream manager reacts by
  /// stopping capture for that panel and by doing nothing else — emphatically NOT by
  /// destroying its video sender.
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
class QueuedPanelInputSource : public IPanelInputSource
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

// ---------------------------------------------------------------------------
// Definitions. Header-inline, see the file comment.
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

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__POINTER_INPUT_HPP_
