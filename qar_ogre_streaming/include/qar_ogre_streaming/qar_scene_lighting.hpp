// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__QAR_SCENE_LIGHTING_HPP_
#define QAR_OGRE_STREAMING__QAR_SCENE_LIGHTING_HPP_

#include <cstddef>
#include <string>
#include <vector>

#include "qar_ogre_streaming/qar_pose_conversion.hpp"
#include "qar_ogre_streaming/types.hpp"

/// @file qar_scene_lighting.hpp
/// @brief A stable lighting rig for the shared scene while it is streamed to QAR.
///
/// ## Why this exists
///
/// RViz creates exactly one light — a **directional** light named
/// `MainDirectional` — and `VisualizationManager::update()` re-aims it every frame
/// at the *local* view camera's direction:
///
/// ```cpp
/// RenderWindowOgreAdapter::getDirectionalLight(render_panel_->getRenderWindow())
///   ->setDirection(view_manager_->getCurrent()->getCamera()->getDerivedDirection());
/// ```
///
/// That is a **headlight**: it is glued to whoever is driving the RViz 3D view. It
/// is the right choice for a single local operator and the wrong one the moment the
/// same scene is rendered from somewhere else. A QAR peer standing anywhere in the
/// scene sees its shading pivot around as the RViz operator orbits their own camera
/// — the light source seems to follow the RViz viewer.
///
/// The scene is genuinely shared (one Ogre::SceneManager, N eye cameras), so the
/// fix has to be in the scene, not in a camera. Two rigs are offered:
///
///   * @ref SceneLightingMode::kOmni — an **omnipresent** rig: raised ambient plus
///     four *directional* lights on a tetrahedron, so **every surface normal faces at
///     least one light and nothing can be unlit**, while the two brighter "from above"
///     directions still give the scene shape. Directional lights are
///     position-independent, so the whole world is lit evenly however far a peer
///     stands from the origin — which a single point light is not: with one point
///     light the far sides and undersides read too dark.
///   * @ref SceneLightingMode::kPerPeer — one point light per connected peer, moved
///     to that peer's head each frame. Restores the "lit from where I am looking"
///     feel, but *per peer* rather than per operator.
///
/// `kOmniAndPerPeer` runs both — an even base plus a personal highlight — and is what
/// the RViz panel uses.
///
/// Suppressing RViz's headlight is separate and optional (@ref
/// SceneLightingConfig::suppress_light_name): adding lights alone leaves the
/// swimming headlight in place, so the symptom only *softens*. Hiding it is what
/// actually removes it, at the cost of changing how the operator's own RViz view is
/// lit. Both the previous visibility and the previous ambient colour are saved and
/// restored by @ref QarSceneLighting::shutdown, so this is fully reversible.
///
/// ## Threading
/// Every method touches Ogre and MUST run on the render thread. @ref
/// update_peer_heads is called by the render scheduler between the "update cameras"
/// and "render targets" phases, inside the batch render lock, so each peer's light
/// is already in place for the frame that peer is about to receive.

namespace Ogre
{
class Light;
class SceneNode;
}  // namespace Ogre

namespace qar_ogre_streaming
{

// NOTE: Rgb lives in types.hpp — the pick-feedback overlay
// (spatial_pick_visual.hpp) needs the same colour triple, and a second one in
// the same library would be a fork of a three-float struct. This header includes
// types.hpp, so users of this header see it.

/// @brief Which lights @ref QarSceneLighting adds to the shared scene.
enum class SceneLightingMode
{
  kOff,            ///< Touch nothing. The host's own lighting is left exactly as is.
  kOmni,           ///< The world-fixed omnipresent directional rig + ambient. Default.
  kPerPeer,        ///< One omni light per connected peer, tracking that peer's head.
  kOmniAndPerPeer  ///< Both of the above.
};

/// @brief Human-readable name (for logs/diagnostics).
const char * to_string(SceneLightingMode mode);

/// @brief Parse a mode from text: `off`, `omni`, `per-peer`, `both`.
/// Case-insensitive; `_` and `-` are interchangeable; surrounding spaces ignored.
/// @return false (leaving @p out untouched) if @p text names no known mode.
bool parse_scene_lighting_mode(const std::string & text, SceneLightingMode & out);

/// @brief How to light the shared scene while streaming.
struct SceneLightingConfig
{
  SceneLightingMode mode = SceneLightingMode::kOmni;

  /// @brief Raise the scene's ambient term so surfaces facing away from every light
  /// keep some shape instead of going black. Ogre's default ambient is near-black.
  bool set_ambient = true;
  Rgb ambient{0.30f, 0.30f, 0.30f};

  // --- kOmni: the omnipresent directional rig ------------------------------
  /// @brief Brightness of the two "from above" directions (the key).
  Rgb key_diffuse{0.75f, 0.75f, 0.75f};
  /// @brief The two "from below" directions are @ref key_diffuse scaled by this, so
  /// undersides are lit but do not read as if the scene were floodlit from the floor.
  /// Must be >= 0; 1.0 gives perfectly uniform, shapeless illumination.
  float fill_ratio = 0.55f;
  Rgb omni_specular{0.2f, 0.2f, 0.2f};

  // --- kPerPeer ------------------------------------------------------------
  /// Modest by default: in `both` mode this adds ON TOP of a rig that already lights
  /// every surface, so a strong peer light would blow out whatever it faces.
  Rgb peer_diffuse{0.35f, 0.35f, 0.35f};
  Rgb peer_specular{0.15f, 0.15f, 0.15f};
  /// @brief World offset added to each peer's head position before placing its light
  /// (e.g. lift it slightly above the eyes). Zero => exactly at the head.
  Vec3 peer_offset{0.0f, 0.0f, 0.0f};

  /// @brief Attenuation range for every light we create. `<= 0` keeps Ogre's default
  /// (range 100000, constant 1) — i.e. **no distance falloff**, which is what a fill
  /// light for a whole scene wants: only surface angle shades, not distance.
  float attenuation_range = 0.0f;

  /// @brief Name of a light **already in the scene** to hide while our rig is
  /// applied, restored on shutdown. For RViz this is `MainDirectional`, the
  /// camera-tracking headlight described in the file comment. Empty => leave the
  /// host's lights alone (our lights are then simply added on top).
  std::string suppress_light_name;

  /// @brief Reject obviously wrong values (negative colour components).
  Result validate() const;
};

/// @brief One peer's head position in **Ogre world space** (already converted from
/// the QAR pose by @ref qar_position_to_ogre), for @ref SceneLightingMode::kPerPeer.
struct PeerHeadLight
{
  QarPeerId peer_id{};
  Vec3 position{};
};

/// @brief Owns the QAR lighting rig added to a host Ogre scene.
///
/// Strictly additive and fully reversible: it creates only lights and scene nodes it
/// names itself (`qar/scene_light/*`), and it restores the host's ambient colour and
/// the suppressed light's visibility in @ref shutdown (also called by the destructor).
class QarSceneLighting
{
public:
  /// @param scene_manager Non-null shared Ogre scene (the same one the streams render).
  /// @param config        Lighting configuration (copied).
  /// @throws std::invalid_argument if @p scene_manager is null or @p config is invalid.
  QarSceneLighting(Ogre::SceneManager * scene_manager, const SceneLightingConfig & config);
  ~QarSceneLighting();

  QarSceneLighting(const QarSceneLighting &) = delete;
  QarSceneLighting & operator=(const QarSceneLighting &) = delete;

  /// @brief Create the rig and (optionally) hide the host's headlight. Idempotent.
  /// A no-op returning Ok when the mode is @ref SceneLightingMode::kOff.
  /// Must run on the render thread.
  Result apply();

  /// @brief Destroy every light/node we created and restore what we changed.
  /// Idempotent, safe to call after a partial @ref apply, and safe to call twice.
  void shutdown();

  /// @brief True once @ref apply has changed the scene (always false for kOff).
  bool applied() const { return applied_; }

  /// @brief Place one light at each peer's head (kPerPeer / kOmniAndPerPeer only).
  ///
  /// Peers absent from @p heads keep their light where it was — a peer that simply
  /// skipped a frame this tick must not have its light snap to the origin. Use
  /// @ref forget_peer when a peer's stream is actually destroyed. No-op in the modes
  /// that have no per-peer lights, or before @ref apply.
  void update_peer_heads(const std::vector<PeerHeadLight> & heads);

  /// @brief Destroy the light belonging to a peer whose stream has gone away.
  void forget_peer(const QarPeerId & peer_id);

  const SceneLightingConfig & config() const { return config_; }

  /// @brief Number of live per-peer lights (diagnostics/tests).
  std::size_t peer_light_count() const { return peer_lights_.size(); }
  /// @brief Number of live world-fixed rig lights (diagnostics/tests).
  std::size_t omni_light_count() const { return omni_lights_.size(); }
  /// @brief True while the host light named by the config is hidden by us.
  bool host_light_suppressed() const { return host_light_suppressed_; }

  /// @brief Last error from @ref apply (empty when it succeeded).
  const std::string & last_error() const { return last_error_; }

private:
  struct PeerLight
  {
    QarPeerId peer_id{};
    Ogre::Light * light = nullptr;
    Ogre::SceneNode * node = nullptr;
  };

  /// Create a point light + its own scene node at @p position. Returns false and
  /// fills last_error_ on an Ogre failure.
  bool create_point_light(
    const std::string & name, const Vec3 & position, const Rgb & diffuse,
    const Rgb & specular, Ogre::Light *& out_light, Ogre::SceneNode *& out_node);

  /// Create a directional light shining along @p direction (the direction the light
  /// TRAVELS, so `(0,0,-1)` shines downward from above in the ROS Z-up world).
  bool create_directional_light(
    const std::string & name, const Vec3 & direction, const Rgb & diffuse,
    const Rgb & specular, Ogre::Light *& out_light);

  void destroy_light(Ogre::Light *& light, Ogre::SceneNode *& node);

  bool wants_omni() const;
  bool wants_per_peer() const;

  Ogre::SceneManager * scene_manager_;
  SceneLightingConfig config_;

  bool applied_ = false;
  std::string last_error_;

  /// The world-fixed directional rig (unattached: a directional light needs only a
  /// direction, no node).
  std::vector<Ogre::Light *> omni_lights_;
  std::vector<PeerLight> peer_lights_;

  /// Saved host state, restored by shutdown().
  bool host_light_suppressed_ = false;
  Rgb saved_ambient_{};
  bool saved_ambient_valid_ = false;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__QAR_SCENE_LIGHTING_HPP_
