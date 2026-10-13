// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// The room <-> ROS frame change, checked against HAND-COMPUTED values.
//
// This is the transform that fails silently when it is wrong: a pick lands
// somewhere the wearer never pointed and nothing errors. So the numbers here are
// written out longhand rather than derived from the implementation, and the
// basis quaternion is pinned to the one qar_ogre_streaming applies to the
// camera (qar_pose_conversion.cpp, "zup" mode) — if the two ever diverge, the
// pick and the view diverge with them.

#include "qar_ar_core/spatial_frames.hpp"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace
{

using qar_ar_core::RoomToRosTransform;
using qar_ar_core::room_direction_to_ros;
using qar_ar_core::room_orientation_to_ros;
using qar_ar_core::room_point_to_ros;
using qar_ar_core::ros_direction_to_room;
using qar_ar_core::ros_point_to_room;

constexpr float kTol = 1e-5f;

RoomToRosTransform plain()
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = 0.0f;
  t.height_m = 0.0f;
  t.back_m = 0.0f;
  return t;
}

void expect_vec(const QarVector3 & actual, float x, float y, float z)
{
  EXPECT_NEAR(actual.x, x, kTol);
  EXPECT_NEAR(actual.y, y, kTol);
  EXPECT_NEAR(actual.z, z, kTol);
}

}  // namespace

// --- the basis change itself ------------------------------------------------

TEST(SpatialFrames, RoomUpBecomesRosUp)
{
  // Room (OpenXR) +Y is up; ROS (REP-103) +Z is up.
  expect_vec(room_point_to_ros(plain(), QarVector3{0.0f, 1.0f, 0.0f}), 0.0f, 0.0f, 1.0f);
}

TEST(SpatialFrames, RoomForwardBecomesRosForward)
{
  // Room forward is -Z; ROS forward is +X.
  expect_vec(room_point_to_ros(plain(), QarVector3{0.0f, 0.0f, -1.0f}), 1.0f, 0.0f, 0.0f);
}

TEST(SpatialFrames, RoomRightBecomesRosNegativeY)
{
  // ROS +Y is LEFT, so room +X (right) must come out as -Y.
  expect_vec(room_point_to_ros(plain(), QarVector3{1.0f, 0.0f, 0.0f}), 0.0f, -1.0f, 0.0f);
}

TEST(SpatialFrames, HandComputedPoint)
{
  // ros = (-room.z, -room.x, room.y)
  expect_vec(room_point_to_ros(plain(), QarVector3{1.0f, 2.0f, 3.0f}), -3.0f, -1.0f, 2.0f);
}

TEST(SpatialFrames, DisabledIsTheIdentityMapping)
{
  RoomToRosTransform t = plain();
  t.enabled = false;
  t.yaw_rad = 1.0f;   // must be ignored along with the rest of the anchor
  t.height_m = 5.0f;
  t.back_m = 7.0f;
  expect_vec(room_point_to_ros(t, QarVector3{1.0f, 2.0f, 3.0f}), 1.0f, 2.0f, 3.0f);
}

// --- the viewer anchor ------------------------------------------------------

TEST(SpatialFrames, AnchorYawOf180NegatesXAndY)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = qar_ar_core::kDefaultAnchorYawRad;  // 180 deg, the shipped default
  // basis: (1,2,3) -> (-3,-1,2); yaw 180: (3, 1, 2)
  expect_vec(room_point_to_ros(t, QarVector3{1.0f, 2.0f, 3.0f}), 3.0f, 1.0f, 2.0f);
}

TEST(SpatialFrames, AnchorYawOf90RotatesInThePlane)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = 1.57079632679f;  // +90 deg about ROS +Z
  // basis: (1,2,3) -> (-3,-1,2); yaw +90: (x*0 - y*1, x*1 + y*0, z) = (1, -3, 2)
  expect_vec(room_point_to_ros(t, QarVector3{1.0f, 2.0f, 3.0f}), 1.0f, -3.0f, 2.0f);
}

TEST(SpatialFrames, AnchorHeightAndStandOff)
{
  RoomToRosTransform t = plain();
  t.height_m = 1.5f;
  t.back_m = 2.0f;
  // basis (-3,-1,2); yaw 0 so the facing direction is +X: back subtracts 2 from X.
  expect_vec(room_point_to_ros(t, QarVector3{1.0f, 2.0f, 3.0f}), -5.0f, -1.0f, 3.5f);
}

TEST(SpatialFrames, DirectionIgnoresTheAnchorTranslation)
{
  RoomToRosTransform t = plain();
  t.height_m = 1.5f;
  t.back_m = 2.0f;
  // A direction is not a place: only the rotation applies, so room up is still ROS up.
  expect_vec(room_direction_to_ros(t, QarVector3{0.0f, 1.0f, 0.0f}), 0.0f, 0.0f, 1.0f);
}

TEST(SpatialFrames, DirectionKeepsItsLength)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = 0.7f;
  const QarVector3 room{0.3f, -0.9f, 2.0f};
  const QarVector3 ros = room_direction_to_ros(t, room);
  const float before = std::sqrt(room.x * room.x + room.y * room.y + room.z * room.z);
  const float after = std::sqrt(ros.x * ros.x + ros.y * ros.y + ros.z * ros.z);
  EXPECT_NEAR(before, after, kTol);
}

// --- round trips ------------------------------------------------------------

TEST(SpatialFrames, PointRoundTripsThroughTheInverse)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = 2.1f;
  t.height_m = 1.7f;
  t.back_m = -0.4f;
  const QarVector3 room{0.25f, -1.5f, 4.0f};
  const QarVector3 back = ros_point_to_room(t, room_point_to_ros(t, room));
  expect_vec(back, room.x, room.y, room.z);
}

TEST(SpatialFrames, DirectionRoundTripsThroughTheInverse)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = -0.9f;
  t.height_m = 3.0f;
  t.back_m = 6.0f;
  const QarVector3 room{-0.6f, 0.2f, 0.75f};
  const QarVector3 back = ros_direction_to_room(t, room_direction_to_ros(t, room));
  expect_vec(back, room.x, room.y, room.z);
}

// --- orientation ------------------------------------------------------------

TEST(SpatialFrames, IdentityOrientationBecomesTheBasisQuaternion)
{
  // qar_pose_conversion.cpp uses (w,x,y,z) = (0.5, 0.5, -0.5, -0.5); QarQuaternion
  // stores xyzw, so the same rotation reads (0.5, -0.5, -0.5, 0.5) here.
  const QarQuaternion out =
    room_orientation_to_ros(plain(), QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
  EXPECT_NEAR(out.x, 0.5f, kTol);
  EXPECT_NEAR(out.y, -0.5f, kTol);
  EXPECT_NEAR(out.z, -0.5f, kTol);
  EXPECT_NEAR(out.w, 0.5f, kTol);
}

TEST(SpatialFrames, BasisQuaternionRotatesRoomAxesLikeThePointMapping)
{
  // The orientation path and the position path must agree, or a pose picked in
  // AR would point somewhere its own position does not.
  const QarQuaternion q =
    room_orientation_to_ros(plain(), QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
  const auto rotate = [&q](const QarVector3 & v) {
      const QarVector3 u{q.x, q.y, q.z};
      const QarVector3 t{
        2.0f * (u.y * v.z - u.z * v.y),
        2.0f * (u.z * v.x - u.x * v.z),
        2.0f * (u.x * v.y - u.y * v.x)};
      return QarVector3{
        v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
        v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
        v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    };
  const QarVector3 room{1.0f, 2.0f, 3.0f};
  const QarVector3 rotated = rotate(room);
  const QarVector3 mapped = room_point_to_ros(plain(), room);
  expect_vec(rotated, mapped.x, mapped.y, mapped.z);
}

TEST(SpatialFrames, AnchorYawIsAppliedToTheOrientationToo)
{
  RoomToRosTransform t = plain();
  t.yaw_rad = qar_ar_core::kDefaultAnchorYawRad;
  // yaw(180) * basis, worked out by hand, is (w,x,y,z) = (0.5, 0.5, 0.5, 0.5).
  const QarQuaternion out = room_orientation_to_ros(t, QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
  EXPECT_NEAR(out.w, 0.5f, kTol);
  EXPECT_NEAR(out.x, 0.5f, kTol);
  EXPECT_NEAR(out.y, 0.5f, kTol);
  EXPECT_NEAR(out.z, 0.5f, kTol);
}

TEST(SpatialFrames, OrientationIsNormalized)
{
  const QarQuaternion out =
    room_orientation_to_ros(plain(), QarQuaternion{0.0f, 0.0f, 0.0f, 4.0f});
  const float norm =
    std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
  EXPECT_NEAR(norm, 1.0f, kTol);
}

TEST(SpatialFrames, DegenerateOrientationFallsBackToIdentity)
{
  const QarQuaternion out =
    room_orientation_to_ros(plain(), QarQuaternion{0.0f, 0.0f, 0.0f, 0.0f});
  EXPECT_NEAR(out.x, 0.0f, kTol);
  EXPECT_NEAR(out.y, 0.0f, kTol);
  EXPECT_NEAR(out.z, 0.0f, kTol);
  EXPECT_NEAR(out.w, 1.0f, kTol);
}

TEST(SpatialFrames, DisabledOrientationOnlyReordersStorage)
{
  RoomToRosTransform t = plain();
  t.enabled = false;
  const QarQuaternion in{0.0f, 0.0f, 0.70710678f, 0.70710678f};
  const QarQuaternion out = room_orientation_to_ros(t, in);
  EXPECT_NEAR(out.x, in.x, kTol);
  EXPECT_NEAR(out.y, in.y, kTol);
  EXPECT_NEAR(out.z, in.z, kTol);
  EXPECT_NEAR(out.w, in.w, kTol);
}

// --- the app-volume placement -----------------------------------------------
//
// Where QarOS is displaying our scene: the app volume's `app_scale` /
// `app_pose` / volume pose. These pin the arithmetic of that mapping and its
// inverse, which the panel reads back live and LOGS.
//
// It is NOT part of the spatial pick's map. Gesture points, the view pose and
// the hand joints all arrive in APP content space (see spatial_frames.hpp, and
// `GesturePointsAreInAppSpace` at the bottom of this file), so a pick takes the
// basis change alone; applying the placement's inverse to them throws picks
// metres off the target. The tests below exercise the ScenePlacement functions
// directly, on purpose — they do not say the pick path should call them.

namespace
{

using qar_ar_core::ScenePlacement;
using qar_ar_core::room_direction_to_scene;
using qar_ar_core::room_point_to_scene;
using qar_ar_core::scene_direction_to_room;
using qar_ar_core::scene_placement_is_identity;
using qar_ar_core::scene_placement_scale;
using qar_ar_core::scene_point_to_room;

/// Yaw about the room's vertical (+Y), as a Qar quaternion (xyzw).
QarQuaternion yaw_y(float rad)
{
  return QarQuaternion{0.0f, std::sin(rad * 0.5f), 0.0f, std::cos(rad * 0.5f)};
}

float length(const QarVector3 & v)
{
  return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

}  // namespace

TEST(ScenePlacementTest, DefaultIsIdentityAndChangesNothing)
{
  const ScenePlacement p;
  EXPECT_TRUE(scene_placement_is_identity(p));
  EXPECT_FLOAT_EQ(scene_placement_scale(p), 1.0f);
  const QarVector3 v{1.5f, -2.25f, 3.75f};
  expect_vec(scene_point_to_room(p, v), v.x, v.y, v.z);
  expect_vec(room_point_to_scene(p, v), v.x, v.y, v.z);
}

TEST(ScenePlacementTest, TransformIsUnchangedWithoutAPlacement)
{
  // The regression guard for every caller that sets no placement: a transform
  // left at its defaults must map exactly as the plain basis change does.
  RoomToRosTransform t = plain();
  ASSERT_TRUE(scene_placement_is_identity(t.placement));
  expect_vec(room_point_to_ros(t, QarVector3{1.0f, 2.0f, 3.0f}), -3.0f, -1.0f, 2.0f);
}

TEST(ScenePlacementTest, KnownRoomPointMapsToTheExpectedScenePoint)
{
  // Hand-computed. Scale 0.25 (the volume shows the scene at a quarter size),
  // the app origin sits 0.5 m along volume +X, and the cuboid's centre is 1 m
  // off the floor -- the geometry the adapter actually creates, shrunk.
  //
  //   scene (2, 4, -8)
  //     * 0.25            -> (0.5, 1, -2)
  //     + app_pose.pos    -> (1.0, 1, -2)
  //     + volume centre   -> (1.0, 2, -2)
  ScenePlacement p;
  p.known = true;
  p.app_scale = 0.25f;
  p.app_pose.position = QarVector3{0.5f, 0.0f, 0.0f};
  p.volume_pose.position = QarVector3{0.0f, 1.0f, 0.0f};

  const QarVector3 scene{2.0f, 4.0f, -8.0f};
  const QarVector3 room = scene_point_to_room(p, scene);
  expect_vec(room, 1.0f, 2.0f, -2.0f);
  // ... and the inverse returns the original.
  expect_vec(room_point_to_scene(p, room), scene.x, scene.y, scene.z);
}

TEST(ScenePlacementTest, AppPoseIsNotScaledByAppScale)
{
  // The vendor is explicit: app pose remains in app-volume metres and is not
  // scaled. A placement whose only non-default field is the app offset must
  // move the answer by exactly that offset, whatever the scale.
  ScenePlacement p;
  p.app_scale = 0.1f;
  p.app_pose.position = QarVector3{3.0f, 0.0f, 0.0f};
  expect_vec(scene_point_to_room(p, QarVector3{0.0f, 0.0f, 0.0f}), 3.0f, 0.0f, 0.0f);
  expect_vec(scene_point_to_room(p, QarVector3{10.0f, 0.0f, 0.0f}), 4.0f, 0.0f, 0.0f);
}

TEST(ScenePlacementTest, RotatedVolumeRoundTrips)
{
  ScenePlacement p;
  p.app_scale = 0.35f;
  p.volume_pose.orientation = yaw_y(1.0471975512f);   // 60 deg about room up
  p.volume_pose.position = QarVector3{-0.4f, 1.2f, 2.5f};
  p.app_pose.orientation = yaw_y(-0.5235987756f);     // -30 deg
  p.app_pose.position = QarVector3{0.25f, -0.75f, 0.5f};

  const QarVector3 scene{4.5f, -2.5f, 7.25f};
  const QarVector3 room = scene_point_to_room(p, scene);
  const QarVector3 back = room_point_to_scene(p, room);
  EXPECT_NEAR(back.x, scene.x, 1e-3f);
  EXPECT_NEAR(back.y, scene.y, 1e-3f);
  EXPECT_NEAR(back.z, scene.z, 1e-3f);
}

TEST(ScenePlacementTest, DirectionKeepsItsLengthAndRoundTrips)
{
  // A direction carries no origin and no size: the scale and the offsets must
  // not touch it, or "2 m along the ray" would silently mean something else.
  ScenePlacement p;
  p.app_scale = 0.08f;
  p.volume_pose.orientation = yaw_y(0.7853981634f);  // 45 deg
  p.volume_pose.position = QarVector3{5.0f, 1.0f, -3.0f};
  p.app_pose.position = QarVector3{2.0f, 2.0f, 2.0f};

  const QarVector3 dir{0.0f, 0.0f, -1.0f};
  const QarVector3 room = scene_direction_to_room(p, dir);
  EXPECT_NEAR(length(room), 1.0f, kTol);
  const QarVector3 back = room_direction_to_scene(p, room);
  expect_vec(back, dir.x, dir.y, dir.z);
}

TEST(ScenePlacementTest, ScaleOfZeroOrNegativeDegradesToUnity)
{
  // A collapsed or mirrored scale is a bad read-back, not a small scene.
  // Dividing by it would publish infinities into a ROS topic.
  ScenePlacement p;
  p.app_scale = 0.0f;
  EXPECT_FLOAT_EQ(scene_placement_scale(p), 1.0f);
  p.app_scale = -2.0f;
  EXPECT_FLOAT_EQ(scene_placement_scale(p), 1.0f);
  p.app_scale = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FLOAT_EQ(scene_placement_scale(p), 1.0f);
  p.app_scale = 0.0f;
  p.volume_pose.position = QarVector3{0.0f, 1.0f, 0.0f};
  expect_vec(room_point_to_scene(p, QarVector3{0.0f, 4.0f, 0.0f}), 0.0f, 3.0f, 0.0f);
}

TEST(ScenePlacementTest, RoomMetresBecomeSceneMetresThroughTheTransform)
{
  // The ARITHMETIC of a non-identity placement, nothing more: "if a quantity
  // really is in room metres, here is what the transform does to it". It is not
  // a statement about picks, because a gesture's action_point does not arrive
  // in room metres. The pick path leaves the placement identity;
  // `GesturePointsAreInAppSpace` below is the test that says so.
  RoomToRosTransform t = plain();
  t.placement.known = true;
  t.placement.app_scale = 0.0736f;
  // Eye and action point on the room's -Z (room forward), so the ROS answer
  // lands on +X and the arithmetic is readable.
  const QarVector3 eye_room{0.0f, 0.0f, 0.0f};
  const QarVector3 action_room{0.0f, 0.0f, -0.695f};
  const QarVector3 eye_ros = room_point_to_ros(t, eye_room);
  const QarVector3 action_ros = room_point_to_ros(t, action_room);
  const QarVector3 d{action_ros.x - eye_ros.x, action_ros.y - eye_ros.y,
    action_ros.z - eye_ros.z};
  EXPECT_NEAR(length(d), 9.443f, 0.01f);
  // And the inverse puts it back in the room where QarOS reported it.
  expect_vec(ros_point_to_room(t, action_ros), action_room.x, action_room.y, action_room.z);
}

TEST(ScenePlacementTest, PointRoundTripsThroughTheFullTransform)
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = 2.0f;
  t.height_m = 1.6f;
  t.back_m = 0.75f;
  t.placement.known = true;
  t.placement.app_scale = 0.12f;
  t.placement.volume_pose.orientation = yaw_y(0.3f);
  t.placement.volume_pose.position = QarVector3{0.2f, 1.0f, -0.5f};
  t.placement.app_pose.position = QarVector3{0.1f, -0.2f, 0.3f};

  const QarVector3 room{1.25f, 1.75f, -2.5f};
  const QarVector3 ros = room_point_to_ros(t, room);
  const QarVector3 back = ros_point_to_room(t, ros);
  EXPECT_NEAR(back.x, room.x, 1e-3f);
  EXPECT_NEAR(back.y, room.y, 1e-3f);
  EXPECT_NEAR(back.z, room.z, 1e-3f);

  const QarVector3 dir{0.3f, -0.6f, 0.74162f};
  const QarVector3 dir_ros = room_direction_to_ros(t, dir);
  EXPECT_NEAR(length(dir_ros), length(dir), 1e-4f);
  const QarVector3 dir_back = ros_direction_to_room(t, dir_ros);
  EXPECT_NEAR(dir_back.x, dir.x, 1e-4f);
  EXPECT_NEAR(dir_back.y, dir.y, 1e-4f);
  EXPECT_NEAR(dir_back.z, dir.z, 1e-4f);
}

TEST(ScenePlacementTest, DisabledModeStillAppliesThePlacement)
{
  // "Room space is already the ROS world" is a statement about AXES. It does
  // not say the volume is displaying the scene at life size.
  RoomToRosTransform t = plain();
  t.enabled = false;
  t.placement.app_scale = 0.5f;
  t.placement.volume_pose.position = QarVector3{0.0f, 1.0f, 0.0f};
  expect_vec(room_point_to_ros(t, QarVector3{0.0f, 3.0f, 0.0f}), 0.0f, 4.0f, 0.0f);
  expect_vec(ros_point_to_room(t, QarVector3{0.0f, 4.0f, 0.0f}), 0.0f, 3.0f, 0.0f);
}

TEST(ScenePlacementTest, OrientationUndoesTheVolumeYaw)
{
  // A volume the operator has yawed rotates the whole scene in the room, so an
  // orientation read out of the room is turned by the same amount and must be
  // turned back before it is published as a ROS pose.
  RoomToRosTransform t = plain();
  t.placement.known = true;
  t.placement.volume_pose.orientation = yaw_y(0.9f);
  const QarQuaternion out = room_orientation_to_ros(t, yaw_y(0.9f));
  // Undoing the volume's yaw leaves the identity, which the basis change then
  // turns into the basis quaternion itself.
  EXPECT_NEAR(out.x, 0.5f, 1e-4f);
  EXPECT_NEAR(out.y, -0.5f, 1e-4f);
  EXPECT_NEAR(out.z, -0.5f, 1e-4f);
  EXPECT_NEAR(out.w, 0.5f, 1e-4f);
}

// --- gesture points are in APP content space --------------------------------
//
// Gesture points, the view pose and the hand joints arrive in APP units, so a
// pick needs the basis change and no placement term. A controlled measurement
// at app_scale = 0.1, aiming at the RViz scene origin, shows it: with the basis
// change alone the median miss is 0.740 m (ordinary human AR aiming error), with
// the placement inverse applied 14.05 m. The evidence is in
// docs/ARCHITECTURE.md#gesture-points-are-in-app-content-space.
//
// So the pick transform's `placement` stays at its identity default, and these
// two tests are what holds it there.

namespace
{

/// The transform the panel actually uses live: the basis change enabled, the
/// default 180 deg anchor yaw, no height and no stand-off -- and a placement
/// LEFT ALONE, which is the whole point.
RoomToRosTransform live_pick_transform()
{
  RoomToRosTransform t;
  t.enabled = true;
  t.yaw_rad = qar_ar_core::kDefaultAnchorYawRad;
  t.height_m = 0.0f;
  t.back_m = 0.0f;
  return t;
}

/// What the volume reports at app_scale = 0.1, read back live:
/// `app_scale=0.1000 volume_centre_room=(0.000, 1.000, 0.000)
///  app_pose_volume=(0,0,0)`, both quaternions identity. Correct, and logged
/// by the panel as a diagnostic -- but never applied to a pick.
ScenePlacement placement_reported_by_the_runtime(float app_scale)
{
  ScenePlacement p;
  p.known = true;
  p.app_scale = app_scale;
  p.volume_pose.position = QarVector3{0.0f, 1.0f, 0.0f};
  return p;
}

}  // namespace

TEST(GesturePointsAreInAppSpace, TheCanonicalClickLandsWhereTheOperatorAimed)
{
  // The canonical sample: the operator aimed at the RViz scene origin and
  // tapped, with the volume reporting app_scale = 0.1.
  const QarVector3 action_room{0.030f, -0.266f, -0.858f};
  const QarVector3 aimed_at{0.0f, 0.0f, 0.0f};

  // The runtime's placement is READ (the panel logs it) and NOT APPLIED.
  const ScenePlacement reported = placement_reported_by_the_runtime(0.1f);
  EXPECT_FALSE(scene_placement_is_identity(reported));
  RoomToRosTransform t = live_pick_transform();
  ASSERT_TRUE(scene_placement_is_identity(t.placement))
    << "the spatial pick must leave RoomToRosTransform::placement at its identity default";

  // The basis change alone: ros = (room.z, room.x, room.y) once the default
  // 180 deg anchor yaw is folded in.
  const QarVector3 ros = room_point_to_ros(t, action_room);
  expect_vec(ros, -0.858f, 0.030f, -0.266f);
  const QarVector3 miss{ros.x - aimed_at.x, ros.y - aimed_at.y, ros.z - aimed_at.z};
  EXPECT_NEAR(length(miss), 0.899f, 0.001f);  // human aiming error, and nothing more

  // And what applying the placement inverse would publish for the same sample:
  // (-8.583, 0.300, -12.656) from the unrounded floats; the room point above is
  // the 3-decimal rounding of the logged value, so 0.01 is the tolerance that
  // comparison deserves.
  RoomToRosTransform broken = live_pick_transform();
  broken.placement = reported;
  const QarVector3 wrong = room_point_to_ros(broken, action_room);
  EXPECT_NEAR(wrong.x, -8.583f, 0.01f);
  EXPECT_NEAR(wrong.y, 0.300f, 0.01f);
  EXPECT_NEAR(wrong.z, -12.656f, 0.01f);
  const QarVector3 wrong_miss{
    wrong.x - aimed_at.x, wrong.y - aimed_at.y, wrong.z - aimed_at.z};
  EXPECT_NEAR(length(wrong_miss), 15.29f, 0.02f);  // 15 m away, and 12 m under the floor
  EXPECT_LT(wrong.z, -10.0f);
}

TEST(GesturePointsAreInAppSpace, ThePublishedPointDoesNotDependOnAppScale)
{
  // THE property, and the cheapest guard there is. A gesture reports the same
  // app-space point whatever size the operator has pinched the scene to, so the
  // published ROS point must not move when app_scale does (app_scale = 1.0 vs
  // 0.1 is a 10x difference in every distance read in app metres).
  const QarVector3 action_room{0.030f, -0.266f, -0.858f};
  const RoomToRosTransform t = live_pick_transform();
  const QarVector3 reference = room_point_to_ros(t, action_room);

  for (const float app_scale : {1.0f, 0.1f, 10.0f}) {
    const ScenePlacement reported = placement_reported_by_the_runtime(app_scale);
    EXPECT_FLOAT_EQ(scene_placement_scale(reported), app_scale);
    // Whatever the runtime reports, the pick transform is the same one.
    const QarVector3 ros = room_point_to_ros(live_pick_transform(), action_room);
    expect_vec(ros, reference.x, reference.y, reference.z);
    expect_vec(ros, -0.858f, 0.030f, -0.266f);

    // The counterfactual, so this test states a property rather than a
    // tautology: feeding the placement in moves the answer, by a different
    // amount for every scale, which is exactly the defect. (Even at
    // app_scale = 1.0 the volume centre alone displaces it by a full metre.)
    RoomToRosTransform broken = t;
    broken.placement = reported;
    const QarVector3 wrong = room_point_to_ros(broken, action_room);
    EXPECT_GT(length(QarVector3{wrong.x - ros.x, wrong.y - ros.y, wrong.z - ros.z}), 0.5f);
  }
}
