// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

/// @file ogre_qar_stereo_stream_support.cpp
/// @brief Pure (Ogre-free) helpers for OgreQarStereoStream.
///
/// Like ogre_stereo_render_target_support.cpp, these live in their own
/// translation unit and call NO Ogre runtime function, so unit tests that
/// reference them (per-eye pose/FOV conversion, separated-frame assembly) do not
/// drag OgreMain.dll onto the headless test binary. The Ogre-touching apply +
/// render lives in ogre_qar_stereo_stream.cpp and is validated by the in-RViz
/// scripted stereo capture (see docs).

#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"

namespace qar_ogre_streaming
{

bool OgreQarStereoStream::current_head_position(Vec3 & out) const
{
  if (!pending_wants_render_) {
    return false;
  }
  // Eye midpoint. Both eyes go through the SAME conversion the cameras use, so the
  // light tracks the peer even under the Z-up world basis change (QAR_POSE_WORLD).
  const Vec3 l = qar_position_to_ogre(pending_info_.left_eye.pose.position);
  const Vec3 r = qar_position_to_ogre(pending_info_.right_eye.pose.position);
  out = Vec3{(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
  return true;
}

EyeRenderParams compute_eye_render_params(const EyeView & eye)
{
  EyeRenderParams p;
  // Pose via the checked path (rejects degenerate quaternions).
  const Result pose_res = qar_pose_to_ogre(eye.pose, p.position, p.orientation);
  p.pose_valid = pose_res.ok();

  // Asymmetric off-axis projection.
  const Result proj_res = make_projection_from_qar_fov(
    eye.fov, eye.near_far.near_plane, eye.near_far.far_plane, p.projection);
  p.projection_valid = proj_res.ok();
  return p;
}

QarOpenGlStereoFrame assemble_separated_stereo_frame(
  const QarPeerId & peer_id,
  uint64_t frame_index,
  const StereoFrameTextures & textures)
{
  QarOpenGlStereoFrame frame{};
  frame.peer_id = peer_id;
  frame.frame_index = frame_index;
  frame.layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;

  // Positional color textures (texture_index 0 = left, 1 = right).
  frame.left_color.size = textures.color_size;
  frame.left_color.texture_id = textures.left_color_id;
  frame.left_color.target = textures.gl_target;

  frame.right_color.size = textures.color_size;
  frame.right_color.texture_id = textures.right_color_id;
  frame.right_color.target = textures.gl_target;

  frame.has_depth = textures.has_depth;
  if (textures.has_depth) {
    frame.left_depth.size = textures.depth_size;
    frame.left_depth.texture_id = textures.left_depth_id;
    frame.left_depth.target = textures.gl_target;

    frame.right_depth.size = textures.depth_size;
    frame.right_depth.texture_id = textures.right_depth_id;
    frame.right_depth.target = textures.gl_target;
  }

  // Explicit per-view descriptors (eye labels + backing texture indices).
  std::size_t n = 0;
  frame.views[n++] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 0u};
  frame.views[n++] = QarRenderFrameView{
    QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 1u};
  if (textures.has_depth) {
    frame.views[n++] = QarRenderFrameView{
      QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 2u};
    frame.views[n++] = QarRenderFrameView{
      QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 3u};
  }
  frame.views_count = n;

  // synchronization left default (qar_waits_on_fence=false): the stream attaches its
  // GL fence when it submits.
  return frame;
}

}  // namespace qar_ogre_streaming
