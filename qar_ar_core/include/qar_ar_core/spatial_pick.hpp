// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__SPATIAL_PICK_HPP_
#define QAR_AR_CORE__SPATIAL_PICK_HPP_

/// @file spatial_pick.hpp
/// @brief Spatial AR selection: points, poses, areas and navigation goals
/// picked directly in the 3D scene. Pure math + state machines, Qt-free.
///
/// ## One input seam, two kinds of interaction
///
/// `panel_input_source.hpp` is THE way an AR pointer enters this library, and
/// qar_qt_streaming's `panel_hit_test.hpp` is the maths that lands it on a panel pixel.
/// This header is that seam's other consumer: the same
/// @ref PanelPointerSample, with the same @ref PointerPhase lifecycle, resolved
/// against the **3D scene** instead of a panel rectangle. Nothing here invents a
/// second pointer type, a second phase enum or a second clock — a sample that
/// misses every panel is a spatial pick, and one sample stream feeds both.
///
/// The sample's room-space fields are what matter here:
///   * `has_room_point` / `room_point` — the app-volume gesture path
///     (`qar_app_volumes_subscribe_gesture_updates` ->
///     `QarAppVolumeGestureEvent::action_point`);
///   * `has_room_ray` / `ray_origin` / `ray_direction` — the aiming ray, either
///     the SDK's own (start point through action point) or a hand ray rebuilt
///     from the hand joints, see `pointing_ray.hpp`.
///   * `has_panel_uv` — a 2D panel event; deliberately NOT a spatial pick.
///
/// Everything in this file works in the **ROS world frame**: room-space samples
/// are brought across by `spatial_frames.hpp` first
/// (@ref spatial_pointer_from_sample), because a pick has to end up in the frame
/// the operator's ROS graph speaks.
///
/// ## The state machines
///
/// AR selection is a gesture, not a click, so each pick kind is an explicit
/// state machine driven by @ref PointerPhase — and each one has a cancel path,
/// because an AR user who changes their mind must be able to abandon a goal
/// without publishing it:
///
/// | Session | Pending | Orienting | Ends as |
/// |---|---|---|---|
/// | @ref PointPickSession | previewing the point under the pointer | — | released / abandoned |
/// | @ref PosePickSession  | position fixed by the press | drag sets the heading | released / abandoned |
/// | @ref AreaPickSession  | first corner / centre fixed | second point sweeps it | released / abandoned |
///
/// Out-of-order phases are ignored rather than trusted: an `Updated` or `Ended`
/// with no `Started` before it changes nothing, and a session locks onto the
/// `pointer_id` that started it so a second hand cannot steer someone else's
/// goal. `Instant` (a tap: the whole gesture in one event) commits in one step.

/// ## What `action_point` is
///
/// The gesture's `action_point` is the **intersection QarOS itself computes**
/// between the wearer's aim and the scene of the app volume — and the ray it
/// casts is **slightly curved, not a straight line**.
///
/// Two consequences run through this whole header:
///
/// 1. @ref PickTargetKind::ActionPoint is not an approximation of anything. It
///    is the runtime's own answer, and publishing it verbatim is the only way
///    to reproduce what the AR user saw themselves select. It is not the
///    default: @ref PickTarget::kind starts as @ref PickTargetKind::GroundPlane.
/// 2. @ref PickTargetKind::GroundPlane, @ref PickTargetKind::Plane and
///    @ref PickTargetKind::Depth reconstruct a **straight** eye-through-hand ray
///    on our side and intersect that. Straight is not the curve QarOS casts, so
///    these are a **local approximation** and will differ from the runtime's
///    answer for the same aim, however precise the arithmetic. They are kept
///    because a plane target is a promise about *where the answer is* (a nav
///    goal must be on the floor) that the action point cannot make — not
///    because they are more accurate. `PointingRay::aim_to_reported_m` is
///    exactly the size of that disagreement; see `pointing_ray.hpp`.

#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/spatial_frames.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qar_ar_core
{

// ---------------------------------------------------------------------------
// What a pointer is resolved AGAINST
// ---------------------------------------------------------------------------

/// @brief The surface a pick is resolved against.
enum class PickTargetKind : std::uint8_t
{
  /// The ROS ground plane: normal +Z, at a chosen height. The default, and what
  /// a navigation goal means.
  GroundPlane = 0,
  /// An arbitrary plane (a wall, a table, a fitted surface).
  Plane,
  /// A fixed distance along the ray — the honest answer when there is no
  /// geometry to hit and the depth has to be supplied.
  Depth,
  /// **The runtime's own answer, taken verbatim.** The app-volume
  /// `action_point` is QarOS's computed intersection of the wearer's aim with the
  /// app volume's scene — see the file comment — so this
  /// target publishes it and does nothing else to it: no plane, no ray, no
  /// depth substitution. Not the default (that is @ref GroundPlane), but the only
  /// target that can be exactly right.
  ActionPoint
};
const char * to_string(PickTargetKind kind);

/// @brief A pick target. Plane fields are in the ROS world frame.
struct PickTarget
{
  PickTargetKind kind = PickTargetKind::GroundPlane;
  QarVector3 plane_point{0.0f, 0.0f, 0.0f};
  QarVector3 plane_normal{0.0f, 0.0f, 1.0f};
  float depth_m = 2.0f;
  /// Reported back with every pick so the operator can see WHICH target
  /// produced the point. Empty means "use @ref to_string(kind)".
  std::string name;

  static PickTarget ground(float height_m = 0.0f);
  static PickTarget plane(
    const QarVector3 & point, const QarVector3 & normal, std::string name = std::string());
  static PickTarget at_depth(float depth_m);
  static PickTarget action_point();

  /// The name reported to the operator (@ref name, or the kind's own name).
  std::string display_name() const;
};

// ---------------------------------------------------------------------------
// The pointer, brought into the ROS frame
// ---------------------------------------------------------------------------

/// @brief One @ref PanelPointerSample expressed in the ROS world frame.
struct SpatialPointer
{
  bool valid = false;  ///< Carried a room point or a room ray (not a panel (u, v)).
  std::uint32_t pointer_id = 0;
  PointerGesture gesture = PointerGesture::Hover;
  PointerPhase phase = PointerPhase::Updated;
  double timestamp_s = 0.0;

  bool has_ray = false;
  QarVector3 ray_origin{0.0f, 0.0f, 0.0f};
  QarVector3 ray_direction{0.0f, 0.0f, 0.0f};

  bool has_point = false;
  QarVector3 point{0.0f, 0.0f, 0.0f};
  /// @ref point is the runtime's own ray/scene intersection, already at the
  /// right depth -- see `PanelPointerSample::room_point_is_scene_hit`.
  bool point_is_scene_hit = false;
};

/// @brief Bring a raw sample across the room -> ROS frame change.
///
/// A sample carrying only a resolved panel (u, v) is not a spatial interaction
/// and comes back `valid == false`: that pointer is on a panel, and
/// qar_qt_streaming's `panel_hit_test.hpp` owns it.
SpatialPointer spatial_pointer_from_sample(
  const PanelPointerSample & sample, const RoomToRosTransform & transform);

// ---------------------------------------------------------------------------
// Point pick
// ---------------------------------------------------------------------------

/// @brief How a @ref SpatialPoint was arrived at. Reported so the operator can
/// see, in one word, whether the pick was aimed or merely positioned.
enum class PickMethod : std::uint8_t
{
  /// Nothing was resolved; @ref SpatialPoint::error says why.
  None = 0,
  /// OUR straight ray was intersected with the target plane. What "Ground plane
  /// (z = 0)" means, and the only method that can put the answer on the plane.
  /// A local approximation: QarOS aims along a curve, so this lands near, not
  /// on, the runtime's own answer for the same gesture.
  RayPlane,
  /// OUR straight ray, advanced `depth_m` along its direction. A local
  /// approximation for the same reason as @ref RayPlane.
  RayDepth,
  /// QarOS's own action point, taken at face value — the runtime's curved-ray
  /// intersection with the app volume's scene. The whole meaning of
  /// @ref PickTargetKind::ActionPoint, and what
  /// @ref PickTargetKind::Depth falls back to when no ray was reconstructed.
  ActionPoint,
  /// The reported point, projected onto the target plane because no ray was
  /// available. Degraded: the pick lands on the plane, which is what the target
  /// promises, but it lands directly below where the pointer was rather than
  /// where the wearer was aiming.
  ProjectedPoint
};
const char * to_string(PickMethod method);

/// @brief A resolved 3D point, and what produced it.
struct SpatialPoint
{
  bool valid = false;
  QarVector3 position{0.0f, 0.0f, 0.0f};  ///< ROS world frame, metres.
  PickTargetKind target = PickTargetKind::GroundPlane;
  std::string target_name;
  /// How @ref position was arrived at. The single field a log should print.
  PickMethod method = PickMethod::None;
  bool from_ray = false;    ///< A ray produced it (else the reported point did).
  /// PROVENANCE, not geometry: the reported point was the runtime's own
  /// ray/scene intersection rather than a bare position. It tells a plane
  /// target's log line that the projected point came from a real aim, and it
  /// tells @ref PickTargetKind::ActionPoint that its answer is a genuine hit.
  /// It does NOT exempt a plane target from landing on its plane -- see
  /// @ref resolve_pick_point.
  bool from_scene_hit = false;
  /// Travel along the ray, when @ref from_ray. For a projected point, the
  /// signed distance it sat off the plane before being projected -- which is
  /// exactly how far the answer would have been wrong had it not been.
  float distance_m = 0.0f;
  /// Why it failed, for the operator: "ray is parallel to the target plane",
  /// "target plane is behind the pointer", ...
  std::string error;
  /// The ONE refusal that means "the operator aimed at empty space": an
  /// @ref PickTargetKind::ActionPoint pick whose reported point was not a scene
  /// hit. Per the header of the drop after the minimum drop the action point IS the
  /// start point -- the hand -- when the runtime intersected nothing, so publishing
  /// it would put the marker on the operator's own hand. A flag rather than a string
  /// match so the controller can count it on its own (qar_rviz_plugin's
  /// `SpatialInputStats::refused_no_intersection`).
  bool refused_no_intersection = false;
};

/// @brief Resolve one pointer against one target. The core of everything below.
///
/// Ray input: intersected with the plane (@ref PickTargetKind::GroundPlane /
/// @ref PickTargetKind::Plane), or advanced `depth_m` along the normalized
/// direction (@ref PickTargetKind::Depth). Point input: projected orthogonally
/// onto the plane, or taken as-is for @ref PickTargetKind::Depth and
/// @ref PickTargetKind::ActionPoint — the action point already carries the
/// runtime's own depth, and overriding it with a guess would move the pick away
/// from what the wearer was actually pointing at.
///
/// A pointer carrying BOTH a ray and a point prefers the ray — EXCEPT for
/// @ref PickTargetKind::ActionPoint, where the point always wins. Our ray is a
/// straight reconstruction; QarOS's is curved and already intersected. For a
/// plane target our ray is the only thing that can land on the plane, so it is
/// used; for the action-point target it would only replace the authoritative
/// answer with an approximation of it.
///
/// A pointer with no action point does NOT fall through to the origin under
/// @ref PickTargetKind::ActionPoint: it comes back `valid == false` with the
/// reason in @ref SpatialPoint::error. The SDK zeroes an absent point rather
/// than leaving it stale, so publishing "the point we were given" would publish
/// (0, 0, 0) — a plausible-looking lie exactly where the map origin is.
///
/// ## A plane target always lands ON its plane
///
/// A plane target never publishes a scene hit **where it landed**. "Ground
/// plane (z = 0)" is a promise about WHERE THE ANSWER IS, and a scene hit
/// metres above the floor does not keep it. Re-resolving the runtime's
/// intersection against a plane and taking the runtime's intersection as it is
/// are two different operations; they are separate targets, and each does
/// exactly one thing:
///
/// | Target | With a ray | Without a ray |
/// |---|---|---|
/// | `GroundPlane` / `Plane` | @ref PickMethod::RayPlane — our straight ray's intersection, an approximation | @ref PickMethod::ProjectedPoint — on the plane, below the pointer |
/// | `Depth` | @ref PickMethod::RayDepth — N m along our straight ray, an approximation | @ref PickMethod::ActionPoint |
/// | `ActionPoint` | the point still wins — QarOS already intersected the real (curved) aim | @ref PickMethod::ActionPoint — the thing being pointed at |
///
/// So "pick the thing I am pointing at" is
/// @ref PickTargetKind::ActionPoint, which is what a scene hit is *for*, and
/// which takes the runtime's intersection at face value. A plane target never
/// turns into that.
///
/// The default target is @ref PickTargetKind::GroundPlane (@ref PickTarget::kind, and
/// `PickTarget::ground()` in the sessions). @ref PickTargetKind::ActionPoint, which a
/// host selects with `PickTarget::action_point()`, is the target for when the answer
/// must be the one the runtime computed and the wearer saw (see the file comment). The
/// plane and depth targets are the variants for when the answer must lie somewhere
/// specific.
///
/// `SpatialPointer::point_is_scene_hit` is PROVENANCE for every target but one
/// (@ref SpatialPoint::from_scene_hit): it tells the log that a projected point
/// came from a real aim rather than from a mouse, and it never changes where a
/// plane target puts the answer, nor what @ref PickTargetKind::Depth publishes.
///
/// **The one exception - @ref PickTargetKind::ActionPoint GATES on it.** The header
/// of the drop after the minimum drop defines the action point as "the intersection
/// the gesture acts on, or the start point when nothing was intersected", and the
/// start point is the hand. So an action point that is NOT a scene hit is the
/// operator's own hand, and publishing it verbatim would put the marker on the
/// hand. Such a pick comes back `valid == false`,
/// @ref SpatialPoint::refused_no_intersection set, and an error that says so:
/// a click on empty space publishes nothing.
///
/// Degenerate input is reported, never silently rounded: a zero-length or
/// non-finite direction, a ray parallel to the plane, a plane behind the
/// pointer, a zero-length plane normal, and a ray-only target with no ray.
SpatialPoint resolve_pick_point(const PickTarget & target, const SpatialPointer & pointer);

// ---------------------------------------------------------------------------
// Session lifecycle, shared by all three pick kinds
// ---------------------------------------------------------------------------

enum class PickState : std::uint8_t
{
  Idle = 0,   ///< Nothing in progress.
  Pending,    ///< A press landed; the first quantity is fixed and previewing.
  Orienting,  ///< The drag has travelled far enough to mean something.
  Committed,  ///< Released; `result()` is valid.
  Canceled    ///< Abandoned; nothing was produced.
};
const char * to_string(PickState state);

/// @brief Preview a point under the pointer, commit it on release.
///
/// Hover phases never commit, which is what makes a hovering ray safe to draw
/// in the streamed scene while the wearer is still deciding.
class PointPickSession
{
public:
  explicit PointPickSession(PickTarget target = PickTarget::ground());

  const PickTarget & target() const { return target_; }
  void set_target(PickTarget target);

  PickState state() const { return state_; }

  /// @brief Feed one pointer. @return true when the session changed.
  bool update(const SpatialPointer & pointer);

  /// The point under the pointer right now (also valid while Committed).
  const SpatialPoint & preview() const { return preview_; }
  /// The committed point; `valid == false` until @ref PickState::Committed.
  const SpatialPoint & result() const { return result_; }

  void cancel();
  void reset();

private:
  PickTarget target_;
  PickState state_ = PickState::Idle;
  bool has_owner_ = false;
  std::uint32_t owner_ = 0;
  SpatialPoint preview_{};
  SpatialPoint result_{};
};

// ---------------------------------------------------------------------------
// Pose pick (position + orientation)
// ---------------------------------------------------------------------------

struct PosePickConfig
{
  /// A drag shorter than this does not establish a heading — a tap is a tap,
  /// and hand tracking jitters by centimetres.
  float min_drag_m = 0.05f;
  /// Commit a heading-less pick with @ref default_yaw_rad rather than refusing
  /// it. A tap then means "here, facing the default direction".
  bool commit_without_heading = true;
  /// Yaw about ROS +Z used when the drag was too short.
  float default_yaw_rad = 0.0f;
};

struct PosePickResult
{
  bool valid = false;
  QarVector3 position{0.0f, 0.0f, 0.0f};
  /// xyzw, like every `QarQuaternion`. Built so local +X points along the drag
  /// and local +Z along the target plane's normal — which, on the ground plane,
  /// is exactly the yaw-only quaternion a nav stack expects.
  QarQuaternion orientation{0.0f, 0.0f, 0.0f, 1.0f};
  float yaw_rad = 0.0f;            ///< ROS yaw of the heading (atan2 of its X/Y).
  bool heading_from_drag = false;  ///< false: @ref PosePickConfig::default_yaw_rad.
  float drag_length_m = 0.0f;
  PickTargetKind target = PickTargetKind::GroundPlane;
  std::string target_name;
};

/// @brief The two-stage AR gesture: press fixes the position, drag sets the
/// heading from the direction of travel, release commits.
class PosePickSession
{
public:
  explicit PosePickSession(
    PickTarget target = PickTarget::ground(), PosePickConfig config = PosePickConfig());

  const PickTarget & target() const { return target_; }
  void set_target(PickTarget target);
  const PosePickConfig & config() const { return config_; }
  void set_config(PosePickConfig config);

  PickState state() const { return state_; }

  bool update(const SpatialPointer & pointer);

  /// The position fixed by the press; valid from @ref PickState::Pending on.
  const SpatialPoint & anchor() const { return anchor_; }
  /// The live pose while dragging — what the scene should draw.
  const PosePickResult & preview() const { return preview_; }
  const PosePickResult & result() const { return result_; }

  void cancel();
  void reset();

private:
  void update_heading(const SpatialPoint & tip);

  PickTarget target_;
  PosePickConfig config_;
  PickState state_ = PickState::Idle;
  bool has_owner_ = false;
  std::uint32_t owner_ = 0;
  SpatialPoint anchor_{};
  PosePickResult preview_{};
  PosePickResult result_{};
};

// ---------------------------------------------------------------------------
// Area / region pick
// ---------------------------------------------------------------------------

enum class AreaShape : std::uint8_t
{
  Rectangle = 0,  ///< Two opposite corners, in the target plane's own basis.
  Circle,         ///< Centre + a rim point.
  Polygon         ///< Vertex by vertex, closed explicitly.
};
const char * to_string(AreaShape shape);

/// @brief An orthonormal basis of a plane, deterministic for a given normal.
///
/// For the ROS ground plane (normal +Z) it comes out as exactly (+X, +Y), so a
/// rectangle picked on the ground is axis-aligned in the world the operator
/// reads off RViz — which is why the helper axis is chosen the way it is.
/// `out_u × out_v` is the normalized normal, so the basis is right-handed.
/// @return false for a zero-length or non-finite normal.
bool plane_basis(const QarVector3 & normal, QarVector3 & out_u, QarVector3 & out_v);

struct AreaPickResult
{
  bool valid = false;
  AreaShape shape = AreaShape::Rectangle;
  QarVector3 first{0.0f, 0.0f, 0.0f};   ///< Corner / centre / first vertex.
  QarVector3 second{0.0f, 0.0f, 0.0f};  ///< Opposite corner / rim point.
  /// Rectangle: 4 corners, counter-clockwise about the plane normal, starting
  /// at @ref first. Circle: empty (a circle is not a polygon). Polygon: as given.
  std::vector<QarVector3> vertices;
  QarVector3 center{0.0f, 0.0f, 0.0f};
  float radius_m = 0.0f;  ///< Circle only.
  float width_m = 0.0f;   ///< Rectangle: extent along the plane basis' u.
  float height_m = 0.0f;  ///< Rectangle: extent along v.
  float area_m2 = 0.0f;   ///< Rectangle w*h, circle pi*r^2, polygon shoelace.
  PickTargetKind target = PickTargetKind::GroundPlane;
  std::string target_name;
  std::string error;
};

AreaPickResult make_rectangle(
  const PickTarget & target, const QarVector3 & corner, const QarVector3 & opposite);
AreaPickResult make_circle(
  const PickTarget & target, const QarVector3 & center, const QarVector3 & rim);
AreaPickResult make_polygon(const PickTarget & target, const std::vector<QarVector3> & vertices);

/// @brief Sweep a region: press fixes the first point, drag sweeps the second,
/// release commits. A polygon instead collects one vertex per tap and is closed
/// with @ref close_polygon.
class AreaPickSession
{
public:
  explicit AreaPickSession(
    PickTarget target = PickTarget::ground(), AreaShape shape = AreaShape::Rectangle);

  const PickTarget & target() const { return target_; }
  void set_target(PickTarget target);
  AreaShape shape() const { return shape_; }
  void set_shape(AreaShape shape);  ///< Resets any gesture in progress.

  PickState state() const { return state_; }

  bool update(const SpatialPointer & pointer);

  /// Polygon mode: commit the vertices collected so far (3 or more).
  bool close_polygon();
  const std::vector<QarVector3> & polygon_vertices() const { return polygon_; }

  const AreaPickResult & preview() const { return preview_; }
  const AreaPickResult & result() const { return result_; }

  void cancel();
  void reset();

private:
  PickTarget target_;
  AreaShape shape_ = AreaShape::Rectangle;
  PickState state_ = PickState::Idle;
  bool has_owner_ = false;
  std::uint32_t owner_ = 0;
  QarVector3 first_{0.0f, 0.0f, 0.0f};
  std::vector<QarVector3> polygon_;
  AreaPickResult preview_{};
  AreaPickResult result_{};
};

// ---------------------------------------------------------------------------
// Navigation goal
// ---------------------------------------------------------------------------

struct NavGoalOptions
{
  /// The frame the goal is published in. "map" is what nav2's `/goal_pose` and
  /// `/initialpose` are read in by default.
  std::string frame_id = "map";
  /// Rebuild the orientation as a pure yaw about ROS +Z. A ground-plane pick is
  /// already yaw-only; this also flattens a pose picked on a tilted plane,
  /// which a planar nav stack cannot act on.
  bool flatten_orientation = true;
  /// Drop the pick onto z = @ref ground_height_m. Planar planners ignore z, and
  /// a goal floating at hand height reads as a bug in every visualizer.
  bool project_to_ground = true;
  float ground_height_m = 0.0f;
};

/// @brief A pose pick in the shape a navigation stack expects.
struct NavGoal
{
  bool valid = false;
  std::string frame_id;
  QarVector3 position{0.0f, 0.0f, 0.0f};
  QarQuaternion orientation{0.0f, 0.0f, 0.0f, 1.0f};  ///< xyzw.
  float yaw_rad = 0.0f;
  bool heading_from_drag = false;
  std::string error;
};

NavGoal make_nav_goal(
  const PosePickResult & pose, const NavGoalOptions & options = NavGoalOptions());

/// @brief Yaw about ROS +Z as a `QarQuaternion` (xyzw). The conversion a nav
/// goal, an `initialpose` and a flattened pose all need.
QarQuaternion yaw_to_quaternion(float yaw_rad);

/// @brief ROS yaw extracted from a quaternion (xyzw), i.e. the heading of its
/// local +X axis projected onto the world XY plane.
float yaw_from_quaternion(const QarQuaternion & q);

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__SPATIAL_PICK_HPP_
