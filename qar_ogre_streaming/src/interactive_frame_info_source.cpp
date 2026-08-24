// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/interactive_frame_info_source.hpp"

#include <algorithm>
#include <cmath>

#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"  // reuse make_eye_fov

namespace qar_ogre_streaming
{
namespace
{

struct V3
{
  float x, y, z;
};

V3 operator+(const V3 & a, const V3 & b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
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

/// Build an orthonormal right-handed basis (e1, e2, u) from an up axis such that
/// for u == +Z it yields e1 == +X, e2 == +Y (matching the mock source / RViz).
void frame_basis(const V3 & up, V3 & e1, V3 & e2, V3 & u)
{
  u = normalize(up);
  if (u.z > 0.9995f && std::fabs(u.x) < 1e-3f && std::fabs(u.y) < 1e-3f) {
    e1 = {1.0f, 0.0f, 0.0f};
    e2 = {0.0f, 1.0f, 0.0f};
    return;
  }
  const V3 seed = (std::fabs(u.x) < 0.9f) ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
  e2 = normalize(cross(u, seed));
  e1 = normalize(cross(e2, u));
}

/// Right-handed look-at orientation as a Qar quaternion (x, y, z, w). Identical
/// construction to the mock source's helper (gluLookAt basis -> quaternion).
QarQuaternion look_at_orientation(const V3 & forward_in, const V3 & up)
{
  V3 f = normalize(forward_in);
  V3 s = cross(f, up);
  if (length(s) < 1e-6f) {
    const V3 alt = (std::fabs(f.x) < 0.9f) ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
    s = cross(f, alt);
  }
  s = normalize(s);
  const V3 uu = cross(s, f);
  const V3 zc = f * -1.0f;

  const float m00 = s.x, m01 = uu.x, m02 = zc.x;
  const float m10 = s.y, m11 = uu.y, m12 = zc.y;
  const float m20 = s.z, m21 = uu.z, m22 = zc.z;

  QarQuaternion q{};
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    float S = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * S;
    q.x = (m21 - m12) / S;
    q.y = (m02 - m20) / S;
    q.z = (m10 - m01) / S;
  } else if (m00 > m11 && m00 > m22) {
    float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / S;
    q.x = 0.25f * S;
    q.y = (m01 + m10) / S;
    q.z = (m02 + m20) / S;
  } else if (m11 > m22) {
    float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / S;
    q.x = (m01 + m10) / S;
    q.y = 0.25f * S;
    q.z = (m12 + m21) / S;
  } else {
    float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / S;
    q.x = (m02 + m20) / S;
    q.y = (m12 + m21) / S;
    q.z = 0.25f * S;
  }
  const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n > 1e-9f) {
    q.x /= n;
    q.y /= n;
    q.z /= n;
    q.w /= n;
  }
  return q;
}

/// Look (forward) direction for a given yaw/pitch about @p up.
V3 forward_dir(const V3 & up, float yaw, float pitch)
{
  V3 e1, e2, u;
  frame_basis(up, e1, e2, u);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  const V3 horizontal = e1 * (std::cos(yaw) * cp) + e2 * (std::sin(yaw) * cp);
  return normalize(horizontal + u * sp);
}

}  // namespace

void InteractiveFrameInfoSource::set_config(const Config & config)
{
  config_ = config;
  reset();
}

void InteractiveFrameInfoSource::reset()
{
  position_ = config_.start_position;
  yaw_ = config_.start_yaw_rad;
  pitch_ = config_.start_pitch_rad;
  set_pitch(pitch_);  // clamp defensively
}

QarVector3 InteractiveFrameInfoSource::forward() const
{
  return to_qar(forward_dir(to_v3(config_.up_axis), yaw_, pitch_));
}

QarVector3 InteractiveFrameInfoSource::right() const
{
  const V3 up = normalize(to_v3(config_.up_axis));
  const V3 f = forward_dir(up, yaw_, pitch_);
  V3 r = cross(f, up);
  if (length(r) < 1e-6f) {
    const V3 alt = (std::fabs(f.x) < 0.9f) ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
    r = cross(f, alt);
  }
  return to_qar(normalize(r));
}

void InteractiveFrameInfoSource::move_forward(float d)
{
  const V3 f = forward_dir(to_v3(config_.up_axis), yaw_, pitch_);
  position_ = to_qar(to_v3(position_) + f * d);
}

void InteractiveFrameInfoSource::move_right(float d)
{
  const V3 r = to_v3(right());
  position_ = to_qar(to_v3(position_) + r * d);
}

void InteractiveFrameInfoSource::move_up(float d)
{
  const V3 up = normalize(to_v3(config_.up_axis));
  position_ = to_qar(to_v3(position_) + up * d);
}

void InteractiveFrameInfoSource::add_yaw(float d)
{
  yaw_ += d;
}

void InteractiveFrameInfoSource::add_pitch(float d)
{
  set_pitch(pitch_ + d);
}

void InteractiveFrameInfoSource::set_pitch(float pitch)
{
  const float lim = std::fabs(config_.max_pitch_rad);
  pitch_ = std::max(-lim, std::min(lim, pitch));
}

QarFrameInfo InteractiveFrameInfoSource::current_frame(uint64_t frame_index) const
{
  const V3 up = normalize(to_v3(config_.up_axis));
  const V3 head = to_v3(position_);
  const V3 f = forward_dir(up, yaw_, pitch_);
  const V3 r = to_v3(right());

  const QarQuaternion orientation = look_at_orientation(f, up);
  const float half_ipd = 0.5f * config_.ipd_metres;

  QarFrameInfo info{};
  info.frame_index = frame_index;
  info.predicted_display_time = QarTimePoint{frame_index, 0};
  info.should_render = true;

  info.left_eye.pose.orientation = orientation;
  info.left_eye.pose.position = to_qar(head + r * (-half_ipd));
  info.left_eye.fov = MockQarFrameInfoSource::make_eye_fov(
    config_.vertical_fov_rad, config_.aspect, -config_.fov_asymmetry_tan);
  info.left_eye.near_far = QarNearFar{config_.near_plane, config_.far_plane};

  info.right_eye.pose.orientation = orientation;
  info.right_eye.pose.position = to_qar(head + r * (half_ipd));
  info.right_eye.fov = MockQarFrameInfoSource::make_eye_fov(
    config_.vertical_fov_rad, config_.aspect, +config_.fov_asymmetry_tan);
  info.right_eye.near_far = QarNearFar{config_.near_plane, config_.far_plane};

  return info;
}

}  // namespace qar_ogre_streaming
