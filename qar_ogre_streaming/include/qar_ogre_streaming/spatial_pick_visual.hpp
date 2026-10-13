// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__SPATIAL_PICK_VISUAL_HPP_
#define QAR_OGRE_STREAMING__SPATIAL_PICK_VISUAL_HPP_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "qar_ogre_streaming/qar_pose_conversion.hpp"
#include "qar_ogre_streaming/types.hpp"

/// @file spatial_pick_visual.hpp
/// @brief Draws the AR user's aim and their pending pick into the shared scene.
///
/// ## Why this exists
///
/// A spatial pick is only usable if the person making it can see where it will
/// land before they commit; this overlay is that feedback (the aiming ray and the
/// pending pick). Because the scene is *shared* — one `Ogre::SceneManager`, N eye
/// cameras (see `qar_scene_lighting.hpp` for the same argument about light) —
/// the feedback has to be drawn INTO the scene, not into a camera overlay:
/// anything else would be invisible to the headset, which is the only viewer
/// that matters here. The RViz plugin does not use it; it is part of the library
/// for hosts that want the feedback inside the shared scene.
///
/// ## The split, and why
///
/// The geometry — where the lines go — is **pure** and lives in
/// `spatial_pick_visual_support.cpp`: a ray is a segment, a point marker is
/// three crossing segments, a pose is an arrow, a region is a closed polyline.
/// It is unit-tested with no Ogre and no GL context, exactly like the pose and
/// projection maths. @ref QarSpatialPickVisual is the thin part that pushes
/// those segments into one `Ogre::ManualObject`.
///
/// Coordinates are the **scene's own** (ROS/REP-103 for RViz). The caller has
/// already crossed the room -> ROS frame change with
/// `qar_ar_core/spatial_frames.hpp`; nothing here re-derives it.
///
/// ## Threading
///
/// Every @ref QarSpatialPickVisual method touches Ogre and MUST run on the
/// render thread, under the host's render lock — the same rule
/// @ref QarSceneLighting states. The geometry builders below are pure and may
/// run anywhere, which is the point of the split: the Qt thread can compute what
/// to draw and hand over a finished @ref PickVisualGeometry.

namespace Ogre
{
class ManualObject;
class SceneManager;
class SceneNode;
}  // namespace Ogre

namespace qar_ogre_streaming
{

/// @brief One line of the feedback overlay, in scene coordinates.
struct PickVisualSegment
{
  Vec3 from{};
  Vec3 to{};
  Rgb color{1.0f, 1.0f, 1.0f};
};

/// @brief A whole frame of feedback: rebuilt each time, never patched.
struct PickVisualGeometry
{
  std::vector<PickVisualSegment> segments;

  bool empty() const { return segments.empty(); }
  void clear() { segments.clear(); }
};

/// @brief Colours and sizes. Defaults are chosen for AR readability: saturated,
/// high-contrast hues that survive a passthrough display, and markers big enough
/// (0.25 m) to read at arm's length without hiding what is behind them.
struct PickVisualStyle
{
  Rgb ray_color{0.20f, 0.80f, 1.00f};    ///< Cyan: where you are aiming.
  Rgb pick_color{1.00f, 0.85f, 0.10f};   ///< Amber: what you have picked.
  Rgb area_color{0.20f, 1.00f, 0.40f};   ///< Green: the region you are sweeping.
  float ray_length_m = 5.0f;   ///< Drawn length when the ray hits nothing.
  float marker_size_m = 0.25f;
  float arrow_length_m = 0.60f;
  float arrow_head_m = 0.15f;
  std::uint32_t circle_segments = 32;
};

/// @brief A straight segment from @p origin along @p direction.
/// A zero-length or non-finite direction appends nothing.
void append_ray(
  PickVisualGeometry & out, const Vec3 & origin, const Vec3 & direction, float length_m,
  const Rgb & color);

/// @brief A three-axis cross centred on @p position — visible from any angle,
/// unlike a flat cross-hair, which is what a head-mounted viewer needs.
void append_point_marker(
  PickVisualGeometry & out, const Vec3 & position, float size_m, const Rgb & color);

/// @brief A shaft along the heading with a two-line arrowhead, plus a small
/// cross at the origin so the position stays readable when the heading is short.
/// @p yaw_rad is measured about the scene's +Z, matching a nav goal's yaw.
void append_pose_marker(
  PickVisualGeometry & out, const Vec3 & position, float yaw_rad, const PickVisualStyle & style,
  const Rgb & color);

/// @brief A polyline through @p vertices, closed back to the first when asked.
void append_polygon(
  PickVisualGeometry & out, const std::vector<Vec3> & vertices, bool closed, const Rgb & color);

/// @brief A circle in the scene's horizontal plane, as @p segments chords.
void append_circle(
  PickVisualGeometry & out, const Vec3 & center, float radius_m, std::uint32_t segments,
  const Rgb & color);

/// @brief One `Ogre::ManualObject` carrying the whole overlay.
///
/// Rebuilding a small line list every frame is cheaper than tracking what
/// changed, and it cannot drift out of sync with the pick state — which, for
/// feedback whose only job is to be TRUE, is the property that matters.
class QarSpatialPickVisual
{
public:
  QarSpatialPickVisual() = default;
  ~QarSpatialPickVisual();

  QarSpatialPickVisual(const QarSpatialPickVisual &) = delete;
  QarSpatialPickVisual & operator=(const QarSpatialPickVisual &) = delete;

  /// @brief Create the object, its node and its material. Render thread.
  /// @param name_suffix Distinguishes several overlays in one scene.
  /// @return InvalidArgument for a null scene manager; Ok otherwise.
  Result initialize(Ogre::SceneManager * scene_manager, const std::string & name_suffix = "");

  bool initialized() const { return object_ != nullptr; }

  /// @brief Replace everything drawn with @p geometry. Render thread.
  /// An empty geometry hides the overlay rather than leaving the last frame up.
  void update(const PickVisualGeometry & geometry);

  /// @brief Stage geometry from ANY thread; @ref apply_pending consumes it on
  /// the render thread.
  ///
  /// The pick state machines run on the host's UI thread (that is where samples
  /// are drained), while the scene may only be touched on the render thread.
  /// Rather than making the host invent a queue, the handoff lives here — one
  /// mutex, at the seam, holding one small vector, which is the same shape the
  /// input seam uses on the way in (`panel_input_source.hpp`).
  void stage(const PickVisualGeometry & geometry);

  /// Geometry has been staged and not yet applied.
  bool has_pending() const;

  /// @brief Apply whatever was staged. Render thread. Does nothing when the
  /// overlay has no scene, so a host may call it before initialize().
  void apply_pending();

  void set_visible(bool visible);
  bool visible() const { return visible_; }

  /// @brief Remove the object, the node and the material. Render thread.
  /// Safe to call twice, and called by the destructor.
  void shutdown();

  /// The names this overlay claimed in the scene, for diagnostics.
  const std::string & object_name() const { return object_name_; }

private:
  Ogre::SceneManager * scene_manager_ = nullptr;
  Ogre::ManualObject * object_ = nullptr;
  Ogre::SceneNode * node_ = nullptr;
  std::string object_name_;
  std::string node_name_;
  std::string material_name_;
  bool visible_ = true;
  bool has_section_ = false;

  mutable std::mutex pending_mutex_;
  PickVisualGeometry pending_;
  bool pending_valid_ = false;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__SPATIAL_PICK_VISUAL_HPP_
