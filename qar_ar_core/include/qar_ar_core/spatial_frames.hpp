// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__SPATIAL_FRAMES_HPP_
#define QAR_AR_CORE__SPATIAL_FRAMES_HPP_

/// @file spatial_frames.hpp
/// @brief The room <-> ROS frame change, made explicit. Pure math, Qt-free.
///
/// ## Why this is the one place the convention lives
///
/// QarOS reports everything — head poses, panel placements, and the gesture
/// `action_point` a spatial pick uses — in **the room's axis convention**:
/// OpenXR's frame, right-handed, **+Y up, -Z forward**
/// (see docs/ARCHITECTURE.md#pose-projection-and-world-basis). The axes are what
/// this file changes; which frame's UNITS a given API states its numbers in is a
/// separate question, answered below ("The SDK reports gesture points in APP
/// content space") and, since the drop after the minimum drop, by its own
/// `@section qar_coordinate_systems`, which calls that frame **App content
/// space** and says an application uses its values as they come.
/// RViz renders a **ROS/REP-103 world**: right-handed, metres, **+X forward,
/// +Y left, +Z up**.
///
/// Feeding a Y-up pose straight into the Z-up world tips the viewer on its side
/// (a "bird's perspective"). The convention this header states is the proper
/// rotation (det = +1)
///
/// @code
///   ros = (-room.z, -room.x, room.y)
/// @endcode
///
/// i.e. the quaternion (w, x, y, z) = (0.5, 0.5, -0.5, -0.5): room up (+Y)
/// becomes ROS up (+Z), and room forward (-Z) becomes ROS forward (+X).
///
/// **A spatial pick MUST use the same map as the camera.** The AR wearer aims at
/// what they SEE, and what they see was placed by `qar_position_to_ogre()` /
/// `qar_pose_to_ogre()` in `qar_ogre_streaming/src/qar_pose_conversion.cpp`.
/// Deriving a second convention here would put the pick somewhere the wearer
/// never pointed, and it would fail *silently*. So this header reproduces that
/// function's transform exactly, including the viewer anchor it applies after
/// the basis change:
///
/// | Knob | `qar_pose_conversion.cpp` env var | Default | Meaning |
/// |---|---|---|---|
/// | @ref RoomToRosTransform::yaw_rad  | `QAR_POSE_YAW_DEG`  | 180 deg | yaw about ROS +Z |
/// | @ref RoomToRosTransform::height_m | `QAR_POSE_HEIGHT_M` | 0 | eye height along ROS +Z |
/// | @ref RoomToRosTransform::back_m   | `QAR_POSE_BACK_M`   | 0 | stand-off behind the facing direction |
///
/// The values are passed in rather than read from the environment because this
/// is a pure unit: the host reads the same environment once and hands the same
/// numbers to both the camera and the picker, which is exactly the coupling
/// that keeps them consistent. @ref room_to_ros_from_environment does that read
/// in one place for hosts that want it.
///
/// `enabled == false` reproduces the library's *other* documented mode, the
/// identity mapping that the capture paths and the pose-conversion tests assert
/// (`QAR_POSE_WORLD` unset). A picker configured that way treats room space as
/// already being the ROS world.
///
/// ## The SDK reports gesture points in APP content space
///
/// The basis change above is the WHOLE map for a gesture. The SDK's
/// `@section qar_coordinate_systems` (`third_party/qar/include/qar_streaming.h`)
/// names the frame and gives it to the view poses, the hand poses and a mapped
/// app volume gesture's start and action points at once:
///
/// > *"**App content space.** The application's own world inside the box, the
/// > space it renders in: app volume space taken into the app pose, positions
/// > divided by the app scale, so in app metres. A render sender bound to an app
/// > volume hands out its view poses and hand poses in it, and a mapped app
/// > volume gesture's start and action points are in it, **so an application
/// > uses them as they come.**"*
///
/// "As they come" is the basis change and nothing else, which is what
/// `RoomToRosTransform::placement` staying at identity on the pick path
/// implements: applying the app volume's placement inverse to a gesture point
/// misplaces the pick by metres. The measurements behind this are in
/// docs/ARCHITECTURE.md#gesture-points-are-in-app-content-space, and
/// `spatial_frames_tests.cpp` pins the canonical numbers.
///
/// @ref ScenePlacement stays: it is read back live, it is logged every tick, and
/// it remains the right model for anything that genuinely IS in room space. What
/// it must not do is reach the pick transform.
/// qar_rviz_plugin's `QarStreamingPanel::refreshScenePlacement()` leaves
/// @ref RoomToRosTransform::placement at its identity default on purpose, with
/// the reason spelled out at that call site.
///
/// Experiments ruled out the scale and the volume-centre translation as terms of
/// the pick. A non-zero `app_pose`, a rotated volume and a rotated app pose are
/// not covered; if picks go wrong with a moved volume, measure THAT rather than
/// putting the whole placement back on the pick path.
///
/// **If you do ever need the placement, it is NOT static and NOT what we passed
/// in.** The app volume we create keeps the runtime's default gesture rules
/// (`gesture_configuration` is left NULL), among them two MANIPULATION mapping
/// rules: dual-pointer distance drives `app_scale` and single-pointer 6DoF
/// drives `app_pose`.
/// Grab-to-move and pinch-to-zoom are therefore the RUNTIME's writes to our
/// volume — the QarOS session log publishes `appVolume/appPose/updated` and
/// `appVolume/appScale/updated` for it — so the only correct source is a
/// read-back through `qar_app_volume_get_pose` / `_get_app_pose` /
/// `_get_app_scale`, taken live. A placement built from the creation-time
/// config is a stale guess.

#include "qar_ar_core/qar_streaming_c.hpp"

#include <string>

namespace qar_ar_core
{

/// @brief Default viewer-anchor yaw, matching `QAR_POSE_YAW_DEG`'s default of 180 deg.
extern const float kDefaultAnchorYawRad;

/// @brief Where QarOS is currently putting our scene, and at what size.
///
/// The vendor's three frames, in the order the header defines them:
///
///   app space --(`app_scale`)--> scaled app --(`app_pose`)--> volume space
///             --(`volume_pose`)--> room space
///
/// "App space" here is our streamed scene in the **room's axis convention**
/// (+Y up), i.e. the ROS world after @ref room_point_to_ros' basis change has
/// been undone. That is the only ordering consistent with the vendor's own
/// definition — volume space is +Y up, and `app_pose` is expressed in volume
/// metres — and it is what keeps a 90 deg volume yaw meaning a yaw about the
/// room's vertical rather than about a ROS axis.
///
/// Default-constructed this is the IDENTITY placement (unit scale, both poses
/// identity), and that default is what the spatial pick path uses: a transform
/// that leaves `placement` alone maps a gesture's app-space point straight into
/// ROS metres, which is what the SDK's app content space calls for (see the
/// file comment). This struct is kept for the DIAGNOSTIC — the panel reads the
/// real placement back every tick and logs it — and for any future caller with a
/// quantity that is genuinely stated in room metres.
struct ScenePlacement
{
  /// True once the values came from a live read-back rather than the defaults.
  /// Carried so a log line can say "assumed identity" instead of quietly
  /// printing 1.000 as though it had been measured.
  bool known = false;

  /// `QarAppVolumeInit::app_scale`, read back with `qar_app_volume_get_app_scale`:
  /// **room metres per app/scene metre**. 0.08 means a 9.4 m scene distance is
  /// 0.75 m of room. Values <= 0 or non-finite are treated as 1.0 by the maths
  /// below rather than producing infinities.
  float app_scale = 1.0f;

  /// The cuboid's centre and rotation in room space
  /// (`qar_app_volume_get_pose`). Not the floor and not a corner — the CENTRE.
  QarPose volume_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

  /// The app origin inside volume space, in volume metres
  /// (`qar_app_volume_get_app_pose`). `app_scale` does NOT scale it — the
  /// vendor is explicit about that, and so is the arithmetic here.
  QarPose app_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
};

/// @brief True when @p placement maps app metres to room metres unchanged.
///
/// The cheap guard that keeps the identity path bit-identical: an identity
/// placement is skipped entirely rather than run through a scale of 1 and two
/// identity quaternions.
bool scene_placement_is_identity(const ScenePlacement & placement);

/// @brief `app_scale`, sanitized: non-finite or <= 0 becomes 1.0.
float scene_placement_scale(const ScenePlacement & placement);

/// @brief App/scene point (room axis convention, +Y up) -> room point.
QarVector3 scene_point_to_room(const ScenePlacement & placement, const QarVector3 & scene);

/// @brief The exact inverse of @ref scene_point_to_room.
QarVector3 room_point_to_scene(const ScenePlacement & placement, const QarVector3 & room);

/// @brief One log line describing @p placement, in the units it is stated in.
///
/// Lives here rather than at each call site because the placement is the fact
/// whoever reads a log has to see: "scale=0.083 room-m/scene-m" is the whole
/// diagnosis, and it must read the same in the panel log, the gesture trace and
/// a test.
std::string describe_scene_placement(const ScenePlacement & placement);

/// @brief The rotation half only, so a normalized direction stays normalized.
QarVector3 scene_direction_to_room(const ScenePlacement & placement, const QarVector3 & scene);

/// @brief The exact inverse of @ref scene_direction_to_room.
QarVector3 room_direction_to_scene(const ScenePlacement & placement, const QarVector3 & room);

/// @brief Room (QarOS/OpenXR, +Y up) -> ROS/RViz world (REP-103, +Z up).
struct RoomToRosTransform
{
  /// false reproduces the identity mapping (`QAR_POSE_WORLD` unset): room
  /// coordinates are used as ROS coordinates unchanged, anchor included.
  bool enabled = true;

  float yaw_rad = 3.14159265358979323846f;  ///< About ROS +Z, applied after the basis change.
  float height_m = 0.0f;                    ///< Added along ROS +Z.
  float back_m = 0.0f;                      ///< Stand-off behind the facing direction.

  /// Where QarOS currently puts our scene. Applied BEFORE the basis change on
  /// the room -> ROS trip and AFTER it on the way back, so the two directions
  /// remain exact inverses. Left at its default this changes nothing.
  ///
  /// **The spatial pick path leaves this at its default, on purpose.** Gesture
  /// action points, the view pose and the hand joints are reported in APP
  /// content space (see the file comment), so they need the basis change
  /// alone; applying the placement to them throws picks metres off the target
  /// and below the floor. Set this only for a quantity you have MEASURED to be
  /// in room metres.
  ///
  /// It is part of THIS struct rather than a second argument threaded through
  /// every call because the pick and the camera must share one map, and a map
  /// you can forget to pass is a map that will be forgotten.
  ScenePlacement placement;
};

/// @brief Read the same environment variables `qar_pose_conversion.cpp` reads.
///
/// `QAR_POSE_WORLD=zup` enables the basis change; `QAR_POSE_YAW_DEG` (180),
/// `QAR_POSE_HEIGHT_M` (0) and `QAR_POSE_BACK_M` (0) fill the anchor. A
/// malformed number keeps its default rather than throwing.
RoomToRosTransform room_to_ros_from_environment();

/// @brief Map a QarOS-reported POINT (metres, +Y up) into the ROS world.
///
/// With the default identity @ref RoomToRosTransform::placement — which is what
/// the spatial pick uses — this is the basis change and the viewer anchor, and
/// nothing else. That is the correct map for everything QarOS reports through
/// the gesture, view-pose and hand-joint APIs, all of which are in APP space
/// (see the file comment).
///
/// A non-default placement is undone FIRST (room metres -> app metres), before
/// the basis change: the scale and the volume offset live on the ROOM side of
/// it, so applying them afterwards would rotate the offset into the wrong axis.
QarVector3 room_point_to_ros(const RoomToRosTransform & transform, const QarVector3 & room);

/// @brief Map a QarOS-reported DIRECTION into the ROS world.
///
/// The rotations only — no anchor translation and no @ref ScenePlacement scale
/// — so a ray direction keeps its length and a normalized ray stays normalized.
/// A distance measured along the mapped ray is therefore in ROS/scene metres,
/// which is the frame every @ref PickTarget quantity is stated in.
QarVector3 room_direction_to_ros(const RoomToRosTransform & transform, const QarVector3 & room);

/// @brief Map a room-space ORIENTATION (`QarQuaternion`, xyzw) into the ROS world.
///
/// Left-multiplication by the basis change and then the anchor yaw, exactly as
/// `qar_pose_to_ogre()` does: the rotated frame's LOCAL axes are unchanged.
/// A degenerate (near-zero-length) input returns the identity quaternion.
QarQuaternion room_orientation_to_ros(
  const RoomToRosTransform & transform, const QarQuaternion & room);

/// @brief The exact inverse of @ref room_point_to_ros.
///
/// Needed to draw a ROS-world position back into the streamed AR scene (the
/// pick marker, the ray) without inventing a second convention for the return
/// trip.
QarVector3 ros_point_to_room(const RoomToRosTransform & transform, const QarVector3 & ros);

/// @brief The exact inverse of @ref room_direction_to_ros.
QarVector3 ros_direction_to_room(const RoomToRosTransform & transform, const QarVector3 & ros);

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__SPATIAL_FRAMES_HPP_
