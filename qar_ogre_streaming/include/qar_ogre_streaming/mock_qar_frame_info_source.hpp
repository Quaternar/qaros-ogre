// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__MOCK_QAR_FRAME_INFO_SOURCE_HPP_
#define QAR_OGRE_STREAMING__MOCK_QAR_FRAME_INFO_SOURCE_HPP_

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Deterministic, animated per-eye stereo frame-info generator.
///
/// Stands in for a real Qar sender's per-frame pose/FOV query while the Qar
/// runtime is unavailable. Given a frame index it produces a @ref QarFrameInfo
/// with an ANIMATED head pose (orbiting a focal point) whose two eyes are offset
/// by a configurable inter-pupillary distance (IPD) and each carry an asymmetric,
/// aspect-matched field of view. It is a pure computation — no Ogre, no Qt, no
/// wall-clock — so tests and captures are fully reproducible from the frame index.
///
/// ## Coordinate convention
/// Poses are produced directly in the Ogre world frame the stream renders in
/// (the Qar->Ogre pose mapping is the identity: X right, Y up, -Z forward,
/// right-handed). RViz's Ogre scene is **Z-up** (its orbit view controller uses
/// world +Z as the fixed yaw axis; grid on the XY plane, content near the
/// origin). The default @ref Config therefore orbits about the world Z axis and
/// looks toward a focal point near the origin, so the generated cameras frame the
/// live RViz scene exactly like RViz's own orbit camera. @ref Config::up_axis and
/// @ref Config::focus are configurable for non-RViz scenes (e.g. a Y-up sample).
///
/// ## Stereo model
/// Parallel stereo: both eyes share the head orientation and are translated by
/// +/- ipd/2 along the head's local right axis. This is the textbook arrangement
/// that produces horizontal parallax whose magnitude scales with 1/depth (nearer
/// geometry shifts more between the eyes than far geometry). Per-eye FOV is
/// asymmetric (off-axis) and mirror-asymmetric between eyes (each eye's temporal
/// side is wider), with the horizontal extent scaled to the target aspect ratio
/// so the rendered image is not stretched.
class MockQarFrameInfoSource
{
public:
  struct Config
  {
    // --- Stereo ------------------------------------------------------------
    /// @brief Inter-pupillary distance in metres (human default ~64 mm).
    float ipd_metres = 0.064f;

    // --- Head orbit (Z-up look-at by default; see class docs) --------------
    QarVector3 focus{0.0f, 0.0f, 1.0f};  ///< Point the head looks at.
    QarVector3 up_axis{0.0f, 0.0f, 1.0f};  ///< World up (RViz: +Z).
    float orbit_distance_m = 9.0f;         ///< Head distance from @ref focus.
    float pitch_rad = 0.42f;               ///< Elevation above the focus plane.
    float start_yaw_rad = 0.785f;          ///< Yaw at frame 0 (matches dev config).
    float yaw_per_frame_rad = 0.01f;       ///< Orbit speed (>0 => animates).

    // --- Per-eye field of view --------------------------------------------
    float vertical_fov_rad = 1.30f;        ///< Total vertical FOV (~74.5 deg).
    float aspect = 16.0f / 9.0f;           ///< Target width/height (no stretch).
    /// @brief Per-eye nasal/temporal asymmetry as a tan-space centre shift. The
    /// left eye's frustum centre shifts by -@c fov_asymmetry_tan, the right eye's
    /// by +@c fov_asymmetry_tan, so each eye's FOV is asymmetric AND the two eyes
    /// differ, while the horizontal tan-extent (and thus the aspect) is preserved.
    /// Kept modest: it shifts both eyes' absolute disparity by a constant that
    /// cancels in the near-vs-far differential, but a small value keeps the raw
    /// left/right images cleanly readable for the parallax eyeball check.
    float fov_asymmetry_tan = 0.008f;

    // --- Clip planes -------------------------------------------------------
    float near_plane = 0.1f;
    float far_plane = 200.0f;
  };

  MockQarFrameInfoSource() = default;
  explicit MockQarFrameInfoSource(const Config & config) : config_(config) {}

  const Config & config() const { return config_; }
  void set_config(const Config & config) { config_ = config; }

  /// @brief Deterministically compute the stereo frame info for @p frame_index.
  ///
  /// @c should_render is always true and @c frame_index is echoed into the
  /// result. The predicted display time is a deterministic function of the index
  /// (index milliseconds) so it too is reproducible.
  QarFrameInfo frame_at(uint64_t frame_index) const;

  /// @brief The world-space head position (eye midpoint) at @p frame_index.
  /// Exposed for tests/diagnostics (the eyes straddle this by +/- ipd/2).
  QarVector3 head_position_at(uint64_t frame_index) const;

  /// @brief Build a single eye's asymmetric, aspect-matched FOV. @p asym_tan is
  /// the tan-space centre shift (left eye negative, right eye positive).
  static QarFov make_eye_fov(
    float vertical_fov_rad, float aspect, float asym_tan);

private:
  Config config_{};
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__MOCK_QAR_FRAME_INFO_SOURCE_HPP_
