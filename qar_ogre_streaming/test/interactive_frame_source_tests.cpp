// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Headless tests for InteractiveFrameInfoSource (M9.5): head-pose state machine
// (WASD translation + yaw/pitch look, pitch clamp) and per-eye stereo derivation
// (eyes head +/- IPD/2 along the head-right axis, shared orientation, look
// direction consistent with the eye orientation). Pure math — no Ogre, no GL.

#include <cmath>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/interactive_frame_info_source.hpp"

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

// Rotate a vector by a Qar quaternion (x,y,z,w).
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

TEST(InteractiveFrameSource, EyesSeparatedByIpdAlongRightAxis)
{
  qos::InteractiveFrameInfoSource::Config cfg;
  cfg.ipd_metres = 0.064f;
  qos::InteractiveFrameInfoSource src(cfg);

  const qos::QarFrameInfo f = src.current_frame(0);
  const Vec l{f.left_eye.pose.position.x, f.left_eye.pose.position.y, f.left_eye.pose.position.z};
  const Vec r{f.right_eye.pose.position.x, f.right_eye.pose.position.y, f.right_eye.pose.position.z};

  const Vec l_to_r{r.x - l.x, r.y - l.y, r.z - l.z};
  EXPECT_NEAR(len(l_to_r), cfg.ipd_metres, 1e-4f) << "eye baseline should equal the IPD";

  // Baseline perpendicular to the shared view forward (parallel stereo).
  const Vec fwd = rotate(f.left_eye.pose.orientation, Vec{0.0f, 0.0f, -1.0f});
  EXPECT_NEAR(dot(norm(l_to_r), norm(fwd)), 0.0f, 1e-3f);

  // Both eyes share the head orientation.
  EXPECT_FLOAT_EQ(f.left_eye.pose.orientation.w, f.right_eye.pose.orientation.w);
  EXPECT_FLOAT_EQ(f.left_eye.pose.orientation.x, f.right_eye.pose.orientation.x);
}

TEST(InteractiveFrameSource, EyeMidpointEqualsHeadPosition)
{
  qos::InteractiveFrameInfoSource src;
  const qos::QarFrameInfo f = src.current_frame(0);
  const QarVector3 head = src.head_position();
  const float mx = 0.5f * (f.left_eye.pose.position.x + f.right_eye.pose.position.x);
  const float my = 0.5f * (f.left_eye.pose.position.y + f.right_eye.pose.position.y);
  const float mz = 0.5f * (f.left_eye.pose.position.z + f.right_eye.pose.position.z);
  EXPECT_NEAR(mx, head.x, 1e-4f);
  EXPECT_NEAR(my, head.y, 1e-4f);
  EXPECT_NEAR(mz, head.z, 1e-4f);
}

TEST(InteractiveFrameSource, EyeOrientationMatchesLookDirection)
{
  qos::InteractiveFrameInfoSource src;
  const qos::QarFrameInfo f = src.current_frame(0);
  // The camera -Z (forward) derived from the eye orientation must equal the
  // source's reported look direction.
  const Vec fwd_from_quat = norm(rotate(f.left_eye.pose.orientation, Vec{0.0f, 0.0f, -1.0f}));
  const QarVector3 fdir = src.forward();
  EXPECT_NEAR(fwd_from_quat.x, fdir.x, 1e-3f);
  EXPECT_NEAR(fwd_from_quat.y, fdir.y, 1e-3f);
  EXPECT_NEAR(fwd_from_quat.z, fdir.z, 1e-3f);
}

TEST(InteractiveFrameSource, MoveForwardTranslatesAlongLookDir)
{
  qos::InteractiveFrameInfoSource src;
  const QarVector3 before = src.head_position();
  const QarVector3 fdir = src.forward();
  src.move_forward(2.0f);
  const QarVector3 after = src.head_position();

  const Vec delta = sub(after, before);
  EXPECT_NEAR(len(delta), 2.0f, 1e-3f) << "should move exactly the requested distance";
  // Movement direction equals the look direction.
  const Vec nd = norm(delta);
  EXPECT_NEAR(nd.x, fdir.x, 1e-3f);
  EXPECT_NEAR(nd.y, fdir.y, 1e-3f);
  EXPECT_NEAR(nd.z, fdir.z, 1e-3f);
}

TEST(InteractiveFrameSource, MoveRightStrafesAlongRightAxis)
{
  qos::InteractiveFrameInfoSource src;
  const QarVector3 before = src.head_position();
  const QarVector3 rdir = src.right();
  src.move_right(1.5f);
  const Vec delta = sub(src.head_position(), before);
  EXPECT_NEAR(len(delta), 1.5f, 1e-3f);
  const Vec nd = norm(delta);
  EXPECT_NEAR(nd.x, rdir.x, 1e-3f);
  EXPECT_NEAR(nd.y, rdir.y, 1e-3f);
  EXPECT_NEAR(nd.z, rdir.z, 1e-3f);
}

TEST(InteractiveFrameSource, YawChangesLookDirection)
{
  qos::InteractiveFrameInfoSource src;
  const QarVector3 f0 = src.forward();
  src.add_yaw(0.5f);
  const QarVector3 f1 = src.forward();
  const float d = len(sub(f1, f0));
  EXPECT_GT(d, 0.1f) << "yaw should rotate the look direction";
}

TEST(InteractiveFrameSource, PitchIsClamped)
{
  qos::InteractiveFrameInfoSource::Config cfg;
  cfg.max_pitch_rad = 1.0f;
  qos::InteractiveFrameInfoSource src(cfg);
  src.add_pitch(100.0f);   // absurd up
  EXPECT_NEAR(src.pitch(), 1.0f, 1e-5f);
  src.add_pitch(-100.0f);  // absurd down
  EXPECT_NEAR(src.pitch(), -1.0f, 1e-5f);
}

TEST(InteractiveFrameSource, ResetRestoresStartPose)
{
  qos::InteractiveFrameInfoSource::Config cfg;
  cfg.start_position = QarVector3{1.0f, 2.0f, 3.0f};
  cfg.start_yaw_rad = 0.25f;
  cfg.start_pitch_rad = -0.1f;
  qos::InteractiveFrameInfoSource src(cfg);

  src.move_forward(5.0f);
  src.add_yaw(1.0f);
  src.add_pitch(0.3f);
  src.reset();

  EXPECT_NEAR(src.head_position().x, 1.0f, 1e-5f);
  EXPECT_NEAR(src.head_position().y, 2.0f, 1e-5f);
  EXPECT_NEAR(src.head_position().z, 3.0f, 1e-5f);
  EXPECT_NEAR(src.yaw(), 0.25f, 1e-5f);
  EXPECT_NEAR(src.pitch(), -0.1f, 1e-5f);
}

TEST(InteractiveFrameSource, PerEyeFovAsymmetricAndAspectMatched)
{
  qos::InteractiveFrameInfoSource::Config cfg;
  cfg.aspect = 4.0f / 3.0f;
  cfg.fov_asymmetry_tan = 0.02f;
  qos::InteractiveFrameInfoSource src(cfg);
  const qos::QarFrameInfo f = src.current_frame(0);

  // Aspect preserved: (tanR - tanL) / (tanU - tanD) == aspect.
  auto aspect_of = [](const QarFov & v) {
    const float h = std::tan(v.angle_right) - std::tan(v.angle_left);
    const float w = std::tan(v.angle_up) - std::tan(v.angle_down);
    return h / w;
  };
  EXPECT_NEAR(aspect_of(f.left_eye.fov), cfg.aspect, 1e-4f);
  EXPECT_NEAR(aspect_of(f.right_eye.fov), cfg.aspect, 1e-4f);
  // Mirror-asymmetric between eyes.
  EXPECT_LT(f.left_eye.fov.angle_left, f.right_eye.fov.angle_left);
}
