// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"

#include <cmath>

namespace qar_ogre_streaming
{
namespace
{

struct V3
{
  float x, y, z;
};

V3 operator+(const V3 & a, const V3 & b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(const V3 & a, const V3 & b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(const V3 & a, float s) { return {a.x * s, a.y * s, a.z * s}; }

float dot(const V3 & a, const V3 & b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(const V3 & a, const V3 & b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const V3 & a) { return std::sqrt(dot(a, a)); }
V3 normalize(const V3 & a)
{
  const float n = length(a);
  return (n > 1e-9f) ? V3{a.x / n, a.y / n, a.z / n} : V3{0.0f, 0.0f, 0.0f};
}

V3 to_v3(const QarVector3 & v) { return {v.x, v.y, v.z}; }
QarVector3 to_qar(const V3 & v) { return QarVector3{v.x, v.y, v.z}; }

/// Build an orthonormal, right-handed orbit basis (e1, e2, u) from an up axis
/// such that for u == +Z it yields e1 == +X, e2 == +Y (matching RViz's orbit
/// view controller formula exactly). e1 x e2 == u for any up.
void orbit_basis(const V3 & up, V3 & e1, V3 & e2, V3 & u)
{
  u = normalize(up);
  if (u.z > 0.9995f && std::fabs(u.x) < 1e-3f && std::fabs(u.y) < 1e-3f) {
    e1 = {1.0f, 0.0f, 0.0f};
    e2 = {0.0f, 1.0f, 0.0f};
    return;
  }
  const V3 seed = (std::fabs(u.x) < 0.9f) ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
  e2 = normalize(cross(u, seed));  // perpendicular to u
  e1 = normalize(cross(e2, u));    // (e1, e2, u) right-handed: e1 x e2 == u
}

/// Right-handed look-at orientation (camera looks down its local -Z, local +Y
/// near @p up), returned as a Qar quaternion (x, y, z, w). Mirrors gluLookAt:
/// forward = normalize(target - eye), right = normalize(forward x up),
/// trueUp = right x forward, camera basis columns = [right, trueUp, -forward].
QarQuaternion look_at_orientation(const V3 & forward_in, const V3 & up)
{
  V3 f = normalize(forward_in);
  V3 s = cross(f, up);
  if (length(s) < 1e-6f) {
    // Forward parallel to up: nudge with an alternate reference axis.
    const V3 alt = (std::fabs(f.x) < 0.9f) ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
    s = cross(f, alt);
  }
  s = normalize(s);
  const V3 uu = cross(s, f);  // true up (right x forward)
  const V3 zc = f * -1.0f;    // camera +Z axis (backward)

  // Rotation matrix, columns [s, uu, zc] => m[row][col].
  const float m00 = s.x, m01 = uu.x, m02 = zc.x;
  const float m10 = s.y, m11 = uu.y, m12 = zc.y;
  const float m20 = s.z, m21 = uu.z, m22 = zc.z;

  QarQuaternion q{};
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    float S = std::sqrt(trace + 1.0f) * 2.0f;  // 4w
    q.w = 0.25f * S;
    q.x = (m21 - m12) / S;
    q.y = (m02 - m20) / S;
    q.z = (m10 - m01) / S;
  } else if (m00 > m11 && m00 > m22) {
    float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;  // 4x
    q.w = (m21 - m12) / S;
    q.x = 0.25f * S;
    q.y = (m01 + m10) / S;
    q.z = (m02 + m20) / S;
  } else if (m11 > m22) {
    float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;  // 4y
    q.w = (m02 - m20) / S;
    q.x = (m01 + m10) / S;
    q.y = 0.25f * S;
    q.z = (m12 + m21) / S;
  } else {
    float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;  // 4z
    q.w = (m10 - m01) / S;
    q.x = (m02 + m20) / S;
    q.y = (m12 + m21) / S;
    q.z = 0.25f * S;
  }
  // Normalize defensively.
  const float n =
    std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n > 1e-9f) {
    q.x /= n;
    q.y /= n;
    q.z /= n;
    q.w /= n;
  }
  return q;
}

}  // namespace

QarVector3 MockQarFrameInfoSource::head_position_at(uint64_t frame_index) const
{
  V3 e1, e2, u;
  orbit_basis(to_v3(config_.up_axis), e1, e2, u);
  const float yaw = config_.start_yaw_rad +
    static_cast<float>(frame_index) * config_.yaw_per_frame_rad;
  const float cp = std::cos(config_.pitch_rad);
  const float sp = std::sin(config_.pitch_rad);
  const V3 offset =
    e1 * (config_.orbit_distance_m * std::cos(yaw) * cp) +
    e2 * (config_.orbit_distance_m * std::sin(yaw) * cp) +
    u * (config_.orbit_distance_m * sp);
  return to_qar(to_v3(config_.focus) + offset);
}

QarFov MockQarFrameInfoSource::make_eye_fov(
  float vertical_fov_rad, float aspect, float asym_tan)
{
  const float half_v = 0.5f * vertical_fov_rad;
  const float tan_v = std::tan(half_v);
  const float hh = std::atan(aspect * tan_v);  // symmetric horizontal half-angle
  const float tl = -std::tan(hh);
  const float tr = std::tan(hh);
  QarFov fov{};
  fov.angle_left = std::atan(tl + asym_tan);
  fov.angle_right = std::atan(tr + asym_tan);
  fov.angle_up = half_v;
  fov.angle_down = -half_v;
  return fov;
}

QarFrameInfo MockQarFrameInfoSource::frame_at(uint64_t frame_index) const
{
  const V3 up = normalize(to_v3(config_.up_axis));
  const V3 head = to_v3(head_position_at(frame_index));
  const V3 focus = to_v3(config_.focus);

  const V3 forward = normalize(focus - head);
  // Head right axis = forward x up (same basis used inside look_at_orientation).
  V3 right = cross(forward, up);
  if (length(right) < 1e-6f) {
    const V3 alt = (std::fabs(forward.x) < 0.9f)
      ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
    right = cross(forward, alt);
  }
  right = normalize(right);

  const QarQuaternion orientation = look_at_orientation(forward, up);
  const float half_ipd = 0.5f * config_.ipd_metres;

  QarFrameInfo info{};
  info.frame_index = frame_index;
  info.predicted_display_time = QarTimePoint{frame_index, 0};  // ms precision
  info.should_render = true;

  // Left eye: shifted to the viewer's left (-right); right eye: +right. Both
  // share the head orientation (parallel stereo).
  info.left_eye.pose.orientation = orientation;
  info.left_eye.pose.position = to_qar(head - right * half_ipd);
  info.left_eye.fov = make_eye_fov(
    config_.vertical_fov_rad, config_.aspect, -config_.fov_asymmetry_tan);
  info.left_eye.near_far = QarNearFar{config_.near_plane, config_.far_plane};

  info.right_eye.pose.orientation = orientation;
  info.right_eye.pose.position = to_qar(head + right * half_ipd);
  info.right_eye.fov = make_eye_fov(
    config_.vertical_fov_rad, config_.aspect, +config_.fov_asymmetry_tan);
  info.right_eye.near_far = QarNearFar{config_.near_plane, config_.far_plane};

  return info;
}

}  // namespace qar_ogre_streaming
