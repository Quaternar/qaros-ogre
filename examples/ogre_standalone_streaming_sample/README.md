# Standalone Ogre streaming sample (`qar_ogre_streaming` reusability proof)

This is a plain Ogre application that uses the reusable **`qar_ogre_streaming`**
library with **no RViz, no ROS, no Qt**. It proves that the library does not depend on
RViz internals: the RViz2 plugin of the repository qar_rviz_plugin and this sample drive
the library through the **same public API**.

The sample needs **no QarOS SDK**. Its peers are synthetic and live in the same process;
nothing is sent anywhere. To render the same scene for a real QarOS Visualizer, use the
example `examples/qaros_live_stream` (see the last section).

What the sample does:

1. Creates its **own** `Ogre::Root`, a GL render system, a hidden GL context
   (a hidden `Ogre::RenderWindow`) and a `SceneManager`.
2. Builds a simple lit scene: a ground grid + several coloured cubes + a
   directional light, so stereo parallax and depth are clearly visible.
3. Instantiates `qar_ogre_streaming` via `OgreQarStreamingConfig` with an
   `ImmediateRenderThreadExecutor` and a recording stand-in for the QarOS render sender
   (`MockQarRenderSenderAdapter`, both from the library's `test_support`), and creates
   **at least two synthetic stereo peers**, each driven by its own animated
   `MockQarFrameInfoSource` with a distinct orbit phase.
4. Runs N batched ticks through the **same** `OgreQarStreamingManager::render_tick()`
   path that a QarOS-backed application uses.
5. Writes per-peer left/right **colour + depth** PNGs (+ raw `.f32` depth) with
   frame metadata, prints a diagnostics snapshot, and finally exercises the
   **zero-copy OpenGL texture bridge** (native GL texture id + OpenGL frame
   marshalling), demonstrating that the bridge works outside RViz too.

## Build

Examples are built by the repository's CMake project and are **ON by default**
(`-DQAR_BUILD_EXAMPLES=ON`). The library itself stays independently buildable.

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
# The preset reads ROS2_INSTALL_ROOT, the ROS 2 Humble distribution that provides
# Ogre (rviz_ogre_vendor).
cmake --preset vs2019-x64
cmake --build --preset vs2019-x64
```

The sample exe lands at
`build/vs2019-x64/examples/ogre_standalone_streaming_sample/RelWithDebInfo/qar_ogre_standalone_streaming_sample.exe`.

To skip the examples: `cmake --preset vs2019-x64 -DQAR_BUILD_EXAMPLES=OFF`.

## Run

The sample needs the Ogre runtime DLLs (`OgreMain.dll`, `RenderSystem_GL*.dll` and
deps) on `PATH`: they live in the distribution's `opt/rviz_ogre_vendor/bin`. In the same
window, use the helper script (it sets `PATH` and passes the plugin dir):

```powershell
pwsh -NoProfile -File scripts\run_ogre_sample.ps1 -Ticks 120 -Peers 3
# outputs -> %TEMP%\qar_ogre_sample\ (override with -OutDir)
```

Or run the exe directly in the same window, setting `PATH` yourself:

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe --ticks 120 --peers 3 --out-dir build\qar_sample_out
```

### CLI flags

| Flag | Default | Meaning |
|---|---|---|
| `--ticks N` | 120 | Number of batched render ticks. |
| `--peers K` | 3 | Number of synthetic in-process stereo peers (raised to 2 if `<2`). |
| `--capture-every M` | 0 | Also capture every M ticks (0 = only the final tick). |
| `--width W` / `--height H` | 1280 / 720 | Per-eye texture size. |
| `--ipd-mm X` | 64 | Inter-pupillary distance (mm). |
| `--out-dir DIR` | `qar_sample_out` | Output directory. |
| `--render-system S` | `auto` | `auto` (legacy GL, the default) / `gl3plus` / `gl`. |
| `--ogre-plugin-dir D` | baked / env | Dir with `RenderSystem_GL*`/`OgreMain`. |
| `--help` | | Show the flags. |

The exit code is `0` when every check passes, `1` when a check fails and `2` when an
argument is wrong or Ogre cannot start. Per peer the sample checks: both eyes contain
content, metric depth is present and non-uniform, the two eyes differ, and the
near depth band shifts more than the far band (correct 1/depth parallax).

## Outputs (per run, under `--out-dir`)

- `peerK_left_color.png` / `peerK_right_color.png`: the stereo colour pair.
- `peerK_left_depth.png` / `peerK_right_depth.png`: metric-depth visualisations
  (near = bright, far = dark).
- `peerK_left_depth.f32` / `peerK_right_depth.f32`: raw depth (float32,
  `width*height`, row-major): metres while `QAR_DEPTH_ENCODING` is not `ndc`, and
  the window-space value when it is `ndc`.
- `peerK_stereo.txt`: per-peer metadata (per-eye pose/FOV, content px, depth
  range, near/far disparity, pass/fail).
- `bridge_demo.txt`: OpenGL bridge result (render system, native GL texture id,
  `fill_opengl_frame` outcome with texture and view counts, `bridge_ran`, `bridge_pass`).
- `summary.txt`: aggregate run summary (render system, per-peer pass, overall).
- `qar_sample_ogre.log`: Ogre's own log.

## Render-system note (Ogre 1.12 / rviz_ogre_vendor)

The default (`auto`) selects the legacy **"OpenGL Rendering Subsystem"** — the same
render system RViz2 Humble uses on this platform. The colour material
is a small hand-written **GLSL-120** program (per-object colour via a `uColour`
uniform, lit by ambient + one directional light); depth reuses the library's
`qar_linear_depth` GLSL scheme. Two quirks of this Ogre build are handled in
`src/sample_scene.cpp`: (1) the legacy GL system advertises GLSL only up to
`glsl120`, so a `#version 150` colour program is silently rejected and falls back to
a white fixed-function pass — hence GLSL 120; (2) `Material::load()` injects a
competing fixed-function technique that would win over the shader technique, so the
sample drops any technique whose pass has no vertex program. `--render-system
gl3plus` is opt-in/experimental (GL3Plus core has no fixed function and would need
the RTShaderSystem for the colour pass). The GL texture bridge is
render-system-agnostic either way.

## The same scene with the real QarOS runtime

This sample has no real-SDK mode. The example `examples/qaros_live_stream`
(target `qar_ogre_live_stream_example`) compiles this sample's `src/sample_scene.cpp`
and streams that scene to a real QarOS Visualizer through the render component of the
adapter. It is built together with the sample; [Getting started](../../docs/GETTING_STARTED.md)
runs it.
