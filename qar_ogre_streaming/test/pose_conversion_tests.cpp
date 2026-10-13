// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Pose-conversion tests. Qar and Ogre camera-local space share the frame
// (right-handed, X right, Y up, -Z forward), so pose conversion is the identity
// mapping: position copies through, orientation copies through (xyzw -> wxyz),
// with normalization. These tests lock in the numeric behavior (identity pose,
// known translation, known orientation) plus normalization and degenerate
// rejection.

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/qar_pose_conversion.hpp"

namespace qos = qar_ogre_streaming;

namespace
{
constexpr float kSqrtHalf = 0.70710678118654752440f;  // sqrt(1/2) = cos/sin(45deg)

// Rotate a 3-vector by an Ogre-ordered (w,x,y,z) unit quaternion using the
// standard v' = v + 2w(u x v) + 2(u x (u x v)) formulation, u = (x,y,z).
std::array<float, 3> rotate(const qos::Quat & q, std::array<float, 3> v)
{
  const std::array<float, 3> u{q.x, q.y, q.z};
  auto cross = [](const std::array<float, 3> & a, const std::array<float, 3> & b) {
    return std::array<float, 3>{
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0]};
  };
  const std::array<float, 3> t_half = cross(u, v);
  const std::array<float, 3> uxt = cross(u, t_half);
  return {
    v[0] + 2.0f * (q.w * t_half[0] + uxt[0]),
    v[1] + 2.0f * (q.w * t_half[1] + uxt[1]),
    v[2] + 2.0f * (q.w * t_half[2] + uxt[2])};
}

void expect_vec_near(const std::array<float, 3> & got, float x, float y, float z, float tol)
{
  EXPECT_NEAR(got[0], x, tol);
  EXPECT_NEAR(got[1], y, tol);
  EXPECT_NEAR(got[2], z, tol);
}
}  // namespace

// ---- Identity pose conversion ------------------------------------------------

TEST(PoseConversion, IdentityPose)
{
  QarPose pose{};
  pose.orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f};  // xyzw identity
  pose.position = QarVector3{0.0f, 0.0f, 0.0f};

  qos::Vec3 p;
  qos::Quat q;
  const qos::Result r = qos::qar_pose_to_ogre(pose, p, q);

  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_FLOAT_EQ(p.x, 0.0f);
  EXPECT_FLOAT_EQ(p.y, 0.0f);
  EXPECT_FLOAT_EQ(p.z, 0.0f);
  EXPECT_FLOAT_EQ(q.w, 1.0f);
  EXPECT_FLOAT_EQ(q.x, 0.0f);
  EXPECT_FLOAT_EQ(q.y, 0.0f);
  EXPECT_FLOAT_EQ(q.z, 0.0f);
}

// ---- Known translation conversion --------------------------------------------

TEST(PoseConversion, KnownTranslationPassesThrough)
{
  // Identity mapping: no axis swap, no sign flip.
  const qos::Vec3 v = qos::qar_position_to_ogre(QarVector3{1.5f, -2.25f, 3.75f});
  EXPECT_FLOAT_EQ(v.x, 1.5f);
  EXPECT_FLOAT_EQ(v.y, -2.25f);
  EXPECT_FLOAT_EQ(v.z, 3.75f);

  QarPose pose{};
  pose.orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
  pose.position = QarVector3{-4.0f, 5.0f, -6.0f};
  qos::Vec3 p;
  qos::Quat q;
  ASSERT_TRUE(qos::qar_pose_to_ogre(pose, p, q).ok());
  EXPECT_FLOAT_EQ(p.x, -4.0f);
  EXPECT_FLOAT_EQ(p.y, 5.0f);
  EXPECT_FLOAT_EQ(p.z, -6.0f);
}

// ---- Known orientation conversion --------------------------------------------
// Component-order (xyzw -> wxyz) plus a geometric check: rotate a basis vector
// and confirm the right-handed result. Right-handed convention (X right, Y up,
// -Z forward):
//   yaw   +90 about +Y : +X -> -Z
//   pitch +90 about +X : +Y -> +Z
//   roll  +90 about +Z : +X -> +Y

TEST(PoseConversion, KnownOrientationYaw90AboutY)
{
  // q_wxyz = (cos45, 0, sin45, 0); Qar stores xyzw = (0, sin45, 0, cos45).
  QarQuaternion q_in{0.0f, kSqrtHalf, 0.0f, kSqrtHalf};
  const qos::Quat q = qos::qar_orientation_to_ogre(q_in);
  EXPECT_NEAR(q.w, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.x, 0.0f, 1e-6f);
  EXPECT_NEAR(q.y, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.z, 0.0f, 1e-6f);
  expect_vec_near(rotate(q, {1.0f, 0.0f, 0.0f}), 0.0f, 0.0f, -1.0f, 1e-5f);
}

TEST(PoseConversion, KnownOrientationPitch90AboutX)
{
  // q_wxyz = (cos45, sin45, 0, 0); Qar xyzw = (sin45, 0, 0, cos45).
  QarQuaternion q_in{kSqrtHalf, 0.0f, 0.0f, kSqrtHalf};
  const qos::Quat q = qos::qar_orientation_to_ogre(q_in);
  EXPECT_NEAR(q.w, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.x, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.y, 0.0f, 1e-6f);
  EXPECT_NEAR(q.z, 0.0f, 1e-6f);
  expect_vec_near(rotate(q, {0.0f, 1.0f, 0.0f}), 0.0f, 0.0f, 1.0f, 1e-5f);
}

TEST(PoseConversion, KnownOrientationRoll90AboutZ)
{
  // q_wxyz = (cos45, 0, 0, sin45); Qar xyzw = (0, 0, sin45, cos45).
  QarQuaternion q_in{0.0f, 0.0f, kSqrtHalf, kSqrtHalf};
  const qos::Quat q = qos::qar_orientation_to_ogre(q_in);
  EXPECT_NEAR(q.w, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.x, 0.0f, 1e-6f);
  EXPECT_NEAR(q.y, 0.0f, 1e-6f);
  EXPECT_NEAR(q.z, kSqrtHalf, 1e-6f);
  expect_vec_near(rotate(q, {1.0f, 0.0f, 0.0f}), 0.0f, 1.0f, 0.0f, 1e-5f);
}

// ---- Non-unit quaternion normalization ---------------------------------------

TEST(PoseConversion, NonUnitQuaternionIsNormalized)
{
  // A yaw-90 quaternion scaled by 3; must normalize back to unit length.
  QarQuaternion q_in{0.0f, 3.0f * kSqrtHalf, 0.0f, 3.0f * kSqrtHalf};
  const qos::Quat q = qos::qar_orientation_to_ogre(q_in);
  const float norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  EXPECT_NEAR(norm, 1.0f, 1e-6f);
  EXPECT_NEAR(q.w, kSqrtHalf, 1e-6f);
  EXPECT_NEAR(q.y, kSqrtHalf, 1e-6f);
}

// ---- Degenerate rejection ----------------------------------------------------

TEST(PoseConversion, ConvenienceOrientationFallsBackToIdentityOnZero)
{
  QarQuaternion q_in{0.0f, 0.0f, 0.0f, 0.0f};
  const qos::Quat q = qos::qar_orientation_to_ogre(q_in);
  EXPECT_FLOAT_EQ(q.w, 1.0f);
  EXPECT_FLOAT_EQ(q.x, 0.0f);
  EXPECT_FLOAT_EQ(q.y, 0.0f);
  EXPECT_FLOAT_EQ(q.z, 0.0f);
}

TEST(PoseConversion, CheckedPoseRejectsZeroQuaternion)
{
  QarPose pose{};
  pose.orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 0.0f};
  pose.position = QarVector3{1.0f, 2.0f, 3.0f};
  qos::Vec3 p;
  qos::Quat q;
  const qos::Result r = qos::qar_pose_to_ogre(pose, p, q);
  EXPECT_EQ(r.status, qos::Status::InvalidArgument);
  EXPECT_FALSE(r.message.empty());
}
