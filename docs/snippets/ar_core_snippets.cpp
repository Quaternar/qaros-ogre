// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Snippets of API.md for the module qar_ar_core: the seam of the panel path, the pointing ray, the room-to-ROS
// frame change and the pick model. The functions are compiled and linked and never run. Their parameters stand
// for what the host owns, so each one is a pointer that the documentation names in its text.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/panel_types.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_ar_core/qar_panel_sender.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/spatial_frames.hpp"
#include "qar_ar_core/spatial_pick.hpp"

void snippet_panel_seam(qar_ar_core::IQarPanelSenderFactory * factory, const std::uint8_t * pixels)
{
  // [qar-snippet:panel_seam]
  qar_ar_core::PanelDesc desc;
  desc.key = "status";                                    // the key that the host uses for the panel
  desc.display_name = "Status";                           // the title that QarOS shows
  desc.buffer_width = 1024;                               // the size of the buffer is fixed for the sender
  desc.buffer_height = 768;
  desc.placement.pose.position = QarVector3{0.0f, 1.6f, -1.0f};
  desc.placement.size = QarGuiPanelSize{0.8f, 0.6f, 1.0f};

  const std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> sender = factory->create_panel_sender(desc);
  if (!sender) {
    std::fprintf(stderr, "panel: %s\n", factory->last_error().c_str());
    return;
  }

  // Once for each tick, on the thread of the host that owns the widget.
  bool ready = false;
  if (sender->try_begin_frame(ready) && ready) {
    qar_ar_core::PanelPixels frame;
    frame.data = pixels;
    frame.width = desc.buffer_width;
    frame.height = desc.buffer_height;
    frame.stride_bytes = desc.buffer_width * 4u;          // RGBA: four bytes for each pixel
    sender->submit_frame(frame);                          // copies the pixels; the buffer stays the host's
  }

  sender->close();                                        // the one teardown, in a fixed order
  // [qar-snippet-end:panel_seam]
}

void snippet_pointing_ray(
  const QarDeviceHandsWithJoints & hands, const QarVector3 & eye_position, qar_ar_core::PointingRaySlot * slot)
{
  // [qar-snippet:pointing_ray]
  // The eye pose of this frame and the hands of the device give the aim: the eye through the index tip.
  qar_ar_core::PointingRayInputs inputs = qar_ar_core::pointing_ray_inputs_from_hands(
    hands, QAR_HAND_RIGHT, qar_ar_core::HandRayJoint::IndexTip);
  inputs.has_eye = true;
  inputs.eye_position = eye_position;
  const qar_ar_core::PointingRay ray = qar_ar_core::pointing_ray_from(inputs);
  if (!ray.valid) {
    std::printf("no ray: %s\n", ray.error.c_str());
  }
  slot->publish(ray, qar_ar_core::hand_joints_probe_from(hands));   // the last writer wins

  // On any other thread:
  const qar_ar_core::PointingRay latest = slot->latest();
  std::printf("ray kind: %s\n", qar_ar_core::to_string(latest.kind));
  // [qar-snippet-end:pointing_ray]
}

void snippet_spatial_frames()
{
  // [qar-snippet:spatial_frames]
  // The camera and the picker must use one map: both read the same variables, once, here.
  const qar_ar_core::RoomToRosTransform transform = qar_ar_core::room_to_ros_from_environment();

  const QarVector3 room_point{1.0f, 0.5f, -2.0f};            // metres, +Y up, -Z forward
  const QarVector3 ros_point = qar_ar_core::room_point_to_ros(transform, room_point);
  const QarVector3 back = qar_ar_core::ros_point_to_room(transform, ros_point);   // the exact inverse
  const QarVector3 ros_direction =
    qar_ar_core::room_direction_to_ros(transform, QarVector3{0.0f, 0.0f, -1.0f});
  const QarQuaternion ros_orientation =
    qar_ar_core::room_orientation_to_ros(transform, QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f});
  std::printf("ros %.2f %.2f %.2f, room again %.2f, forward x %.2f, w %.2f\n", ros_point.x, ros_point.y,
    ros_point.z, back.x, ros_direction.x, ros_orientation.w);
  // [qar-snippet-end:spatial_frames]
}

void snippet_pick_session(qar_ar_core::IQarPanelInputSource * source)
{
  // [qar-snippet:pick_session]
  const qar_ar_core::RoomToRosTransform transform = qar_ar_core::room_to_ros_from_environment();
  qar_ar_core::PointPickSession point_pick(qar_ar_core::PickTarget::action_point());
  qar_ar_core::PosePickSession pose_pick(qar_ar_core::PickTarget::ground());

  std::vector<qar_ar_core::PanelPointerSample> samples;
  source->drain(samples);
  for (const qar_ar_core::PanelPointerSample & sample : samples) {
    const qar_ar_core::SpatialPointer pointer = qar_ar_core::spatial_pointer_from_sample(sample, transform);
    if (!pointer.valid) {
      continue;                                              // a pointer on a panel is not a spatial one
    }
    point_pick.update(pointer);                              // a host feeds the session of the kind of pick
    pose_pick.update(pointer);                               // that the operator asked for
  }

  if (point_pick.state() == qar_ar_core::PickState::Committed) {
    const qar_ar_core::SpatialPoint & point = point_pick.result();
    std::printf("picked %.2f %.2f %.2f by %s\n", point.position.x, point.position.y, point.position.z,
      qar_ar_core::to_string(point.method));
  }
  if (pose_pick.state() == qar_ar_core::PickState::Committed) {
    const qar_ar_core::NavGoal goal = qar_ar_core::make_nav_goal(pose_pick.result());
    std::printf("goal in frame %s, yaw %.2f rad\n", goal.frame_id.c_str(), goal.yaw_rad);
  }
  // [qar-snippet-end:pick_session]
}
