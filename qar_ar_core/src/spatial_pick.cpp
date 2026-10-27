// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ar_core/spatial_pick.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace qar_ar_core
{
namespace
{

constexpr float kEpsilon = 1e-6f;
constexpr float kPi = 3.14159265358979323846f;

bool finite(const QarVector3 & v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

QarVector3 add(const QarVector3 & a, const QarVector3 & b)
{
  return QarVector3{a.x + b.x, a.y + b.y, a.z + b.z};
}

QarVector3 subtract(const QarVector3 & a, const QarVector3 & b)
{
  return QarVector3{a.x - b.x, a.y - b.y, a.z - b.z};
}

QarVector3 scale(const QarVector3 & v, float s)
{
  return QarVector3{v.x * s, v.y * s, v.z * s};
}

float dot(const QarVector3 & a, const QarVector3 & b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

QarVector3 cross(const QarVector3 & a, const QarVector3 & b)
{
  return QarVector3{
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x};
}

float length(const QarVector3 & v)
{
  return std::sqrt(dot(v, v));
}

bool normalize(const QarVector3 & v, QarVector3 & out)
{
  if (!finite(v)) {
    return false;
  }
  const float len = length(v);
  if (!(len > kEpsilon)) {
    return false;
  }
  out = scale(v, 1.0f / len);
  return true;
}

/// The plane a pick is oriented against. Depth / ActionPoint targets have no
/// plane of their own, so headings and regions there are measured against the
/// world's horizontal plane — the only up an operator means by "heading".
QarVector3 reference_normal(const PickTarget & target)
{
  QarVector3 n{0.0f, 0.0f, 1.0f};
  if (target.kind == PickTargetKind::Plane || target.kind == PickTargetKind::GroundPlane) {
    QarVector3 normalized{0.0f, 0.0f, 1.0f};
    if (normalize(target.plane_normal, normalized)) {
      n = normalized;
    }
  }
  return n;
}

/// Rotation matrix (columns = the local axes, expressed in world) -> xyzw quaternion.
QarQuaternion quaternion_from_axes(
  const QarVector3 & x_axis, const QarVector3 & y_axis, const QarVector3 & z_axis)
{
  const float m00 = x_axis.x, m01 = y_axis.x, m02 = z_axis.x;
  const float m10 = x_axis.y, m11 = y_axis.y, m12 = z_axis.y;
  const float m20 = x_axis.z, m21 = y_axis.z, m22 = z_axis.z;
  const float trace = m00 + m11 + m22;
  float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    w = 0.25f * s;
    x = (m21 - m12) / s;
    y = (m02 - m20) / s;
    z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    w = (m21 - m12) / s;
    x = 0.25f * s;
    y = (m01 + m10) / s;
    z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    w = (m02 - m20) / s;
    x = (m01 + m10) / s;
    y = 0.25f * s;
    z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    w = (m10 - m01) / s;
    x = (m02 + m20) / s;
    y = (m12 + m21) / s;
    z = 0.25f * s;
  }
  return QarQuaternion{x, y, z, w};
}

}  // namespace

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

const char * to_string(PickTargetKind kind)
{
  switch (kind) {
    case PickTargetKind::GroundPlane: return "ground plane";
    case PickTargetKind::Plane: return "plane";
    case PickTargetKind::Depth: return "fixed depth";
    case PickTargetKind::ActionPoint: return "QarOS action point";
  }
  return "unknown";
}

const char * to_string(PickMethod method)
{
  switch (method) {
    case PickMethod::None: return "none";
    case PickMethod::RayPlane: return "ray/plane";
    case PickMethod::RayDepth: return "ray/depth";
    case PickMethod::ActionPoint: return "QarOS action point";
    case PickMethod::ProjectedPoint: return "projected";
  }
  return "unknown";
}

PickTarget PickTarget::ground(float height_m)
{
  PickTarget target;
  target.kind = PickTargetKind::GroundPlane;
  target.plane_point = QarVector3{0.0f, 0.0f, height_m};
  target.plane_normal = QarVector3{0.0f, 0.0f, 1.0f};
  return target;
}

PickTarget PickTarget::plane(
  const QarVector3 & point, const QarVector3 & normal, std::string name)
{
  PickTarget target;
  target.kind = PickTargetKind::Plane;
  target.plane_point = point;
  target.plane_normal = normal;
  target.name = std::move(name);
  return target;
}

PickTarget PickTarget::at_depth(float depth_m)
{
  PickTarget target;
  target.kind = PickTargetKind::Depth;
  target.depth_m = depth_m;
  return target;
}

PickTarget PickTarget::action_point()
{
  PickTarget target;
  target.kind = PickTargetKind::ActionPoint;
  return target;
}

std::string PickTarget::display_name() const
{
  return name.empty() ? std::string(to_string(kind)) : name;
}

// ---------------------------------------------------------------------------
// Pointer conversion
// ---------------------------------------------------------------------------

SpatialPointer spatial_pointer_from_sample(
  const PanelPointerSample & sample, const RoomToRosTransform & transform)
{
  SpatialPointer out;
  out.pointer_id = sample.pointer_id;
  out.gesture = sample.gesture;
  out.phase = sample.phase;
  out.timestamp_s = sample.timestamp_s;

  if (sample.has_room_ray) {
    out.has_ray = true;
    out.ray_origin = room_point_to_ros(transform, sample.ray_origin);
    out.ray_direction = room_direction_to_ros(transform, sample.ray_direction);
  }
  if (sample.has_room_point) {
    out.has_point = true;
    out.point = room_point_to_ros(transform, sample.room_point);
    out.point_is_scene_hit = sample.room_point_is_scene_hit;
  }
  // A sample that only carries a resolved panel (u, v) belongs to the panel
  // path, not here; qar_qt_streaming's panel_hit_test.hpp owns that geometry.
  out.valid = out.has_ray || out.has_point;
  return out;
}

// ---------------------------------------------------------------------------
// Point resolution
// ---------------------------------------------------------------------------

SpatialPoint resolve_pick_point(const PickTarget & target, const SpatialPointer & pointer)
{
  SpatialPoint out;
  out.target = target.kind;
  out.target_name = target.display_name();

  if (!pointer.valid || (!pointer.has_ray && !pointer.has_point)) {
    out.error = "pointer carries neither a room point nor a room ray";
    return out;
  }

  // A reported action point already knows its own depth; a ray does not. Prefer
  // the ray when both are present (a ray is aimed, a point is merely where the
  // hand was) EXCEPT for ActionPoint, whose whole meaning is the point.
  const bool use_ray =
    pointer.has_ray && !(target.kind == PickTargetKind::ActionPoint && pointer.has_point);

  if (use_ray) {
    QarVector3 direction{0.0f, 0.0f, 0.0f};
    if (!finite(pointer.ray_origin) || !normalize(pointer.ray_direction, direction)) {
      out.error = "ray origin or direction is degenerate";
      return out;
    }
    out.from_ray = true;
    switch (target.kind) {
      case PickTargetKind::Depth: {
        if (!(target.depth_m > 0.0f) || !std::isfinite(target.depth_m)) {
          out.error = "pick depth must be a positive number of metres";
          return out;
        }
        out.position = add(pointer.ray_origin, scale(direction, target.depth_m));
        out.distance_m = target.depth_m;
        out.method = PickMethod::RayDepth;
        out.valid = true;
        return out;
      }
      case PickTargetKind::ActionPoint: {
        out.error =
          "the QarOS action-point target needs the runtime's own action point, and this "
          "sample carried only a ray";
        return out;
      }
      case PickTargetKind::GroundPlane:
      case PickTargetKind::Plane: {
        QarVector3 normal{0.0f, 0.0f, 1.0f};
        if (!normalize(target.plane_normal, normal)) {
          out.error = "target plane normal is degenerate";
          return out;
        }
        const float denominator = dot(normal, direction);
        if (std::fabs(denominator) < kEpsilon) {
          out.error = "ray is parallel to the target plane";
          return out;
        }
        const float t =
          dot(normal, subtract(target.plane_point, pointer.ray_origin)) / denominator;
        if (!std::isfinite(t) || t < 0.0f) {
          out.error = "target plane is behind the pointer";
          return out;
        }
        out.position = add(pointer.ray_origin, scale(direction, t));
        out.distance_m = t;
        out.method = PickMethod::RayPlane;
        out.valid = true;
        return out;
      }
    }
    out.error = "unknown pick target";
    return out;
  }

  if (!finite(pointer.point)) {
    out.error = "the reported action point is not finite";
    return out;
  }
  switch (target.kind) {
    case PickTargetKind::Depth: {
      // The depth is already in the sample. Imposing depth_m on it would move
      // the pick away from where the wearer's hand actually was. A no-ray Depth
      // pick publishes the reported point, scene hit or not; the gate of the
      // ActionPoint target below does not apply to it.
      out.position = pointer.point;
      out.from_scene_hit = pointer.point_is_scene_hit;
      out.method = PickMethod::ActionPoint;
      out.valid = true;
      return out;
    }
    case PickTargetKind::ActionPoint: {
      // This is the target that MEANS "the thing I am pointing at", so a scene
      // hit is exactly right here and is taken at face value -- and ONLY a scene
      // hit. The installed runtime's header defines the action point as "the
      // start point when nothing was intersected", i.e. the operator's HAND, and
      // publishing that verbatim would put the marker on the hand. A click on
      // empty space therefore publishes nothing.
      if (!pointer.point_is_scene_hit) {
        out.refused_no_intersection = true;
        out.error =
          "the runtime intersected nothing (action point == start point, i.e. the hand) - "
          "nothing to pick";
        return out;
      }
      out.position = pointer.point;
      out.from_scene_hit = true;
      out.method = PickMethod::ActionPoint;
      out.valid = true;
      return out;
    }
    case PickTargetKind::GroundPlane:
    case PickTargetKind::Plane: {
      QarVector3 normal{0.0f, 0.0f, 1.0f};
      if (!normalize(target.plane_normal, normal)) {
        out.error = "target plane normal is degenerate";
        return out;
      }
      // No ray, so the honest answer is the orthogonal projection. A plane
      // target is a promise about WHERE the answer is; publishing a point
      // metres off the plane because the runtime happened to resolve one there
      // breaks that promise. The scene-hit flag is carried as provenance so the
      // log can say the projected point came from a real aim, and distance_m
      // says how far off the plane it was -- i.e. by how much an unprojected
      // answer would have been wrong.
      const float offset = dot(normal, subtract(pointer.point, target.plane_point));
      out.position = subtract(pointer.point, scale(normal, offset));
      out.from_scene_hit = pointer.point_is_scene_hit;
      out.distance_m = offset;  // signed distance the point sat off the plane
      out.method = PickMethod::ProjectedPoint;
      out.valid = true;
      return out;
    }
  }
  out.error = "unknown pick target";
  return out;
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

namespace
{
/// Did the preview actually move? The app-volume runtime repeats an unchanged
/// hover at full rate (runs of byte-identical samples), and reporting each
/// repeat as a change would make "the session advanced" meaningless.
bool preview_moved(const SpatialPoint & before, const SpatialPoint & after)
{
  if (before.valid != after.valid) {
    return true;
  }
  if (!after.valid) {
    return false;
  }
  const QarVector3 d = subtract(after.position, before.position);
  return length(d) > 1e-4f;
}
}  // namespace

const char * to_string(PickState state)
{
  switch (state) {
    case PickState::Idle: return "idle";
    case PickState::Pending: return "pending";
    case PickState::Orienting: return "orienting";
    case PickState::Committed: return "committed";
    case PickState::Canceled: return "canceled";
  }
  return "unknown";
}

// --- point ------------------------------------------------------------------

PointPickSession::PointPickSession(PickTarget target)
: target_(std::move(target))
{
}

void PointPickSession::set_target(PickTarget target)
{
  target_ = std::move(target);
  reset();
}

bool PointPickSession::update(const SpatialPointer & pointer)
{
  if (!pointer.valid) {
    return false;
  }
  const bool in_flight = state_ == PickState::Pending;
  if (in_flight && has_owner_ && pointer.pointer_id != owner_) {
    return false;  // a second hand may not steer someone else's pick
  }
  const SpatialPoint resolved = resolve_pick_point(target_, pointer);

  // A hover is a hand passing through the air, not a decision: it may move the
  // preview -- which is what makes a hover ray safe to draw while the wearer is
  // still deciding -- and it may never arm or commit, whatever PHASE it arrives
  // with. The gate is on the GESTURE and not the phase because the app-volume
  // runtime emits hover as QAR_GESTURE_PHASE_INSTANT, the same phase a tap
  // uses; dispatching on the phase alone published a pick for every hover.
  if (!gesture_commits_pick(pointer.gesture)) {
    if (in_flight || !resolved.valid) {
      return false;  // a hover must not steer, or end, a click in flight
    }
    const bool moved = preview_moved(preview_, resolved);
    preview_ = resolved;
    return moved;
  }

  switch (pointer.phase) {
    case PointerPhase::Started: {
      if (!resolved.valid) {
        return false;  // a press that resolves to nothing arms nothing
      }
      owner_ = pointer.pointer_id;
      has_owner_ = true;
      preview_ = resolved;
      result_ = SpatialPoint{};
      state_ = PickState::Pending;
      return true;
    }
    case PointerPhase::Updated: {
      if (!in_flight) {
        return false;  // out of order: an Updated click with no Started
      }
      preview_ = resolved;
      return true;
    }
    case PointerPhase::Ended: {
      if (!in_flight) {
        return false;
      }
      if (resolved.valid) {
        preview_ = resolved;
      }
      has_owner_ = false;
      if (preview_.valid) {
        result_ = preview_;
        state_ = PickState::Committed;
      } else {
        result_ = SpatialPoint{};
        state_ = PickState::Canceled;
      }
      return true;
    }
    case PointerPhase::Instant: {
      if (!resolved.valid) {
        return false;
      }
      has_owner_ = false;
      preview_ = resolved;
      result_ = resolved;
      state_ = PickState::Committed;
      return true;
    }
    case PointerPhase::Canceled: {
      if (!in_flight) {
        return false;
      }
      cancel();
      return true;
    }
  }
  return false;
}

void PointPickSession::cancel()
{
  has_owner_ = false;
  result_ = SpatialPoint{};
  state_ = PickState::Canceled;
}

void PointPickSession::reset()
{
  has_owner_ = false;
  owner_ = 0;
  preview_ = SpatialPoint{};
  result_ = SpatialPoint{};
  state_ = PickState::Idle;
}

// --- pose -------------------------------------------------------------------

PosePickSession::PosePickSession(PickTarget target, PosePickConfig config)
: target_(std::move(target)), config_(config)
{
}

void PosePickSession::set_target(PickTarget target)
{
  target_ = std::move(target);
  reset();
}

void PosePickSession::set_config(PosePickConfig config)
{
  config_ = config;
}

void PosePickSession::update_heading(const SpatialPoint & tip)
{
  const QarVector3 normal = reference_normal(target_);
  QarVector3 delta = subtract(tip.position, anchor_.position);
  // Only travel WITHIN the plane turns into a heading: a hand that drifts along
  // the normal has not changed where the pose faces.
  delta = subtract(delta, scale(normal, dot(normal, delta)));
  const float travel = length(delta);
  preview_.drag_length_m = travel;

  QarVector3 heading{0.0f, 0.0f, 0.0f};
  if (travel >= config_.min_drag_m && normalize(delta, heading)) {
    const QarVector3 y_axis = cross(normal, heading);
    preview_.orientation = quaternion_from_axes(heading, y_axis, normal);
    preview_.yaw_rad = std::atan2(heading.y, heading.x);
    preview_.heading_from_drag = true;
    state_ = PickState::Orienting;
  } else {
    preview_.orientation = yaw_to_quaternion(config_.default_yaw_rad);
    preview_.yaw_rad = config_.default_yaw_rad;
    preview_.heading_from_drag = false;
    state_ = PickState::Pending;
  }
}

bool PosePickSession::update(const SpatialPointer & pointer)
{
  if (!pointer.valid) {
    return false;
  }
  const bool in_flight = state_ == PickState::Pending || state_ == PickState::Orienting;
  if (in_flight && has_owner_ && pointer.pointer_id != owner_) {
    return false;
  }
  const SpatialPoint resolved = resolve_pick_point(target_, pointer);

  // Hover previews a position and nothing else: there is no heading to preview
  // until a press anchors one. It never arms and never commits -- see
  // PointPickSession::update for why the gate is on the gesture, not the phase.
  if (!gesture_commits_pick(pointer.gesture)) {
    if (in_flight || !resolved.valid) {
      return false;
    }
    const bool moved = !preview_.valid ||
      length(subtract(resolved.position, preview_.position)) > 1e-4f;
    preview_ = PosePickResult{};
    preview_.valid = true;
    preview_.position = resolved.position;
    preview_.target = resolved.target;
    preview_.target_name = resolved.target_name;
    preview_.orientation = yaw_to_quaternion(config_.default_yaw_rad);
    preview_.yaw_rad = config_.default_yaw_rad;
    return moved;
  }

  const auto begin = [&]() {
      owner_ = pointer.pointer_id;
      has_owner_ = true;
      anchor_ = resolved;
      result_ = PosePickResult{};
      preview_ = PosePickResult{};
      preview_.valid = true;
      preview_.position = resolved.position;
      preview_.target = resolved.target;
      preview_.target_name = resolved.target_name;
      preview_.orientation = yaw_to_quaternion(config_.default_yaw_rad);
      preview_.yaw_rad = config_.default_yaw_rad;
      state_ = PickState::Pending;
    };

  switch (pointer.phase) {
    case PointerPhase::Started: {
      if (!resolved.valid) {
        return false;
      }
      begin();
      return true;
    }
    case PointerPhase::Updated: {
      if (!in_flight || !resolved.valid) {
        return false;
      }
      update_heading(resolved);
      return true;
    }
    case PointerPhase::Ended: {
      if (!in_flight) {
        return false;
      }
      if (resolved.valid) {
        update_heading(resolved);
      }
      has_owner_ = false;
      if (state_ == PickState::Orienting || config_.commit_without_heading) {
        result_ = preview_;
        state_ = PickState::Committed;
      } else {
        result_ = PosePickResult{};
        state_ = PickState::Canceled;
      }
      return true;
    }
    case PointerPhase::Instant: {
      // A tap has no drag, so it can only mean the default heading.
      if (!resolved.valid || !config_.commit_without_heading) {
        return false;
      }
      begin();
      has_owner_ = false;
      result_ = preview_;
      state_ = PickState::Committed;
      return true;
    }
    case PointerPhase::Canceled: {
      if (!in_flight) {
        return false;
      }
      cancel();
      return true;
    }
  }
  return false;
}

void PosePickSession::cancel()
{
  has_owner_ = false;
  result_ = PosePickResult{};
  state_ = PickState::Canceled;
}

void PosePickSession::reset()
{
  has_owner_ = false;
  owner_ = 0;
  anchor_ = SpatialPoint{};
  preview_ = PosePickResult{};
  result_ = PosePickResult{};
  state_ = PickState::Idle;
}

// ---------------------------------------------------------------------------
// Areas
// ---------------------------------------------------------------------------

const char * to_string(AreaShape shape)
{
  switch (shape) {
    case AreaShape::Rectangle: return "rectangle";
    case AreaShape::Circle: return "circle";
    case AreaShape::Polygon: return "polygon";
  }
  return "unknown";
}

bool plane_basis(const QarVector3 & normal, QarVector3 & out_u, QarVector3 & out_v)
{
  QarVector3 n{0.0f, 0.0f, 1.0f};
  if (!normalize(normal, n)) {
    return false;
  }
  // The helper axis is +Y for a near-vertical normal so that the ground plane
  // (+Z) yields exactly (+X, +Y) — the basis an operator reads off RViz.
  const QarVector3 helper =
    std::fabs(n.z) >= 0.9f ? QarVector3{0.0f, 1.0f, 0.0f} : QarVector3{0.0f, 0.0f, 1.0f};
  QarVector3 u{1.0f, 0.0f, 0.0f};
  if (!normalize(cross(helper, n), u)) {
    if (!normalize(cross(QarVector3{1.0f, 0.0f, 0.0f}, n), u)) {
      return false;
    }
  }
  out_u = u;
  out_v = cross(n, u);
  return true;
}

namespace
{

void stamp_target(AreaPickResult & result, const PickTarget & target)
{
  result.target = target.kind;
  result.target_name = target.display_name();
}

}  // namespace

AreaPickResult make_rectangle(
  const PickTarget & target, const QarVector3 & corner, const QarVector3 & opposite)
{
  AreaPickResult out;
  out.shape = AreaShape::Rectangle;
  stamp_target(out, target);
  out.first = corner;
  out.second = opposite;
  if (!finite(corner) || !finite(opposite)) {
    out.error = "rectangle corner is not finite";
    return out;
  }
  const QarVector3 normal = reference_normal(target);
  QarVector3 u{1.0f, 0.0f, 0.0f};
  QarVector3 v{0.0f, 1.0f, 0.0f};
  if (!plane_basis(normal, u, v)) {
    out.error = "target plane normal is degenerate";
    return out;
  }
  QarVector3 delta = subtract(opposite, corner);
  delta = subtract(delta, scale(normal, dot(normal, delta)));
  const float a = dot(delta, u);
  const float b = dot(delta, v);
  if (std::fabs(a) < kEpsilon || std::fabs(b) < kEpsilon) {
    out.error = "rectangle has zero extent in one direction";
    return out;
  }
  const QarVector3 along_u = scale(u, a);
  const QarVector3 along_v = scale(v, b);
  const QarVector3 c0 = corner;
  const QarVector3 c1 = add(corner, along_u);
  const QarVector3 c2 = add(add(corner, along_u), along_v);
  const QarVector3 c3 = add(corner, along_v);
  // Counter-clockwise about the plane normal whichever quadrant was swept.
  if (a * b < 0.0f) {
    out.vertices = {c0, c3, c2, c1};
  } else {
    out.vertices = {c0, c1, c2, c3};
  }
  out.center = add(corner, scale(add(along_u, along_v), 0.5f));
  out.width_m = std::fabs(a);
  out.height_m = std::fabs(b);
  out.area_m2 = out.width_m * out.height_m;
  out.valid = true;
  return out;
}

AreaPickResult make_circle(
  const PickTarget & target, const QarVector3 & center, const QarVector3 & rim)
{
  AreaPickResult out;
  out.shape = AreaShape::Circle;
  stamp_target(out, target);
  out.first = center;
  out.second = rim;
  if (!finite(center) || !finite(rim)) {
    out.error = "circle centre or rim point is not finite";
    return out;
  }
  const QarVector3 normal = reference_normal(target);
  QarVector3 delta = subtract(rim, center);
  delta = subtract(delta, scale(normal, dot(normal, delta)));
  const float radius = length(delta);
  if (!(radius > kEpsilon)) {
    out.error = "circle has zero radius";
    return out;
  }
  out.center = center;
  out.radius_m = radius;
  out.width_m = 2.0f * radius;
  out.height_m = 2.0f * radius;
  out.area_m2 = kPi * radius * radius;
  out.valid = true;
  return out;
}

AreaPickResult make_polygon(const PickTarget & target, const std::vector<QarVector3> & vertices)
{
  AreaPickResult out;
  out.shape = AreaShape::Polygon;
  stamp_target(out, target);
  if (vertices.size() < 3u) {
    out.error = "a polygon needs at least three vertices";
    return out;
  }
  for (const auto & vertex : vertices) {
    if (!finite(vertex)) {
      out.error = "polygon vertex is not finite";
      return out;
    }
  }
  const QarVector3 normal = reference_normal(target);
  QarVector3 u{1.0f, 0.0f, 0.0f};
  QarVector3 v{0.0f, 1.0f, 0.0f};
  if (!plane_basis(normal, u, v)) {
    out.error = "target plane normal is degenerate";
    return out;
  }
  out.vertices = vertices;
  out.first = vertices.front();
  out.second = vertices.back();

  QarVector3 centroid{0.0f, 0.0f, 0.0f};
  for (const auto & vertex : vertices) {
    centroid = add(centroid, vertex);
  }
  out.center = scale(centroid, 1.0f / static_cast<float>(vertices.size()));

  // Shoelace in the plane's own 2D basis, so a polygon on a wall measures the
  // same as one on the floor. Signed area, reported unsigned: the operator
  // asked for a region, not a winding order.
  double twice_area = 0.0;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    const QarVector3 p = subtract(vertices[i], out.center);
    const QarVector3 q = subtract(vertices[(i + 1) % vertices.size()], out.center);
    twice_area += static_cast<double>(dot(p, u)) * dot(q, v) -
      static_cast<double>(dot(q, u)) * dot(p, v);
  }
  out.area_m2 = static_cast<float>(std::fabs(twice_area) * 0.5);
  out.valid = true;
  return out;
}

AreaPickSession::AreaPickSession(PickTarget target, AreaShape shape)
: target_(std::move(target)), shape_(shape)
{
}

void AreaPickSession::set_target(PickTarget target)
{
  target_ = std::move(target);
  reset();
}

void AreaPickSession::set_shape(AreaShape shape)
{
  shape_ = shape;
  reset();
}

bool AreaPickSession::update(const SpatialPointer & pointer)
{
  if (!pointer.valid) {
    return false;
  }
  const bool in_flight = state_ == PickState::Pending || state_ == PickState::Orienting;
  if (in_flight && has_owner_ && pointer.pointer_id != owner_) {
    return false;
  }
  const SpatialPoint resolved = resolve_pick_point(target_, pointer);

  // A region is built from deliberate corners. A hover has nothing to preview
  // before the first one is placed and must never place one -- see
  // PointPickSession::update for why the gate is on the gesture, not the phase.
  if (!gesture_commits_pick(pointer.gesture)) {
    return false;
  }

  if (shape_ == AreaShape::Polygon) {
    switch (pointer.phase) {
      case PointerPhase::Started: {
        if (!resolved.valid) {
          return false;
        }
        owner_ = pointer.pointer_id;
        has_owner_ = true;
        if (state_ == PickState::Committed || state_ == PickState::Canceled) {
          polygon_.clear();
          result_ = AreaPickResult{};
        }
        state_ = PickState::Pending;
        return true;
      }
      case PointerPhase::Ended:
      case PointerPhase::Instant: {
        if (!resolved.valid) {
          return false;
        }
        if (state_ == PickState::Committed || state_ == PickState::Canceled) {
          polygon_.clear();
          result_ = AreaPickResult{};
        }
        polygon_.push_back(resolved.position);
        preview_ = make_polygon(target_, polygon_);
        state_ = polygon_.size() >= 3u ? PickState::Orienting : PickState::Pending;
        has_owner_ = false;
        return true;
      }
      case PointerPhase::Canceled: {
        cancel();
        return true;
      }
      case PointerPhase::Updated:
        return false;
    }
    return false;
  }

  const auto build = [&](const QarVector3 & second) {
      return shape_ == AreaShape::Circle ? make_circle(target_, first_, second)
                                         : make_rectangle(target_, first_, second);
    };

  switch (pointer.phase) {
    case PointerPhase::Started: {
      if (!resolved.valid) {
        return false;
      }
      owner_ = pointer.pointer_id;
      has_owner_ = true;
      first_ = resolved.position;
      preview_ = build(first_);  // degenerate on purpose: nothing swept yet
      result_ = AreaPickResult{};
      state_ = PickState::Pending;
      return true;
    }
    case PointerPhase::Updated: {
      if (!in_flight || !resolved.valid) {
        return false;
      }
      preview_ = build(resolved.position);
      state_ = preview_.valid ? PickState::Orienting : PickState::Pending;
      return true;
    }
    case PointerPhase::Ended: {
      if (!in_flight) {
        return false;
      }
      if (resolved.valid) {
        preview_ = build(resolved.position);
      }
      has_owner_ = false;
      if (preview_.valid) {
        result_ = preview_;
        state_ = PickState::Committed;
      } else {
        // A zero-length sweep is not a region; refuse rather than publish a
        // degenerate one.
        result_ = AreaPickResult{};
        state_ = PickState::Canceled;
      }
      return true;
    }
    case PointerPhase::Instant: {
      // One tap cannot describe an extent.
      return false;
    }
    case PointerPhase::Canceled: {
      if (!in_flight) {
        return false;
      }
      cancel();
      return true;
    }
  }
  return false;
}

bool AreaPickSession::close_polygon()
{
  if (shape_ != AreaShape::Polygon || polygon_.size() < 3u) {
    return false;
  }
  result_ = make_polygon(target_, polygon_);
  preview_ = result_;
  state_ = result_.valid ? PickState::Committed : PickState::Canceled;
  has_owner_ = false;
  return result_.valid;
}

void AreaPickSession::cancel()
{
  has_owner_ = false;
  polygon_.clear();
  result_ = AreaPickResult{};
  state_ = PickState::Canceled;
}

void AreaPickSession::reset()
{
  has_owner_ = false;
  owner_ = 0;
  first_ = QarVector3{0.0f, 0.0f, 0.0f};
  polygon_.clear();
  preview_ = AreaPickResult{};
  result_ = AreaPickResult{};
  state_ = PickState::Idle;
}

// ---------------------------------------------------------------------------
// Navigation goals
// ---------------------------------------------------------------------------

QarQuaternion yaw_to_quaternion(float yaw_rad)
{
  const float half = yaw_rad * 0.5f;
  return QarQuaternion{0.0f, 0.0f, std::sin(half), std::cos(half)};
}

float yaw_from_quaternion(const QarQuaternion & q)
{
  const float siny = 2.0f * (q.w * q.z + q.x * q.y);
  const float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
  return std::atan2(siny, cosy);
}

NavGoal make_nav_goal(const PosePickResult & pose, const NavGoalOptions & options)
{
  NavGoal goal;
  if (!pose.valid) {
    goal.error = "the pose pick produced no pose";
    return goal;
  }
  if (options.frame_id.empty()) {
    goal.error = "a navigation goal needs a frame id";
    return goal;
  }
  goal.frame_id = options.frame_id;
  goal.position = pose.position;
  if (options.project_to_ground) {
    goal.position.z = options.ground_height_m;
  }
  goal.yaw_rad = pose.yaw_rad;
  goal.orientation =
    options.flatten_orientation ? yaw_to_quaternion(pose.yaw_rad) : pose.orientation;
  goal.heading_from_drag = pose.heading_from_drag;
  goal.valid = true;
  return goal;
}

}  // namespace qar_ar_core
