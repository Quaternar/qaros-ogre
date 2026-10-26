// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__QAR_POSE_CONVERSION_HPP_
#define QAR_OGRE_STREAMING__QAR_POSE_CONVERSION_HPP_

#include "qar_ogre_streaming/types.hpp"

/// @file qar_pose_conversion.hpp
/// @brief Qar pose/FOV -> Ogre camera transform + projection helpers.
///
/// ## Coordinate conventions (unit-tested)
///
/// A full write-up lives in `docs/ARCHITECTURE.md#pose-projection-and-world-basis`.
/// Summary:
///
/// **Qar** uses the OpenXR convention (verified against the vendored
/// `qar_streaming.h`: `QAR_HANDEDNESS_RIGHT` is the default, gesture docs put
/// Y=up / Z=forward-back and yaw/pitch/roll on the y/x/z axes, and `QarFov`'s
/// default `{-45,+45,+45,-45}°` matches `XrFovf` exactly):
///   - Right-handed. **X right, Y up, -Z forward** (the view/camera looks down
///     its local -Z axis).
///   - `QarQuaternion` is stored **x, y, z, w** (xyzw); identity is `{0,0,0,1}`.
///   - `QarFov` carries **signed half-angles in radians**: `angle_left` and
///     `angle_down` are typically negative, `angle_right`/`angle_up` positive.
///
/// **Ogre** cameras are also right-handed and, in the camera's local space,
/// look down **-Z** with **X right, Y up** — the *same* frame as Qar/OpenXR.
/// `Ogre::Quaternion` is stored **w, x, y, z** (wxyz).
///
/// ### Consequence: the pose mapping is the IDENTITY mapping
///   - Position is copied component-for-component (no axis swap, no sign flip).
///   - Orientation is copied component-for-component; only the *storage order*
///     changes (Qar xyzw -> Ogre/@ref Quat wxyz). It is the SAME rotation — no
///     handedness flip, no conjugation. "Identity passthrough" means exactly
///     this: the numbers are preserved; only the field layout is reindexed.
///   - This is the default. QAR_POSE_WORLD=zup additionally rotates the WORLD basis
///     (Y-up -> Z-up) for Z-up content such as RViz scenes; see qar_pose_conversion.cpp.
///
/// The output types (@ref Vec3, @ref Quat, @ref Mat4) are plain PODs so the
/// public API never exposes Ogre math types. @ref Quat uses Ogre's w,x,y,z field
/// order, so it can be handed straight to `Ogre::Quaternion(q.w,q.x,q.y,q.z)` and
/// assigned to a camera scene node. @ref Mat4 is **row-major** and matches
/// `Ogre::Matrix4`'s memory layout, so `Ogre::Matrix4(m.m[0], m.m[1], ...)` (its
/// 16-float row-major constructor) reproduces it directly, as does
/// `Ogre::Camera::setCustomProjectionMatrix(true, that_matrix)`.

namespace qar_ogre_streaming
{

/// @brief Plain 3-vector output (avoids exposing Ogre::Vector3 in the header).
struct Vec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

/// @brief Plain quaternion output. Field order is Ogre's **w, x, y, z**, so it
/// maps directly onto `Ogre::Quaternion(w, x, y, z)`.
struct Quat
{
  float w = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

/// @brief Row-major 4x4 matrix (16 floats), `m[row * 4 + col]`.
///
/// This is byte-for-byte compatible with `Ogre::Matrix4` (which is row-major,
/// `m[i][j]` = row i, col j). The stored matrix follows the OpenGL /
/// `Ogre::Frustum::getProjectionMatrix()` convention: right-handed view space
/// looking down -Z, clip-space depth in **[-1, +1]**.
struct Mat4
{
  float m[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1};
};

/// @brief Quaternions with a norm below this are treated as degenerate.
constexpr float kMinQuaternionNorm = 1e-6f;

/// @brief Convert a Qar position to an Ogre-space position (identity mapping).
///
/// Qar and Ogre camera-local space share the frame (X right, Y up, -Z forward,
/// right-handed), so the position is copied unchanged. Cannot fail.
Vec3 qar_position_to_ogre(const QarVector3 & position);

/// @brief Convert a Qar orientation to an Ogre-space orientation.
///
/// Reorders storage xyzw -> wxyz (same rotation) and normalizes. This is the
/// convenience form: a degenerate (near-zero-length) quaternion falls back to
/// the identity orientation instead of returning an error. Use
/// @ref qar_pose_to_ogre when you need explicit rejection of degenerate input.
Quat qar_orientation_to_ogre(const QarQuaternion & orientation);

/// @brief Convert a full Qar pose to an Ogre camera position + orientation.
///
/// @param pose         Qar pose (orientation quaternion + position).
/// @param[out] out_position     Receives the Ogre-space position.
/// @param[out] out_orientation  Receives the normalized Ogre-space orientation.
/// @return Ok on success; InvalidArgument if the orientation quaternion has a
///         near-zero length (< @ref kMinQuaternionNorm) and cannot be
///         normalized. On error the outputs are left at identity.
///
/// Non-unit (but non-degenerate) quaternions are normalized. This is the checked
/// path intended for per-eye camera updates.
Result qar_pose_to_ogre(
  const QarPose & pose,
  Vec3 & out_position,
  Quat & out_orientation);

/// @brief Build an asymmetric off-axis perspective projection from a QarFov.
///
/// The frustum extents are taken at the near plane:
///   left   = near * tan(fov.angle_left)   (typically < 0)
///   right  = near * tan(fov.angle_right)   (typically > 0)
///   bottom = near * tan(fov.angle_down)    (typically < 0)
///   top    = near * tan(fov.angle_up)      (typically > 0)
///
/// and fed into the standard OpenGL off-axis (glFrustum) matrix, which is
/// identical to `Ogre::Frustum::setFrustumExtents(left,right,top,bottom)` +
/// `getProjectionMatrix()` (depth range [-1, +1], right-handed, looking down -Z).
///
/// @param fov         OpenXR-style asymmetric field of view (signed radians).
/// @param near_plane  Near clip distance ( > 0 ).
/// @param far_plane   Far clip distance ( > near_plane ).
/// @param[out] out_projection  Receives the row-major projection matrix.
/// @return Ok on valid input; InvalidArgument for a degenerate frustum
///         (near <= 0, far <= near, angle_right <= angle_left,
///         angle_up <= angle_down, or any |angle| >= pi/2 where tan is undefined).
Result make_projection_from_qar_fov(
  const QarFov & fov,
  float near_plane,
  float far_plane,
  Mat4 & out_projection);

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__QAR_POSE_CONVERSION_HPP_
