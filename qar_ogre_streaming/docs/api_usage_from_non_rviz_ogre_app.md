# Using `qar_ogre_streaming` from a non-RViz Ogre application

`qar_ogre_streaming` is a reusable library: given *any* `Ogre::SceneManager` plus a
few generic collaborators, it renders stereo views and hands them to Qar. It has no
dependency on RViz, Qt, ROS or ament (enforced by the `check_no_rviz_includes`
CTest, which scans both `qar_ogre_streaming/**` and `examples/**`).

This document shows that the **RViz2 plugin (`qar_rviz_streaming`)** and the
**standalone sample (`examples/ogre_standalone_streaming_sample/`)** drive the
library through the **same public API** — they differ only in *what* they plug into
the three generic seams. This is the reusability requirement, proven
by milestone M8.

## The three seams a host must supply

`OgreQarStreamingConfig` is the whole contract:

```cpp
struct OgreQarStreamingConfig {
  Ogre::SceneManager * scene_manager;                       // (1) the scene to render
  std::shared_ptr<IRenderThreadExecutor> render_thread_executor; // (2) the render thread
  std::shared_ptr<IQarRenderSenderFactory> qar_sender_factory;   // (3) the Qar sender
  OgreQarDiagnosticsSink * diagnostics;                     // optional
  RenderBudgetConfig render_budget;                         // optional (FPS/time budget)
  std::shared_ptr<IRenderLock> render_lock;                 // optional (host lock)
  std::shared_ptr<IClock> clock;                            // optional (defaults to real)
  std::shared_ptr<IGlFenceHook> fence_hook;                 // optional (M7 GL fence)
};
```

The library "does not know whether `scene_manager` came from RViz2, another Ogre
application, or a test harness." Only the three seams change between hosts:

| Seam | RViz plugin (`qar_rviz_streaming`) | Standalone sample (`examples/...`) |
|---|---|---|
| `scene_manager` | `RVizDisplayContextAdapter::getSceneManager()` (RViz's shared scene) | `SampleScene::scene_manager()` (its own `Ogre::Root`'s scene) |
| `render_thread_executor` | `RVizRenderThreadExecutor` (Qt `QMetaObject::invokeMethod` to the qApp thread) | `test_support::ImmediateRenderThreadExecutor` (inline; single-threaded loop) |
| `qar_sender_factory` | `AnimatedMockSenderFactory` (plugin-private mock) | `SampleMockSenderFactory` (wraps `test_support::MockQarRenderSenderAdapter`) |
| `render_lock` | `RVizRenderLock` → `DisplayContext::lockRender()/unlockRender()` | `nullptr` (single-threaded → no host lock needed) |
| `diagnostics` | the Qt panel (appends to its log/`OgreQarDiagnosticsSink`) | `StdoutDiagnosticsSink` (printf) |

Everything below the config — stream creation, the batched render tick, the
diagnostics snapshot, the GL bridge — is **identical library code**.

## Side-by-side: the same public calls

### 1. Construct the manager

RViz plugin (`qar_streaming_panel.cpp`, `onInitialize`):

```cpp
qar_ogre_streaming::OgreQarStreamingConfig cfg;
cfg.scene_manager          = adapter_->getSceneManager();         // RViz scene
cfg.render_thread_executor = executor_;                            // RVizRenderThreadExecutor
cfg.qar_sender_factory     = sender_factory_;                      // AnimatedMockSenderFactory
cfg.diagnostics            = this;                                 // the panel
cfg.render_lock            = render_lock_;                         // RVizRenderLock
cfg.render_budget.max_batch_milliseconds = 16.0;
cfg.render_budget.floor_fps              = 30;
cfg.fence_hook             = std::make_shared<qar_ogre_streaming::GlFenceHook>();  // M7
manager_ = std::make_unique<qar_ogre_streaming::OgreQarStreamingManager>(cfg);
```

Standalone sample (`main.cpp`):

```cpp
qos::OgreQarStreamingConfig cfg;
cfg.scene_manager          = scene.scene_manager();                // own Ogre::Root scene
cfg.render_thread_executor = executor;                             // ImmediateRenderThreadExecutor
cfg.qar_sender_factory     = factory;                              // SampleMockSenderFactory
cfg.diagnostics            = diag_sink.get();                      // StdoutDiagnosticsSink
cfg.render_lock            = nullptr;                              // single-threaded
cfg.render_budget.max_batch_milliseconds = 200.0;
cfg.render_budget.floor_fps              = 30;
cfg.fence_hook             = std::make_shared<qos::GlFenceHook>();  // M7 (same hook)
qos::OgreQarStreamingManager manager(cfg);
```

### 2. Create peers and start streaming — identical API

```cpp
qar_ogre_streaming::StereoStreamConfig sc;
sc.peer_id = id; sc.width = w; sc.height = h; sc.max_fps = 60;
manager.create_stream(sc);              // -> Result
manager.get_stream(id)->set_active(true);
manager.set_streaming_active(true);
```

Both the plugin (`startStreamInternal` / `onStartQar`) and the sample use exactly
these calls.

### 3. Render — the same batched tick

Both hosts call the one batched entry point on the render thread; the library runs
the whole §12.2 sequence (collect → lock → begin → update → render → fence →
submit → unlock) inside it:

```cpp
manager.render_tick();     // plugin: from a ~60 Hz QTimer; sample: from a paced loop
```

### 4. Read results — the same accessors

```cpp
auto * stream = manager.get_stream(id);
stream->initialize_targets();                      // idempotent
stream->left_target()->readColorRgba(rgba);        // OgreStereoRenderTarget
stream->left_target()->readDepthMetric(depth);
const auto snap = manager.diagnostics_snapshot();  // DiagnosticsSnapshot (copyable)
```

The plugin's `captureStereoPair` and the sample's `capture_peer` read the eye
targets through these same accessors.

### 5. M7 zero-copy GL bridge — the same spike + bridge

Both install `GlFenceHook` (above) and can run the reusable spike / bridge on a
rendered target:

```cpp
auto res = qar_ogre_streaming::run_color_texture_roundtrip_spike(*stream->left_target());
qar_ogre_streaming::OgreOpenGlTextureBridge bridge;
QarVideoTextureOpenGL tex{};
bridge.extract_gl_texture(/* Ogre::Texture* */, tex);   // native GL id (getCustomAttribute("GLID"))
QarVideoFrameOpenGL out{};
qar_ogre_streaming::OgreOpenGlTextureBridge::fill_opengl_frame(submitted_frame, out);
```

The RViz plugin runs this inside `DisplayContext::lockRender()` on the Qt render
thread; the sample runs it on its main thread (which owns the GL context) — same
calls, different host thread. The M7 finding that RViz uses the legacy
"OpenGL Rendering Subsystem" on a GL 4.6 context also holds for the sample's own
`Ogre::Root` (default `auto` selects the same legacy GL system), and the bridge is
render-system-agnostic regardless.

## Consequence

Because the sample links only `qar_ogre_streaming` (+ its header-only
`test_support` helpers) and Ogre — verified: its executable imports `OgreMain.dll`
and the CRT, **no `Qt5*` and no `rviz_*` DLLs** — the library is provably reusable
outside RViz. Any other Ogre application (simulation tool, internal Qar test app)
integrates the same way: supply a `SceneManager`, an `IRenderThreadExecutor`, and
an `IQarRenderSenderFactory`, then drive `render_tick()`.

See `examples/ogre_standalone_streaming_sample/README.md` to build and run it.
