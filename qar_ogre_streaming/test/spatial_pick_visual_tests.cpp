// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Feedback geometry: what the AR user sees before they commit. Pure — these
// never touch Ogre, exactly like the pose/projection suites.

#include "qar_ogre_streaming/spatial_pick_visual.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace
{

using qar_ogre_streaming::PickVisualGeometry;
using qar_ogre_streaming::PickVisualStyle;
using qar_ogre_streaming::Rgb;
using qar_ogre_streaming::Vec3;
using qar_ogre_streaming::append_circle;
using qar_ogre_streaming::append_point_marker;
using qar_ogre_streaming::append_polygon;
using qar_ogre_streaming::append_pose_marker;
using qar_ogre_streaming::append_ray;

constexpr float kTol = 1e-4f;
const Rgb kColor{1.0f, 0.5f, 0.0f};

float length_of(const qar_ogre_streaming::PickVisualSegment & s)
{
  const float dx = s.to.x - s.from.x;
  const float dy = s.to.y - s.from.y;
  const float dz = s.to.z - s.from.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

TEST(PickVisual, RayIsOneSegmentOfTheRequestedLength)
{
  PickVisualGeometry geometry;
  append_ray(geometry, Vec3{1.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, -3.0f}, 5.0f, kColor);
  ASSERT_EQ(geometry.segments.size(), 1u);
  EXPECT_NEAR(length_of(geometry.segments[0]), 5.0f, kTol);
  EXPECT_NEAR(geometry.segments[0].to.z, -3.0f, kTol);  // 2 - 5 along -Z
  EXPECT_FLOAT_EQ(geometry.segments[0].color.g, 0.5f);
}

TEST(PickVisual, DegenerateRayDrawsNothing)
{
  PickVisualGeometry geometry;
  append_ray(geometry, Vec3{0, 0, 0}, Vec3{0, 0, 0}, 5.0f, kColor);
  append_ray(geometry, Vec3{0, 0, 0}, Vec3{1, 0, 0}, 0.0f, kColor);
  append_ray(geometry, Vec3{0, 0, 0}, Vec3{std::nanf(""), 0, 0}, 5.0f, kColor);
  EXPECT_TRUE(geometry.empty());
}

TEST(PickVisual, PointMarkerIsAThreeAxisCross)
{
  PickVisualGeometry geometry;
  append_point_marker(geometry, Vec3{1.0f, 2.0f, 3.0f}, 0.4f, kColor);
  ASSERT_EQ(geometry.segments.size(), 3u);
  for (const auto & segment : geometry.segments) {
    EXPECT_NEAR(length_of(segment), 0.4f, kTol);
  }
  // One segment per axis, each centred on the position.
  EXPECT_NEAR(geometry.segments[0].from.x, 0.8f, kTol);
  EXPECT_NEAR(geometry.segments[1].from.y, 1.8f, kTol);
  EXPECT_NEAR(geometry.segments[2].from.z, 2.8f, kTol);
}

TEST(PickVisual, PoseMarkerIsACrossAShaftAndTwoBarbs)
{
  PickVisualGeometry geometry;
  PickVisualStyle style;
  append_pose_marker(geometry, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, style, kColor);
  ASSERT_EQ(geometry.segments.size(), 6u);  // 3 cross + shaft + 2 barbs
  const auto & shaft = geometry.segments[3];
  EXPECT_NEAR(shaft.to.x, style.arrow_length_m, kTol);  // yaw 0 points along +X
  EXPECT_NEAR(shaft.to.y, 0.0f, kTol);
  // The barbs start at the tip and fold backwards, so they end behind it.
  EXPECT_LT(geometry.segments[4].to.x, shaft.to.x);
  EXPECT_LT(geometry.segments[5].to.x, shaft.to.x);
  EXPECT_NEAR(geometry.segments[4].to.y, -geometry.segments[5].to.y, kTol);
}

TEST(PickVisual, PoseMarkerFollowsTheYaw)
{
  PickVisualGeometry geometry;
  PickVisualStyle style;
  append_pose_marker(geometry, Vec3{0, 0, 0}, 1.57079632679f, style, kColor);
  const auto & shaft = geometry.segments[3];
  EXPECT_NEAR(shaft.to.x, 0.0f, 1e-3f);
  EXPECT_NEAR(shaft.to.y, style.arrow_length_m, kTol);
}

TEST(PickVisual, PoseMarkerKeepsTheCrossWhenTheArrowIsSuppressed)
{
  PickVisualGeometry geometry;
  PickVisualStyle style;
  style.arrow_length_m = 0.0f;
  append_pose_marker(geometry, Vec3{0, 0, 0}, 0.0f, style, kColor);
  EXPECT_EQ(geometry.segments.size(), 3u);
}

TEST(PickVisual, PolygonClosesOnlyWhenAsked)
{
  const std::vector<Vec3> vertices{
    Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}};
  PickVisualGeometry open;
  append_polygon(open, vertices, false, kColor);
  EXPECT_EQ(open.segments.size(), 3u);

  PickVisualGeometry closed;
  append_polygon(closed, vertices, true, kColor);
  ASSERT_EQ(closed.segments.size(), 4u);
  EXPECT_NEAR(closed.segments.back().to.x, 0.0f, kTol);
  EXPECT_NEAR(closed.segments.back().to.y, 0.0f, kTol);
}

TEST(PickVisual, PolygonNeedsTwoGoodVertices)
{
  PickVisualGeometry geometry;
  append_polygon(geometry, {Vec3{0, 0, 0}}, true, kColor);
  EXPECT_TRUE(geometry.empty());
  append_polygon(geometry, {Vec3{0, 0, 0}, Vec3{std::nanf(""), 0, 0}}, true, kColor);
  EXPECT_TRUE(geometry.empty());
}

TEST(PickVisual, CircleIsClosedAndOnTheRim)
{
  PickVisualGeometry geometry;
  append_circle(geometry, Vec3{1.0f, 2.0f, 0.5f}, 3.0f, 8u, kColor);
  ASSERT_EQ(geometry.segments.size(), 8u);
  for (const auto & segment : geometry.segments) {
    const float dx = segment.from.x - 1.0f;
    const float dy = segment.from.y - 2.0f;
    EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), 3.0f, 1e-3f);
    EXPECT_NEAR(segment.from.z, 0.5f, kTol);
  }
  // Exactly closed: the last segment ends where the first began.
  EXPECT_NEAR(geometry.segments.back().to.x, geometry.segments.front().from.x, kTol);
  EXPECT_NEAR(geometry.segments.back().to.y, geometry.segments.front().from.y, kTol);
}

TEST(PickVisual, CircleRefusesDegenerateInput)
{
  PickVisualGeometry geometry;
  append_circle(geometry, Vec3{0, 0, 0}, 0.0f, 8u, kColor);
  append_circle(geometry, Vec3{0, 0, 0}, 1.0f, 2u, kColor);
  EXPECT_TRUE(geometry.empty());
}

TEST(PickVisual, GeometryAccumulatesAndClears)
{
  PickVisualGeometry geometry;
  append_ray(geometry, Vec3{0, 0, 2}, Vec3{0, 0, -1}, 2.0f, kColor);
  append_point_marker(geometry, Vec3{0, 0, 0}, 0.2f, kColor);
  EXPECT_EQ(geometry.segments.size(), 4u);
  EXPECT_FALSE(geometry.empty());
  geometry.clear();
  EXPECT_TRUE(geometry.empty());
}

// --- the cross-thread handoff ----------------------------------------------
//
// These drive the real class but never reach Ogre: with no scene manager,
// update() returns before its first Ogre call, which is what lets the staging
// contract be tested in the headless suite.

TEST(PickVisualHandoff, StagingIsConsumedExactlyOnce)
{
  qar_ogre_streaming::QarSpatialPickVisual visual;
  EXPECT_FALSE(visual.has_pending());
  EXPECT_FALSE(visual.initialized());

  PickVisualGeometry geometry;
  append_ray(geometry, Vec3{0, 0, 2}, Vec3{0, 0, -1}, 2.0f, kColor);
  visual.stage(geometry);
  EXPECT_TRUE(visual.has_pending());

  visual.apply_pending();
  EXPECT_FALSE(visual.has_pending());
  visual.apply_pending();  // a second call is a no-op, not a re-draw
  EXPECT_FALSE(visual.has_pending());
}

TEST(PickVisualHandoff, RestagingReplacesRatherThanQueues)
{
  qar_ogre_streaming::QarSpatialPickVisual visual;
  PickVisualGeometry first;
  append_point_marker(first, Vec3{0, 0, 0}, 0.2f, kColor);
  PickVisualGeometry second;
  append_point_marker(second, Vec3{5, 5, 5}, 0.2f, kColor);
  visual.stage(first);
  visual.stage(second);  // the newer frame wins; feedback must never lag
  EXPECT_TRUE(visual.has_pending());
  visual.apply_pending();
  EXPECT_FALSE(visual.has_pending());
}

TEST(PickVisualHandoff, InitializeRejectsANullSceneManager)
{
  qar_ogre_streaming::QarSpatialPickVisual visual;
  const auto result = visual.initialize(nullptr);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(visual.initialized());
}
