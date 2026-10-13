// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Spatial picks: the maths, the state machines, and the degenerate cases
// that would otherwise publish a nav goal nobody asked for.

#include "qar_ar_core/spatial_pick.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using qar_ar_core::AreaPickResult;
using qar_ar_core::AreaPickSession;
using qar_ar_core::AreaShape;
using qar_ar_core::NavGoalOptions;
using qar_ar_core::PanelPointerSample;
using qar_ar_core::PickState;
using qar_ar_core::PickTarget;
using qar_ar_core::PickMethod;
using qar_ar_core::PickTargetKind;
using qar_ar_core::PointPickSession;
using qar_ar_core::PointerGesture;
using qar_ar_core::PointerPhase;
using qar_ar_core::PosePickConfig;
using qar_ar_core::PosePickSession;
using qar_ar_core::RoomToRosTransform;
using qar_ar_core::SpatialPointer;
using qar_ar_core::make_circle;
using qar_ar_core::make_nav_goal;
using qar_ar_core::make_polygon;
using qar_ar_core::make_rectangle;
using qar_ar_core::plane_basis;
using qar_ar_core::resolve_pick_point;
using qar_ar_core::spatial_pointer_from_sample;
using qar_ar_core::yaw_from_quaternion;
using qar_ar_core::yaw_to_quaternion;

constexpr float kTol = 1e-4f;
constexpr float kHalfPi = 1.57079632679f;

/// A pointer already in the ROS frame, carrying a 3D point (the app-volume
/// gesture path).
SpatialPointer point_pointer(
  float x, float y, float z, PointerPhase phase,
  PointerGesture gesture = PointerGesture::Click, std::uint32_t id = 1)
{
  SpatialPointer p;
  p.valid = true;
  p.pointer_id = id;
  p.phase = phase;
  p.gesture = gesture;
  p.has_point = true;
  p.point = QarVector3{x, y, z};
  // A runtime action point that HIT something -- which is what this helper
  // stands for. The flag is explicit because the ActionPoint target refuses a
  // non-hit (the runtime's "start point when nothing was intersected", i.e. the
  // hand).
  p.point_is_scene_hit = true;
  return p;
}

SpatialPointer ray_pointer(
  const QarVector3 & origin, const QarVector3 & direction,
  PointerPhase phase = PointerPhase::Updated)
{
  SpatialPointer p;
  p.valid = true;
  p.pointer_id = 1;
  p.phase = phase;
  p.gesture = PointerGesture::Click;
  p.has_ray = true;
  p.ray_origin = origin;
  p.ray_direction = direction;
  return p;
}

void expect_vec(const QarVector3 & actual, float x, float y, float z)
{
  EXPECT_NEAR(actual.x, x, kTol);
  EXPECT_NEAR(actual.y, y, kTol);
  EXPECT_NEAR(actual.z, z, kTol);
}

}  // namespace

// ---------------------------------------------------------------------------
// Sample -> spatial pointer
// ---------------------------------------------------------------------------

TEST(SpatialPointerFromSample, RoomPointCrossesTheFrameChange)
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = 0.0f;
  PanelPointerSample sample;
  sample.pointer_id = 7;
  sample.phase = PointerPhase::Started;
  sample.gesture = PointerGesture::Click;
  sample.has_room_point = true;
  sample.room_point = QarVector3{1.0f, 2.0f, 3.0f};

  const SpatialPointer p = spatial_pointer_from_sample(sample, t);
  EXPECT_TRUE(p.valid);
  EXPECT_TRUE(p.has_point);
  EXPECT_EQ(p.pointer_id, 7u);
  EXPECT_EQ(p.phase, PointerPhase::Started);
  expect_vec(p.point, -3.0f, -1.0f, 2.0f);
}

TEST(SpatialPointerFromSample, RoomRayCrossesTheFrameChange)
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = 0.0f;
  PanelPointerSample sample;
  sample.has_room_ray = true;
  sample.ray_origin = QarVector3{0.0f, 1.0f, 0.0f};
  sample.ray_direction = QarVector3{0.0f, 0.0f, -1.0f};

  const SpatialPointer p = spatial_pointer_from_sample(sample, t);
  EXPECT_TRUE(p.valid);
  EXPECT_TRUE(p.has_ray);
  expect_vec(p.ray_origin, 0.0f, 0.0f, 1.0f);
  expect_vec(p.ray_direction, 1.0f, 0.0f, 0.0f);
}

TEST(SpatialPointerFromSample, PanelUvIsNotASpatialPick)
{
  PanelPointerSample sample;
  sample.has_panel_uv = true;
  sample.panel_key = "displays";
  sample.u = 0.5f;
  sample.v = 0.5f;
  const SpatialPointer p = spatial_pointer_from_sample(sample, RoomToRosTransform{});
  EXPECT_FALSE(p.valid);
}

// ---------------------------------------------------------------------------
// Point resolution
// ---------------------------------------------------------------------------

TEST(ResolvePickPoint, RayMeetsTheGroundPlane)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(), ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, -1.0f}));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 2.0f, 0.0f, 0.0f);
  EXPECT_TRUE(out.from_ray);
  EXPECT_NEAR(out.distance_m, std::sqrt(8.0f), kTol);
  EXPECT_EQ(out.target, PickTargetKind::GroundPlane);
  EXPECT_EQ(out.target_name, "ground plane");
}

TEST(ResolvePickPoint, RaysDirectionNeedNotBeNormalized)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{100.0f, 0.0f, -100.0f}));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 2.0f, 0.0f, 0.0f);
}

TEST(ResolvePickPoint, RayParallelToThePlaneIsRefused)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(), ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, 0.0f}));
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.error, "ray is parallel to the target plane");
}

TEST(ResolvePickPoint, PlaneBehindThePointerIsRefused)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(), ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, 1.0f}));
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.error, "target plane is behind the pointer");
}

TEST(ResolvePickPoint, ZeroLengthDirectionIsRefused)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(), ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, 0.0f}));
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.error, "ray origin or direction is degenerate");
}

TEST(ResolvePickPoint, NonFiniteDirectionIsRefused)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{std::nanf(""), 0.0f, -1.0f}));
  EXPECT_FALSE(out.valid);
}

TEST(ResolvePickPoint, ArbitraryPlane)
{
  const auto target =
    PickTarget::plane(QarVector3{5.0f, 0.0f, 0.0f}, QarVector3{1.0f, 0.0f, 0.0f}, "wall");
  const auto out = resolve_pick_point(
    target, ray_pointer(QarVector3{0.0f, 0.0f, 1.0f}, QarVector3{1.0f, 0.0f, 0.0f}));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 5.0f, 0.0f, 1.0f);
  EXPECT_EQ(out.target_name, "wall");
}

TEST(ResolvePickPoint, DegeneratePlaneNormalIsRefused)
{
  const auto target =
    PickTarget::plane(QarVector3{0.0f, 0.0f, 0.0f}, QarVector3{0.0f, 0.0f, 0.0f});
  const auto out = resolve_pick_point(
    target, ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, -1.0f}));
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.error, "target plane normal is degenerate");
}

TEST(ResolvePickPoint, FixedDepthAlongTheRay)
{
  const auto out = resolve_pick_point(
    PickTarget::at_depth(3.0f),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, 0.0f}));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 3.0f, 0.0f, 2.0f);
  EXPECT_NEAR(out.distance_m, 3.0f, kTol);
}

TEST(ResolvePickPoint, NonPositiveDepthIsRefused)
{
  const auto out = resolve_pick_point(
    PickTarget::at_depth(0.0f),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, 0.0f}));
  EXPECT_FALSE(out.valid);
}

TEST(ResolvePickPoint, AGroundTargetProjectsTheActionPointOntoThePlane)
{
  const auto out =
    resolve_pick_point(PickTarget::ground(), point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 1.0f, 2.0f, 0.0f);
  EXPECT_FALSE(out.from_ray);
  EXPECT_NEAR(out.distance_m, 3.0f, kTol);  // how far off the plane it sat
}

TEST(ResolvePickPoint, TheActionPointTargetKeepsTheDepthTheRuntimeResolved)
{
  const auto out = resolve_pick_point(
    PickTarget::action_point(), point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 1.0f, 2.0f, 3.0f);
}

// ---------------------------------------------------------------------------
// The ActionPoint target GATES on the scene-hit flag. The header of the drop after
// the minimum drop says the action point is "the start point when nothing was
// intersected" -- the operator's hand -- so publishing a non-hit verbatim would put
// the marker on the hand.
// ---------------------------------------------------------------------------

TEST(ResolvePickPoint, TheActionPointTargetPublishesASceneHitVerbatim)
{
  SpatialPointer p = point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant);
  p.point_is_scene_hit = true;  // explicit: this test is ABOUT the flag
  const auto out = resolve_pick_point(PickTarget::action_point(), p);
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 1.0f, 2.0f, 3.0f);
  EXPECT_EQ(out.method, PickMethod::ActionPoint);
  EXPECT_TRUE(out.from_scene_hit);
  EXPECT_FALSE(out.refused_no_intersection);
}

TEST(ResolvePickPoint, TheActionPointTargetRefusesAPointTheRuntimeDidNotIntersect)
{
  SpatialPointer p = point_pointer(0.4f, 1.3f, -0.2f, PointerPhase::Instant);
  p.point_is_scene_hit = false;  // action == start: the hand, not a hit
  const auto out = resolve_pick_point(PickTarget::action_point(), p);
  EXPECT_FALSE(out.valid);
  EXPECT_TRUE(out.refused_no_intersection);
  EXPECT_NE(out.error.find("intersected nothing"), std::string::npos) << out.error;
  // A refused pick publishes nothing: no position may leak out as the answer.
  expect_vec(out.position, 0.0f, 0.0f, 0.0f);
}

TEST(ResolvePickPoint, TheDepthTargetStillPublishesANonHitPointUnchanged)
{
  // The gate is ActionPoint's alone: a Depth target with no ray still publishes
  // the reported point unchanged, scene hit or not.
  SpatialPointer p = point_pointer(0.4f, 1.3f, -0.2f, PointerPhase::Instant);
  p.point_is_scene_hit = false;
  const auto out = resolve_pick_point(PickTarget::at_depth(2.0f), p);
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 0.4f, 1.3f, -0.2f);
  EXPECT_EQ(out.method, PickMethod::ActionPoint);
  EXPECT_FALSE(out.from_scene_hit);
  EXPECT_FALSE(out.refused_no_intersection);
}

TEST(ResolvePickPoint, TheActionPointTargetRefusesARayOnlyPointer)
{
  const auto out = resolve_pick_point(
    PickTarget::action_point(),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, -1.0f}));
  EXPECT_FALSE(out.valid);
}

TEST(ResolvePickPoint, RayWinsOverAReportedPointForAPlaneTarget)
{
  SpatialPointer p = ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, -1.0f});
  p.has_point = true;
  p.point = QarVector3{9.0f, 9.0f, 9.0f};
  const auto out = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(out.valid) << out.error;
  EXPECT_TRUE(out.from_ray);
  expect_vec(out.position, 0.0f, 0.0f, 0.0f);
}

TEST(ResolvePickPoint, EmptyPointerIsRefused)
{
  SpatialPointer p;
  p.valid = true;  // claims validity but carries nothing
  const auto out = resolve_pick_point(PickTarget::ground(), p);
  EXPECT_FALSE(out.valid);
}

TEST(ResolvePickPoint, GroundPlaneHeightIsHonoured)
{
  const auto out = resolve_pick_point(
    PickTarget::ground(1.25f),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{0.0f, 0.0f, -1.0f}));
  ASSERT_TRUE(out.valid) << out.error;
  expect_vec(out.position, 0.0f, 0.0f, 1.25f);
  EXPECT_NEAR(out.distance_m, 0.75f, kTol);
}

// ---------------------------------------------------------------------------
// Point pick session
// ---------------------------------------------------------------------------

TEST(PointPickSession, PressUpdateReleaseCommits)
{
  PointPickSession session;
  EXPECT_EQ(session.state(), PickState::Idle);
  EXPECT_TRUE(session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Started)));
  EXPECT_EQ(session.state(), PickState::Pending);
  EXPECT_FALSE(session.result().valid);

  EXPECT_TRUE(session.update(point_pointer(2.0f, 3.0f, 1.0f, PointerPhase::Updated)));
  expect_vec(session.preview().position, 2.0f, 3.0f, 0.0f);

  EXPECT_TRUE(session.update(point_pointer(2.0f, 4.0f, 1.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Committed);
  ASSERT_TRUE(session.result().valid);
  expect_vec(session.result().position, 2.0f, 4.0f, 0.0f);
}

TEST(PointPickSession, InstantTapCommitsInOneEvent)
{
  PointPickSession session;
  EXPECT_TRUE(session.update(point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant)));
  EXPECT_EQ(session.state(), PickState::Committed);
  expect_vec(session.result().position, 1.0f, 2.0f, 0.0f);
}

TEST(PointPickSession, CancelPhaseCommitsNothing)
{
  PointPickSession session;
  session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Started));
  EXPECT_TRUE(session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Canceled)));
  EXPECT_EQ(session.state(), PickState::Canceled);
  EXPECT_FALSE(session.result().valid);
}

TEST(PointPickSession, EndedWithoutStartedIsIgnored)
{
  PointPickSession session;
  EXPECT_FALSE(session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Idle);
  EXPECT_FALSE(session.result().valid);
}

TEST(PointPickSession, UpdatedClickWithoutStartedIsIgnored)
{
  PointPickSession session;
  EXPECT_FALSE(session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Updated)));
  EXPECT_EQ(session.state(), PickState::Idle);
}

TEST(PointPickSession, HoverPreviewsWithoutArming)
{
  PointPickSession session;
  EXPECT_TRUE(
    session.update(
      point_pointer(4.0f, 5.0f, 1.0f, PointerPhase::Updated, PointerGesture::Hover)));
  EXPECT_EQ(session.state(), PickState::Idle);
  expect_vec(session.preview().position, 4.0f, 5.0f, 0.0f);
  EXPECT_FALSE(session.result().valid);
}

TEST(PointPickSession, ASecondPointerCannotStealAGestureInFlight)
{
  PointPickSession session;
  session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Started, PointerGesture::Click, 1));
  EXPECT_FALSE(
    session.update(
      point_pointer(9.0f, 9.0f, 1.0f, PointerPhase::Updated, PointerGesture::Click, 2)));
  expect_vec(session.preview().position, 1.0f, 1.0f, 0.0f);
  EXPECT_EQ(session.state(), PickState::Pending);
}

TEST(PointPickSession, ResetReturnsToIdle)
{
  PointPickSession session;
  session.update(point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Instant));
  session.reset();
  EXPECT_EQ(session.state(), PickState::Idle);
  EXPECT_FALSE(session.result().valid);
}

// ---------------------------------------------------------------------------
// Pose pick session
// ---------------------------------------------------------------------------

TEST(PosePickSession, PressPositionsAndDragOrients)
{
  PosePickSession session;
  ASSERT_TRUE(session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started)));
  EXPECT_EQ(session.state(), PickState::Pending);
  expect_vec(session.anchor().position, 0.0f, 0.0f, 0.0f);

  // A 1 m drag along +Y is a heading of +90 deg.
  ASSERT_TRUE(session.update(point_pointer(0.0f, 1.0f, 0.0f, PointerPhase::Updated)));
  EXPECT_EQ(session.state(), PickState::Orienting);
  EXPECT_NEAR(session.preview().yaw_rad, kHalfPi, kTol);
  EXPECT_TRUE(session.preview().heading_from_drag);
  // The position stays where the press fixed it: only the heading follows the drag.
  expect_vec(session.preview().position, 0.0f, 0.0f, 0.0f);

  ASSERT_TRUE(session.update(point_pointer(0.0f, 1.0f, 0.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Committed);
  ASSERT_TRUE(session.result().valid);
  EXPECT_NEAR(session.result().yaw_rad, kHalfPi, kTol);
  EXPECT_NEAR(session.result().drag_length_m, 1.0f, kTol);
  // Yaw-only quaternion about ROS +Z.
  EXPECT_NEAR(session.result().orientation.x, 0.0f, kTol);
  EXPECT_NEAR(session.result().orientation.y, 0.0f, kTol);
  EXPECT_NEAR(session.result().orientation.z, 0.70710678f, kTol);
  EXPECT_NEAR(session.result().orientation.w, 0.70710678f, kTol);
}

TEST(PosePickSession, HeadingAlongPlusXIsYawZero)
{
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(2.0f, 0.0f, 0.0f, PointerPhase::Ended));
  ASSERT_TRUE(session.result().valid);
  EXPECT_NEAR(session.result().yaw_rad, 0.0f, kTol);
  EXPECT_NEAR(yaw_from_quaternion(session.result().orientation), 0.0f, kTol);
}

TEST(PosePickSession, ZeroLengthDragCommitsWithTheDefaultHeading)
{
  PosePickSession session;
  session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Ended));
  EXPECT_EQ(session.state(), PickState::Committed);
  ASSERT_TRUE(session.result().valid);
  EXPECT_FALSE(session.result().heading_from_drag);
  EXPECT_NEAR(session.result().yaw_rad, 0.0f, kTol);
}

TEST(PosePickSession, ADragUnderTheThresholdIsNotAHeading)
{
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(0.01f, 0.0f, 0.0f, PointerPhase::Updated));
  EXPECT_EQ(session.state(), PickState::Pending);
  EXPECT_FALSE(session.preview().heading_from_drag);
}

TEST(PosePickSession, HeadingIsRequiredWhenConfigured)
{
  PosePickConfig config;
  config.commit_without_heading = false;
  PosePickSession session(PickTarget::ground(), config);
  session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Ended));
  EXPECT_EQ(session.state(), PickState::Canceled);
  EXPECT_FALSE(session.result().valid);
}

TEST(PosePickSession, DefaultYawIsUsedForATap)
{
  PosePickConfig config;
  config.default_yaw_rad = kHalfPi;
  PosePickSession session(PickTarget::ground(), config);
  ASSERT_TRUE(session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Instant)));
  EXPECT_EQ(session.state(), PickState::Committed);
  EXPECT_NEAR(session.result().yaw_rad, kHalfPi, kTol);
  EXPECT_FALSE(session.result().heading_from_drag);
}

TEST(PosePickSession, CancelMidDragProducesNothing)
{
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Updated));
  ASSERT_EQ(session.state(), PickState::Orienting);
  ASSERT_TRUE(session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Canceled)));
  EXPECT_EQ(session.state(), PickState::Canceled);
  EXPECT_FALSE(session.result().valid);
}

TEST(PosePickSession, OutOfOrderUpdateIsIgnored)
{
  PosePickSession session;
  EXPECT_FALSE(session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Updated)));
  EXPECT_FALSE(session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Idle);
}

TEST(PosePickSession, DriftAlongTheNormalDoesNotCountAsADrag)
{
  // The pick lands on the ground plane, so a hand that rises does not turn the
  // pose: the projected travel is zero.
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(0.0f, 0.0f, 2.0f, PointerPhase::Updated));
  EXPECT_EQ(session.state(), PickState::Pending);
  EXPECT_NEAR(session.preview().drag_length_m, 0.0f, kTol);
}

TEST(PosePickSession, RestartingAPressReanchors)
{
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Updated));
  session.update(point_pointer(5.0f, 5.0f, 0.0f, PointerPhase::Started));
  EXPECT_EQ(session.state(), PickState::Pending);
  expect_vec(session.anchor().position, 5.0f, 5.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Plane basis + areas
// ---------------------------------------------------------------------------

TEST(PlaneBasis, GroundPlaneIsWorldXThenY)
{
  QarVector3 u{}, v{};
  ASSERT_TRUE(plane_basis(QarVector3{0.0f, 0.0f, 1.0f}, u, v));
  expect_vec(u, 1.0f, 0.0f, 0.0f);
  expect_vec(v, 0.0f, 1.0f, 0.0f);
}

TEST(PlaneBasis, VerticalWallGetsAnUpwardSecondAxis)
{
  QarVector3 u{}, v{};
  ASSERT_TRUE(plane_basis(QarVector3{1.0f, 0.0f, 0.0f}, u, v));
  expect_vec(u, 0.0f, 1.0f, 0.0f);
  expect_vec(v, 0.0f, 0.0f, 1.0f);
}

TEST(PlaneBasis, DegenerateNormalIsRefused)
{
  QarVector3 u{}, v{};
  EXPECT_FALSE(plane_basis(QarVector3{0.0f, 0.0f, 0.0f}, u, v));
}

TEST(AreaPick, RectangleCornersAndArea)
{
  const AreaPickResult r = make_rectangle(
    PickTarget::ground(), QarVector3{0.0f, 0.0f, 0.0f}, QarVector3{2.0f, 3.0f, 0.0f});
  ASSERT_TRUE(r.valid) << r.error;
  ASSERT_EQ(r.vertices.size(), 4u);
  expect_vec(r.vertices[0], 0.0f, 0.0f, 0.0f);
  expect_vec(r.vertices[1], 2.0f, 0.0f, 0.0f);
  expect_vec(r.vertices[2], 2.0f, 3.0f, 0.0f);
  expect_vec(r.vertices[3], 0.0f, 3.0f, 0.0f);
  expect_vec(r.center, 1.0f, 1.5f, 0.0f);
  EXPECT_NEAR(r.width_m, 2.0f, kTol);
  EXPECT_NEAR(r.height_m, 3.0f, kTol);
  EXPECT_NEAR(r.area_m2, 6.0f, kTol);
}

TEST(AreaPick, RectangleSweptBackwardsStaysCounterClockwise)
{
  const AreaPickResult r = make_rectangle(
    PickTarget::ground(), QarVector3{0.0f, 0.0f, 0.0f}, QarVector3{-2.0f, 3.0f, 0.0f});
  ASSERT_TRUE(r.valid) << r.error;
  ASSERT_EQ(r.vertices.size(), 4u);
  double twice_area = 0.0;
  for (std::size_t i = 0; i < r.vertices.size(); ++i) {
    const QarVector3 & p = r.vertices[i];
    const QarVector3 & q = r.vertices[(i + 1) % r.vertices.size()];
    twice_area += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
  }
  EXPECT_GT(twice_area, 0.0);  // positive shoelace == counter-clockwise about +Z
  EXPECT_NEAR(r.area_m2, 6.0f, kTol);
}

TEST(AreaPick, RectangleWithZeroExtentIsRefused)
{
  const AreaPickResult r = make_rectangle(
    PickTarget::ground(), QarVector3{1.0f, 1.0f, 0.0f}, QarVector3{1.0f, 4.0f, 0.0f});
  EXPECT_FALSE(r.valid);
  EXPECT_EQ(r.error, "rectangle has zero extent in one direction");
}

TEST(AreaPick, CircleFromCentreAndRim)
{
  const AreaPickResult r = make_circle(
    PickTarget::ground(), QarVector3{1.0f, 1.0f, 0.0f}, QarVector3{1.0f, 4.0f, 0.0f});
  ASSERT_TRUE(r.valid) << r.error;
  EXPECT_NEAR(r.radius_m, 3.0f, kTol);
  EXPECT_NEAR(r.area_m2, 3.14159265f * 9.0f, 1e-3f);
  EXPECT_TRUE(r.vertices.empty());
  expect_vec(r.center, 1.0f, 1.0f, 0.0f);
}

TEST(AreaPick, CircleWithZeroRadiusIsRefused)
{
  const AreaPickResult r = make_circle(
    PickTarget::ground(), QarVector3{1.0f, 1.0f, 0.0f}, QarVector3{1.0f, 1.0f, 0.0f});
  EXPECT_FALSE(r.valid);
}

TEST(AreaPick, PolygonAreaByShoelace)
{
  const std::vector<QarVector3> vertices{
    QarVector3{0.0f, 0.0f, 0.0f}, QarVector3{4.0f, 0.0f, 0.0f}, QarVector3{4.0f, 3.0f, 0.0f}};
  const AreaPickResult r = make_polygon(PickTarget::ground(), vertices);
  ASSERT_TRUE(r.valid) << r.error;
  EXPECT_NEAR(r.area_m2, 6.0f, kTol);
  ASSERT_EQ(r.vertices.size(), 3u);
}

TEST(AreaPick, PolygonNeedsThreeVertices)
{
  const std::vector<QarVector3> vertices{
    QarVector3{0.0f, 0.0f, 0.0f}, QarVector3{1.0f, 0.0f, 0.0f}};
  const AreaPickResult r = make_polygon(PickTarget::ground(), vertices);
  EXPECT_FALSE(r.valid);
  EXPECT_EQ(r.error, "a polygon needs at least three vertices");
}

TEST(AreaPickSessionTest, SweepARectangleAndCommit)
{
  AreaPickSession session;
  ASSERT_TRUE(session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started)));
  EXPECT_EQ(session.state(), PickState::Pending);
  ASSERT_TRUE(session.update(point_pointer(2.0f, 3.0f, 0.0f, PointerPhase::Updated)));
  EXPECT_EQ(session.state(), PickState::Orienting);
  ASSERT_TRUE(session.update(point_pointer(2.0f, 3.0f, 0.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Committed);
  ASSERT_TRUE(session.result().valid);
  EXPECT_NEAR(session.result().area_m2, 6.0f, kTol);
}

TEST(AreaPickSessionTest, ZeroLengthSweepIsRefusedRatherThanPublished)
{
  AreaPickSession session;
  session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Started));
  ASSERT_TRUE(session.update(point_pointer(1.0f, 1.0f, 0.0f, PointerPhase::Ended)));
  EXPECT_EQ(session.state(), PickState::Canceled);
  EXPECT_FALSE(session.result().valid);
}

TEST(AreaPickSessionTest, CircleShape)
{
  AreaPickSession session(PickTarget::ground(), AreaShape::Circle);
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Started));
  session.update(point_pointer(0.0f, 2.0f, 0.0f, PointerPhase::Ended));
  ASSERT_TRUE(session.result().valid);
  EXPECT_EQ(session.result().shape, AreaShape::Circle);
  EXPECT_NEAR(session.result().radius_m, 2.0f, kTol);
}

TEST(AreaPickSessionTest, PolygonCollectsTapsAndClosesExplicitly)
{
  AreaPickSession session(PickTarget::ground(), AreaShape::Polygon);
  EXPECT_TRUE(session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Instant)));
  EXPECT_FALSE(session.close_polygon());  // one vertex is not a region
  session.update(point_pointer(4.0f, 0.0f, 0.0f, PointerPhase::Instant));
  session.update(point_pointer(4.0f, 3.0f, 0.0f, PointerPhase::Instant));
  EXPECT_EQ(session.polygon_vertices().size(), 3u);
  ASSERT_TRUE(session.close_polygon());
  EXPECT_EQ(session.state(), PickState::Committed);
  EXPECT_NEAR(session.result().area_m2, 6.0f, kTol);
}

TEST(AreaPickSessionTest, CancelClearsCollectedVertices)
{
  AreaPickSession session(PickTarget::ground(), AreaShape::Polygon);
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Instant));
  session.update(point_pointer(1.0f, 0.0f, 0.0f, PointerPhase::Instant));
  session.cancel();
  EXPECT_EQ(session.state(), PickState::Canceled);
  EXPECT_TRUE(session.polygon_vertices().empty());
}

TEST(AreaPickSessionTest, TapCannotDescribeARectangle)
{
  AreaPickSession session;
  EXPECT_FALSE(session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Instant)));
  EXPECT_EQ(session.state(), PickState::Idle);
}

// ---------------------------------------------------------------------------
// Navigation goals
// ---------------------------------------------------------------------------

TEST(YawQuaternion, RoundTrips)
{
  for (float yaw : {-3.0f, -kHalfPi, 0.0f, 0.5f, kHalfPi, 3.0f}) {
    EXPECT_NEAR(yaw_from_quaternion(yaw_to_quaternion(yaw)), yaw, kTol) << "yaw " << yaw;
  }
}

TEST(NavGoal, FlattensAndGrounds)
{
  PosePickSession session;
  session.update(point_pointer(1.0f, 2.0f, 5.0f, PointerPhase::Started));
  session.update(point_pointer(1.0f, 3.0f, 5.0f, PointerPhase::Ended));
  ASSERT_TRUE(session.result().valid);

  const auto goal = make_nav_goal(session.result());
  ASSERT_TRUE(goal.valid) << goal.error;
  EXPECT_EQ(goal.frame_id, "map");
  expect_vec(goal.position, 1.0f, 2.0f, 0.0f);
  EXPECT_NEAR(goal.yaw_rad, kHalfPi, kTol);
  EXPECT_NEAR(goal.orientation.x, 0.0f, kTol);
  EXPECT_NEAR(goal.orientation.y, 0.0f, kTol);
  EXPECT_NEAR(goal.orientation.z, 0.70710678f, kTol);
  EXPECT_NEAR(goal.orientation.w, 0.70710678f, kTol);
  EXPECT_TRUE(goal.heading_from_drag);
}

TEST(NavGoal, KeepsHeightWhenGroundingIsOff)
{
  PosePickSession session(PickTarget::action_point());
  session.update(point_pointer(1.0f, 2.0f, 5.0f, PointerPhase::Instant));
  ASSERT_TRUE(session.result().valid);
  NavGoalOptions options;
  options.project_to_ground = false;
  const auto goal = make_nav_goal(session.result(), options);
  ASSERT_TRUE(goal.valid) << goal.error;
  expect_vec(goal.position, 1.0f, 2.0f, 5.0f);
}

TEST(NavGoal, RefusesAnInvalidPose)
{
  const auto goal = make_nav_goal(qar_ar_core::PosePickResult{});
  EXPECT_FALSE(goal.valid);
  EXPECT_EQ(goal.error, "the pose pick produced no pose");
}

TEST(NavGoal, RefusesAnEmptyFrameId)
{
  PosePickSession session;
  session.update(point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Instant));
  NavGoalOptions options;
  options.frame_id.clear();
  const auto goal = make_nav_goal(session.result(), options);
  EXPECT_FALSE(goal.valid);
  EXPECT_EQ(goal.error, "a navigation goal needs a frame id");
}

TEST(NavGoal, CustomGroundHeight)
{
  PosePickSession session(PickTarget::action_point());
  session.update(point_pointer(1.0f, 2.0f, 5.0f, PointerPhase::Instant));
  NavGoalOptions options;
  options.ground_height_m = 0.5f;
  const auto goal = make_nav_goal(session.result(), options);
  ASSERT_TRUE(goal.valid) << goal.error;
  EXPECT_NEAR(goal.position.z, 0.5f, kTol);
}

// ---------------------------------------------------------------------------
// Hovers never commit picks, and the published point is the runtime's
// intersection rather than a flattened position.
// ---------------------------------------------------------------------------

namespace
{

/// A pointer carrying the runtime's own scene intersection -- the app-volume
/// action point, which is what the live path delivers.
SpatialPointer scene_hit_pointer(
  float x, float y, float z, PointerPhase phase,
  PointerGesture gesture = PointerGesture::Click, std::uint32_t id = 1)
{
  SpatialPointer p = point_pointer(x, y, z, phase, gesture, id);
  p.point_is_scene_hit = true;
  return p;
}

}  // namespace

TEST(ResolvePickPoint, RayHitsThePlaneWhereItCrossesAndNotBelowItsOrigin)
{
  // The distinction the live bug turned on. The origin is at (4, 4, 3) and the
  // ray leans, so the plane crossing is nowhere near (4, 4, 0) -- which is what
  // projecting the origin would have produced.
  const SpatialPointer p =
    ray_pointer(QarVector3{4.0f, 4.0f, 3.0f}, QarVector3{1.0f, 0.0f, -1.0f});
  const auto hit = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_TRUE(hit.from_ray);
  expect_vec(hit.position, 7.0f, 4.0f, 0.0f);
  EXPECT_NEAR(hit.distance_m, std::sqrt(18.0f), kTol);
  // The failure mode being guarded against: the origin flattened onto z = 0.
  EXPECT_GT(std::fabs(hit.position.x - p.ray_origin.x), 1.0f);
}

TEST(ResolvePickPoint, AGroundPickWithNoRayStillLandsOnTheGround)
{
  // A scene hit must not be published where it landed: "Ground plane (z = 0)"
  // would produce a point metres above the floor. The target is a promise about
  // WHERE THE ANSWER IS; without a ray the honest way to keep it is the
  // orthogonal projection, and z must come out 0 either way.
  const auto hit = resolve_pick_point(
    PickTarget::ground(), scene_hit_pointer(1.5f, -2.0f, 2.5f, PointerPhase::Instant));
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_EQ(hit.method, PickMethod::ProjectedPoint);
  EXPECT_FALSE(hit.from_ray);
  expect_vec(hit.position, 1.5f, -2.0f, 0.0f);
  // The provenance survives, because a log that says "projected" should also be
  // able to say the projected point came from a real aim rather than a mouse...
  EXPECT_TRUE(hit.from_scene_hit);
  // ... and the operator is told how far off the plane it had been.
  EXPECT_NEAR(hit.distance_m, 2.5f, kTol);
}

TEST(ResolvePickPoint, ASceneHitIsTakenAtFaceValueByTheActionPointTarget)
{
  // The other half of the correction: "pick the thing I am pointing at" is NOT
  // lost, it is a different target. ActionPoint is what a scene hit is for.
  const auto hit = resolve_pick_point(
    PickTarget::action_point(), scene_hit_pointer(1.5f, -2.0f, 2.5f, PointerPhase::Instant));
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_EQ(hit.method, PickMethod::ActionPoint);
  EXPECT_TRUE(hit.from_scene_hit);
  expect_vec(hit.position, 1.5f, -2.0f, 2.5f);
}

TEST(ResolvePickPoint, ABarePositionIsStillProjectedOntoThePlane)
{
  // A mouse, a test or a simulator reports a position, not an intersection.
  // "What is directly below this?" is the right answer there.
  SpatialPointer bare = point_pointer(1.5f, -2.0f, 2.5f, PointerPhase::Instant);
  bare.point_is_scene_hit = false;  // the premise: a position, not a hit
  const auto hit = resolve_pick_point(PickTarget::ground(), bare);
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_EQ(hit.method, PickMethod::ProjectedPoint);
  EXPECT_FALSE(hit.from_scene_hit);
  expect_vec(hit.position, 1.5f, -2.0f, 0.0f);
}

TEST(ResolvePickPoint, TheTargetModeChoosesRayCastVersusActionPoint)
{
  // The dispatch, target by target, on a pointer that carries BOTH a ray and a
  // reported point -- which is exactly what a live sample carries once the hand
  // ray is attached. Each target must pick the input its meaning requires.
  SpatialPointer p = ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, -1.0f});
  p.has_point = true;
  p.point = QarVector3{9.0f, 9.0f, 9.0f};
  p.point_is_scene_hit = true;

  // Ground plane: the ray wins and is INTERSECTED. (0,0,2) heading (1,0,-1)
  // crosses z = 0 at (2, 0, 0) -- not at the origin, and not at the point.
  const auto ground = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(ground.valid) << ground.error;
  EXPECT_EQ(ground.method, PickMethod::RayPlane);
  expect_vec(ground.position, 2.0f, 0.0f, 0.0f);

  // Fixed depth: the ray wins and is ADVANCED, not intersected.
  const auto depth = resolve_pick_point(PickTarget::at_depth(std::sqrt(2.0f)), p);
  ASSERT_TRUE(depth.valid) << depth.error;
  EXPECT_EQ(depth.method, PickMethod::RayDepth);
  expect_vec(depth.position, 1.0f, 0.0f, 1.0f);

  // Action point: the POINT wins, ray or no ray -- QarOS already intersected
  // the real (curved) aim, and our straight ray must not overrule it.
  const auto reported = resolve_pick_point(PickTarget::action_point(), p);
  ASSERT_TRUE(reported.valid) << reported.error;
  EXPECT_EQ(reported.method, PickMethod::ActionPoint);
  expect_vec(reported.position, 9.0f, 9.0f, 9.0f);
}

TEST(ResolvePickPoint, TheActionPointTargetTouchesNothingAboutTheRuntimesAnswer)
{
  // QarOS casts its own -- slightly CURVED -- ray against the app volume's
  // scene and reports the intersection as
  // the action point. That is the authoritative answer, so this target must
  // publish it BIT FOR BIT: no plane projection, no ray, no depth substitution.
  //
  // The pointer deliberately carries a straight ray as well, aimed somewhere
  // else entirely. A straight line cannot reproduce a curve, so letting it win
  // here would replace the truth with an approximation of it.
  SpatialPointer p = ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, -1.0f});
  p.has_point = true;
  p.point = QarVector3{1.5f, -2.0f, 2.5f};
  p.point_is_scene_hit = true;

  const auto hit = resolve_pick_point(PickTarget::action_point(), p);
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_EQ(hit.method, PickMethod::ActionPoint);
  EXPECT_FALSE(hit.from_ray);          // the ray was not consulted
  EXPECT_TRUE(hit.from_scene_hit);     // and the answer is a genuine hit
  EXPECT_FLOAT_EQ(hit.position.x, p.point.x);
  EXPECT_FLOAT_EQ(hit.position.y, p.point.y);
  EXPECT_FLOAT_EQ(hit.position.z, p.point.z);

  // Same pointer, a plane target: THAT one projects. The two targets must not
  // be the same operation wearing two names.
  const auto ground = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(ground.valid) << ground.error;
  EXPECT_NE(ground.position.z, hit.position.z);
}

TEST(ResolvePickPoint, TheActionPointTargetRefusesAGestureThatCarriedNoPoint)
{
  // `has_action_point == false` must NOT publish (0, 0, 0). The SDK documents an
  // absent point as zeroed rather than left stale, so the origin is exactly what
  // a naive "publish whatever we were given" would send -- a plausible-looking
  // lie sitting on the map origin. The refusal has to be explicit.
  SpatialPointer p;
  p.valid = true;  // a real sample: a phase arrived, it just carried no point
  p.gesture = PointerGesture::Click;
  p.phase = PointerPhase::Instant;
  p.has_point = false;
  p.has_ray = false;

  const auto out = resolve_pick_point(PickTarget::action_point(), p);
  EXPECT_FALSE(out.valid);
  EXPECT_FALSE(out.error.empty());
  // And the position it did not resolve is still the default, not a published one.
  expect_vec(out.position, 0.0f, 0.0f, 0.0f);
  EXPECT_EQ(out.method, PickMethod::None);
}

TEST(ResolvePickPoint, ARayOnlyGestureUnderTheActionPointTargetSaysWhyItRefused)
{
  // The other shape of the same refusal: the hands gave us a ray this frame but
  // the runtime reported no intersection. Our straight ray is not a substitute
  // for QarOS's curved one, so there is nothing to publish and the operator is
  // told which of the two is missing.
  const auto out = resolve_pick_point(
    PickTarget::action_point(),
    ray_pointer(QarVector3{0.0f, 0.0f, 2.0f}, QarVector3{1.0f, 0.0f, -1.0f}));
  EXPECT_FALSE(out.valid);
  EXPECT_NE(out.error.find("action point"), std::string::npos) << out.error;
  expect_vec(out.position, 0.0f, 0.0f, 0.0f);
}

TEST(ResolvePickPoint, ARayIntersectionReturnsTheCrossingAndNeverTheOrigin)
{
  // A plane pick whose ray starts ON the interesting side must return where the
  // ray CROSSES, not where it started -- the failure that would look correct in
  // exactly one pose (the operator standing at the target).
  const SpatialPointer p =
    ray_pointer(QarVector3{-3.0f, 1.0f, 1.6f}, QarVector3{1.0f, 0.0f, -0.4f});
  const auto hit = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_EQ(hit.method, PickMethod::RayPlane);
  expect_vec(hit.position, 1.0f, 1.0f, 0.0f);
  EXPECT_NE(hit.position.x, p.ray_origin.x);
  EXPECT_NE(hit.position.z, p.ray_origin.z);
  EXPECT_NEAR(hit.distance_m, std::sqrt((4.0f * 4.0f) + (1.6f * 1.6f)), 1e-3f);
}

TEST(ResolvePickPoint, ARayParallelToThePlaneIsRefusedRatherThanGuessed)
{
  // Horizontal aim, horizontal plane: there IS no intersection, and inventing
  // one would publish a goal nobody pointed at.
  const SpatialPointer p =
    ray_pointer(QarVector3{0.0f, 0.0f, 1.7f}, QarVector3{1.0f, 0.0f, 0.0f});
  const auto hit = resolve_pick_point(PickTarget::ground(), p);
  EXPECT_FALSE(hit.valid);
  EXPECT_EQ(hit.method, PickMethod::None);
  EXPECT_EQ(hit.error, "ray is parallel to the target plane");
}

TEST(ResolvePickPoint, APlaneBehindThePointerIsRefusedRatherThanProjectedBackwards)
{
  // Aiming UP at the sky: the ground is behind the aim, so t < 0. Returning the
  // backward crossing would put the pick behind the operator.
  const SpatialPointer p =
    ray_pointer(QarVector3{0.0f, 0.0f, 1.7f}, QarVector3{1.0f, 0.0f, 1.0f});
  const auto hit = resolve_pick_point(PickTarget::ground(), p);
  EXPECT_FALSE(hit.valid);
  EXPECT_EQ(hit.error, "target plane is behind the pointer");
}

TEST(ResolvePickPoint, ARayBeatsASceneHitWhenBothArePresent)
{
  SpatialPointer p =
    ray_pointer(QarVector3{4.0f, 4.0f, 3.0f}, QarVector3{1.0f, 0.0f, -1.0f});
  p.has_point = true;
  p.point = QarVector3{0.0f, 0.0f, 9.0f};
  p.point_is_scene_hit = true;
  const auto hit = resolve_pick_point(PickTarget::ground(), p);
  ASSERT_TRUE(hit.valid) << hit.error;
  EXPECT_TRUE(hit.from_ray);
  EXPECT_EQ(hit.method, PickMethod::RayPlane);
  expect_vec(hit.position, 7.0f, 4.0f, 0.0f);
}

TEST(SpatialPointerFromSample, SceneHitProvenanceSurvivesTheFrameChange)
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = 0.0f;
  PanelPointerSample sample;
  sample.has_room_point = true;
  sample.room_point_is_scene_hit = true;
  sample.room_point = QarVector3{1.0f, 2.0f, 3.0f};
  EXPECT_TRUE(spatial_pointer_from_sample(sample, t).point_is_scene_hit);

  sample.room_point_is_scene_hit = false;
  EXPECT_FALSE(spatial_pointer_from_sample(sample, t).point_is_scene_hit);
}

TEST(PointPickSession, AHoverNeverCommitsWhateverPhaseItArrivesWith)
{
  // The live failure verbatim: the runtime emits hover as INSTANT, and 95,648
  // of them published a point.
  const PointerPhase phases[] = {
    PointerPhase::Started, PointerPhase::Updated, PointerPhase::Ended, PointerPhase::Instant,
    PointerPhase::Canceled};
  for (const PointerPhase phase : phases) {
    PointPickSession session(PickTarget::action_point());
    session.update(point_pointer(1.0f, 2.0f, 3.0f, phase, PointerGesture::Hover));
    EXPECT_NE(session.state(), PickState::Committed) << static_cast<int>(phase);
    EXPECT_FALSE(session.result().valid) << static_cast<int>(phase);
  }
}

TEST(PointPickSession, AHoverPreviewsWhileATapCommits)
{
  PointPickSession session(PickTarget::action_point());
  EXPECT_TRUE(
    session.update(
      point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant, PointerGesture::Hover)));
  EXPECT_EQ(session.state(), PickState::Idle);
  EXPECT_TRUE(session.preview().valid);
  expect_vec(session.preview().position, 1.0f, 2.0f, 3.0f);
  EXPECT_FALSE(session.result().valid);

  // The same point, arriving as a tap, is a decision.
  ASSERT_TRUE(
    session.update(
      point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant, PointerGesture::Click)));
  EXPECT_EQ(session.state(), PickState::Committed);
  ASSERT_TRUE(session.result().valid);
  expect_vec(session.result().position, 1.0f, 2.0f, 3.0f);
}

TEST(PointPickSession, ARepeatedUnmovedHoverReportsNoChange)
{
  // The app-volume runtime repeats an unchanged hover at full rate.
  PointPickSession session(PickTarget::action_point());
  EXPECT_TRUE(
    session.update(
      point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant, PointerGesture::Hover)));
  EXPECT_FALSE(
    session.update(
      point_pointer(1.0f, 2.0f, 3.0f, PointerPhase::Instant, PointerGesture::Hover)));
  EXPECT_TRUE(
    session.update(
      point_pointer(1.0f, 2.5f, 3.0f, PointerPhase::Instant, PointerGesture::Hover)));
}

TEST(PointPickSession, AHoverCannotEndAClickInFlight)
{
  PointPickSession session(PickTarget::action_point());
  ASSERT_TRUE(
    session.update(
      point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Started, PointerGesture::Click)));
  ASSERT_EQ(session.state(), PickState::Pending);
  EXPECT_FALSE(
    session.update(
      point_pointer(9.0f, 9.0f, 9.0f, PointerPhase::Ended, PointerGesture::Hover)));
  EXPECT_EQ(session.state(), PickState::Pending);
  // The press's own release still commits, at the press's point.
  ASSERT_TRUE(
    session.update(
      point_pointer(1.0f, 1.0f, 1.0f, PointerPhase::Ended, PointerGesture::Click)));
  EXPECT_EQ(session.state(), PickState::Committed);
  expect_vec(session.result().position, 1.0f, 1.0f, 1.0f);
}

TEST(PosePickSession, AHoverPreviewsAPositionButNeverCommitsAGoal)
{
  PosePickSession session(PickTarget::action_point());
  EXPECT_TRUE(
    session.update(
      point_pointer(2.0f, 3.0f, 0.0f, PointerPhase::Instant, PointerGesture::Hover)));
  EXPECT_NE(session.state(), PickState::Committed);
  EXPECT_FALSE(session.result().valid);
  EXPECT_TRUE(session.preview().valid);
  expect_vec(session.preview().position, 2.0f, 3.0f, 0.0f);

  ASSERT_TRUE(
    session.update(
      point_pointer(2.0f, 3.0f, 0.0f, PointerPhase::Instant, PointerGesture::Click)));
  EXPECT_EQ(session.state(), PickState::Committed);
}

TEST(AreaPickSession, AHoverNeverPlacesACorner)
{
  AreaPickSession session(PickTarget::action_point(), AreaShape::Polygon);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(
      session.update(
        point_pointer(
          static_cast<float>(i), 0.0f, 0.0f, PointerPhase::Instant, PointerGesture::Hover)));
  }
  EXPECT_EQ(session.state(), PickState::Idle);

  ASSERT_TRUE(
    session.update(
      point_pointer(0.0f, 0.0f, 0.0f, PointerPhase::Instant, PointerGesture::Click)));
  EXPECT_EQ(session.state(), PickState::Pending);
}

// ---------------------------------------------------------------------------
// Gesture points are in APP content space
//
// The same ground truth spatial_frames_tests.cpp pins at the frame level, taken
// the whole way the panel takes it: PanelPointerSample ->
// spatial_pointer_from_sample -> resolve_pick_point on the ActionPoint target.
// That is the path that publishes the runtime's point, so it is the one that must
// not regress.
// ---------------------------------------------------------------------------

TEST(GesturePointsAreInAppSpace, TheCanonicalClickSurvivesThePublishPath)
{
  // The canonical sample: the operator aimed at the RViz scene origin and
  // tapped while the app volume reported app_scale = 0.1. Dividing the
  // placement back out would publish (-8.583, 0.300, -12.656) for it -- 15 m
  // away and 12 m under the floor. The action point is in APP content space
  // (eye-to-fingertip is 0.500 m at app_scale = 1.0 and 5.002 m at 0.1), so it
  // needs the basis change alone.
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = qar_ar_core::kDefaultAnchorYawRad;  // 180 deg, the shipped default
  ASSERT_TRUE(qar_ar_core::scene_placement_is_identity(t.placement))
    << "the spatial pick must leave RoomToRosTransform::placement at its identity default";

  PanelPointerSample sample;
  sample.pointer_id = 1;
  sample.phase = PointerPhase::Instant;
  sample.gesture = PointerGesture::Click;
  sample.has_room_point = true;
  sample.room_point_is_scene_hit = true;
  sample.room_point = QarVector3{0.030f, -0.266f, -0.858f};

  const SpatialPointer p = spatial_pointer_from_sample(sample, t);
  ASSERT_TRUE(p.has_point);
  expect_vec(p.point, -0.858f, 0.030f, -0.266f);

  const auto out = resolve_pick_point(PickTarget::action_point(), p);
  ASSERT_TRUE(out.valid) << out.error;
  EXPECT_EQ(out.method, PickMethod::ActionPoint);
  expect_vec(out.position, -0.858f, 0.030f, -0.266f);
  // 0.899 m from the origin the operator was aiming at: ordinary human AR
  // aiming error. The published answer was 15.29 m out.
  EXPECT_NEAR(
    std::sqrt(
      (out.position.x * out.position.x) + (out.position.y * out.position.y) +
      (out.position.z * out.position.z)),
    0.899f, 0.002f);
}
