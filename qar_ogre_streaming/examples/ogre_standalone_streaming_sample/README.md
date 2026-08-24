# Standalone Ogre streaming sample (`qar_ogre_streaming` reusability proof)

This is a plain Ogre application that uses the reusable **`qar_ogre_streaming`**
library with **no RViz, no ROS, no Qt** — the key architectural requirement of the
whole project. It proves the library is not
accidentally coupled to RViz internals: the RViz2 plugin (`qar_rviz_streaming`) and
this sample drive the library through the **same public API**.

What the sample does:

1. Creates its **own** `Ogre::Root`, a GL render system, a hidden GL context
   (a hidden `Ogre::RenderWindow`) and a `SceneManager`.
2. Builds a simple lit scene: a ground grid + several coloured cubes + a
   directional light, so stereo parallax and depth are clearly visible.
3. Instantiates `qar_ogre_streaming` via `OgreQarStreamingConfig` with a
   `MockQarRenderSenderAdapter` (from the library's `test_support`) + an
   `ImmediateRenderThreadExecutor`, and creates **≥2 mock stereo peers**, each
   with a distinct `MockQarFrameInfoSource` orbit phase.
4. Runs N batched ticks through the **same** `OgreQarStreamingManager::render_tick()`
   path the RViz plugin uses.
5. Writes per-peer left/right **colour + depth** PNGs (+ raw `.f32` depth) with
   frame metadata, prints a diagnostics snapshot, and finally exercises the M7
   **zero-copy OpenGL texture bridge** (native GL texture id + `glFenceSync`) —
   demonstrating the bridge works outside RViz too.

## Build

Examples are built by the repo-root CMake project and are **ON by default**
(`-DQAR_BUILD_EXAMPLES=ON`). The library itself stays independently buildable.

```powershell
# From the repo root. Point at the ROS 2 Humble distro for Ogre (rviz_ogre_vendor):
#   -DQAR_ROS2_INSTALL_ROOT=<distro>/ros2-windows, or env ROS2_INSTALL_ROOT,
#   or the audited-machine default D:/development/ros2_humble/ros2-windows.
cmake --preset vs2019-x64
cmake --build --preset vs2019-x64
```

The sample exe lands at
`build/vs2019-x64/examples/ogre_standalone_streaming_sample/RelWithDebInfo/qar_ogre_standalone_streaming_sample.exe`.

To skip the examples: `cmake --preset vs2019-x64 -DQAR_BUILD_EXAMPLES=OFF`.

## Run

The sample needs the Ogre runtime DLLs (`OgreMain.dll`, `RenderSystem_GL*.dll` and
deps) on `PATH` — they live in the distro's `opt/rviz_ogre_vendor/bin`. Use the
helper script (it sets `PATH` and passes the plugin dir):

```powershell
powershell -File scripts/run_ogre_sample.ps1 -Ticks 120 -Peers 3
# outputs -> build/qar_sample_out/
```

Or run the exe directly, setting `PATH` yourself:

```powershell
$env:PATH = "D:/development/ros2_humble/ros2-windows/opt/rviz_ogre_vendor/bin;$env:PATH"
& <exe> --ticks 120 --peers 3 --out-dir build/qar_sample_out
```

### CLI flags

| Flag | Default | Meaning |
|---|---|---|
| `--ticks N` | 120 | Number of batched render ticks. |
| `--peers K` | 3 | Number of mock stereo peers (raised to 2 if `<2`). |
| `--capture-every M` | 0 | Also capture every M ticks (0 = only the final tick). |
| `--width W` / `--height H` | 1280 / 720 | Per-eye texture size. |
| `--ipd-mm X` | 64 | Inter-pupillary distance (mm). |
| `--out-dir DIR` | `qar_sample_out` | Output directory. |
| `--render-system S` | `auto` | `auto` (legacy GL, the default) / `gl3plus` / `gl`. |
| `--ogre-plugin-dir D` | baked / env | Dir with `RenderSystem_GL*`/`OgreMain`. |

Exit code is `0` on success. Per peer the sample checks: both eyes contain
content, metric depth is present and non-uniform, the two eyes differ, and the
near depth band shifts more than the far band (correct 1/depth parallax).

## Outputs (per run, under `--out-dir`)

- `peerK_left_color.png` / `peerK_right_color.png` — the stereo colour pair.
- `peerK_left_depth.png` / `peerK_right_depth.png` — metric-depth visualisations
  (near = bright, far = dark).
- `peerK_left_depth.f32` / `peerK_right_depth.f32` — raw metric depth (float32,
  `width*height`, row-major, metres).
- `peerK_stereo.txt` — per-peer metadata (per-eye pose/FOV, content px, depth
  range, near/far disparity, pass/fail).
- `bridge_demo.txt` — M7 zero-copy bridge result (native GL id, `glIsTexture`,
  round-trip match, fence status).
- `summary.txt` — aggregate run summary (render system, per-peer pass, overall).
- `qar_sample_ogre.log` — Ogre's own log.

## Render-system note (Ogre 1.12 / rviz_ogre_vendor)

The default (`auto`) selects the legacy **"OpenGL Rendering Subsystem"** — the same
render system RViz2 Humble uses on this platform (M7 finding). The colour material
is a small hand-written **GLSL-120** program (per-object colour via a `uColour`
uniform, lit by ambient + one directional light); depth reuses the library's
`qar_linear_depth` GLSL scheme. Two quirks of this Ogre build are handled in
`src/sample_scene.cpp`: (1) the legacy GL system advertises GLSL only up to
`glsl120`, so a `#version 150` colour program is silently rejected and falls back to
a white fixed-function pass — hence GLSL 120; (2) `Material::load()` injects a
competing fixed-function technique that would win over the shader technique, so the
sample drops any technique whose pass has no vertex program. `--render-system
gl3plus` is opt-in/experimental (GL3Plus core has no fixed function and would need
the RTShaderSystem for the colour pass). The M7 GL texture bridge is
render-system-agnostic either way.
