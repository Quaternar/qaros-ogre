// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_SAMPLE__SAMPLE_SCENE_HPP_
#define QAR_OGRE_SAMPLE__SAMPLE_SCENE_HPP_

#include <string>

// Forward declarations only — no Ogre headers leak into the sample's public
// surface (and definitely no rviz/Qt/ROS: this is the reusability proof).
namespace Ogre
{
class Root;
class SceneManager;
class RenderWindow;
}  // namespace Ogre

namespace qar_ogre_sample
{

/// @brief Which GL render system the standalone sample asked Ogre to use.
enum class RenderSystemChoice
{
  Auto,     ///< Prefer legacy GL (fixed-function capable, matches RViz), else GL3Plus.
  GL3Plus,  ///< Force "OpenGL 3+ Rendering Subsystem".
  GL        ///< Force legacy "OpenGL Rendering Subsystem".
};

RenderSystemChoice parse_render_system_choice(const std::string & s);

/// @brief A minimal, self-contained Ogre application: its OWN Ogre::Root, a GL
/// render system, a hidden RenderWindow (for a current GL context), a
/// SceneManager, and a simple lit scene (ground grid + several coloured cubes +
/// a directional light).
///
/// This exists ONLY to prove that `qar_ogre_streaming` is reusable outside RViz2
///: it links no rviz_common / Qt / ROS, builds the scene from
/// scratch, and hands its plain `Ogre::SceneManager*` to the library exactly the
/// way the RViz plugin hands over RViz's scene manager.
///
/// ## Rendering approach (see sample_scene.cpp for the gory details)
/// The colour material is a small hand-written GLSL-120 lit program: per-object
/// colour arrives as a uniform (uColour), shaded by the scene's ambient + one
/// directional light. The metric-depth pass reuses the library's own
/// "qar_linear_depth" GLSL scheme. Both run on the legacy "OpenGL Rendering
/// Subsystem" — the render system RViz2 Humble uses on this platform (M7 finding)
/// and the M8 default. Two Ogre-1.12/rviz_ogre_vendor quirks are handled in code:
/// (1) this render system advertises GLSL profiles only up to glsl120, so the
/// colour shader is written in GLSL 120 (a #version 150 program is silently
/// rejected and falls back to a white fixed-function pass); (2) load() injects a
/// competing fixed-function technique that would win over the shader technique, so
/// the sample drops any technique whose pass has no vertex program. The M7 GL
/// texture bridge is render-system-agnostic (M7: works on this legacy GL 4.6
/// context). GL3Plus is opt-in (--render-system gl3plus).
class SampleScene
{
public:
  SampleScene();
  ~SampleScene();

  SampleScene(const SampleScene &) = delete;
  SampleScene & operator=(const SampleScene &) = delete;

  /// @brief Bring up Ogre and build the scene. Must run on the thread that will
  /// own the GL context (here: main thread).
  /// @param plugin_dir   Directory containing RenderSystem_GL*/OgreMain (the
  ///                     rviz_ogre_vendor bin dir).
  /// @param choice       Which GL render system to request.
  /// @param log_path     Ogre.log path.
  /// @param error        Receives a human-readable message on failure.
  /// @return true on success.
  bool initialize(
    const std::string & plugin_dir,
    RenderSystemChoice choice,
    const std::string & log_path,
    std::string & error);

  /// @brief The scene manager to hand to qar_ogre_streaming (null before init).
  Ogre::SceneManager * scene_manager() const { return scene_manager_; }

  /// @brief Name of the render system Ogre actually selected (for logs).
  const std::string & render_system_name() const { return render_system_name_; }

  /// @brief Release Ogre (safe to call once; also called by the destructor).
  void shutdown();

private:
  bool load_render_system(
    const std::string & plugin_dir, RenderSystemChoice choice, std::string & error);
  void build_scene();

  Ogre::Root * root_ = nullptr;
  Ogre::RenderWindow * window_ = nullptr;
  Ogre::SceneManager * scene_manager_ = nullptr;
  std::string render_system_name_ = "<none>";
  bool initialized_ = false;
};

}  // namespace qar_ogre_sample

#endif  // QAR_OGRE_SAMPLE__SAMPLE_SCENE_HPP_
