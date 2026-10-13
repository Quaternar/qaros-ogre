// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Projection tests (symmetric FOV, asymmetric FOV, near/far plane behavior) plus
// degenerate-input rejection and a numerical cross-check against Ogre's own
// frustum formula.
//
// Target convention: make_projection_from_qar_fov produces the classic OpenGL
// off-axis (glFrustum) matrix, which is identical to
//   Ogre::Frustum::setFrustumExtents(left, right, top, bottom) then
//   Ogre::Frustum::getProjectionMatrix()
// i.e. right-handed view space looking down -Z, clip-space depth in [-1, +1].
//
// Matrix storage is row-major: m[row * 4 + col].

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/qar_pose_conversion.hpp"

namespace qos = qar_ogre_streaming;

namespace
{
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;

// Multiply a row-major Mat4 by a homogeneous column vector (x,y,z,w).
std::array<float, 4> mul(const qos::Mat4 & m, float x, float y, float z, float w)
{
  std::array<float, 4> out{};
  const float v[4] = {x, y, z, w};
  for (int row = 0; row < 4; ++row) {
    float acc = 0.0f;
    for (int col = 0; col < 4; ++col) {
      acc += m.m[row * 4 + col] * v[col];
    }
    out[static_cast<size_t>(row)] = acc;
  }
  return out;
}

// Independent reimplementation of Ogre 1.12's perspective projection for a
// manually-set frustum extent, from OgreFrustum.cpp::updateFrustumImpl()
// (PT_PERSPECTIVE branch, non-infinite far, no oblique/reflection). This is the
// matrix Ogre::Frustum::getProjectionMatrix() returns for extents set via
// setFrustumExtents(left, right, top, bottom). Returned row-major.
//
// Source: OGRE OgreFrustum.cpp, updateFrustumImpl():
//   inv_w = 1/(right-left), inv_h = 1/(top-bottom), inv_d = 1/(far-near)
//   A = 2*near*inv_w,  B = 2*near*inv_h
//   C = (right+left)*inv_w,  D = (top+bottom)*inv_h
//   q = -(far+near)*inv_d,   qn = -2*(far*near)*inv_d
//   proj = ZERO, [0][0]=A, [0][2]=C, [1][1]=B, [1][2]=D,
//              [2][2]=q, [2][3]=qn, [3][2]=-1
qos::Mat4 ogre_reference_projection(
  float left, float right, float top, float bottom, float near_d, float far_d)
{
  const float inv_w = 1.0f / (right - left);
  const float inv_h = 1.0f / (top - bottom);
  const float inv_d = 1.0f / (far_d - near_d);
  qos::Mat4 m;
  for (float & e : m.m) {
    e = 0.0f;
  }
  m.m[0] = 2.0f * near_d * inv_w;        // [0][0] A
  m.m[2] = (right + left) * inv_w;       // [0][2] C
  m.m[5] = 2.0f * near_d * inv_h;        // [1][1] B
  m.m[6] = (top + bottom) * inv_h;       // [1][2] D
  m.m[10] = -(far_d + near_d) * inv_d;   // [2][2] q
  m.m[11] = -2.0f * far_d * near_d * inv_d;  // [2][3] qn
  m.m[14] = -1.0f;                       // [3][2]
  return m;
}
}  // namespace

// ---- Symmetric FOV vs gluPerspective -----------------------------------------

TEST(Projection, SymmetricFovMatchesGluPerspective)
{
  // gluPerspective(fovy=60deg, aspect=1.6, near, far).
  const float fovy = 60.0f * 3.14159265358979323846f / 180.0f;
  const float aspect = 1.6f;
  const float half_v = fovy * 0.5f;
  const float half_h = std::atan(aspect * std::tan(half_v));

  // Symmetric QarFov: left=-right, down=-up.
  QarFov fov{-half_h, half_h, half_v, -half_v};
  qos::Mat4 m;
  ASSERT_TRUE(qos::make_projection_from_qar_fov(fov, kNear, kFar, m).ok());

  // gluPerspective reference matrix (row-major):
  //   f_ = cot(fovy/2) = 1/tan(fovy/2)
  //   [0][0] = f_/aspect   [1][1] = f_
  //   [2][2] = (far+near)/(near-far)   [2][3] = 2*far*near/(near-far)
  //   [3][2] = -1
  const float f_ = 1.0f / std::tan(half_v);
  EXPECT_NEAR(m.m[0], f_ / aspect, 1e-5f);
  EXPECT_NEAR(m.m[5], f_, 1e-5f);
  EXPECT_NEAR(m.m[2], 0.0f, 1e-6f);   // no shear when symmetric
  EXPECT_NEAR(m.m[6], 0.0f, 1e-6f);
  EXPECT_NEAR(m.m[10], (kFar + kNear) / (kNear - kFar), 1e-5f);
  EXPECT_NEAR(m.m[11], 2.0f * kFar * kNear / (kNear - kFar), 1e-5f);
  EXPECT_NEAR(m.m[14], -1.0f, 1e-6f);
  EXPECT_NEAR(m.m[15], 0.0f, 1e-6f);
}

// ---- Asymmetric FOV, hand-computed entries -----------------------------------

TEST(Projection, AsymmetricFovHandComputedEntries)
{
  // Concrete asymmetric FOV (radians): left=-0.6, right=0.4, down=-0.3, up=0.5.
  // near=0.1, far=100. Extents on the near plane = near * tan(angle):
  //   left   = 0.1 * tan(-0.6) = 0.1 * -0.68413681 = -0.068413681
  //   right  = 0.1 * tan( 0.4) = 0.1 *  0.42279322 =  0.042279322
  //   bottom = 0.1 * tan(-0.3) = 0.1 * -0.30933625 = -0.030933625
  //   top    = 0.1 * tan( 0.5) = 0.1 *  0.54630249 =  0.054630249
  //   r-l = 0.110693003   t-b = 0.085563874
  // glFrustum entries:
  //   m[0]  = 2n/(r-l)     = 0.2/0.110693003 = 1.8068013
  //   m[2]  = (r+l)/(r-l)  = -0.026134359/0.110693003 = -0.2361072
  //   m[5]  = 2n/(t-b)     = 0.2/0.085563874 = 2.3374010
  //   m[6]  = (t+b)/(t-b)  = 0.023696624/0.085563874 = 0.2769419
  //   m[10] = -(f+n)/(f-n) = -100.1/99.9 = -1.0020020
  //   m[11] = -2fn/(f-n)   = -20/99.9    = -0.2002002
  //   m[14] = -1
  QarFov fov{-0.6f, 0.4f, 0.5f, -0.3f};
  qos::Mat4 m;
  ASSERT_TRUE(qos::make_projection_from_qar_fov(fov, kNear, kFar, m).ok());

  EXPECT_NEAR(m.m[0], 1.8068013f, 1e-4f);
  EXPECT_NEAR(m.m[2], -0.2361072f, 1e-4f);
  EXPECT_NEAR(m.m[5], 2.3374010f, 1e-4f);
  EXPECT_NEAR(m.m[6], 0.2769419f, 1e-4f);
  EXPECT_NEAR(m.m[10], -1.0020020f, 1e-5f);
  EXPECT_NEAR(m.m[11], -0.2002002f, 1e-5f);
  EXPECT_NEAR(m.m[14], -1.0f, 1e-6f);

  // Off-axis shear signs: a right edge wider than the left gives (r+l) < 0 and so
  // m[2] < 0; an up edge wider than the down edge gives (t+b) > 0 and so m[6] > 0.
  EXPECT_LT(m.m[2], 0.0f);
  EXPECT_GT(m.m[6], 0.0f);
}

// ---- Near/far plane behavior -------------------------------------------------

TEST(Projection, NearFarPlaneDepthMapping)
{
  QarFov fov{-0.5f, 0.5f, 0.5f, -0.5f};
  qos::Mat4 m;
  ASSERT_TRUE(qos::make_projection_from_qar_fov(fov, kNear, kFar, m).ok());

  // A point on the view axis at z = -near maps to NDC z = -1 after divide.
  const auto clip_near = mul(m, 0.0f, 0.0f, -kNear, 1.0f);
  ASSERT_GT(std::abs(clip_near[3]), 1e-9f);
  EXPECT_NEAR(clip_near[2] / clip_near[3], -1.0f, 1e-5f);

  // A point on the view axis at z = -far maps to NDC z = +1 after divide.
  const auto clip_far = mul(m, 0.0f, 0.0f, -kFar, 1.0f);
  ASSERT_GT(std::abs(clip_far[3]), 1e-9f);
  EXPECT_NEAR(clip_far[2] / clip_far[3], 1.0f, 1e-5f);

  // Perspective w equals the positive view-space depth (-z) for both.
  EXPECT_NEAR(clip_near[3], kNear, 1e-6f);
  EXPECT_NEAR(clip_far[3], kFar, 1e-4f);
}

// ---- Numerical validation against Ogre's own formula -------------------------

TEST(Projection, MatchesOgreFrustumSymmetric)
{
  QarFov fov{-0.5f, 0.5f, 0.5f, -0.5f};
  qos::Mat4 mine;
  ASSERT_TRUE(qos::make_projection_from_qar_fov(fov, kNear, kFar, mine).ok());

  const float l = kNear * std::tan(fov.angle_left);
  const float r = kNear * std::tan(fov.angle_right);
  const float t = kNear * std::tan(fov.angle_up);
  const float b = kNear * std::tan(fov.angle_down);
  const qos::Mat4 ogre = ogre_reference_projection(l, r, t, b, kNear, kFar);

  for (int i = 0; i < 16; ++i) {
    EXPECT_NEAR(mine.m[i], ogre.m[i], 1e-5f) << "entry " << i;
  }
}

TEST(Projection, MatchesOgreFrustumAsymmetric)
{
  QarFov fov{-0.6f, 0.4f, 0.5f, -0.3f};
  qos::Mat4 mine;
  ASSERT_TRUE(qos::make_projection_from_qar_fov(fov, kNear, kFar, mine).ok());

  const float l = kNear * std::tan(fov.angle_left);
  const float r = kNear * std::tan(fov.angle_right);
  const float t = kNear * std::tan(fov.angle_up);
  const float b = kNear * std::tan(fov.angle_down);
  const qos::Mat4 ogre = ogre_reference_projection(l, r, t, b, kNear, kFar);

  for (int i = 0; i < 16; ++i) {
    EXPECT_NEAR(mine.m[i], ogre.m[i], 1e-5f) << "entry " << i;
  }
}

// ---- Degenerate-input rejection ----------------------------------------------

TEST(Projection, RejectsBadNearFar)
{
  QarFov fov{-0.5f, 0.5f, 0.5f, -0.5f};
  qos::Mat4 m;
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(fov, 0.0f, 100.0f, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(fov, -1.0f, 100.0f, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(fov, 10.0f, 5.0f, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(fov, 10.0f, 10.0f, m).status,
    qos::Status::InvalidArgument);  // far == near
}

TEST(Projection, RejectsBadHorizontalAngles)
{
  qos::Mat4 m;
  // angle_left >= angle_right
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{0.5f, 0.5f, 0.5f, -0.5f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{0.6f, 0.4f, 0.5f, -0.5f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
}

TEST(Projection, RejectsBadVerticalAngles)
{
  qos::Mat4 m;
  // angle_down >= angle_up
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{-0.5f, 0.5f, 0.5f, 0.5f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{-0.5f, 0.5f, 0.3f, 0.6f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
}

TEST(Projection, RejectsOutOfRangeAngles)
{
  qos::Mat4 m;
  // |angle| >= pi/2 where tan is undefined / non-monotonic.
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{-1.6f, 0.5f, 0.5f, -0.5f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
  EXPECT_EQ(
    qos::make_projection_from_qar_fov(QarFov{-0.5f, 1.6f, 0.5f, -0.5f}, kNear, kFar, m).status,
    qos::Status::InvalidArgument);
}
