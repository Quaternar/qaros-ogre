// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "sample_scene.hpp"

#include <cstdint>

// Ogre's own headers are not /W4-clean; isolate them behind a warning push/pop.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreColourValue.h>
#include <OgreGpuProgramParams.h>
#include <OgreHighLevelGpuProgram.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreLight.h>
#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRenderSystem.h>
#include <OgreRenderWindow.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreResourceGroupManager.h>
#include <OgreVector3.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_sample
{
namespace
{

const char * const kResourceGroup = "General";

const char * const kGL3PlusName = "OpenGL 3+ Rendering Subsystem";
const char * const kLegacyGLName = "OpenGL Rendering Subsystem";

const char * const kColourVp = "qar/SampleColourVP";
const char * const kColourFp = "qar/SampleColourFP";

// GLSL-120 lit colour: material colour (a per-material uniform) shaded by the
// scene's ambient + one directional light. Colour comes through a UNIFORM, not a
// vertex attribute and not fixed-function material state: on this Ogre 1.12
// rviz_ogre_vendor build the legacy GL render system ignores fixed-function
// material/vertex colours (RViz itself gets colour via shader materials). GLSL 120
// is used deliberately: this render system advertises GLSL profiles only up to
// glsl120, so a #version 150 program is rejected as "unsupported" and silently
// falls back to the white fixed-function pipeline (the library's depth scheme gets
// away with #version 150 only because a scheme listener bypasses the support check;
// the GL 4.x context itself compiles either). GLSL 120 binds and runs directly here.
const char * const kColourVertexSrc =
  "#version 120\n"
  "attribute vec4 vertex;\n"
  "attribute vec3 normal;\n"
  "uniform mat4 worldViewProj;\n"
  "uniform mat4 worldMatrix;\n"
  "varying vec3 vNormalW;\n"
  "void main()\n"
  "{\n"
  "    gl_Position = worldViewProj * vertex;\n"
  "    vNormalW = mat3(worldMatrix) * normal;\n"
  "}\n";

const char * const kColourFragmentSrc =
  "#version 120\n"
  "varying vec3 vNormalW;\n"
  "uniform vec4 uColour;\n"
  "uniform vec4 lightDirW;\n"  // direction the light travels (world space)
  "uniform vec4 lightCol;\n"
  "uniform vec4 ambient;\n"
  "void main()\n"
  "{\n"
  "    vec3 N = normalize(vNormalW);\n"
  "    vec3 L = normalize(-lightDirW.xyz);\n"  // surface -> light
  "    float ndl = max(dot(N, L), 0.0);\n"
  "    vec3 lit = uColour.rgb * (ambient.rgb + lightCol.rgb * ndl);\n"
  "    gl_FragColor = vec4(clamp(lit, 0.0, 1.0), 1.0);\n"
  "}\n";

// Create the shared colour GPU programs once.
void ensure_colour_programs()
{
  auto & hgpm = Ogre::HighLevelGpuProgramManager::getSingleton();
  if (!hgpm.getByName(kColourVp, kResourceGroup)) {
    Ogre::HighLevelGpuProgramPtr vp =
      hgpm.createProgram(kColourVp, kResourceGroup, "glsl", Ogre::GPT_VERTEX_PROGRAM);
    vp->setSource(kColourVertexSrc);
    vp->load();
  }
  if (!hgpm.getByName(kColourFp, kResourceGroup)) {
    Ogre::HighLevelGpuProgramPtr fp =
      hgpm.createProgram(kColourFp, kResourceGroup, "glsl", Ogre::GPT_FRAGMENT_PROGRAM);
    fp->setSource(kColourFragmentSrc);
    fp->load();
  }
}

// Create (once) a GLSL lit material whose colour is baked into the uColour uniform.
const Ogre::String & ensure_material(const std::string & name, const Ogre::ColourValue & c)
{
  auto & mm = Ogre::MaterialManager::getSingleton();
  Ogre::MaterialPtr mat = mm.getByName(name, kResourceGroup);
  if (!mat) {
    ensure_colour_programs();
    mat = mm.create(name, kResourceGroup);
    Ogre::Pass * pass = mat->createTechnique()->createPass();
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setVertexProgram(kColourVp);
    pass->setFragmentProgram(kColourFp);

    Ogre::GpuProgramParametersSharedPtr vparams = pass->getVertexProgramParameters();
    vparams->setNamedAutoConstant("worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
    vparams->setNamedAutoConstant("worldMatrix", Ogre::GpuProgramParameters::ACT_WORLD_MATRIX);

    Ogre::GpuProgramParametersSharedPtr fparams = pass->getFragmentProgramParameters();
    // ACT_LIGHT_DIRECTION is the light direction in WORLD space (Ogre has no
    // *_WORLD_SPACE variant; the bare name is world space). Auto-constants resolve
    // by name AFTER the program links, so they can be declared before load().
    fparams->setNamedAutoConstant("lightDirW", Ogre::GpuProgramParameters::ACT_LIGHT_DIRECTION, 0);
    fparams->setNamedAutoConstant("lightCol", Ogre::GpuProgramParameters::ACT_LIGHT_DIFFUSE_COLOUR, 0);
    fparams->setNamedAutoConstant("ambient", Ogre::GpuProgramParameters::ACT_AMBIENT_LIGHT_COLOUR);

    // Link the GLSL programs so the named-constant definitions (incl. uColour)
    // exist, THEN set the manual uColour. (A manual named constant set before the
    // program links is silently dropped — the constant offset isn't known yet.)
    mat->load();

    // On this Ogre 1.12 build, load()/compile() injects a competing
    // fixed-function technique at index 0 (no vertex program), which would be
    // chosen over our GLSL technique and render white. Drop any technique whose
    // first pass has no vertex program, leaving only our shader technique.
    for (int ti = static_cast<int>(mat->getNumTechniques()) - 1; ti >= 0; --ti) {
      Ogre::Technique * t = mat->getTechnique(static_cast<unsigned short>(ti));
      if (t->getNumPasses() == 0 || !t->getPass(0)->hasVertexProgram()) {
        mat->removeTechnique(static_cast<unsigned short>(ti));
      }
    }
    // Set the manual uColour on the surviving GLSL pass (its named-constant
    // definitions now exist post-link).
    if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
      mat->getTechnique(0)->getPass(0)->getFragmentProgramParameters()->setNamedConstant(
        "uColour", Ogre::ColourValue(c.r, c.g, c.b, 1.0f));
    }
  }
  return mat->getName();
}

// Append an outward-facing quad (two triangles) to an OPEN triangle-list section.
void emit_quad(
  Ogre::ManualObject * mo, std::uint32_t & base,
  const Ogre::Vector3 & v0, const Ogre::Vector3 & v1,
  const Ogre::Vector3 & v2, const Ogre::Vector3 & v3,
  const Ogre::Vector3 & n)
{
  mo->position(v0); mo->normal(n);
  mo->position(v1); mo->normal(n);
  mo->position(v2); mo->normal(n);
  mo->position(v3); mo->normal(n);
  mo->index(base + 0); mo->index(base + 1); mo->index(base + 2);
  mo->index(base + 0); mo->index(base + 2); mo->index(base + 3);
  base += 4;
}

// Add an axis-aligned box centred at c with half-extents h as its OWN section of
// @p mo, using material @p mat (so the box renders in that material's colour).
void add_box(
  Ogre::ManualObject * mo, const Ogre::String & mat,
  const Ogre::Vector3 & c, const Ogre::Vector3 & h)
{
  const float xn = c.x - h.x, xp = c.x + h.x;
  const float yn = c.y - h.y, yp = c.y + h.y;
  const float zn = c.z - h.z, zp = c.z + h.z;
  using V = Ogre::Vector3;
  mo->begin(mat, Ogre::RenderOperation::OT_TRIANGLE_LIST, kResourceGroup);
  std::uint32_t base = 0;
  emit_quad(mo, base, V(xp, yn, zn), V(xp, yp, zn), V(xp, yp, zp), V(xp, yn, zp), V(1, 0, 0));
  emit_quad(mo, base, V(xn, yp, zn), V(xn, yn, zn), V(xn, yn, zp), V(xn, yp, zp), V(-1, 0, 0));
  emit_quad(mo, base, V(xp, yp, zn), V(xn, yp, zn), V(xn, yp, zp), V(xp, yp, zp), V(0, 1, 0));
  emit_quad(mo, base, V(xn, yn, zn), V(xp, yn, zn), V(xp, yn, zp), V(xn, yn, zp), V(0, -1, 0));
  emit_quad(mo, base, V(xn, yn, zp), V(xp, yn, zp), V(xp, yp, zp), V(xn, yp, zp), V(0, 0, 1));
  emit_quad(mo, base, V(xp, yn, zn), V(xn, yn, zn), V(xn, yp, zn), V(xp, yp, zn), V(0, 0, -1));
  mo->end();
}

}  // namespace

RenderSystemChoice parse_render_system_choice(const std::string & s)
{
  if (s == "gl3plus" || s == "GL3Plus" || s == "gl3") {
    return RenderSystemChoice::GL3Plus;
  }
  if (s == "gl" || s == "GL" || s == "legacy") {
    return RenderSystemChoice::GL;
  }
  return RenderSystemChoice::Auto;
}

SampleScene::SampleScene() = default;

SampleScene::~SampleScene()
{
  shutdown();
}

bool SampleScene::load_render_system(
  const std::string & plugin_dir, RenderSystemChoice choice, std::string & error)
{
  const std::string dir = plugin_dir.empty() ? std::string(".") : plugin_dir;
  auto load_abs = [&](const char * name) {
    try {
      // Absolute path so loading does not depend on the process working dir; Ogre
      // appends the platform suffix (.dll) if absent.
      root_->loadPlugin(dir + "/" + name);
      return true;
    } catch (const Ogre::Exception &) {
      try {
        // Fallback: bare name (works when plugin_dir is already on the loader path).
        root_->loadPlugin(name);
        return true;
      } catch (const Ogre::Exception &) {
        return false;
      }
    }
  };

  if (choice == RenderSystemChoice::GL3Plus) {
    load_abs("RenderSystem_GL3Plus");
  } else if (choice == RenderSystemChoice::GL) {
    load_abs("RenderSystem_GL");
  } else {  // Auto: load both, prefer legacy GL below.
    load_abs("RenderSystem_GL");
    load_abs("RenderSystem_GL3Plus");
  }

  Ogre::RenderSystem * rs = nullptr;
  if (choice == RenderSystemChoice::GL3Plus) {
    rs = root_->getRenderSystemByName(kGL3PlusName);
  } else if (choice == RenderSystemChoice::GL) {
    rs = root_->getRenderSystemByName(kLegacyGLName);
  } else {  // Auto: legacy GL first (fixed-function capable; matches RViz), else GL3Plus.
    rs = root_->getRenderSystemByName(kLegacyGLName);
    if (rs == nullptr) {
      rs = root_->getRenderSystemByName(kGL3PlusName);
    }
  }

  if (rs == nullptr) {
    error = "no suitable OpenGL render system could be loaded from the plugin dir";
    return false;
  }
  root_->setRenderSystem(rs);
  render_system_name_ = rs->getName();
  return true;
}

void SampleScene::build_scene()
{
  // A Z-up world so the default MockQarFrameInfoSource (orbits about +Z, looks at
  // a focal point near the origin — the same convention RViz uses) frames this
  // scene naturally.
  scene_manager_->setAmbientLight(Ogre::ColourValue(0.5f, 0.5f, 0.5f));

  Ogre::Light * light = scene_manager_->createLight("qar_sample_sun");
  light->setType(Ogre::Light::LT_DIRECTIONAL);
  light->setDiffuseColour(Ogre::ColourValue(0.85f, 0.85f, 0.78f));
  Ogre::SceneNode * lnode = scene_manager_->getRootSceneNode()->createChildSceneNode();
  lnode->attachObject(light);
  lnode->setDirection(
    Ogre::Vector3(-0.4f, -0.55f, -0.75f).normalisedCopy(), Ogre::Node::TS_WORLD);

  Ogre::SceneNode * root = scene_manager_->getRootSceneNode();
  using V = Ogre::Vector3;
  using C = Ogre::ColourValue;

  // --- Ground grid (lit line list on the z=0 plane) ------------------------
  const Ogre::String grid_mat = ensure_material("qar/mat_grid", C(0.62f, 0.64f, 0.70f));
  Ogre::ManualObject * grid = scene_manager_->createManualObject("qar_sample_grid");
  grid->begin(grid_mat, Ogre::RenderOperation::OT_LINE_LIST, kResourceGroup);
  const int n = 15;
  const float ext = static_cast<float>(n);
  for (int i = -n; i <= n; ++i) {
    const float f = static_cast<float>(i);
    grid->position(-ext, f, 0.0f); grid->position(ext, f, 0.0f);  // parallel to X
    grid->position(f, -ext, 0.0f); grid->position(f, ext, 0.0f);  // parallel to Y
  }
  grid->end();
  root->createChildSceneNode()->attachObject(grid);

  // --- Coloured, lit cubes at varied positions/depths (parallax + depth) ---
  Ogre::ManualObject * shapes = scene_manager_->createManualObject("qar_sample_shapes");
  add_box(shapes, ensure_material("qar/mat_orange", C(1.00f, 0.52f, 0.12f)),
    V(0.0f, 0.0f, 1.0f), V(1.0f, 1.0f, 1.0f));    // orange, centre
  add_box(shapes, ensure_material("qar/mat_green", C(0.20f, 0.85f, 0.35f)),
    V(5.0f, 0.0f, 0.9f), V(0.9f, 0.9f, 0.9f));    // green
  add_box(shapes, ensure_material("qar/mat_blue", C(0.30f, 0.55f, 1.00f)),
    V(-4.0f, 4.0f, 0.9f), V(0.9f, 0.9f, 0.9f));   // blue
  add_box(shapes, ensure_material("qar/mat_magenta", C(0.95f, 0.30f, 0.85f)),
    V(2.0f, -5.0f, 2.0f), V(0.6f, 0.6f, 2.0f));   // magenta pillar
  add_box(shapes, ensure_material("qar/mat_yellow", C(0.95f, 0.90f, 0.20f)),
    V(-6.0f, -3.0f, 0.6f), V(0.6f, 0.6f, 0.6f));  // yellow
  root->createChildSceneNode()->attachObject(shapes);
}

bool SampleScene::initialize(
  const std::string & plugin_dir,
  RenderSystemChoice choice,
  const std::string & log_path,
  std::string & error)
{
  try {
    // Own Ogre::Root — no plugins.cfg / ogre.cfg; plugins loaded explicitly below.
    root_ = new Ogre::Root("", "", log_path);

    if (!load_render_system(plugin_dir, choice, error)) {
      return false;
    }

    root_->initialise(false);

    // Hidden window: gives a current GL context without a visible desktop window.
    Ogre::NameValuePairList params;
    params["hidden"] = "true";
    params["vsync"] = "false";
    params["FSAA"] = "0";
    window_ = root_->createRenderWindow("QarOgreSampleHidden", 320, 240, false, &params);
    if (window_ != nullptr) {
      window_->setActive(true);
      window_->setAutoUpdated(false);
    }

    Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

    scene_manager_ = root_->createSceneManager();

    build_scene();

    initialized_ = true;
    return true;
  } catch (const Ogre::Exception & e) {
    error = std::string("Ogre initialization failed: ") + e.getFullDescription();
    return false;
  }
}

void SampleScene::shutdown()
{
  scene_manager_ = nullptr;
  window_ = nullptr;
  if (root_ != nullptr) {
    delete root_;  // shuts Ogre down and unloads plugins
    root_ = nullptr;
  }
  initialized_ = false;
}

}  // namespace qar_ogre_sample
