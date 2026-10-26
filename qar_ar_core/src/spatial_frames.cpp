// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ar_core/spatial_frames.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace qar_ar_core
{
namespace
{

// The basis change as a quaternion, (w, x, y, z). Kept as named constants so it
// reads the same here as it does in qar_ogre_streaming/src/qar_pose_conversion.cpp,
// which is the point: one convention, written twice only because the two
// libraries must not depend on each other (Ogre-free / Qt-free boundaries).
constexpr float kWorldQw = 0.5f;
constexpr float kWorldQx = 0.5f;
constexpr float kWorldQy = -0.5f;
constexpr float kWorldQz = -0.5f;

constexpr float kMinQuaternionNorm = 1e-6f;

struct Wxyz
{
  float w = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

/// Hamilton product a*b, both (w, x, y, z).
Wxyz quat_mul(const Wxyz & a, const Wxyz & b)
{
  return Wxyz{
    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

/// std::getenv is the portable read; MSVC deprecates it in favour of a
/// non-portable alternative, and this is a read-only lookup on startup data, so
/// the warning is suppressed here rather than the API being forked per platform.
const char * read_env(const char * name)
{
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4996)
#endif
  return std::getenv(name);
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
}

float env_float(const char * name, float fallback)
{
  const char * v = read_env(name);
  if (v == nullptr) {
    return fallback;
  }
  try {
    return std::stof(std::string(v));
  } catch (...) {
    return fallback;  // malformed value: keep the default rather than throwing
  }
}

/// The pure basis change, no anchor: ros = (-room.z, -room.x, room.y).
QarVector3 basis_room_to_ros(const QarVector3 & room)
{
  return QarVector3{-room.z, -room.x, room.y};
}

/// Inverse of @ref basis_room_to_ros.
QarVector3 basis_ros_to_room(const QarVector3 & ros)
{
  return QarVector3{-ros.y, ros.z, -ros.x};
}

// ---------------------------------------------------------------------------
// App-volume placement helpers.
//
// Rotating a vector by a quaternion, and by its conjugate. Written out rather
// than pulled from a maths library because this file must stay dependency-free
// (see the header: the same convention is duplicated in qar_ogre_streaming for
// exactly that reason).
// ---------------------------------------------------------------------------

/// Rotate @p v by the unit quaternion @p q (Qar layout, xyzw).
/// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
QarVector3 rotate_by(const QarQuaternion & q, const QarVector3 & v)
{
  const float tx = 2.0f * (q.y * v.z - q.z * v.y);
  const float ty = 2.0f * (q.z * v.x - q.x * v.z);
  const float tz = 2.0f * (q.x * v.y - q.y * v.x);
  return QarVector3{
    v.x + q.w * tx + (q.y * tz - q.z * ty),
    v.y + q.w * ty + (q.z * tx - q.x * tz),
    v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

/// Rotate @p v by the CONJUGATE of @p q, i.e. undo @ref rotate_by.
QarVector3 rotate_by_inverse(const QarQuaternion & q, const QarVector3 & v)
{
  return rotate_by(QarQuaternion{-q.x, -q.y, -q.z, q.w}, v);
}

/// A unit-length copy of @p q. A degenerate quaternion becomes the identity
/// rather than poisoning every subsequent point with NaN: an unreadable volume
/// orientation must degrade to "no rotation", never to "no pick".
QarQuaternion unit(const QarQuaternion & q)
{
  const float n2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  if (!(n2 >= kMinQuaternionNorm * kMinQuaternionNorm)) {  // also catches NaN
    return QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
  }
  const float inv = 1.0f / std::sqrt(n2);
  return QarQuaternion{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

bool is_identity_quaternion(const QarQuaternion & q)
{
  return q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && std::abs(q.w) == 1.0f;
}

bool is_zero_vector(const QarVector3 & v)
{
  return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
}

}  // namespace

bool scene_placement_is_identity(const ScenePlacement & placement)
{
  return placement.app_scale == 1.0f &&
         is_identity_quaternion(placement.volume_pose.orientation) &&
         is_zero_vector(placement.volume_pose.position) &&
         is_identity_quaternion(placement.app_pose.orientation) &&
         is_zero_vector(placement.app_pose.position);
}

float scene_placement_scale(const ScenePlacement & placement)
{
  const float s = placement.app_scale;
  // A zero or negative scale is not a smaller scene, it is a collapsed or
  // mirrored one, and dividing by it would publish infinities into a ROS topic.
  // Refusing it here keeps a bad read-back from ever reaching a pick.
  if (!(s > 0.0f) || !std::isfinite(s)) {
    return 1.0f;
  }
  return s;
}

QarVector3 scene_point_to_room(const ScenePlacement & placement, const QarVector3 & scene)
{
  if (scene_placement_is_identity(placement)) {
    return scene;
  }
  const float s = scene_placement_scale(placement);
  // app metres -> volume metres: scale, then the app pose (which app_scale does
  // NOT scale — the vendor is explicit, and so is this line).
  const QarVector3 scaled{scene.x * s, scene.y * s, scene.z * s};
  const QarQuaternion app_q = unit(placement.app_pose.orientation);
  const QarVector3 rotated = rotate_by(app_q, scaled);
  const QarVector3 in_volume{
    rotated.x + placement.app_pose.position.x,
    rotated.y + placement.app_pose.position.y,
    rotated.z + placement.app_pose.position.z};
  // volume metres -> room metres: the cuboid's own pose, whose position is the
  // CENTRE of the box.
  const QarQuaternion vol_q = unit(placement.volume_pose.orientation);
  const QarVector3 in_room = rotate_by(vol_q, in_volume);
  return QarVector3{
    in_room.x + placement.volume_pose.position.x,
    in_room.y + placement.volume_pose.position.y,
    in_room.z + placement.volume_pose.position.z};
}

QarVector3 room_point_to_scene(const ScenePlacement & placement, const QarVector3 & room)
{
  if (scene_placement_is_identity(placement)) {
    return room;
  }
  const float s = scene_placement_scale(placement);
  const QarVector3 centred{
    room.x - placement.volume_pose.position.x,
    room.y - placement.volume_pose.position.y,
    room.z - placement.volume_pose.position.z};
  const QarVector3 in_volume = rotate_by_inverse(unit(placement.volume_pose.orientation), centred);
  const QarVector3 off_app{
    in_volume.x - placement.app_pose.position.x,
    in_volume.y - placement.app_pose.position.y,
    in_volume.z - placement.app_pose.position.z};
  const QarVector3 unrotated = rotate_by_inverse(unit(placement.app_pose.orientation), off_app);
  const float inv = 1.0f / s;
  return QarVector3{unrotated.x * inv, unrotated.y * inv, unrotated.z * inv};
}

std::string describe_scene_placement(const ScenePlacement & placement)
{
  // 320 was too small: an identity placement formats to 355 characters, so the
  // "[IDENTITY ...]" clause -- the one a reader most needs -- was being silently
  // cut off by snprintf. Sized with room to spare; this is a log line, not a hot
  // path.
  char buffer[512];
  const float s = scene_placement_scale(placement);
  std::snprintf(
    buffer, sizeof(buffer),
    "%s app_scale=%.4f room-m/scene-m (1 scene m renders as %.4f room m; "
    "a room metre is %.3f scene m) volume_centre_room=(%.3f, %.3f, %.3f) "
    "volume_quat=(%.3f, %.3f, %.3f, %.3f) app_pose_volume=(%.3f, %.3f, %.3f) "
    "app_quat=(%.3f, %.3f, %.3f, %.3f)%s",
    placement.known ? "read back from the runtime:" : "ASSUMED (never read back):",
    static_cast<double>(placement.app_scale), static_cast<double>(s),
    static_cast<double>(1.0f / s),
    static_cast<double>(placement.volume_pose.position.x),
    static_cast<double>(placement.volume_pose.position.y),
    static_cast<double>(placement.volume_pose.position.z),
    static_cast<double>(placement.volume_pose.orientation.x),
    static_cast<double>(placement.volume_pose.orientation.y),
    static_cast<double>(placement.volume_pose.orientation.z),
    static_cast<double>(placement.volume_pose.orientation.w),
    static_cast<double>(placement.app_pose.position.x),
    static_cast<double>(placement.app_pose.position.y),
    static_cast<double>(placement.app_pose.position.z),
    static_cast<double>(placement.app_pose.orientation.x),
    static_cast<double>(placement.app_pose.orientation.y),
    static_cast<double>(placement.app_pose.orientation.z),
    static_cast<double>(placement.app_pose.orientation.w),
    scene_placement_is_identity(placement)
      ? " [IDENTITY - the volume shows our scene life size, at the room origin]"
      : "");
  return std::string(buffer);
}

QarVector3 scene_direction_to_room(const ScenePlacement & placement, const QarVector3 & scene)
{
  if (scene_placement_is_identity(placement)) {
    return scene;
  }
  // Rotations only: a direction has no origin to offset and no length to scale.
  return rotate_by(
    unit(placement.volume_pose.orientation),
    rotate_by(unit(placement.app_pose.orientation), scene));
}

QarVector3 room_direction_to_scene(const ScenePlacement & placement, const QarVector3 & room)
{
  if (scene_placement_is_identity(placement)) {
    return room;
  }
  return rotate_by_inverse(
    unit(placement.app_pose.orientation),
    rotate_by_inverse(unit(placement.volume_pose.orientation), room));
}

const float kDefaultAnchorYawRad = 3.14159265358979323846f;

RoomToRosTransform room_to_ros_from_environment()
{
  RoomToRosTransform transform;
  const char * world = read_env("QAR_POSE_WORLD");
  transform.enabled = world != nullptr && std::string(world) == "zup";
  transform.yaw_rad =
    env_float("QAR_POSE_YAW_DEG", 180.0f) * 3.14159265358979323846f / 180.0f;
  transform.height_m = env_float("QAR_POSE_HEIGHT_M", 0.0f);
  transform.back_m = env_float("QAR_POSE_BACK_M", 0.0f);
  return transform;
}

QarVector3 room_point_to_ros(const RoomToRosTransform & transform, const QarVector3 & room)
{
  // The placement comes off FIRST and goes on LAST (see ros_point_to_room): it
  // describes where QarOS put our scene in the ROOM, so it lives on the room
  // side of the basis change. It is applied even in the disabled/identity mode,
  // because "room space is already the ROS world" is a statement about AXES and
  // says nothing about the size the volume is displaying our content at.
  const QarVector3 scene = room_point_to_scene(transform.placement, room);
  if (!transform.enabled) {
    return scene;
  }
  const QarVector3 b = basis_room_to_ros(scene);
  const float c = std::cos(transform.yaw_rad);
  const float s = std::sin(transform.yaw_rad);
  // After the yaw the viewer faces f = (cos yaw, sin yaw, 0), so the stand-off
  // is -back * f — the same expression qar_position_to_ogre() applies.
  return QarVector3{
    b.x * c - b.y * s - transform.back_m * c,
    b.x * s + b.y * c - transform.back_m * s,
    b.z + transform.height_m};
}

QarVector3 room_direction_to_ros(const RoomToRosTransform & transform, const QarVector3 & room)
{
  const QarVector3 scene = room_direction_to_scene(transform.placement, room);
  if (!transform.enabled) {
    return scene;
  }
  const QarVector3 b = basis_room_to_ros(scene);
  const float c = std::cos(transform.yaw_rad);
  const float s = std::sin(transform.yaw_rad);
  return QarVector3{b.x * c - b.y * s, b.x * s + b.y * c, b.z};
}

QarQuaternion room_orientation_to_ros(
  const RoomToRosTransform & transform, const QarQuaternion & room)
{
  const float norm = std::sqrt(
    room.w * room.w + room.x * room.x + room.y * room.y + room.z * room.z);
  if (!(norm >= kMinQuaternionNorm)) {  // also catches NaN
    return QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
  }
  const float inv = 1.0f / norm;
  Wxyz q{room.w * inv, room.x * inv, room.y * inv, room.z * inv};
  // Undo the placement's rotation first, for the same reason the point mapping
  // undoes its offset first: a volume the operator has yawed rotates our whole
  // scene in the room, and an orientation read back out of it is turned by the
  // same amount.
  if (!scene_placement_is_identity(transform.placement)) {
    const QarQuaternion vol = unit(transform.placement.volume_pose.orientation);
    const QarQuaternion app = unit(transform.placement.app_pose.orientation);
    const Wxyz total = quat_mul(
      Wxyz{vol.w, vol.x, vol.y, vol.z}, Wxyz{app.w, app.x, app.y, app.z});
    q = quat_mul(Wxyz{total.w, -total.x, -total.y, -total.z}, q);
  }
  if (!transform.enabled) {
    return QarQuaternion{q.x, q.y, q.z, q.w};
  }
  const float half = transform.yaw_rad * 0.5f;
  const Wxyz yaw{std::cos(half), 0.0f, 0.0f, std::sin(half)};  // about ROS +Z
  const Wxyz out = quat_mul(yaw, quat_mul(Wxyz{kWorldQw, kWorldQx, kWorldQy, kWorldQz}, q));
  return QarQuaternion{out.x, out.y, out.z, out.w};
}

QarVector3 ros_point_to_room(const RoomToRosTransform & transform, const QarVector3 & ros)
{
  if (!transform.enabled) {
    return scene_point_to_room(transform.placement, ros);
  }
  const float c = std::cos(transform.yaw_rad);
  const float s = std::sin(transform.yaw_rad);
  // Undo the stand-off, then the yaw, then the height.
  const float x = ros.x + transform.back_m * c;
  const float y = ros.y + transform.back_m * s;
  const QarVector3 unyawed{x * c + y * s, -x * s + y * c, ros.z - transform.height_m};
  // ... and only then put the scene back where the volume is displaying it.
  return scene_point_to_room(transform.placement, basis_ros_to_room(unyawed));
}

QarVector3 ros_direction_to_room(const RoomToRosTransform & transform, const QarVector3 & ros)
{
  if (!transform.enabled) {
    return scene_direction_to_room(transform.placement, ros);
  }
  const float c = std::cos(transform.yaw_rad);
  const float s = std::sin(transform.yaw_rad);
  const QarVector3 unyawed{ros.x * c + ros.y * s, -ros.x * s + ros.y * c, ros.z};
  return scene_direction_to_room(transform.placement, basis_ros_to_room(unyawed));
}

}  // namespace qar_ar_core
