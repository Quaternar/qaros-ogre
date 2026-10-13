// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/qar_pose_conversion.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

namespace qar_ogre_streaming
{

// ---------------------------------------------------------------------------
// Pose conversion.
//
// Qar and Ogre camera-local space share the SAME frame: right-handed, X right,
// Y up, -Z forward. So position copies through unchanged and orientation copies
// through unchanged (only the storage layout differs: Qar xyzw -> Ogre wxyz).
// See docs/ARCHITECTURE.md#pose-projection-and-world-basis for the derivation.
// ---------------------------------------------------------------------------

namespace
{
// ---------------------------------------------------------------------------
// WORLD-basis conversion.
//
// The note above is about the camera-LOCAL frame, and it is correct. The WORLD
// frames however do NOT agree: QAR hands us head poses in a Y-up (OpenGL/AR)
// world, while RViz/Ogre render a ROS world that is Z-up (REP-103: x forward,
// y left, z up). Feeding a Y-up pose straight into a Z-up world tips the view on
// its side: the consumer starts at the origin looking straight down at the scene
// (a "bird's perspective").
//
// Camera-local axes agreeing means only the world basis needs rotating:
//     ros = (-gl_z, -gl_x, gl_y)
// which is the proper rotation (det = +1) with quaternion (w,x,y,z) =
// (0.5, 0.5, -0.5, -0.5): it maps gl up (+Y) -> ros up (+Z) and gl forward (-Z)
// -> ros forward (+X).
//
// Gated by QAR_POSE_WORLD so the world basis can be switched at run time without a
// rebuild, and so the conversion tests (which assert the documented identity
// mapping) hold by default:
//     QAR_POSE_WORLD=zup   apply the Y-up -> Z-up world rotation (for Z-up content)
//     unset / anything else  identity (the default)
bool world_zup_enabled()
{
  static const bool on = [] {
      const char * v = std::getenv("QAR_POSE_WORLD");
      return v != nullptr && std::string(v) == "zup";
    }();
  return on;
}

constexpr float kWorldQw = 0.5f;
constexpr float kWorldQx = 0.5f;
constexpr float kWorldQy = -0.5f;
constexpr float kWorldQz = -0.5f;

// Viewer anchor, applied AFTER the basis change (zup mode only).
//   QAR_POSE_YAW_DEG    yaw about the ROS vertical (Z) axis, degrees (default 180)
//   QAR_POSE_HEIGHT_M   eye height added along ROS +Z, metres    (default 0)
//   QAR_POSE_BACK_M     stand-off BEHIND the facing direction, metres (default 0).
//                       It follows the yaw, so retuning the yaw keeps it valid.
//
// The translations default to ZERO on purpose: QAR reports the viewer in its own frame.
// The XR runtime sets the room origin, not applications, and a volume can be placed
// (QarAppVolumeInit::pose, qar_app_volumes_change_pose), but this stack creates its one
// volume at the room origin, centre raised, and never moves it. A translation applied
// here is therefore invisible to QAR, so the Visualizer would keep reporting 0 while the
// viewer actually stood metres away in the RViz world. Keeping XYZ at 0 makes the two
// agree; only the ORIENTATION is corrected, which it must be, because the two worlds
// genuinely disagree about which axis is up. The knobs remain for experimentation.
//
// Note on what the Visualizer reports: its readout is in QAR's own Y-up frame, where
// Z is the horizontal "backwards" axis (forward is -Z) — a QAR Z of 7 is 7 m
// horizontally back, NOT a height.
float env_float(const char * name, float fallback)
{
  const char * v = std::getenv(name);
  if (v == nullptr) { return fallback; }
  try {
    return std::stof(std::string(v));
  } catch (...) {
    return fallback;  // malformed value: keep the default rather than throwing
  }
}

float anchor_yaw_rad()
{
  static const float rad =
    env_float("QAR_POSE_YAW_DEG", 180.0f) * 3.14159265358979323846f / 180.0f;
  return rad;
}

float anchor_height_m()
{
  static const float h = env_float("QAR_POSE_HEIGHT_M", 0.0f);
  return h;
}

float anchor_back_m()
{
  static const float b = env_float("QAR_POSE_BACK_M", 0.0f);
  return b;
}

/// Hamilton product a*b, both (w,x,y,z).
Quat quat_mul(const Quat & a, const Quat & b)
{
  return Quat{
    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
}  // namespace

Vec3 qar_position_to_ogre(const QarVector3 & position)
{
  if (world_zup_enabled()) {
    // Basis change: ros = (-gl_z, -gl_x, gl_y)
    const float x = -position.z;
    const float y = -position.x;
    const float z = position.y;
    // Then the viewer anchor: yaw about ROS +Z, stand back along the facing
    // direction, and lift to eye height. After the yaw the viewer faces
    // f = (cos yaw, sin yaw, 0), so standing back is -back * f.
    const float a = anchor_yaw_rad();
    const float c = std::cos(a), s = std::sin(a);
    const float back = anchor_back_m();
    return Vec3{
      x * c - y * s - back * c,
      x * s + y * c - back * s,
      z + anchor_height_m()};
  }
  // Identity mapping: same axes, same handedness.
  return Vec3{position.x, position.y, position.z};
}

namespace
{
// Normalize a Qar quaternion (xyzw) into an Ogre-ordered Quat (wxyz).
// Returns false if the input length is below kMinQuaternionNorm.
bool normalize_orientation(const QarQuaternion & in, Quat & out)
{
  const float x = in.x;
  const float y = in.y;
  const float z = in.z;
  const float w = in.w;
  const float norm = std::sqrt(w * w + x * x + y * y + z * z);
  if (!(norm >= kMinQuaternionNorm)) {  // also catches NaN
    out = Quat{};  // identity
    return false;
  }
  const float inv = 1.0f / norm;
  out = Quat{w * inv, x * inv, y * inv, z * inv};
  return true;
}
}  // namespace

Quat qar_orientation_to_ogre(const QarQuaternion & orientation)
{
  Quat out;
  // Convenience form: degenerate input silently falls back to identity.
  normalize_orientation(orientation, out);
  return out;
}

Result qar_pose_to_ogre(
  const QarPose & pose,
  Vec3 & out_position,
  Quat & out_orientation)
{
  Quat q;
  if (!normalize_orientation(pose.orientation, q)) {
    out_position = Vec3{};
    out_orientation = Quat{};
    return Result::error(
      Status::InvalidArgument,
      "qar_pose_to_ogre: orientation quaternion has near-zero length "
      "(cannot be normalized)");
  }
  // Re-express the camera's orientation in the Ogre/ROS world basis, then apply the
  // anchor yaw. The camera's LOCAL axes are unchanged, so both are left-multiplications.
  if (world_zup_enabled()) {
    const float half = anchor_yaw_rad() * 0.5f;
    const Quat yaw{std::cos(half), 0.0f, 0.0f, std::sin(half)};  // about ROS +Z
    out_orientation =
      quat_mul(yaw, quat_mul(Quat{kWorldQw, kWorldQx, kWorldQy, kWorldQz}, q));
  } else {
    out_orientation = q;
  }
  out_position = qar_position_to_ogre(pose.position);
  return Result::success();
}

// ---------------------------------------------------------------------------
// Asymmetric off-axis perspective projection.
//
// Extents at the near plane from the signed FOV half-angles, then the standard
// OpenGL glFrustum matrix (== Ogre::Frustum::setFrustumExtents + getProjection-
// Matrix(), depth in [-1,+1], right-handed, looking down -Z).
// ---------------------------------------------------------------------------

Result make_projection_from_qar_fov(
  const QarFov & fov,
  float near_plane,
  float far_plane,
  Mat4 & out_projection)
{
  if (!(near_plane > 0.0f)) {
    return Result::error(Status::InvalidArgument, "near_plane must be > 0");
  }
  if (!(far_plane > near_plane)) {
    return Result::error(Status::InvalidArgument, "far_plane must be > near_plane");
  }
  if (!(fov.angle_right > fov.angle_left)) {
    return Result::error(
      Status::InvalidArgument, "QarFov requires angle_right > angle_left");
  }
  if (!(fov.angle_up > fov.angle_down)) {
    return Result::error(
      Status::InvalidArgument, "QarFov requires angle_up > angle_down");
  }

  // tan() is only well-defined (and monotonic, keeping the extents ordered) on
  // the open half-angle range (-pi/2, pi/2). Reject anything at/outside it.
  constexpr float kHalfPi = 1.57079632679489661923f;  // pi/2
  if (std::abs(fov.angle_left) >= kHalfPi || std::abs(fov.angle_right) >= kHalfPi ||
    std::abs(fov.angle_up) >= kHalfPi || std::abs(fov.angle_down) >= kHalfPi)
  {
    return Result::error(
      Status::InvalidArgument,
      "QarFov half-angles must lie in the open interval (-pi/2, pi/2)");
  }

  const float n = near_plane;
  const float f = far_plane;

  // Frustum extents on the near plane.
  const float left = n * std::tan(fov.angle_left);
  const float right = n * std::tan(fov.angle_right);
  const float bottom = n * std::tan(fov.angle_down);
  const float top = n * std::tan(fov.angle_up);

  const float inv_w = 1.0f / (right - left);
  const float inv_h = 1.0f / (top - bottom);
  const float inv_d = 1.0f / (f - n);

  Mat4 out;  // starts as identity; overwrite every relevant entry.
  for (float & e : out.m) {
    e = 0.0f;
  }

  // Row-major: out.m[row * 4 + col]. Classic glFrustum / Ogre perspective form.
  out.m[0] = 2.0f * n * inv_w;             // [0][0]
  out.m[2] = (right + left) * inv_w;       // [0][2]  x shear (off-axis)
  out.m[5] = 2.0f * n * inv_h;             // [1][1]
  out.m[6] = (top + bottom) * inv_h;       // [1][2]  y shear (off-axis)
  out.m[10] = -(f + n) * inv_d;            // [2][2]
  out.m[11] = -2.0f * f * n * inv_d;       // [2][3]
  out.m[14] = -1.0f;                       // [3][2]  perspective divide by -z
  // out.m[15] stays 0.

  out_projection = out;
  return Result::success();
}

}  // namespace qar_ogre_streaming
