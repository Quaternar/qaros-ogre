// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Headless tests for MockQarFrameInfoSource (M5): determinism, IPD offset,
// per-eye asymmetric aspect-matched FOV, look-at direction, and animation.
// Pure math — no Ogre, no GL context.

#include <cmath>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"

namespace qos = qar_ogre_streaming;

namespace
{

struct Vec
{
  float x, y, z;
};

Vec sub(const QarVector3 & a, const QarVector3 & b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(const Vec & a, const Vec & b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(const Vec & a) { return std::sqrt(dot(a, a)); }
Vec norm(const Vec & a)
{
  const float n = len(a);
  return {a.x / n, a.y / n, a.z / n};
}

// Rotate a vector by a Qar quaternion (x,y,z,w). v' = v + 2w(q x v) + 2 q x (q x v).
Vec rotate(const QarQuaternion & q, const Vec & v)
{
  const Vec u{q.x, q.y, q.z};
  const Vec uxv{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
  const Vec uxuxv{
    u.y * uxv.z - u.z * uxv.y, u.z * uxv.x - u.x * uxv.z, u.x * uxv.y - u.y * uxv.x};
  return {
    v.x + 2.0f * (q.w * uxv.x + uxuxv.x),
    v.y + 2.0f * (q.w * uxv.y + uxuxv.y),
    v.z + 2.0f * (q.w * uxv.z + uxuxv.z)};
}

}  // namespace

TEST(MockFrameSource, DeterministicGivenIndex)
{
  qos::MockQarFrameInfoSource src;
  const qos::QarFrameInfo a = src.frame_at(7);
  const qos::QarFrameInfo b = src.frame_at(7);
  EXPECT_EQ(a.frame_index, 7u);
  EXPECT_TRUE(a.should_render);
  EXPECT_FLOAT_EQ(a.left_eye.pose.position.x, b.left_eye.pose.position.x);
  EXPECT_FLOAT_EQ(a.left_eye.pose.position.y, b.left_eye.pose.position.y);
  EXPECT_FLOAT_EQ(a.left_eye.pose.position.z, b.left_eye.pose.position.z);
  EXPECT_FLOAT_EQ(a.right_eye.pose.orientation.w, b.right_eye.pose.orientation.w);
  EXPECT_FLOAT_EQ(a.left_eye.fov.angle_left, b.left_eye.fov.angle_left);
}

TEST(MockFrameSource, AnimatesOverFrames)
{
  qos::MockQarFrameInfoSource src;
  const QarVector3 p0 = src.head_position_at(0);
  const QarVector3 p1 = src.head_position_at(60);
  const float moved = len(sub(p1, p0));
  EXPECT_GT(moved, 0.1f) << "head pose should orbit / change over frames";
}

TEST(MockFrameSource, EyesSeparatedByIpdAlongRightAxis)
{
  qos::MockQarFrameInfoSource::Config cfg;
  cfg.ipd_metres = 0.064f;
  qos::MockQarFrameInfoSource src(cfg);

  const qos::QarFrameInfo f = src.frame_at(3);
  const Vec l{f.left_eye.pose.position.x, f.left_eye.pose.position.y, f.left_eye.pose.position.z};
  const Vec r{f.right_eye.pose.position.x, f.right_eye.pose.position.y, f.right_eye.pose.position.z};

  const Vec l_to_r{r.x - l.x, r.y - l.y, r.z - l.z};
  EXPECT_NEAR(len(l_to_r), cfg.ipd_metres, 1e-4f) << "eye baseline should equal the IPD";

  // The baseline must be perpendicular to the shared view forward direction
  // (parallel stereo => pure horizontal separation, no vergence).
  const Vec fwd = rotate(f.left_eye.pose.orientation, Vec{0.0f, 0.0f, -1.0f});
  EXPECT_NEAR(dot(norm(l_to_r), norm(fwd)), 0.0f, 1e-3f);

  // Both eyes share the head orientation (parallel).
  EXPECT_FLOAT_EQ(f.left_eye.pose.orientation.w, f.right_eye.pose.orientation.w);
  EXPECT_FLOAT_EQ(f.left_eye.pose.orientation.x, f.right_eye.pose.orientation.x);
}

TEST(MockFrameSource, CamerasLookAtFocus)
{
  qos::MockQarFrameInfoSource::Config cfg;
  cfg.focus = QarVector3{0.0f, 0.0f, 1.0f};
  qos::MockQarFrameInfoSource src(cfg);

  const qos::QarFrameInfo f = src.frame_at(11);
  const QarVector3 head = src.head_position_at(11);
  const Vec to_focus = norm(sub(cfg.focus, head));
  const Vec fwd = norm(rotate(f.left_eye.pose.orientation, Vec{0.0f, 0.0f, -1.0f}));
  // Camera -Z (forward) should point from the head toward the focus point.
  EXPECT_NEAR(dot(fwd, to_focus), 1.0f, 1e-3f);
}

TEST(MockFrameSource, PerEyeFovIsAsymmetricAndAspectMatched)
{
  qos::MockQarFrameInfoSource::Config cfg;
  cfg.aspect = 16.0f / 9.0f;
  cfg.fov_asymmetry_tan = 0.03f;
  qos::MockQarFrameInfoSource src(cfg);
  const qos::QarFrameInfo f = src.frame_at(0);

  // Each eye's horizontal FOV is off-axis (asymmetric): |left| != |right|.
  EXPECT_NE(f.left_eye.fov.angle_left, -f.left_eye.fov.angle_right);
  EXPECT_NE(f.right_eye.fov.angle_left, -f.right_eye.fov.angle_right);

  // The two eyes differ (mirror-asymmetric): the left eye is wider on the left.
  EXPECT_LT(f.left_eye.fov.angle_left, f.right_eye.fov.angle_left);
  EXPECT_LT(f.left_eye.fov.angle_right, f.right_eye.fov.angle_right);

  // Aspect is preserved exactly: (tanR - tanL) / (tanU - tanD) == aspect.
  auto aspect_of = [](const QarFov & v) {
    const float h = std::tan(v.angle_right) - std::tan(v.angle_left);
    const float w = std::tan(v.angle_up) - std::tan(v.angle_down);
    return h / w;
  };
  EXPECT_NEAR(aspect_of(f.left_eye.fov), cfg.aspect, 1e-4f);
  EXPECT_NEAR(aspect_of(f.right_eye.fov), cfg.aspect, 1e-4f);

  // Sanity: vertical FOV symmetric and equal to the configured total.
  EXPECT_NEAR(f.left_eye.fov.angle_up, 0.5f * cfg.vertical_fov_rad, 1e-5f);
  EXPECT_NEAR(f.left_eye.fov.angle_down, -0.5f * cfg.vertical_fov_rad, 1e-5f);
}

TEST(MockFrameSource, NearFarPropagated)
{
  qos::MockQarFrameInfoSource::Config cfg;
  cfg.near_plane = 0.2f;
  cfg.far_plane = 123.0f;
  qos::MockQarFrameInfoSource src(cfg);
  const qos::QarFrameInfo f = src.frame_at(0);
  EXPECT_FLOAT_EQ(f.left_eye.near_far.near_plane, 0.2f);
  EXPECT_FLOAT_EQ(f.right_eye.near_far.far_plane, 123.0f);
}
