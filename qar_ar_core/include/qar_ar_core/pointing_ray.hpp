// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__POINTING_RAY_HPP_
#define QAR_AR_CORE__POINTING_RAY_HPP_

/// @file pointing_ray.hpp
/// @brief The AR pointing ray. Pure math, Qt-free, and the answer to "where is
/// the ray?" that `spatial_pick.hpp` needs.
///
/// ## Read this first — the SDK ships the ray
///
/// @ref qar_ar_core::pointing_ray_from_gesture_points is the preferred
/// source and everything below it is the FALLBACK. Since the drop after the minimum
/// drop, `qar_app_volume_gesture_get_points` states that the start point is the hand,
/// the action point is what the aim intersected *or the start point when
/// nothing was intersected*, and that **"from the start point to the action
/// point is the ray the user is pointing along, which an app can draw itself"**.
/// So the gesture carries a real aim, in the runtime's own geometry, and the
/// reconstruction below is needed only for the no-intersection case.
///
/// An action point equal to the start point is that documented no-intersection
/// case, not a sign that "the pair is not a ray". A drop older than the drop after
/// the minimum drop reports the two equal on every instant gesture, so with it the
/// fallback is always the ray in use.
///
/// ## Why this file exists
///
/// Two of the three pick targets are ray operations and cannot be done with a
/// point:
///
/// | Target | What it means | Needs |
/// |---|---|---|
/// | `PickTargetKind::GroundPlane` / `Plane` | intersect the aim with z = h | a ray |
/// | `PickTargetKind::Depth` | N metres along the aim | a ray |
/// | `PickTargetKind::ActionPoint` | the thing being pointed at | a point |
///
/// **The RECONSTRUCTED ray is an approximation, and knowingly so** (this
/// applies to @ref qar_ar_core::pointing_ray_from and not to the SDK ray
/// above, which is the runtime's own geometry). QarOS
/// resolves a pick by casting its own ray — which it says is
/// *slightly curved, not a straight line* — and reporting the intersection as
/// the gesture's `action_point`. This file builds a **straight** line through
/// the eye and the hand. A straight line is not that curve, so these two never
/// agree exactly; they agree closely where the curve is shallow. That is why
/// the action-point pick target publishes the runtime's point rather than anything
/// computed here, and why the plane and depth targets are documented to the
/// operator as a *local approximation* rather than as the truth.
///
/// Besides its action and start points, nothing on `QarAppVolumeGesture`
/// carries a direction: `_get_deltas` is motion accumulated since the gesture
/// started, which is zero for an instant gesture, and `_get_classification` /
/// `_get_lifecycle` / `_get_timestamp` carry no geometry.
///
/// So the FALLBACK ray has to be built, and the runtime does expose the two
/// things it takes:
///
///   * **`qar_render_sender_last_hands`** — a full `QarDeviceHandsWithJoints`,
///     26 joint poses per hand. It is a mandatory symbol of the
///     `render_stream_sender` module (a session whose DLL lacked it would fail
///     `qar_library_load` outright), and it is exported by the shipping
///     `QuaternarRemoting.dll`.
///   * **`qar_render_frame_info_get_view_pose`** — the per-eye pose, which the
///     render-sender adapter already reads every frame to drive the stereo
///     cameras.
///
/// Eye position through hand position is the conventional AR pointing ray: it is
/// what the wearer perceives themselves to be aiming, because they sight along
/// their finger with their eye. The hand's OWN forward axis is the fallback for
/// a session that reports hands but no eye pose; it is a worse ray (a small
/// wrist rotation swings it a long way downrange) but it is still an aim, and it
/// is named in the result so the log says which one produced a pick.
///
/// ## Frames
///
/// Everything here is **app content space** — OpenXR's handedness, +Y up, -Z
/// forward, in app units, the same frame the gesture's action point and start
/// point arrive in, so the SDK ray and the reconstruction are interchangeable
/// without any conversion (see `@section qar_coordinate_systems` in
/// `third_party/qar/include/qar_streaming.h` and `spatial_frames.hpp`).
/// Crossing to the ROS world is `spatial_frames.hpp`'s job and happens once,
/// downstream, in `spatial_pointer_from_sample`. Nothing in this header knows
/// about ROS.
///
/// The frame the runtime reports hand joints in is **not stated** in the vendored
/// header (`QarDeviceHandsWithJoints` carries a `device_id` and a timestamp and
/// no frame note), and app content space is the only frame the rest of the
/// tracking API speaks. @ref PointingRay therefore carries
/// @ref PointingRay::aim_to_reported_m: the perpendicular distance from the
/// derived straight ray to the action point the runtime resolved for the same
/// gesture.
///
/// ## What `aim_to_reported_m` measures
///
/// It is the **error of the fallback path**: how far our reconstruction lands
/// from the runtime's own action point on an aim where the runtime did
/// intersect something. It never chooses a ray and it is not a frame
/// proof. It is computed against the reconstruction even when the SDK ray was
/// the one used, because against the SDK's own ray it is zero by construction.
///
/// The number mixes **two** effects that it cannot separate:
///
///   1. **Curvature.** QarOS casts a curved ray; we cast a straight one. Even
///      with a perfect frame and perfect joints, the straight line misses the
///      curve's endpoint by however much the curve bent on that aim.
///   2. **Frame error.** Hands reported in some frame other than app content
///      space.
///
/// So a SMALL value does not prove the frame assumption — it is equally
/// consistent with a shallow curve masking a modest frame error — and a LARGE
/// value does not indict the frame, because a strongly curved aim produces one
/// legitimately. Only the extremes say anything: metres-scale offsets on
/// *every* aim are a frame fault (no plausible curve is metres wide near the
/// hand), and a residual that grows with distance down-range is the signature of
/// a curve rather than of a frame.
///
/// **What would restore the check**: any one of (a) the runtime exposing the ray
/// it actually cast (origin + direction, or its curve parameters), so the
/// residual is measured against the same geometry; (b) a stated frame for
/// `QarDeviceHandsWithJoints`, which makes the check unnecessary; or (c) a
/// controlled rig — a known flat surface, the hand held still, several aims at
/// known positions — where the frame term is pinned by construction and whatever
/// is left is the curve. None of the three exists, so the honest
/// statement is: **this number is an upper bound on the frame error, not a
/// measurement of it.** It is still worth logging — it is the size of the
/// disagreement between what we compute and what QarOS computes, which is
/// exactly what the operator needs to know before selecting a straight-ray
/// target.
///
/// ## Threads
///
/// The hands are polled on the render thread (once per frame, as the vendor
/// requires — "poll it once per frame, and no more often than the device sends")
/// and consumed on the QarOS gesture-callback thread, which fires at ~100 Hz.
/// @ref PointingRaySlot is the one-value hand-off between them: last writer wins,
/// readers get a copy, and a reader never blocks a frame.

#include "qar_ar_core/qar_streaming_c.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

namespace qar_ar_core
{

// ---------------------------------------------------------------------------
// What built a ray
// ---------------------------------------------------------------------------

/// @brief How a @ref PointingRay was derived, reported so a log names it.
enum class PointingRayKind : std::uint8_t
{
  /// Nothing usable arrived; @ref PointingRay::error says what was missing.
  None = 0,
  /// Eye position through hand position: the ray the wearer believes they are
  /// aiming, because they sight along their finger. The FALLBACK; the SDK ray
  /// (drop after the minimum drop) is preferred - see @ref GestureStartToAction.
  EyeThroughHand,
  /// The aiming joint's own forward axis (OpenXR: local -Z runs distally, out
  /// along the bone). Used when no eye pose is available.
  HandForward,
  /// The SDK's own aim: `qar_app_volume_gesture_get_points`' **start point**
  /// (the hand) through its **action point** (what the aim intersected). The
  /// PREFERRED source (drop after the minimum drop): the header states it outright,
  /// see the file comment. It needs no eye pose, no hand joints and no
  /// reconstruction, and it is the runtime's own geometry rather than our
  /// straight-line guess at it.
  GestureStartToAction
};
const char * to_string(PointingRayKind kind);

/// @brief Which joint aims the ray.
enum class HandRayJoint : std::uint8_t
{
  /// `QAR_HAND_JOINT_INDEX_TIP_EXT` — the fingertip the wearer points with.
  IndexTip = 0,
  /// `QAR_HAND_JOINT_PALM_EXT` — steadier, and tracked when a curled index is
  /// not. The fallback, and what a fist-shaped "pointing" pose really aims.
  Palm
};
const char * to_string(HandRayJoint joint);

// ---------------------------------------------------------------------------
// The ray
// ---------------------------------------------------------------------------

/// @brief A room-space aiming ray, and the provenance a log needs to trust it.
struct PointingRay
{
  bool valid = false;
  QarVector3 origin{0.0f, 0.0f, 0.0f};
  /// Unit length when @ref valid.
  QarVector3 direction{0.0f, 0.0f, 0.0f};
  PointingRayKind kind = PointingRayKind::None;
  HandRayJoint joint = HandRayJoint::IndexTip;
  QarHand hand = QAR_HAND_RIGHT;
  /// The producer's own clock, seconds. Carried so a consumer can see how stale
  /// the ray is relative to the gesture it is about to be attached to.
  double timestamp_s = 0.0;

  /// Perpendicular distance, in app units, from **our eye-through-fingertip
  /// reconstruction** to the action point the runtime reported for the same
  /// aim. Negative means "not computed" (no reconstruction, or no reported
  /// point to compare against).
  ///
  /// **What it measures.** It is not a frame check and not a curvature
  /// measurement: it is the *error of the fallback path*. The SDK hands out its
  /// own aim (@ref PointingRayKind::GestureStartToAction), so the reconstruction is
  /// only used when the runtime intersected nothing. This number says how far
  /// that fallback would have been from the runtime's own answer on an aim
  /// where both existed — i.e. how much accuracy an operator loses on the
  /// gestures that fall back. Small means the fallback is a fair stand-in;
  /// metres on every aim means the fallback is unusable and the hand joints
  /// are in some frame we do not think they are in.
  ///
  /// It is always computed against the reconstruction, whichever ray was
  /// actually chosen, because against the SDK's own ray it would be zero by
  /// construction and would say nothing at all.
  float aim_to_reported_m = -1.0f;

  /// Why there is no ray, for the operator. Empty when @ref valid.
  std::string error;
};

/// @brief Everything @ref pointing_ray_from needs, with no SDK handles in it, so
/// the maths is testable without a session.
struct PointingRayInputs
{
  bool has_eye = false;
  QarVector3 eye_position{0.0f, 0.0f, 0.0f};

  bool hand_tracked = false;
  QarVector3 hand_position{0.0f, 0.0f, 0.0f};
  bool has_hand_orientation = false;
  QarQuaternion hand_orientation{0.0f, 0.0f, 0.0f, 1.0f};  ///< xyzw

  QarHand hand = QAR_HAND_RIGHT;
  HandRayJoint joint = HandRayJoint::IndexTip;
  double timestamp_s = 0.0;

  /// Below this eye-to-hand separation the direction is dominated by tracking
  /// noise rather than by where the wearer is aiming, so the eye-through-hand
  /// construction is refused rather than amplified. 15 cm is closer than anyone
  /// holds a pointing hand to their own eye.
  float min_separation_m = 0.15f;
};

/// @brief Build the aiming ray. Eye-through-hand when both are there, the hand's
/// own forward axis when only the hand is, and an explained failure otherwise.
///
/// Degenerate input is reported, never silently rounded: an untracked hand, a
/// non-finite position, an eye and a hand closer together than
/// @ref PointingRayInputs::min_separation_m, and a hand orientation that does
/// not normalize.
PointingRay pointing_ray_from(const PointingRayInputs & inputs);

/// @brief The SDK's OWN aiming ray: a gesture's start point through its action
/// point. Prefer this over @ref pointing_ray_from; fall back to that one when
/// this returns an invalid ray.
///
/// The header of the drop after the minimum drop (`qar_app_volume_gesture_get_points`)
/// makes the pair a ray outright:
///
/// > *The start point is the hand ... The action point is the intersection the
/// > gesture acts on, **or the start point when nothing was intersected** ...
/// > from the start point to the action point is the ray the user is pointing
/// > along, which an app can draw itself.*
///
/// So the two being EQUAL is not "the pair is not a ray" — it is the documented
/// no-intersection case, and the only case in which this function refuses.
/// (A drop older than the drop after the minimum drop reports the two equal on
/// every instant gesture, so with it this function always refuses and the
/// reconstruction is used.)
///
/// Both points are in **app content space**, in app units (the vendor's
/// @ref qar_coordinate_systems section states it), which is the same frame the
/// reconstructed ray and the action point already ride in, so a caller may
/// swap one for the other without any change of frame.
///
/// @param min_span the shortest start→action separation that still carries a
///        direction rather than noise, in app units. The runtime copies the
///        start point verbatim when it intersected nothing, so the real
///        no-hit case is exact equality; this threshold only guards against a
///        near-degenerate one.
PointingRay pointing_ray_from_gesture_points(
  bool has_start_point, const QarVector3 & start_point, bool has_action_point,
  const QarVector3 & action_point, double timestamp_s = 0.0, float min_span = 1e-3f);

/// @brief Rotate @p v by the (xyzw) quaternion @p q. Identity for a degenerate q.
QarVector3 rotate_by_quaternion(const QarQuaternion & q, const QarVector3 & v);

/// @brief Perpendicular distance from the ray to @p point, or -1 for a
/// degenerate ray. Fills @ref PointingRay::aim_to_reported_m — the
/// straight-versus-curved residual, not a frame proof.
float ray_point_distance(
  const QarVector3 & origin, const QarVector3 & direction, const QarVector3 & point);

// ---------------------------------------------------------------------------
// Reading the SDK's hand sample
// ---------------------------------------------------------------------------

/// @brief Fill the hand half of @p io from one hand's joints.
///
/// Reads @p joint, falling back to `QAR_HAND_JOINT_PALM_EXT` when the requested
/// joint has no valid position — a curled index reports the palm even when the
/// tip does not. Honours `QAR_POSITION_VALID_BIT` / `QAR_ORIENTATION_VALID_BIT`
/// rather than trusting whatever is in the array.
///
/// @return false when the hand is untracked or no usable joint was found; @p io
///         is then left with `hand_tracked == false`.
bool hand_ray_inputs(
  const QarHandJoints & hand_joints, QarHand which, HandRayJoint joint,
  PointingRayInputs & io);

/// @brief Choose a hand out of a device sample and read its aiming joint.
///
/// @p preferred wins when it is tracked; otherwise the other hand is used, so a
/// left-handed operator is not silently ignored. Neither tracked gives
/// `hand_tracked == false`, which @ref pointing_ray_from turns into an explained
/// invalid ray.
PointingRayInputs pointing_ray_inputs_from_hands(
  const QarDeviceHandsWithJoints & hands, QarHand preferred, HandRayJoint joint);

// ---------------------------------------------------------------------------
// The hand itself -- the measurement that says what `action_point` IS
// ---------------------------------------------------------------------------

/// @brief One device hand sample reduced to joint POSITIONS, plus the eye pose
/// of the frame it was read on.
///
/// @ref PointingRay answers "where is the operator aiming". This answers the
/// different question "where is the operator's HAND", and the two together
/// settle the only question that matters about a gesture's `action_point`: the
/// runtime documents it as the intersection QarOS computes between the
/// wearer's curved aim and the app volume's scene. If the
/// distance from the reported action point to the nearest tracked hand joint is
/// a few centimetres on every gesture, then what the API returns is the hand and
/// the description does not match the shipped behaviour. If it is metres, the
/// runtime is reporting something out in the scene and any remaining error is
/// ours. Nothing short of this distance distinguishes the two.
///
/// Built by the same render-thread poll of `qar_render_sender_last_hands` that
/// builds the ray and handed across on the same @ref PointingRaySlot, so the
/// probe and the ray always describe ONE frame. Mixing frames would measure a
/// distance that never existed.
///
/// Positions only: orientation and velocity cannot shorten a distance, and every
/// byte here is copied under a mutex once per frame.
struct HandJointsProbe
{
  /// A hand sample was read at all. False means `qar_render_sender_last_hands`
  /// refused, and every distance below is UNKNOWN rather than zero.
  bool valid = false;
  bool has_eye = false;
  QarVector3 eye_position{0.0f, 0.0f, 0.0f};
  /// The producer's own clock, seconds -- the same value @ref PointingRay
  /// carries, so a reader can see how stale the sample is.
  double timestamp_s = 0.0;

  /// @brief One hand's joint positions, indexed by `QarHandJoint`.
  struct Hand
  {
    bool tracked = false;
    /// `QAR_POSITION_VALID_BIT` was set AND the position is finite. A joint that
    /// fails either test is absent, never silently (0, 0, 0).
    bool has_joint[QAR_HAND_JOINT_COUNT] = {};
    QarVector3 joint[QAR_HAND_JOINT_COUNT] = {};
  };
  Hand left;
  Hand right;

  const Hand & hand(QarHand which) const
  {
    return which == QAR_HAND_LEFT ? left : right;
  }
};

/// @brief The closest tracked joint of either hand to a query point.
struct NearestHandJoint
{
  bool found = false;
  QarHand hand = QAR_HAND_RIGHT;
  /// A `QarHandJoint` value; name it with @ref hand_joint_name.
  std::uint32_t joint_id = 0;
  QarVector3 position{0.0f, 0.0f, 0.0f};
  /// Metres. Negative means no joint was tracked to compare against.
  float distance_m = -1.0f;
};

/// @brief A `QarHandJoint`'s name, for a log line a human reads.
const char * hand_joint_name(std::uint32_t joint_id);

/// @brief Read every valid joint position out of a device hand sample.
///
/// The eye and the timestamp are left to the caller, exactly as
/// @ref pointing_ray_inputs_from_hands leaves them: the frame's view pose and
/// the clock conversion belong to the host that polls, not to this maths.
HandJointsProbe hand_joints_probe_from(const QarDeviceHandsWithJoints & hands);

/// @brief The nearest tracked joint of EITHER hand to @p point, and how far.
///
/// THE measurement. Centimetres means the runtime reported the operator's own
/// hand; metres means it reported something out in the scene.
NearestHandJoint nearest_hand_joint(const HandJointsProbe & probe, const QarVector3 & point);

/// @brief Distance from one named joint to @p point, or -1 when that joint is
/// not tracked.
///
/// Reported alongside the nearest-of-all answer because the index tip is the
/// joint a user believes they are pointing with, so "it landed on my fingertip"
/// and "it landed somewhere on my hand" are different findings.
float hand_joint_distance(
  const HandJointsProbe & probe, QarHand which, QarHandJoint joint, const QarVector3 & point);

// ---------------------------------------------------------------------------
// The thread hand-off
// ---------------------------------------------------------------------------

/// @brief One published ray, handed from the render thread to the gesture
/// thread. Last writer wins; a reader takes a copy and never blocks a frame.
class PointingRaySlot
{
public:
  /// @brief Publish (render thread). An invalid ray is published too — that is
  /// how "the hands went away" reaches the consumer instead of a stale aim.
  void publish(const PointingRay & ray);

  /// @brief Publish the ray together with the hand sample it was built FROM
  /// (render thread).
  ///
  /// One call, because they describe one frame: a consumer that took the ray
  /// from this frame and the hand from the previous one would measure a distance
  /// that never existed. The ray-only overload publishes an empty probe for the
  /// same reason -- "no hands were read" must not read as "the hand is at the
  /// origin".
  void publish(const PointingRay & ray, const HandJointsProbe & probe);

  /// @brief The most recent ray (any thread). Invalid until the first publish.
  PointingRay latest() const;

  /// @brief The hand sample the latest ray was built from (any thread).
  /// `valid == false` until a probe is published.
  HandJointsProbe latest_probe() const;

  /// @brief How many rays have been published, and how many of those were valid.
  /// The two numbers that say whether the runtime is sending hands at all.
  std::uint64_t published() const;
  std::uint64_t published_valid() const;

  /// @brief Forget the ray and the counters (teardown).
  void clear();

private:
  mutable std::mutex mutex_;
  PointingRay ray_{};
  HandJointsProbe probe_{};
  std::uint64_t published_ = 0;
  std::uint64_t published_valid_ = 0;
};

// ---------------------------------------------------------------------------
// Implementation
//
// Header-inline, like panel_input_source.hpp: qar_qar_adapter is the code that both
// produces the ray, on the render thread (component `render`), and consumes it, on the
// gesture thread (component `gestures`), and both components include this one definition,
// which needs no translation unit of its own.
// ---------------------------------------------------------------------------

/// Small shared helpers. A NAMED namespace, not an anonymous one: an anonymous
/// namespace in a header gives every translation unit its own copy of these, and
/// the inline functions below that call them would then be ODR-violating.
namespace pointing_ray_detail
{

constexpr float kEpsilon = 1e-6f;

inline bool finite(const QarVector3 & v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline QarVector3 subtract(const QarVector3 & a, const QarVector3 & b)
{
  return QarVector3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float dot(const QarVector3 & a, const QarVector3 & b)
{
  return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline float length(const QarVector3 & v)
{
  return std::sqrt(dot(v, v));
}

inline bool normalize(const QarVector3 & in, QarVector3 & out)
{
  if (!finite(in)) {
    return false;
  }
  const float len = length(in);
  if (!(len > kEpsilon) || !std::isfinite(len)) {
    return false;
  }
  out = QarVector3{in.x / len, in.y / len, in.z / len};
  return true;
}

/// The joint index the enum names.
inline std::uint32_t joint_index(HandRayJoint joint)
{
  return joint == HandRayJoint::Palm
    ? static_cast<std::uint32_t>(QAR_HAND_JOINT_PALM_EXT)
    : static_cast<std::uint32_t>(QAR_HAND_JOINT_INDEX_TIP_EXT);
}

}  // namespace pointing_ray_detail

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

inline const char * to_string(PointingRayKind kind)
{
  switch (kind) {
    case PointingRayKind::None: return "none";
    case PointingRayKind::EyeThroughHand: return "eye-through-hand";
    case PointingRayKind::HandForward: return "hand-forward";
    case PointingRayKind::GestureStartToAction: return "gesture-start-to-action";
  }
  return "unknown";
}

inline const char * to_string(HandRayJoint joint)
{
  switch (joint) {
    case HandRayJoint::IndexTip: return "index tip";
    case HandRayJoint::Palm: return "palm";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------

inline QarVector3 rotate_by_quaternion(const QarQuaternion & q, const QarVector3 & v)
{
  // v' = v + 2 * cross(qv, cross(qv, v) + w * v) -- the standard branch-free
  // form. A degenerate quaternion cannot be normalized, so the rotation is the
  // identity rather than a silent scale of the vector.
  const float len2 = (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w);
  if (!std::isfinite(len2) || !(len2 > pointing_ray_detail::kEpsilon)) {
    return v;
  }
  const float inv = 1.0f / std::sqrt(len2);
  const float x = q.x * inv;
  const float y = q.y * inv;
  const float z = q.z * inv;
  const float w = q.w * inv;

  const QarVector3 u{x, y, z};
  const QarVector3 uxv{
    (u.y * v.z) - (u.z * v.y), (u.z * v.x) - (u.x * v.z), (u.x * v.y) - (u.y * v.x)};
  const QarVector3 inner{uxv.x + (w * v.x), uxv.y + (w * v.y), uxv.z + (w * v.z)};
  const QarVector3 outer{
    (u.y * inner.z) - (u.z * inner.y), (u.z * inner.x) - (u.x * inner.z),
    (u.x * inner.y) - (u.y * inner.x)};
  return QarVector3{v.x + (2.0f * outer.x), v.y + (2.0f * outer.y), v.z + (2.0f * outer.z)};
}

inline float ray_point_distance(
  const QarVector3 & origin, const QarVector3 & direction, const QarVector3 & point)
{
  QarVector3 unit{0.0f, 0.0f, 0.0f};
  namespace detail = pointing_ray_detail;
  if (!detail::finite(origin) || !detail::finite(point) ||
    !detail::normalize(direction, unit))
  {
    return -1.0f;
  }
  const QarVector3 to_point = detail::subtract(point, origin);
  const float along = detail::dot(to_point, unit);
  const QarVector3 perpendicular{
    to_point.x - (unit.x * along), to_point.y - (unit.y * along),
    to_point.z - (unit.z * along)};
  return detail::length(perpendicular);
}

// ---------------------------------------------------------------------------
// Building the ray
// ---------------------------------------------------------------------------

inline PointingRay pointing_ray_from_gesture_points(
  bool has_start_point, const QarVector3 & start_point, bool has_action_point,
  const QarVector3 & action_point, double timestamp_s, float min_span)
{
  namespace detail = pointing_ray_detail;
  PointingRay out;
  // No hand and no joint took part in this construction, so nothing here may be
  // read as one: the trace names the KIND, and the kind says where it came from.
  out.timestamp_s = timestamp_s;

  if (!has_start_point || !has_action_point) {
    // The header: the action point "is absent only when the start point is
    // too", so in practice this is "the gesture carried no points at all".
    out.error = "the gesture carried no start/action pair to aim along";
    return out;
  }
  if (!detail::finite(start_point) || !detail::finite(action_point)) {
    out.error = "the gesture's start or action point is not finite";
    return out;
  }

  const QarVector3 aim = detail::subtract(action_point, start_point);
  if (!(detail::length(aim) >= min_span) || !detail::normalize(aim, out.direction)) {
    // THE documented no-intersection case: "the action point is ... the start
    // point when nothing was intersected". It is legal and expected -- an aim
    // into empty space -- so it is a refusal of THIS construction and not an
    // error of the gesture. The caller falls back to the reconstruction.
    out.direction = QarVector3{0.0f, 0.0f, 0.0f};
    out.error =
      "the gesture's action point IS its start point, so the runtime intersected nothing";
    return out;
  }

  out.origin = start_point;
  out.kind = PointingRayKind::GestureStartToAction;
  out.valid = true;
  return out;
}

inline PointingRay pointing_ray_from(const PointingRayInputs & inputs)
{
  PointingRay out;
  out.hand = inputs.hand;
  out.joint = inputs.joint;
  out.timestamp_s = inputs.timestamp_s;

  if (!inputs.hand_tracked) {
    out.error = "no hand is tracked, so there is nothing to aim with";
    return out;
  }
  if (!pointing_ray_detail::finite(inputs.hand_position)) {
    out.error = "the tracked hand position is not finite";
    return out;
  }

  // Preferred: the ray the wearer perceives -- their eye, through their fingertip.
  if (inputs.has_eye) {
    if (!pointing_ray_detail::finite(inputs.eye_position)) {
      out.error = "the eye position is not finite";
      return out;
    }
    const QarVector3 aim =
      pointing_ray_detail::subtract(inputs.hand_position, inputs.eye_position);
    const float separation = pointing_ray_detail::length(aim);
    if (!(separation >= inputs.min_separation_m)) {
      // Not an error worth failing the whole pick over if the hand can still
      // aim on its own, but it IS a refusal of this construction: at 2 cm of
      // separation the direction is tracking noise, not intent.
      if (!inputs.has_hand_orientation) {
        out.error = "the hand is too close to the eye to aim with, and carries no orientation";
        return out;
      }
    } else if (pointing_ray_detail::normalize(aim, out.direction)) {
      out.origin = inputs.eye_position;
      out.kind = PointingRayKind::EyeThroughHand;
      out.valid = true;
      return out;
    }
  }

  // Fallback: the aiming joint's own forward axis. OpenXR hand-joint poses put
  // local -Z along the bone in the distal direction, so -Z is "out of the
  // fingertip" -- the same -Z-forward convention room space itself uses.
  if (inputs.has_hand_orientation) {
    const QarVector3 forward =
      rotate_by_quaternion(inputs.hand_orientation, QarVector3{0.0f, 0.0f, -1.0f});
    if (pointing_ray_detail::normalize(forward, out.direction)) {
      out.origin = inputs.hand_position;
      out.kind = PointingRayKind::HandForward;
      out.valid = true;
      return out;
    }
    out.error = "the hand orientation does not produce a usable forward axis";
    return out;
  }

  out.error = "the hand carries no orientation and there is no eye pose to aim from";
  return out;
}

// ---------------------------------------------------------------------------
// Reading the SDK's hand sample
// ---------------------------------------------------------------------------

inline bool hand_ray_inputs(
  const QarHandJoints & hand_joints, QarHand which, HandRayJoint joint,
  PointingRayInputs & io)
{
  io.hand = which;
  io.joint = joint;
  io.hand_tracked = false;
  io.has_hand_orientation = false;

  if (!hand_joints.is_tracked) {
    return false;
  }

  // Try the requested joint, then the palm: a curled index reports the palm even
  // when the tip does not, and a pointing pose that is really a fist still aims.
  const HandRayJoint candidates[2] = {joint, HandRayJoint::Palm};
  for (const HandRayJoint candidate : candidates) {
    const std::uint32_t index = pointing_ray_detail::joint_index(candidate);
    if (index >= QAR_HAND_JOINT_COUNT) {
      continue;
    }
    const QarHandJointLocation & location = hand_joints.joint_locations[index];
    if ((location.location_flags & QAR_POSITION_VALID_BIT) == 0u) {
      continue;
    }
    if (!std::isfinite(location.pose.position.x) || !std::isfinite(location.pose.position.y) ||
      !std::isfinite(location.pose.position.z))
    {
      continue;
    }
    io.hand_tracked = true;
    io.joint = candidate;
    io.hand_position = location.pose.position;
    io.has_hand_orientation = (location.location_flags & QAR_ORIENTATION_VALID_BIT) != 0u;
    io.hand_orientation = location.pose.orientation;
    return true;
  }
  return false;
}

inline PointingRayInputs pointing_ray_inputs_from_hands(
  const QarDeviceHandsWithJoints & hands, QarHand preferred, HandRayJoint joint)
{
  PointingRayInputs inputs;
  inputs.hand = preferred;
  inputs.joint = joint;

  const QarHandJoints & first =
    preferred == QAR_HAND_LEFT ? hands.left_hand : hands.right_hand;
  if (hand_ray_inputs(first, preferred, joint, inputs)) {
    return inputs;
  }
  // The other hand, so a left-handed operator is not silently ignored.
  const QarHand other = preferred == QAR_HAND_LEFT ? QAR_HAND_RIGHT : QAR_HAND_LEFT;
  const QarHandJoints & second = other == QAR_HAND_LEFT ? hands.left_hand : hands.right_hand;
  hand_ray_inputs(second, other, joint, inputs);
  return inputs;
}

// ---------------------------------------------------------------------------
// The hand itself
// ---------------------------------------------------------------------------

inline const char * hand_joint_name(std::uint32_t joint_id)
{
  switch (static_cast<QarHandJoint>(joint_id)) {
    case QAR_HAND_JOINT_PALM_EXT: return "palm";
    case QAR_HAND_JOINT_WRIST_EXT: return "wrist";
    case QAR_HAND_JOINT_THUMB_METACARPAL_EXT: return "thumb metacarpal";
    case QAR_HAND_JOINT_THUMB_PROXIMAL_EXT: return "thumb proximal";
    case QAR_HAND_JOINT_THUMB_DISTAL_EXT: return "thumb distal";
    case QAR_HAND_JOINT_THUMB_TIP_EXT: return "thumb tip";
    case QAR_HAND_JOINT_INDEX_METACARPAL_EXT: return "index metacarpal";
    case QAR_HAND_JOINT_INDEX_PROXIMAL_EXT: return "index proximal";
    case QAR_HAND_JOINT_INDEX_INTERMEDIATE_EXT: return "index intermediate";
    case QAR_HAND_JOINT_INDEX_DISTAL_EXT: return "index distal";
    case QAR_HAND_JOINT_INDEX_TIP_EXT: return "index tip";
    case QAR_HAND_JOINT_MIDDLE_METACARPAL_EXT: return "middle metacarpal";
    case QAR_HAND_JOINT_MIDDLE_PROXIMAL_EXT: return "middle proximal";
    case QAR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT: return "middle intermediate";
    case QAR_HAND_JOINT_MIDDLE_DISTAL_EXT: return "middle distal";
    case QAR_HAND_JOINT_MIDDLE_TIP_EXT: return "middle tip";
    case QAR_HAND_JOINT_RING_METACARPAL_EXT: return "ring metacarpal";
    case QAR_HAND_JOINT_RING_PROXIMAL_EXT: return "ring proximal";
    case QAR_HAND_JOINT_RING_INTERMEDIATE_EXT: return "ring intermediate";
    case QAR_HAND_JOINT_RING_DISTAL_EXT: return "ring distal";
    case QAR_HAND_JOINT_RING_TIP_EXT: return "ring tip";
    case QAR_HAND_JOINT_LITTLE_METACARPAL_EXT: return "little metacarpal";
    case QAR_HAND_JOINT_LITTLE_PROXIMAL_EXT: return "little proximal";
    case QAR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT: return "little intermediate";
    case QAR_HAND_JOINT_LITTLE_DISTAL_EXT: return "little distal";
    case QAR_HAND_JOINT_LITTLE_TIP_EXT: return "little tip";
    case QAR_HAND_JOINT_MAX_ENUM_EXT: break;
  }
  return "unknown joint";
}

inline HandJointsProbe hand_joints_probe_from(const QarDeviceHandsWithJoints & hands)
{
  HandJointsProbe probe;
  probe.valid = true;

  const QarHandJoints * const sources[2] = {&hands.left_hand, &hands.right_hand};
  HandJointsProbe::Hand * const targets[2] = {&probe.left, &probe.right};
  for (int i = 0; i < 2; ++i) {
    const QarHandJoints & source = *sources[i];
    HandJointsProbe::Hand & target = *targets[i];
    target.tracked = source.is_tracked;
    if (!source.is_tracked) {
      continue;
    }
    for (std::uint32_t j = 0; j < QAR_HAND_JOINT_COUNT; ++j) {
      // Indexed BY the QarHandJoint enum, the same way hand_ray_inputs() reads
      // it. QarHandJointLocation also carries its own joint_id; the array layout
      // is what the rest of this file already relies on, so the two must not be
      // mixed within one build.
      const QarHandJointLocation & location = source.joint_locations[j];
      if ((location.location_flags & QAR_POSITION_VALID_BIT) == 0u) {
        continue;
      }
      if (!pointing_ray_detail::finite(location.pose.position)) {
        continue;
      }
      target.has_joint[j] = true;
      target.joint[j] = location.pose.position;
    }
  }
  return probe;
}

inline NearestHandJoint nearest_hand_joint(
  const HandJointsProbe & probe, const QarVector3 & point)
{
  namespace detail = pointing_ray_detail;
  NearestHandJoint out;
  if (!probe.valid || !detail::finite(point)) {
    return out;
  }
  const QarHand sides[2] = {QAR_HAND_LEFT, QAR_HAND_RIGHT};
  for (const QarHand which : sides) {
    const HandJointsProbe::Hand & hand = probe.hand(which);
    if (!hand.tracked) {
      continue;
    }
    for (std::uint32_t j = 0; j < QAR_HAND_JOINT_COUNT; ++j) {
      if (!hand.has_joint[j]) {
        continue;
      }
      const float distance = detail::length(detail::subtract(hand.joint[j], point));
      if (!std::isfinite(distance)) {
        continue;
      }
      if (!out.found || distance < out.distance_m) {
        out.found = true;
        out.hand = which;
        out.joint_id = j;
        out.position = hand.joint[j];
        out.distance_m = distance;
      }
    }
  }
  return out;
}

inline float hand_joint_distance(
  const HandJointsProbe & probe, QarHand which, QarHandJoint joint, const QarVector3 & point)
{
  namespace detail = pointing_ray_detail;
  if (!probe.valid || !detail::finite(point)) {
    return -1.0f;
  }
  const std::uint32_t index = static_cast<std::uint32_t>(joint);
  if (index >= QAR_HAND_JOINT_COUNT) {
    return -1.0f;
  }
  const HandJointsProbe::Hand & hand = probe.hand(which);
  if (!hand.tracked || !hand.has_joint[index]) {
    return -1.0f;
  }
  const float distance = detail::length(detail::subtract(hand.joint[index], point));
  return std::isfinite(distance) ? distance : -1.0f;
}

// ---------------------------------------------------------------------------
// The thread hand-off
// ---------------------------------------------------------------------------

inline void PointingRaySlot::publish(const PointingRay & ray)
{
  // No hand sample means NO hand sample. Keeping the previous frame's joints
  // would let a consumer measure a fresh action point against a stale hand and
  // report a distance that never existed -- the one error this whole
  // measurement cannot afford.
  publish(ray, HandJointsProbe{});
}

inline void PointingRaySlot::publish(const PointingRay & ray, const HandJointsProbe & probe)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  ray_ = ray;
  probe_ = probe;
  ++published_;
  if (ray.valid) {
    ++published_valid_;
  }
}

inline HandJointsProbe PointingRaySlot::latest_probe() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return probe_;
}

inline PointingRay PointingRaySlot::latest() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return ray_;
}

inline std::uint64_t PointingRaySlot::published() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return published_;
}

inline std::uint64_t PointingRaySlot::published_valid() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return published_valid_;
}

inline void PointingRaySlot::clear()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  ray_ = PointingRay{};
  probe_ = HandJointsProbe{};
  published_ = 0;
  published_valid_ = 0;
}

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__POINTING_RAY_HPP_
