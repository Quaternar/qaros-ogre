// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The Ogre half of spatial_pick_visual.hpp. Every function here MUST run on the
// render thread under the host's render lock; the geometry it draws is built
// elsewhere (spatial_pick_visual_support.cpp), with no Ogre at all.

#include "qar_ogre_streaming/spatial_pick_visual.hpp"

// Ogre's own headers are not /W4-clean; isolate them behind a warning push/pop.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreColourValue.h>
#include <OgreCommon.h>
#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgreRenderOperation.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreVector.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_streaming
{
namespace
{

/// Everything this class adds is named under one prefix, so a scene dump makes
/// it obvious which objects are ours and shutdown() can never remove a host's.
const char * const kNamePrefix = "qar/spatial_pick/";

/// Ogre's `ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME` is an exported
/// DATA symbol, and so are `Vector3::ZERO` / `Quaternion::IDENTITY`. Importing
/// any of them defeats the /DELAYLOAD:OgreMain.dll the headless test suite
/// relies on (LNK1194), so this file spells out the same values and passes every
/// defaulted argument explicitly. "General" IS that default group's name.
const char * const kResourceGroup = "General";

}  // namespace

QarSpatialPickVisual::~QarSpatialPickVisual()
{
  shutdown();
}

Result QarSpatialPickVisual::initialize(
  Ogre::SceneManager * scene_manager, const std::string & name_suffix)
{
  if (scene_manager == nullptr) {
    return Result::error(
      Status::InvalidArgument, "QarSpatialPickVisual::initialize: scene_manager is null");
  }
  if (object_ != nullptr) {
    return Result::success();  // idempotent: re-initializing must not orphan the object
  }
  scene_manager_ = scene_manager;
  const std::string base = std::string(kNamePrefix) + (name_suffix.empty() ? "overlay" : name_suffix);
  object_name_ = base;
  node_name_ = base + "/node";
  material_name_ = base + "/material";

  // A material of our own rather than a built-in: the overlay needs unlit,
  // per-vertex colours (the ray, the pick and the region are told apart by
  // colour alone) and no depth WRITE, so the feedback never occludes the scene
  // it is describing. Depth TESTING stays on, so the marker is still hidden by
  // geometry in front of it — feedback that floats through walls would lie
  // about where the pick is.
  Ogre::MaterialPtr material =
    Ogre::MaterialManager::getSingleton().getByName(material_name_, kResourceGroup);
  if (!material) {
    material = Ogre::MaterialManager::getSingleton().create(material_name_, kResourceGroup);
    material->setReceiveShadows(false);
    Ogre::Pass * pass = material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setDepthWriteEnabled(false);
  }

  object_ = scene_manager_->createManualObject(object_name_);
  object_->setDynamic(true);  // rebuilt every frame the pick changes
  node_ = scene_manager_->getRootSceneNode()->createChildSceneNode(
    node_name_, Ogre::Vector3(0.0f, 0.0f, 0.0f), Ogre::Quaternion(1.0f, 0.0f, 0.0f, 0.0f));
  node_->attachObject(object_);
  node_->setVisible(visible_);
  return Result::success();
}

void QarSpatialPickVisual::update(const PickVisualGeometry & geometry)
{
  if (object_ == nullptr) {
    return;
  }
  if (geometry.empty()) {
    // Nothing to say: clear rather than leave the previous frame standing, which
    // would keep showing a ray that has ended.
    if (has_section_) {
      object_->clear();
      has_section_ = false;
    }
    if (node_ != nullptr) {
      node_->setVisible(false);
    }
    return;
  }

  if (has_section_) {
    object_->beginUpdate(0);
  } else {
    object_->begin(material_name_, Ogre::RenderOperation::OT_LINE_LIST, kResourceGroup);
  }
  for (const auto & segment : geometry.segments) {
    object_->position(segment.from.x, segment.from.y, segment.from.z);
    object_->colour(segment.color.r, segment.color.g, segment.color.b, 1.0f);
    object_->position(segment.to.x, segment.to.y, segment.to.z);
    object_->colour(segment.color.r, segment.color.g, segment.color.b, 1.0f);
  }
  object_->end();
  has_section_ = true;
  if (node_ != nullptr) {
    node_->setVisible(visible_);
  }
}

void QarSpatialPickVisual::stage(const PickVisualGeometry & geometry)
{
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_ = geometry;
  pending_valid_ = true;
}

bool QarSpatialPickVisual::has_pending() const
{
  std::lock_guard<std::mutex> lock(pending_mutex_);
  return pending_valid_;
}

void QarSpatialPickVisual::apply_pending()
{
  PickVisualGeometry geometry;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!pending_valid_) {
      return;
    }
    geometry.segments.swap(pending_.segments);
    pending_valid_ = false;
  }
  // The staged frame is consumed even when there is no scene yet: a host that
  // stages before initialize() must not have a stale frame appear later.
  update(geometry);
}

void QarSpatialPickVisual::set_visible(bool visible)
{
  visible_ = visible;
  if (node_ != nullptr) {
    node_->setVisible(visible && has_section_);
  }
}

void QarSpatialPickVisual::shutdown()
{
  if (scene_manager_ == nullptr) {
    object_ = nullptr;
    node_ = nullptr;
    has_section_ = false;
    return;
  }
  if (node_ != nullptr) {
    node_->detachAllObjects();
    scene_manager_->destroySceneNode(node_);
    node_ = nullptr;
  }
  if (object_ != nullptr) {
    scene_manager_->destroyManualObject(object_);
    object_ = nullptr;
  }
  if (!material_name_.empty()) {
    // The material was created by us under our own prefix, so removing it can
    // never take a host material with it.
    Ogre::MaterialManager::getSingleton().remove(material_name_, kResourceGroup);
  }
  scene_manager_ = nullptr;
  has_section_ = false;
}

}  // namespace qar_ogre_streaming
