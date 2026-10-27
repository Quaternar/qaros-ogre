// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The AR pointing ray: the thing the app-volume gesture does NOT carry, and
// which two of the three pick targets cannot work without. These tests pin the
// construction, the degenerate refusals, the SDK hand-sample reading, and the
// thread hand-off.

#include "qar_ar_core/pointing_ray.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace
{

using qar_ar_core::HandJointsProbe;
using qar_ar_core::HandRayJoint;
using qar_ar_core::NearestHandJoint;
using qar_ar_core::PointingRay;
using qar_ar_core::PointingRayInputs;
using qar_ar_core::PointingRayKind;
using qar_ar_core::PointingRaySlot;
using qar_ar_core::hand_joint_distance;
using qar_ar_core::hand_joint_name;
using qar_ar_core::hand_joints_probe_from;
using qar_ar_core::hand_ray_inputs;
using qar_ar_core::nearest_hand_joint;
using qar_ar_core::pointing_ray_from;
using qar_ar_core::pointing_ray_inputs_from_hands;
using qar_ar_core::ray_point_distance;
using qar_ar_core::rotate_by_quaternion;

constexpr float kTol = 1e-4f;

void expect_vec(const QarVector3 & v, float x, float y, float z)
{
  EXPECT_NEAR(v.x, x, kTol);
  EXPECT_NEAR(v.y, y, kTol);
  EXPECT_NEAR(v.z, z, kTol);
}

/// An eye at the origin and a hand a metre in front of it (room space is -Z
/// forward), i.e. someone pointing straight ahead.
PointingRayInputs pointing_forward()
{
  PointingRayInputs in;
  in.has_eye = true;
  in.eye_position = QarVector3{0.0f, 1.6f, 0.0f};
  in.hand_tracked = true;
  in.hand_position = QarVector3{0.0f, 1.6f, -0.6f};
  return in;
}

/// One hand's joints with a valid position (and optionally orientation) at the
/// named joint, shaped the way the SDK fills QarHandJoints.
QarHandJoints hand_with(
  QarHandJoint joint, const QarVector3 & position, bool tracked = true,
  bool with_orientation = false,
  const QarQuaternion & orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f})
{
  QarHandJoints hand = qar_hand_joints_default();
  hand.is_tracked = tracked;
  hand.is_active = tracked;
  for (std::uint32_t i = 0; i < QAR_HAND_JOINT_COUNT; ++i) {
    hand.joint_locations[i].joint_id = i;
    hand.joint_locations[i].location_flags = 0u;
  }
  const std::uint32_t index = static_cast<std::uint32_t>(joint);
  hand.joint_locations[index].pose.position = position;
  hand.joint_locations[index].pose.orientation = orientation;
  hand.joint_locations[index].location_flags =
    QAR_POSITION_VALID_BIT | (with_orientation ? QAR_ORIENTATION_VALID_BIT : 0ull);
  return hand;
}

}  // namespace

// ---------------------------------------------------------------------------
// The SDK's OWN ray: start point -> action point
//
// `qar_app_volume_gesture_get_points`: "from the start point to the action
// point is the ray the user is pointing along, which an app can draw itself",
// and the action point is "the intersection the gesture acts on, or the start
// point when nothing was intersected". So the pair IS a ray, except in exactly
// one documented case -- and that case is the fallback's whole job.
// ---------------------------------------------------------------------------

TEST(GestureRay, StartToActionIsTheRayAndItStartsAtTheHand)
{
  const QarVector3 start{0.0f, 1.4f, 0.0f};     // the hand
  const QarVector3 action{0.0f, 1.4f, -7.0f};   // what the aim hit, 7 m out
  const PointingRay ray =
    qar_ar_core::pointing_ray_from_gesture_points(true, start, true, action, 12.5);
  ASSERT_TRUE(ray.valid) << ray.error;
  EXPECT_EQ(ray.kind, PointingRayKind::GestureStartToAction);
  // The origin is the START point -- the hand -- because that is where the
  // runtime says the ray begins.
  expect_vec(ray.origin, 0.0f, 1.4f, 0.0f);
  expect_vec(ray.direction, 0.0f, 0.0f, -1.0f);
  EXPECT_NEAR(std::sqrt(
      (ray.direction.x * ray.direction.x) + (ray.direction.y * ray.direction.y) +
      (ray.direction.z * ray.direction.z)), 1.0f, kTol);
  EXPECT_DOUBLE_EQ(ray.timestamp_s, 12.5);
  EXPECT_TRUE(ray.error.empty());
}

TEST(GestureRay, TheRayPassesThroughTheActionPointItWasBuiltFrom)
{
  // The property that makes it usable at all: walk |action - start| along the
  // direction from the origin and you land on the action point.
  const QarVector3 start{0.3f, 1.5f, -0.2f};
  const QarVector3 action{4.3f, -0.5f, -8.2f};
  const PointingRay ray =
    qar_ar_core::pointing_ray_from_gesture_points(true, start, true, action);
  ASSERT_TRUE(ray.valid) << ray.error;
  EXPECT_NEAR(
    qar_ar_core::ray_point_distance(ray.origin, ray.direction, action), 0.0f, kTol);
}

TEST(GestureRay, EqualPointsAreRefusedAsTheNoIntersectionCaseAndNotAsAZeroRay)
{
  // THE case that is easy to read backwards. Equality is not "the pair is not a
  // ray"; it is the runtime saying it intersected nothing, which is legal and
  // must produce an explained refusal rather than a zero-length direction that
  // a plane intersection would then divide by.
  const QarVector3 both{1.0f, 1.2f, -0.5f};
  const PointingRay ray =
    qar_ar_core::pointing_ray_from_gesture_points(true, both, true, both);
  EXPECT_FALSE(ray.valid);
  EXPECT_EQ(ray.kind, PointingRayKind::None);
  expect_vec(ray.direction, 0.0f, 0.0f, 0.0f);
  EXPECT_NE(ray.error.find("intersected nothing"), std::string::npos) << ray.error;
  // And a degenerate ray is refused by the geometry too, so nothing downstream
  // can accidentally use it.
  EXPECT_LT(
    qar_ar_core::ray_point_distance(ray.origin, ray.direction, both), 0.0f);
}

TEST(GestureRay, AMissingPointIsRefusedWithItsOwnExplanation)
{
  const QarVector3 p{1.0f, 2.0f, 3.0f};
  const PointingRay no_action =
    qar_ar_core::pointing_ray_from_gesture_points(true, p, false, QarVector3{});
  EXPECT_FALSE(no_action.valid);
  EXPECT_FALSE(no_action.error.empty());

  const PointingRay no_start =
    qar_ar_core::pointing_ray_from_gesture_points(false, QarVector3{}, true, p);
  EXPECT_FALSE(no_start.valid);
  EXPECT_FALSE(no_start.error.empty());
}

TEST(GestureRay, ANonFinitePointIsRefused)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const PointingRay ray = qar_ar_core::pointing_ray_from_gesture_points(
    true, QarVector3{0.0f, 0.0f, 0.0f}, true, QarVector3{nan, 0.0f, -1.0f});
  EXPECT_FALSE(ray.valid);
  EXPECT_NE(ray.error.find("finite"), std::string::npos) << ray.error;
}

TEST(GestureRay, ASubMillimetreSpanIsTooShortToCarryADirection)
{
  // Not the documented no-hit case (the runtime copies the point verbatim, so
  // that one is exact equality) but the same refusal: a span that short is
  // numerically a point, and a direction taken from it is noise.
  const QarVector3 start{0.0f, 1.0f, 0.0f};
  const QarVector3 action{0.0f, 1.0f, -1e-5f};
  const PointingRay ray =
    qar_ar_core::pointing_ray_from_gesture_points(true, start, true, action);
  EXPECT_FALSE(ray.valid);
}

TEST(GestureRay, ItNamesItselfInTheLog)
{
  // The trace must say which source produced a ray, so a run can be read from
  // the log alone.
  EXPECT_STREQ(
    qar_ar_core::to_string(PointingRayKind::GestureStartToAction),
    "gesture-start-to-action");
  EXPECT_STRNE(
    qar_ar_core::to_string(PointingRayKind::GestureStartToAction),
    qar_ar_core::to_string(PointingRayKind::EyeThroughHand));
}

// ---------------------------------------------------------------------------
// Building the ray
// ---------------------------------------------------------------------------

TEST(PointingRay, EyeThroughHandIsTheRayAndItStartsAtTheEye)
{
  const PointingRay ray = pointing_ray_from(pointing_forward());
  ASSERT_TRUE(ray.valid) << ray.error;
  EXPECT_EQ(ray.kind, PointingRayKind::EyeThroughHand);
  // The origin is the EYE, not the hand: a ray cast from the fingertip and a
  // ray cast from the eye through the fingertip cross the ground plane in
  // different places, and only the second is what the wearer is sighting.
  expect_vec(ray.origin, 0.0f, 1.6f, 0.0f);
  expect_vec(ray.direction, 0.0f, 0.0f, -1.0f);
  EXPECT_NEAR(std::sqrt(
      (ray.direction.x * ray.direction.x) + (ray.direction.y * ray.direction.y) +
      (ray.direction.z * ray.direction.z)), 1.0f, kTol);
}

TEST(PointingRay, AimingDownwardsGivesADownwardDirection)
{
  PointingRayInputs in = pointing_forward();
  in.hand_position = QarVector3{0.0f, 1.0f, -0.6f};  // hand held low
  const PointingRay ray = pointing_ray_from(in);
  ASSERT_TRUE(ray.valid) << ray.error;
  EXPECT_LT(ray.direction.y, 0.0f) << "a hand below the eye must aim downward";
}

TEST(PointingRay, NoHandMeansNoRayAndAnExplanation)
{
  PointingRayInputs in = pointing_forward();
  in.hand_tracked = false;
  const PointingRay ray = pointing_ray_from(in);
  EXPECT_FALSE(ray.valid);
  EXPECT_EQ(ray.kind, PointingRayKind::None);
  EXPECT_FALSE(ray.error.empty());
}

TEST(PointingRay, AHandTooCloseToTheEyeIsRefusedRatherThanAmplified)
{
  PointingRayInputs in = pointing_forward();
  in.hand_position = QarVector3{0.0f, 1.6f, -0.02f};  // 2 cm: tracking noise
  const PointingRay ray = pointing_ray_from(in);
  EXPECT_FALSE(ray.valid);
  EXPECT_NE(ray.error.find("too close"), std::string::npos) << ray.error;
}

TEST(PointingRay, WithoutAnEyeTheHandsOwnForwardAxisAims)
{
  // No eye pose (a session that renders nothing), but the joint reports an
  // orientation. OpenXR joint poses run local -Z out along the bone, so the
  // identity orientation aims along room -Z.
  PointingRayInputs in;
  in.hand_tracked = true;
  in.hand_position = QarVector3{0.3f, 1.2f, -0.5f};
  in.has_hand_orientation = true;
  const PointingRay ray = pointing_ray_from(in);
  ASSERT_TRUE(ray.valid) << ray.error;
  EXPECT_EQ(ray.kind, PointingRayKind::HandForward);
  expect_vec(ray.origin, 0.3f, 1.2f, -0.5f);
  expect_vec(ray.direction, 0.0f, 0.0f, -1.0f);
}

TEST(PointingRay, ANonFiniteHandPositionIsRefused)
{
  PointingRayInputs in = pointing_forward();
  in.hand_position = QarVector3{std::nanf(""), 0.0f, 0.0f};
  EXPECT_FALSE(pointing_ray_from(in).valid);
}

// ---------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------

TEST(PointingRay, QuaternionRotationTurnsForwardAsExpected)
{
  // 90 degrees about +Y takes room -Z (forward) to -X.
  const float h = std::sqrt(0.5f);
  const QarQuaternion yaw90{0.0f, h, 0.0f, h};
  expect_vec(rotate_by_quaternion(yaw90, QarVector3{0.0f, 0.0f, -1.0f}), -1.0f, 0.0f, 0.0f);
  // A degenerate quaternion is the identity, never a silent scale.
  const QarQuaternion zero{0.0f, 0.0f, 0.0f, 0.0f};
  expect_vec(rotate_by_quaternion(zero, QarVector3{1.0f, 2.0f, 3.0f}), 1.0f, 2.0f, 3.0f);
}

TEST(PointingRay, RayPointDistanceIsThePerpendicularAndTheFrameCrossCheck)
{
  // The number the log uses to decide whether the hands and the action point
  // are in the same frame: zero when the point lies ON the ray, whatever its
  // distance downrange.
  const QarVector3 origin{0.0f, 0.0f, 0.0f};
  const QarVector3 direction{1.0f, 0.0f, 0.0f};
  EXPECT_NEAR(ray_point_distance(origin, direction, QarVector3{7.0f, 0.0f, 0.0f}), 0.0f, kTol);
  EXPECT_NEAR(ray_point_distance(origin, direction, QarVector3{7.0f, 0.0f, 3.0f}), 3.0f, kTol);
  // A degenerate ray cannot answer, and says so with -1 rather than a 0 that
  // would read as perfect agreement.
  EXPECT_LT(ray_point_distance(origin, QarVector3{0.0f, 0.0f, 0.0f}, origin), 0.0f);
}

// ---------------------------------------------------------------------------
// Reading the SDK's hand sample
// ---------------------------------------------------------------------------

TEST(PointingRay, TheIndexTipIsReadWhenItIsValid)
{
  const QarHandJoints hand =
    hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.2f, 1.1f, -0.4f});
  PointingRayInputs in;
  ASSERT_TRUE(hand_ray_inputs(hand, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip, in));
  EXPECT_TRUE(in.hand_tracked);
  EXPECT_EQ(in.joint, HandRayJoint::IndexTip);
  expect_vec(in.hand_position, 0.2f, 1.1f, -0.4f);
}

TEST(PointingRay, ACurledIndexFallsBackToThePalmRatherThanLosingTheRay)
{
  // Only the palm reports a valid position -- which is what a fist-shaped
  // "pointing" pose actually gives. Losing the ray there would drop the pick.
  const QarHandJoints hand = hand_with(QAR_HAND_JOINT_PALM_EXT, QarVector3{0.1f, 1.0f, -0.2f});
  PointingRayInputs in;
  ASSERT_TRUE(hand_ray_inputs(hand, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip, in));
  EXPECT_EQ(in.joint, HandRayJoint::Palm);
  expect_vec(in.hand_position, 0.1f, 1.0f, -0.2f);
}

TEST(PointingRay, AnUntrackedHandIsNotRead)
{
  const QarHandJoints hand =
    hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.2f, 1.1f, -0.4f}, /*tracked=*/false);
  PointingRayInputs in;
  EXPECT_FALSE(hand_ray_inputs(hand, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip, in));
  EXPECT_FALSE(in.hand_tracked);
}

TEST(PointingRay, AJointWithoutThePositionValidBitIsNotTrusted)
{
  QarHandJoints hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{5.0f, 5.0f, 5.0f});
  hand.joint_locations[QAR_HAND_JOINT_INDEX_TIP_EXT].location_flags = 0u;
  PointingRayInputs in;
  EXPECT_FALSE(hand_ray_inputs(hand, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip, in))
    << "a stale array entry must not become an aim";
}

TEST(PointingRay, TheOtherHandIsUsedWhenThePreferredOneIsNotTracked)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand.is_tracked = false;
  hands.left_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{-0.3f, 1.2f, -0.5f});
  const PointingRayInputs in =
    pointing_ray_inputs_from_hands(hands, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip);
  ASSERT_TRUE(in.hand_tracked) << "a left-handed operator must not be silently ignored";
  EXPECT_EQ(in.hand, QAR_HANDEDNESS_LEFT);
  expect_vec(in.hand_position, -0.3f, 1.2f, -0.5f);
}

TEST(PointingRay, NeitherHandTrackedGivesAnInvalidRayAndNotAZeroOne)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.left_hand.is_tracked = false;
  hands.right_hand.is_tracked = false;
  PointingRayInputs in =
    pointing_ray_inputs_from_hands(hands, QAR_HANDEDNESS_RIGHT, HandRayJoint::IndexTip);
  in.has_eye = true;
  in.eye_position = QarVector3{0.0f, 1.6f, 0.0f};
  const PointingRay ray = pointing_ray_from(in);
  EXPECT_FALSE(ray.valid);
  // A zero-length direction published as "valid" would make every plane target
  // refuse with "degenerate" instead of falling back cleanly.
  expect_vec(ray.direction, 0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// The thread hand-off
// ---------------------------------------------------------------------------

TEST(PointingRaySlot, LastWriterWinsAndCountsBothWaysForTheLog)
{
  PointingRaySlot slot;
  EXPECT_FALSE(slot.latest().valid);
  EXPECT_EQ(slot.published(), 0u);

  slot.publish(pointing_ray_from(pointing_forward()));
  EXPECT_TRUE(slot.latest().valid);
  EXPECT_EQ(slot.published(), 1u);
  EXPECT_EQ(slot.published_valid(), 1u);

  // Hands went away. The INVALID ray must be published too: keeping the last
  // good one would aim every later pick from where the hand was.
  PointingRayInputs gone = pointing_forward();
  gone.hand_tracked = false;
  slot.publish(pointing_ray_from(gone));
  EXPECT_FALSE(slot.latest().valid);
  EXPECT_EQ(slot.published(), 2u);
  EXPECT_EQ(slot.published_valid(), 1u);

  slot.clear();
  EXPECT_EQ(slot.published(), 0u);
  EXPECT_FALSE(slot.latest().valid);
}

// ---------------------------------------------------------------------------
// The hand itself -- the measurement that says what `action_point` IS
//
// The runtime documents a gesture's action_point as its own intersection of the
// wearer's aim with the app volume's scene; a published point that lands on the
// hand instead would contradict that. A distance from the reported point to a
// hand JOINT is the only thing that tells those two apart, so these tests pin
// that distance's arithmetic and, just as importantly,
// its refusals: an untracked hand must measure as UNKNOWN (-1) and never as 0,
// because a 0 would read as "the point is exactly on the hand" -- the very
// conclusion the measurement exists to test.
// ---------------------------------------------------------------------------

TEST(HandJointsProbe, ReadsEveryValidJointAndSkipsTheOnesTheRuntimeDidNotFill)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.2f, 1.4f, -0.5f});
  // A second joint on the same hand, so "nearest" has something to choose from.
  hands.right_hand.joint_locations[QAR_HAND_JOINT_PALM_EXT].pose.position =
    QarVector3{0.2f, 1.4f, -0.4f};
  hands.right_hand.joint_locations[QAR_HAND_JOINT_PALM_EXT].location_flags =
    QAR_POSITION_VALID_BIT;

  const HandJointsProbe probe = hand_joints_probe_from(hands);
  ASSERT_TRUE(probe.valid);
  EXPECT_TRUE(probe.right.tracked);
  EXPECT_FALSE(probe.left.tracked);
  EXPECT_TRUE(probe.right.has_joint[QAR_HAND_JOINT_INDEX_TIP_EXT]);
  EXPECT_TRUE(probe.right.has_joint[QAR_HAND_JOINT_PALM_EXT]);
  expect_vec(probe.right.joint[QAR_HAND_JOINT_INDEX_TIP_EXT], 0.2f, 1.4f, -0.5f);
  // Every other entry of the array is stale memory as far as we know, so it must
  // not become a joint the measurement can land on.
  EXPECT_FALSE(probe.right.has_joint[QAR_HAND_JOINT_THUMB_TIP_EXT]);
}

TEST(HandJointsProbe, AJointWithoutThePositionValidBitIsAbsentRatherThanAtTheOrigin)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{9.0f, 9.0f, 9.0f});
  hands.right_hand.joint_locations[QAR_HAND_JOINT_INDEX_TIP_EXT].location_flags = 0u;

  const HandJointsProbe probe = hand_joints_probe_from(hands);
  EXPECT_FALSE(probe.right.has_joint[QAR_HAND_JOINT_INDEX_TIP_EXT]);
  EXPECT_FALSE(nearest_hand_joint(probe, QarVector3{0.0f, 0.0f, 0.0f}).found);
}

TEST(HandJointsProbe, ANonFiniteJointIsRefused)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(
    QAR_HAND_JOINT_INDEX_TIP_EXT,
    QarVector3{std::numeric_limits<float>::quiet_NaN(), 1.4f, -0.5f});
  const HandJointsProbe probe = hand_joints_probe_from(hands);
  EXPECT_FALSE(probe.right.has_joint[QAR_HAND_JOINT_INDEX_TIP_EXT]);
}

TEST(NearestHandJointTest, PicksTheClosestJointOfEitherHandAndNamesIt)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{1.0f, 0.0f, 0.0f});
  hands.left_hand = hand_with(QAR_HAND_JOINT_PALM_EXT, QarVector3{0.1f, 0.0f, 0.0f});

  const HandJointsProbe probe = hand_joints_probe_from(hands);
  const NearestHandJoint nearest = nearest_hand_joint(probe, QarVector3{0.0f, 0.0f, 0.0f});
  ASSERT_TRUE(nearest.found);
  EXPECT_EQ(nearest.hand, QAR_HANDEDNESS_LEFT);
  EXPECT_EQ(nearest.joint_id, static_cast<std::uint32_t>(QAR_HAND_JOINT_PALM_EXT));
  EXPECT_NEAR(nearest.distance_m, 0.1f, kTol);
  EXPECT_STREQ(hand_joint_name(nearest.joint_id), "palm");
}

TEST(NearestHandJointTest, AnUntrackedHandMeasuresAsUnknownAndNotAsZero)
{
  // The distinction the whole diagnosis rests on: "no hands this frame" must
  // never be reported as a distance of zero, which would read as "the runtime's
  // point is exactly on the hand".
  const HandJointsProbe empty;
  EXPECT_FALSE(empty.valid);
  EXPECT_FALSE(nearest_hand_joint(empty, QarVector3{0.0f, 0.0f, 0.0f}).found);
  EXPECT_LT(nearest_hand_joint(empty, QarVector3{0.0f, 0.0f, 0.0f}).distance_m, 0.0f);
  EXPECT_LT(
    hand_joint_distance(
      empty, QAR_HANDEDNESS_RIGHT, QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.0f, 0.0f, 0.0f}),
    0.0f);
}

TEST(HandJointDistance, MeasuresTheNamedJointAndRefusesOneThatIsNotTracked)
{
  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.0f, 0.0f, 3.0f});
  const HandJointsProbe probe = hand_joints_probe_from(hands);

  EXPECT_NEAR(
    hand_joint_distance(
      probe, QAR_HANDEDNESS_RIGHT, QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.0f, 0.0f, 0.0f}),
    3.0f, kTol);
  // The same joint on the hand that is not tracked.
  EXPECT_LT(
    hand_joint_distance(
      probe, QAR_HANDEDNESS_LEFT, QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.0f, 0.0f, 0.0f}),
    0.0f);
  // A joint the runtime did not fill on a hand that IS tracked.
  EXPECT_LT(
    hand_joint_distance(
      probe, QAR_HANDEDNESS_RIGHT, QAR_HAND_JOINT_WRIST_EXT, QarVector3{0.0f, 0.0f, 0.0f}),
    0.0f);
}

TEST(PointingRaySlot, TheRayAndTheHandSampleTravelAsOneFrame)
{
  // The pairing is the point: a consumer that took this frame's ray and the
  // previous frame's hand would measure a distance that never existed.
  PointingRaySlot slot;
  EXPECT_FALSE(slot.latest_probe().valid);

  QarDeviceHandsWithJoints hands = qar_device_hand_with_joints_default();
  hands.right_hand = hand_with(QAR_HAND_JOINT_INDEX_TIP_EXT, QarVector3{0.0f, 1.6f, -0.6f});
  HandJointsProbe probe = hand_joints_probe_from(hands);
  probe.has_eye = true;
  probe.eye_position = QarVector3{0.0f, 1.6f, 0.0f};

  slot.publish(pointing_ray_from(pointing_forward()), probe);
  EXPECT_TRUE(slot.latest().valid);
  ASSERT_TRUE(slot.latest_probe().valid);
  expect_vec(
    slot.latest_probe().right.joint[QAR_HAND_JOINT_INDEX_TIP_EXT], 0.0f, 1.6f, -0.6f);
  EXPECT_EQ(slot.published(), 1u);

  // The ray-only overload publishes NO hands rather than keeping the last ones:
  // a stale hand would be measured against a fresh point.
  slot.publish(pointing_ray_from(pointing_forward()));
  EXPECT_FALSE(slot.latest_probe().valid);

  slot.publish(pointing_ray_from(pointing_forward()), probe);
  slot.clear();
  EXPECT_FALSE(slot.latest_probe().valid);
}
