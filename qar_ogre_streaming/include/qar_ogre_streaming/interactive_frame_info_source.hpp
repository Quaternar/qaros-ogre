// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__INTERACTIVE_FRAME_INFO_SOURCE_HPP_
#define QAR_OGRE_STREAMING__INTERACTIVE_FRAME_INFO_SOURCE_HPP_

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Interactive (user-driven) per-eye stereo frame-info generator (M9.5).
///
/// Where @ref MockQarFrameInfoSource *animates* a head pose deterministically
/// from a frame index, this source holds an EXPLICIT head pose that the host
/// mutates in response to user input (WASD translation + mouse-drag look). Given
/// its current head pose it produces a @ref QarFrameInfo whose two eyes are offset
/// by +/- IPD/2 along the head's local right axis, each with the same asymmetric,
/// aspect-matched field of view produced by @ref MockQarFrameInfoSource::make_eye_fov.
///
/// It is a pure computation — no Ogre, no Qt, no wall-clock — so the interactive
/// head-pose math is unit-testable headlessly, and the Qt key/mouse handling stays
/// entirely in the RViz plugin (which only calls the movement primitives below).
///
/// ## Coordinate convention
/// Same as @ref MockQarFrameInfoSource: poses are produced directly in the Ogre
/// world frame the stream renders in (M2 Qar->Ogre pose map is the identity: X
/// right, Y up, -Z forward, right-handed). RViz's Ogre scene is **Z-up**, so the
/// default @ref Config uses world +Z as the up axis. The head look direction is
/// parameterised by (yaw, pitch): yaw rotates about the up axis, pitch tilts the
/// look up/down. At yaw=0, pitch=0 the head looks along the first basis axis
/// (+X for a Z-up world).
///
/// ## Stereo model
/// Parallel stereo, identical to the mock source: both eyes share the head
/// orientation and are translated by +/- ipd/2 along the head's local right axis.
class InteractiveFrameInfoSource
{
public:
  struct Config
  {
    // --- Stereo ------------------------------------------------------------
    float ipd_metres = 0.064f;             ///< Inter-pupillary distance (m).

    // --- Frame / up axis (Z-up look for RViz by default) -------------------
    QarVector3 up_axis{0.0f, 0.0f, 1.0f};  ///< World up (RViz: +Z).

    // --- Initial head pose -------------------------------------------------
    /// @brief Head (eye-midpoint) position at construction. Default places the
    /// viewer back along +X and slightly up, looking toward the origin — a good
    /// default framing of the RViz grid/markers near the origin.
    QarVector3 start_position{6.0f, 6.0f, 4.0f};
    float start_yaw_rad = 3.926990f;   ///< ~225 deg: looks back toward -X,-Y (origin).
    float start_pitch_rad = -0.45f;    ///< Tilted slightly down toward the ground plane.

    // --- Per-eye field of view --------------------------------------------
    float vertical_fov_rad = 1.30f;        ///< Total vertical FOV (~74.5 deg).
    float aspect = 4.0f / 3.0f;            ///< Target width/height (no stretch).
    float fov_asymmetry_tan = 0.008f;      ///< Per-eye nasal/temporal tan-space shift.

    // --- Clip planes -------------------------------------------------------
    float near_plane = 0.1f;
    float far_plane = 500.0f;

    // --- Interaction limits ------------------------------------------------
    /// @brief Pitch is clamped to +/- this (radians) so the view never flips over
    /// or gimbal-locks straight up/down. ~85 deg.
    float max_pitch_rad = 1.4835298f;
  };

  InteractiveFrameInfoSource() { reset(); }
  explicit InteractiveFrameInfoSource(const Config & config) : config_(config) { reset(); }

  const Config & config() const { return config_; }
  void set_config(const Config & config);

  /// @brief Reset the live head pose back to the configured start pose.
  void reset();

  // --- Interactive controls (host maps key/mouse deltas to these) ----------
  /// @brief Translate the head along its current look (forward) direction by @p d
  /// metres (negative moves backward). W/S.
  void move_forward(float d);
  /// @brief Translate the head along its local right axis by @p d metres
  /// (negative strafes left). A/D.
  void move_right(float d);
  /// @brief Translate the head along the world up axis by @p d metres. Q/E (or R/F).
  void move_up(float d);
  /// @brief Add @p d radians to the yaw (turn about the up axis). Mouse X drag.
  void add_yaw(float d);
  /// @brief Add @p d radians to the pitch (look up/down), clamped to +/- max_pitch.
  /// Mouse Y drag.
  void add_pitch(float d);

  void set_head_position(const QarVector3 & p) { position_ = p; }
  void set_yaw(float yaw) { yaw_ = yaw; }
  void set_pitch(float pitch);

  // --- Live head-pose accessors --------------------------------------------
  const QarVector3 & head_position() const { return position_; }
  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }

  /// @brief Current head look (forward) direction (unit vector).
  QarVector3 forward() const;
  /// @brief Current head right direction (unit vector).
  QarVector3 right() const;

  /// @brief Build the stereo frame info for the CURRENT head pose. @p frame_index
  /// is echoed into the result (and the predicted display time, ms) for parity
  /// with the mock source; the pose itself does not depend on it.
  QarFrameInfo current_frame(uint64_t frame_index) const;

private:
  Config config_{};
  QarVector3 position_{};
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__INTERACTIVE_FRAME_INFO_SOURCE_HPP_
