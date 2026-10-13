// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/qar_scene_lighting.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

// Ogre's own headers are not /W4-clean; isolate them behind a warning push/pop.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreColourValue.h>
#include <OgreLight.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreVector.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_streaming
{
namespace
{

/// Every object we add is named under this prefix, so a scene dump makes it obvious
/// which lights are ours and shutdown() can never remove a host light by accident.
const char * const kNamePrefix = "qar/scene_light/";

Ogre::ColourValue to_ogre(const Rgb & c)
{
  return Ogre::ColourValue(c.r, c.g, c.b, 1.0f);
}

Rgb from_ogre(const Ogre::ColourValue & c)
{
  return Rgb{c.r, c.g, c.b};
}

Ogre::Vector3 to_ogre(const Vec3 & v)
{
  return Ogre::Vector3(v.x, v.y, v.z);
}

}  // namespace

// NOTE: to_string / parse_scene_lighting_mode / SceneLightingConfig::validate live
// in qar_scene_lighting_support.cpp — they are Ogre-free so the headless unit tests
// can exercise them without OgreMain.dll.

// ===========================================================================
//  QarSceneLighting
// ===========================================================================

QarSceneLighting::QarSceneLighting(
  Ogre::SceneManager * scene_manager, const SceneLightingConfig & config)
: scene_manager_(scene_manager), config_(config)
{
  if (scene_manager_ == nullptr) {
    throw std::invalid_argument("QarSceneLighting: scene_manager must not be null");
  }
  const Result valid = config_.validate();
  if (!valid.ok()) {
    throw std::invalid_argument("QarSceneLighting: invalid config: " + valid.message);
  }
}

QarSceneLighting::~QarSceneLighting()
{
  shutdown();
}

bool QarSceneLighting::wants_omni() const
{
  return config_.mode == SceneLightingMode::kOmni ||
         config_.mode == SceneLightingMode::kOmniAndPerPeer;
}

bool QarSceneLighting::wants_per_peer() const
{
  return config_.mode == SceneLightingMode::kPerPeer ||
         config_.mode == SceneLightingMode::kOmniAndPerPeer;
}

Result QarSceneLighting::apply()
{
  if (config_.mode == SceneLightingMode::kOff) {
    return Result::success();  // Explicitly opted out: never touch the host's scene.
  }
  if (applied_) {
    return Result::success();  // idempotent
  }
  last_error_.clear();

  try {
    // Ambient first: if anything below fails we still restore it in shutdown().
    if (config_.set_ambient) {
      saved_ambient_ = from_ogre(scene_manager_->getAmbientLight());
      saved_ambient_valid_ = true;
      scene_manager_->setAmbientLight(to_ogre(config_.ambient));
    }

    // Hide the host's camera-tracking headlight (RViz's "MainDirectional"). Its
    // visibility is restored verbatim in shutdown(); we never destroy it.
    if (!config_.suppress_light_name.empty() &&
      scene_manager_->hasLight(config_.suppress_light_name))
    {
      Ogre::Light * host = scene_manager_->getLight(config_.suppress_light_name);
      if (host != nullptr && host->isVisible()) {
        host->setVisible(false);
        host_light_suppressed_ = true;
      }
    }

    if (wants_omni()) {
      // Four directions on a tetrahedron. Every unit normal has a positive dot product
      // with at least one tetrahedron vertex, so NO surface in the scene can be
      // unlit — that is what makes this rig "omnipresent" where a single point light
      // was not. Directions below are the direction the light TRAVELS (from the vertex
      // toward the origin), in the ROS Z-up world: the first two arrive from above and
      // carry the key, the last two arrive from below and are scaled by fill_ratio so
      // undersides are lit without the scene looking floodlit from the floor.
      const float k = 0.57735027f;  // 1/sqrt(3)
      const Rgb fill{
        config_.key_diffuse.r * config_.fill_ratio,
        config_.key_diffuse.g * config_.fill_ratio,
        config_.key_diffuse.b * config_.fill_ratio};
      const struct { const char * suffix; Vec3 direction; const Rgb & diffuse; } rig[] = {
        {"key_a",  Vec3{-k, -k, -k}, config_.key_diffuse},  // from (+X,+Y, above)
        {"key_b",  Vec3{ k,  k, -k}, config_.key_diffuse},  // from (-X,-Y, above)
        {"fill_a", Vec3{-k,  k,  k}, fill},                 // from (+X,-Y, below)
        {"fill_b", Vec3{ k, -k,  k}, fill},                 // from (-X,+Y, below)
      };
      for (const auto & entry : rig) {
        Ogre::Light * light = nullptr;
        if (!create_directional_light(
            std::string(kNamePrefix) + entry.suffix, entry.direction, entry.diffuse,
            config_.omni_specular, light))
        {
          shutdown();
          return Result::error(Status::InvalidState, last_error_);
        }
        omni_lights_.push_back(light);
      }
    }

    applied_ = true;
    return Result::success();
  } catch (const Ogre::Exception & e) {
    last_error_ = std::string("scene lighting apply failed: ") + e.getFullDescription();
    shutdown();
    return Result::error(Status::InvalidState, last_error_);
  }
}

void QarSceneLighting::shutdown()
{
  for (PeerLight & pl : peer_lights_) {
    destroy_light(pl.light, pl.node);
  }
  peer_lights_.clear();
  for (Ogre::Light *& light : omni_lights_) {
    Ogre::SceneNode * no_node = nullptr;  // the rig's lights are unattached
    destroy_light(light, no_node);
  }
  omni_lights_.clear();

  if (host_light_suppressed_ && scene_manager_ != nullptr) {
    try {
      if (scene_manager_->hasLight(config_.suppress_light_name)) {
        Ogre::Light * host = scene_manager_->getLight(config_.suppress_light_name);
        if (host != nullptr) {
          host->setVisible(true);
        }
      }
    } catch (const Ogre::Exception &) {
      // Scene already torn down: nothing left to restore.
    }
    host_light_suppressed_ = false;
  }

  if (saved_ambient_valid_ && scene_manager_ != nullptr) {
    try {
      scene_manager_->setAmbientLight(to_ogre(saved_ambient_));
    } catch (const Ogre::Exception &) {
    }
    saved_ambient_valid_ = false;
  }

  applied_ = false;
}

void QarSceneLighting::update_peer_heads(const std::vector<PeerHeadLight> & heads)
{
  if (!applied_ || !wants_per_peer()) {
    return;
  }
  for (const PeerHeadLight & head : heads) {
    const Vec3 pos{
      head.position.x + config_.peer_offset.x,
      head.position.y + config_.peer_offset.y,
      head.position.z + config_.peer_offset.z};

    auto it = std::find_if(
      peer_lights_.begin(), peer_lights_.end(),
      [&head](const PeerLight & pl) { return peer_id_equal(pl.peer_id, head.peer_id); });

    if (it == peer_lights_.end()) {
      PeerLight pl;
      pl.peer_id = head.peer_id;
      if (!create_point_light(
          std::string(kNamePrefix) + "peer/" + peer_id_to_hex(head.peer_id), pos,
          config_.peer_diffuse, config_.peer_specular, pl.light, pl.node))
      {
        continue;  // last_error_ carries why; a missing peer light is not fatal.
      }
      peer_lights_.push_back(pl);
      continue;
    }
    if (it->node != nullptr) {
      try {
        it->node->setPosition(to_ogre(pos));
      } catch (const Ogre::Exception &) {
      }
    }
  }
}

void QarSceneLighting::forget_peer(const QarPeerId & peer_id)
{
  auto it = std::find_if(
    peer_lights_.begin(), peer_lights_.end(),
    [&peer_id](const PeerLight & pl) { return peer_id_equal(pl.peer_id, peer_id); });
  if (it == peer_lights_.end()) {
    return;
  }
  destroy_light(it->light, it->node);
  peer_lights_.erase(it);
}

bool QarSceneLighting::create_point_light(
  const std::string & name, const Vec3 & position, const Rgb & diffuse,
  const Rgb & specular, Ogre::Light *& out_light, Ogre::SceneNode *& out_node)
{
  try {
    // A previous panel instance (or a crashed teardown) may have left our name behind.
    // The name is ours by prefix, so reclaiming it is safe.
    if (scene_manager_->hasLight(name)) {
      scene_manager_->destroyLight(name);
    }
    Ogre::Light * light = scene_manager_->createLight(name);
    light->setType(Ogre::Light::LT_POINT);
    light->setDiffuseColour(to_ogre(diffuse));
    light->setSpecularColour(to_ogre(specular));
    if (config_.attenuation_range > 0.0f) {
      light->setAttenuation(config_.attenuation_range, 1.0f, 0.0f, 0.0f);
    }
    // Position via a scene node rather than the deprecated Light::setPosition, and
    // because an unattached light's position is not what Ogre 1.12 documents as the
    // supported placement path.
    //
    // Both arguments are passed EXPLICITLY: createChildSceneNode()'s defaults are
    // Vector3::ZERO / Quaternion::IDENTITY, which are dllimport DATA symbols — and a
    // DATA import cannot be delay-loaded, so relying on them breaks the headless unit
    // test exe's /DELAYLOAD:OgreMain.dll (LNK1194). Same reason the render target uses
    // the literal "General" instead of DEFAULT_RESOURCE_GROUP_NAME.
    Ogre::SceneNode * node = scene_manager_->getRootSceneNode()->createChildSceneNode(
      to_ogre(position), Ogre::Quaternion(1.0f, 0.0f, 0.0f, 0.0f));
    node->attachObject(light);

    out_light = light;
    out_node = node;
    return true;
  } catch (const Ogre::Exception & e) {
    last_error_ = "failed to create light '" + name + "': " + e.getFullDescription();
    return false;
  }
}

bool QarSceneLighting::create_directional_light(
  const std::string & name, const Vec3 & direction, const Rgb & diffuse,
  const Rgb & specular, Ogre::Light *& out_light)
{
  try {
    if (scene_manager_->hasLight(name)) {
      scene_manager_->destroyLight(name);
    }
    Ogre::Light * light = scene_manager_->createLight(name);
    light->setType(Ogre::Light::LT_DIRECTIONAL);
    light->setDiffuseColour(to_ogre(diffuse));
    light->setSpecularColour(to_ogre(specular));
    // Directional lights have no position, so no scene node is needed — and setting
    // the direction on an unattached light is exactly how RViz drives its own
    // MainDirectional, so this path is well-trodden in this Ogre build.
    light->setDirection(to_ogre(direction));
    out_light = light;
    return true;
  } catch (const Ogre::Exception & e) {
    last_error_ = "failed to create light '" + name + "': " + e.getFullDescription();
    return false;
  }
}

void QarSceneLighting::destroy_light(Ogre::Light *& light, Ogre::SceneNode *& node)
{
  if (scene_manager_ == nullptr) {
    light = nullptr;
    node = nullptr;
    return;
  }
  try {
    if (node != nullptr && light != nullptr) {
      node->detachObject(light);
    }
    if (light != nullptr) {
      scene_manager_->destroyLight(light);
    }
    if (node != nullptr) {
      scene_manager_->destroySceneNode(node);
    }
  } catch (const Ogre::Exception &) {
    // Already gone (e.g. the whole scene was torn down first): nothing to do.
  }
  light = nullptr;
  node = nullptr;
}

}  // namespace qar_ogre_streaming
