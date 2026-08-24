# Architecture

Cross-cutting architecture reference spanning both products. For depth on a
single product, see [`qar_ogre_streaming/docs/ogre_library_architecture.md`](../qar_ogre_streaming/docs/ogre_library_architecture.md)
(the reusable library) and `docs/rviz_plugin_architecture.md` in the `qar_rviz_plugin` repository
(the RViz2 packaging/UI layer).

## 1. The boundary between the two products

```
qar_rviz_streaming   (ament/colcon package; MAY use rviz_common/rviz_rendering, Qt, ament, rclcpp)
        |
        |  consumes via CMake add_subdirectory() + link, at the PUBLIC API only
        v
qar_ogre_streaming   (plain CMake, reusable static lib; MUST NOT use rviz/Qt/ament/rclcpp)
        |
        |  depends on
        v
Ogre 1.12 (via rviz_ogre_vendor) + the vendored Qar C header + the C++ stdlib
```

This is a **one-way, enforced** boundary:

- `qar_ogre_streaming` never `#include`s anything RViz-, Qt-, ament-, or
  rclcpp-specific. It only knows about `Ogre::SceneManager`/`Ogre::Camera`/
  `Ogre::RenderTexture` and the vendored Qar C types.
- `scripts/check_no_rviz_includes.ps1` scans `qar_ogre_streaming/**` and
  `examples/**` for forbidden `#include` targets (rviz_*, Qt, ament_*,
  rclcpp/rcl) and is wired as the CTest test `check_no_rviz_includes` — it
  runs on every `ctest` invocation and fails the build if the boundary is
  ever violated. It deliberately does **not** scan `qar_rviz_streaming/**`,
  which is *allowed* to depend on all of those.
- `examples/ogre_standalone_streaming_sample` (M8) is the existence proof:
  it links only `qar_ogre_streaming` + Ogre, and `dumpbin /dependents` shows
  no `Qt5*`/`rviz_*` DLL imports at all.

Everything either product does with the library goes through exactly three
"seam" types the host supplies via `OgreQarStreamingConfig`:
a `Ogre::SceneManager*` (whose scene to render), an `IRenderThreadExecutor`
(which thread may touch Ogre/GL), and an `IQarRenderSenderFactory` (how to
reach Qar) — plus three optional seams (`IRenderLock`, `IClock`,
`IGlFenceHook`). `qar_ogre_streaming` "does not know" whether the scene came
from RViz2, a standalone Ogre app, or a test harness (see the side-by-side in
[`api_usage_from_non_rviz_ogre_app.md`](../qar_ogre_streaming/docs/api_usage_from_non_rviz_ogre_app.md)).

## 2. Component responsibilities

### `qar_ogre_streaming` (reusable library)

| Component | Responsibility |
|---|---|
| `OgreQarStreamingManager` | Top-level coordinator: owns the scheduler + the per-peer stream map; `create_stream`/`destroy_stream`/`get_stream`; exposes `render_tick()` and a `DiagnosticsSnapshot`. |
| `OgreQarRenderScheduler` | The **one** batched render tick (§12.2): collect due streams (budget) → **one lock** → begin → update cameras → render → fence → submit → unlock. Thread-affinity guard (refuses off-render-thread calls). |
| `RenderBudget` | Pure, clock-injected FPS pacing + fair shedding + 30 FPS floor + 16 ms/tick budget (§12.3, C8). |
| `OgreQarStereoStream` | One peer: two `OgreStereoRenderTarget`s (L/R) + a Qar sender; per-eye pose/FOV via `qar_pose_conversion`; assembles the separated-texture stereo frame. |
| `OgreStereoRenderTarget` | One eye's color (`PF_R8G8B8A8`) + metric-depth (`PF_FLOAT32_R`, `qar_linear_depth` scheme) `Ogre::RenderTexture` pair; create/resize/destroy; CPU readback accessors. |
| `qar_pose_conversion` | Qar pose/FOV (OpenXR convention) → `Ogre::Camera` transform + custom projection matrix. Pure math, Ogre-free (PODs only). |
| `OgreOpenGlTextureBridge` / `gl_interop` | Native GL texture-id extraction (`Texture::getCustomAttribute("GLID")`) + `glFenceSync` fence + fills the proposed `QarVideoFrameOpenGL`. Runtime-resolved GL entry points (no link-time GL dependency). |
| `IQarRenderSenderAdapter` / `IQarRenderSenderFactory` | The **only** seam to Qar (§13): `begin_frame`/`submit_opengl_stereo_frame`, one adapter per peer. Real implementation is M10 (blocked); `test_support::MockQarRenderSenderAdapter` stands in until then. |
| `IRenderThreadExecutor` / `IRenderLock` / `IClock` / `IGlFenceHook` | Injected seams so the library is host-agnostic and unit-testable (fake clock, immediate executor, recording lock). |

### `qar_rviz_streaming` (RViz2 plugin, ament package)

| Component | Responsibility |
|---|---|
| `QarStreamingPanel` | `rviz_common::Panel` (a `QWidget`), exported via `pluginlib`. Start/Stop Qar, add/remove mock peer, per-peer start/stop, resolution/FPS selectors, diagnostics text, last-error line. Implements `OgreQarDiagnosticsSink`. Owns the `OgreQarStreamingManager`. **No authorization UI** (contractor decision C6 — a peer id only routes a stream, it doesn't gate access). |
| `RVizDisplayContextAdapter` | Wraps `getDisplayContext()`: exposes `Ogre::SceneManager*`, `lockRender()`/`unlockRender()`/`queueRender()`, `getViewCamera()`. The single RViz-specific seam that keeps the library RViz-agnostic. |
| `RVizRenderThreadExecutor` | `IRenderThreadExecutor` on the Qt GUI/render thread; `post_to_render_thread()` → `QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection)`; `is_render_thread()` → `QThread::currentThread() == qApp->thread()`. |
| `RvizPeerPanelModel` | Qt-free mock peer list model (id/label/resolution/fps/streaming flag); generates deterministic `QarPeerId`s. |
| `AnimatedMockSenderFactory` + `MockQarFrameInfoSource` | The mock Qar sender factory (real one arrives in M10) + the reusable deterministic animated pose/FOV source (library-side, shared with the M8 sample). |

## 3. Render-thread model

All Ogre/GL work — camera/texture/viewport creation, `RenderTarget::update()`,
`blitToMemory` readback, GL texture-id extraction, `glFenceSync` — happens on
**exactly one thread**, referred to as *the render thread*, and only while
holding *the render lock*. QarOS's encoder runs **in-process on the main/render
thread, sharing RViz's GL context** — no cross-process handoff and no
`WGL_NV_DX_interop` are needed.

```
                    Qar network callback (any thread)
                             |
                             |  MUST NOT touch Ogre/GL directly
                             v
              IRenderThreadExecutor::post_to_render_thread(work)
                             |
                             v
   +-------------------------------------------------------------+
   |  RENDER THREAD (RViz: Qt GUI thread == qApp->thread();       |
   |  sample: its single main thread)                             |
   |                                                                |
   |  IRenderLock::lock_render() / DisplayContext::lockRender()    |
   |  --------------------------------------------------------    |
   |  OgreQarRenderScheduler::render_tick():                       |
   |    collect_streams_that_need_frames()  (budget, pure, no GL)  |
   |    begin_all_qar_frames()              (sender->begin_frame)  |
   |    update_all_cameras()                (M2 pose/FOV -> Camera)|
   |    render_all_targets()                (RenderTexture::update)|
   |    fence_all_textures()                (glFenceSync)          |
   |    submit_all_frames()                 (assemble + submit)    |
   |  --------------------------------------------------------    |
   |  IRenderLock::unlock_render()                                 |
   +-------------------------------------------------------------+
```

- `OgreQarRenderScheduler::render_tick()` asserts
  `executor->is_render_thread()` and returns `Status::RenderThreadRequired`
  (touching **neither** Ogre nor the lock) if called off-thread — covered by
  a unit test (`Scheduler.RefusesOffRenderThread`).
- Exactly **one** lock/unlock pair wraps the *entire* batch of all active
  peers per tick (not one lock per peer) — every peer samples the same scene
  snapshot, and the render lock (which, in RViz, is `DisplayContext`'s own
  lock — the same one RViz's own render loop uses) is held for the shortest
  contiguous span that covers all Ogre/GL work.
- In the RViz plugin, the render thread is driven by a `QTimer` tick (~60 Hz)
  calling `manager_->render_tick()`; in the standalone sample, a paced loop
  calls the same method on its single thread.
- The GL fence step is a seam (`IGlFenceHook`, no-op until M7 filled it):
  the producer inserts one `glFenceSync` per stream per batch and **owns**
  the resulting `GLsync`; the consumer (Qar) may only wait on it, never
  delete it (see `docs/api_gaps.md` for the full ownership contract and why
  this is only correct in-process/shared-context, per C2).

## 4. Qar adapter model

```
OgreQarStereoStream
   -> IQarRenderSenderAdapter (per peer)
        begin_frame(QarFrameInfo&) -> bool           // Qar supplies pose/FOV/near-far for this frame
        submit_opengl_stereo_frame(QarOpenGlStereoFrame) // library hands back rendered GL textures + fence
```

- `IQarRenderSenderAdapter`/`IQarRenderSenderFactory` are the
  **only** seam through which the library talks to Qar. Nothing else in
  `qar_ogre_streaming` or `qar_rviz_streaming` reasons about Qar sessions,
  peers, or transport.
- The library reuses the **real** vendored QarOS C types directly at this seam
  (`QarVideoFrameOpenGl`, `QarFov`, `QarPose`, `QarQuaternion`, ...) — there is no
  conversion layer, so values round-trip to the QarOS ABI unchanged. (The OpenGL
  producer structs were designed here against a locally-proposed set before SDK
  v0.1.1 shipped them for real; that proposed header has since been retired and the
  real ABI struct is used throughout. See [`api_gaps.md`](api_gaps.md).)
- `test_support::MockQarRenderSenderAdapter` (recording, deterministic) is
  the only implementation today. M10 (blocked on the QarOS binary) adds a
  thin *real* adapter behind the exact same interface — **no other code
  changes** when that happens.
- `MockQarFrameInfoSource` (library-side, reused by both the plugin and the
  sample) is the deterministic per-eye pose/FOV generator standing in for a
  real Qar session's per-frame query (head orbiting a focal point, ±IPD/2
  eye offset, asymmetric per-eye FOV) — pure math, no wall clock, so a frame
  index reproduces an exact stereo view.

## 5. RViz/Ogre object ownership

| Object | Owned by | Notes |
|---|---|---|
| `Ogre::Root`, the main `Ogre::RenderWindow`, the live scene's `Ogre::SceneManager` | **RViz2** (`rviz_common`/`rviz_rendering`), for the whole process lifetime | The plugin only ever obtains a raw, non-owning `Ogre::SceneManager*` via `RVizDisplayContextAdapter::getSceneManager()`. It never constructs, resizes, or destroys RViz's own scene manager or window. |
| The active view camera (`getViewManager()->getCurrent()->getCamera()`) | RViz's `ViewManager` | The offscreen render clones its **pose + projection** (`cloneCameraFrom`) onto the plugin's own offscreen camera; it never mutates or takes ownership of RViz's camera. |
| Offscreen `Ogre::Camera`s (one pair per stream: L/R) | `OgreStereoRenderTarget` (owned in turn by `OgreQarStereoStream`, owned by the manager's stream map) | Created lazily (`initialize_targets()`, idempotent, both-or-neither), destroyed with the stream. |
| Color/depth `Ogre::RenderTexture`s | `OgreStereoRenderTarget` | `setAutoUpdated(false)` — RViz's own render loop never drives them; only the scheduler's explicit `RenderTexture::update(false)` does, under the render lock. |
| `Ogre::MaterialManager::Listener` for the `qar_linear_depth` scheme | Library-global (registered once) | Supplies the depth-shader technique for every renderable, scheme-wide, so a second pass through the scheme yields metric depth for the whole scene without per-object material edits. |
| Native GL texture ids (`GLuint`) extracted via `getCustomAttribute("GLID")` | The underlying `Ogre::Texture` (Ogre keeps owning the GL object) | The bridge only **reads** the id; it never deletes or recreates the GL texture. Valid only within the process/GL-context that created it (see C2 in `docs/api_gaps.md`). |
| `GLsync` fences | The **producer** (this library), per the contract in `docs/api_gaps.md` | At most one outstanding per stream; replaced (old one deleted) when the next is inserted, always on the render thread inside the batch lock; never deleted in a stream's destructor (which may run off-thread). |
| The standalone sample's own `Ogre::Root`/window/`SceneManager` (M8) | The sample's `SampleScene` | Entirely separate from RViz's; proves the library imposes no RViz-specific ownership assumptions. |

## 6. Where to look next

- Full library public API + build instructions:
  [`qar_ogre_streaming/docs/ogre_library_architecture.md`](../qar_ogre_streaming/docs/ogre_library_architecture.md)
- Plugin-specific classes + pluginlib wiring + auto-dump verification:
  `docs/rviz_plugin_architecture.md` in the `qar_rviz_plugin` repository
- Pose/FOV math derivation: [`docs/coordinate_conventions.md`](coordinate_conventions.md)
- Offscreen render narrative: `docs/offscreen_rendering.md` in the `qar_rviz_plugin` repository
- API gaps, the metric-depth decision, the GL fence contract, Gate B evidence:
  [`docs/api_gaps.md`](api_gaps.md)
- How to exercise/verify every piece of this end to end:
  [`docs/testing.md`](testing.md)
