# qar_ogre_streaming — Library Architecture

`qar_ogre_streaming` is a reusable, plain-CMake C++17 library that renders stereo
views from an Ogre scene and hands them to Qar streaming through a narrow adapter.
It is deliberately independent of RViz2, Qt, ament and ROS so it can be reused in
any Ogre-based application.

> Status: **feature-complete through M8; M9 finalized this document.** Public
> API, build system, mocks and tests are in place and validated: pose/projection
> math (M2), single-eye offscreen render (M4), the single-peer stereo stream
> (M5), the batched multi-peer scheduler + budgeting (M6), and the zero-copy
> OpenGL texture handoff (M7, `OgreOpenGlTextureBridge` +
> `run_color_texture_roundtrip_spike`). M8 adds the standalone Ogre sample
> (`examples/ogre_standalone_streaming_sample/`) proving the library is reusable with
> NO rviz/Qt/ROS — see [`api_usage_from_non_rviz_ogre_app.md`](api_usage_from_non_rviz_ogre_app.md).
> M10 (real Qar session integration, behind the existing `IQarRenderSenderAdapter`
> seam) is blocked on the user supplying the QarOS binary/SDK — see the project
> root [`README.md`](../../README.md) and [`docs/api_gaps.md`](../../docs/api_gaps.md).
> For how this library's responsibilities relate to the RViz2 plugin that
> consumes it, see the cross-cutting [`docs/architecture.md`](../../docs/architecture.md).

## Dependency rule (enforced)

```
qar_rviz_streaming  ->  qar_ogre_streaming  ->  Qar C API / Ogre
```

The library must **never** depend on `rviz_common`, `rviz_rendering`, Qt widgets,
ament, or `rclcpp`/`rcl`. This is enforced automatically by
`scripts/check_no_rviz_includes.ps1`, wired as the CTest test
`check_no_rviz_includes`, which scans `qar_ogre_streaming/**` and `examples/**` for
forbidden `#include`s and fails the test suite if any are found.

Allowed dependencies only: **Ogre** (via `rviz_ogre_vendor::OgreMain`), the vendored
**Qar C header** (`third_party/qar/include/qar_streaming.h`), and the **C++ standard
library**.

## Public API surface

| Header (`include/qar_ogre_streaming/`) | Responsibility |
|---|---|
| `types.hpp` | Shared POD types; reuses the real Qar C types; `Status`/`Result`; `StereoStreamConfig`; `QarFrameInfo`; `QarOpenGlStereoFrame`; peer-id helpers. |
| `ogre_qar_streaming_manager.hpp` | `OgreQarStreamingConfig` + `OgreQarStreamingManager` coordinator: owns scheduler + streams, create/destroy/lookup, diagnostics snapshot. |
| `ogre_qar_render_scheduler.hpp` | `OgreQarRenderScheduler`: one batched render tick on the render thread, FPS/time budgeting, thread-affinity guard. |
| `ogre_qar_stereo_stream.hpp` | `OgreQarStereoStream`: one peer's left/right cameras + render targets + sender; per-eye pose/FOV apply + separated-frame assembly (section 11.6, **implemented in M5**). |
| `mock_qar_frame_info_source.hpp` | `MockQarFrameInfoSource`: deterministic, animated per-eye poses/FOVs (orbiting head, configurable IPD, asymmetric FOV) driving mock streams (**M5**). |
| `ogre_stereo_render_target.hpp` | `OgreStereoRenderTarget`: safe create/resize/destroy of one eye's color+depth render texture. |
| `ogre_opengl_texture_bridge.hpp` | `OgreOpenGlTextureBridge`: Ogre texture -> Qar OpenGL frame; native GL id + fence (section 11.8; M7 spike). |
| `qar_render_sender_adapter.hpp` | `IQarRenderSenderAdapter` + `IQarRenderSenderFactory`: the only seam to Qar. |
| `qar_pose_conversion.hpp` | Qar pose/FOV -> Ogre transform/projection helpers (section 11.9; **implemented in M2**). |
| `render_budget.hpp` | `RenderBudget` / `RenderBudgetConfig`: per-stream FPS gate + global tick budget. |
| `diagnostics.hpp` | `OgreQarDiagnosticsSink` interface + copyable `DiagnosticsSnapshot`/`StreamDiagnostics`. |
| `render_thread_executor.hpp` | `IRenderThreadExecutor` (verbatim signature). |
| `qar_streaming_c.hpp` | Warning-suppressed wrapper that includes the vendored `qar_streaming.h`. |

## Threading model

- All Ogre / OpenGL work happens on **one** render thread. The library obtains that
  thread through `IRenderThreadExecutor` — RViz supplies a Qt-backed implementation,
  a standalone app supplies its own, tests use `ImmediateRenderThreadExecutor`.
- Qar networking callbacks must **never** render. They only enqueue work via
  `post_to_render_thread()`. The scheduler calls `is_render_thread()` and refuses
  (`Status::RenderThreadRequired`) if invoked off-thread — verified by a unit test.
- `render_tick()` renders every active stream in one coherent batch:
  `collect -> lock -> begin frames -> update cameras -> render targets -> fence ->
  submit -> unlock`.

## Qar adapter model

The library talks to Qar only through `IQarRenderSenderAdapter`
(`begin_frame` / `submit_opengl_stereo_frame`), created per-peer by
`IQarRenderSenderFactory`. Because the real Qar OpenGL producer path does not exist
natively, and the OpenGL frame structs used at that seam are the real ABI structs
from the vendored header. A concrete adapter (`qar_qar_adapter`, in this
repository) implements the seam against the QarOS runtime; a recording
`MockQarRenderSenderAdapter` (in `test_support/`) implements it for the unit tests
and for the standalone sample, which needs no QarOS runtime at all.

### Qar header interop

The library **reuses the real Qar C types directly** (`QarFov`, `QarPose`,
`QarQuaternion`, `QarVector3`, `QarNearFar`, `QarPeerId`, `QarFrameLayout`,
`QarPixelFormat`, `QarRenderFrameView`, `QarVideoFrameView`, `QarTextureSize`, …)
rather than mirroring them, so values round-trip to the Qar ABI with no conversion
layer. The vendored header is pure C with `extern "C"` guards; it is included once
through `qar_streaming_c.hpp`, which brackets the include in compiler
warning-suppression pragmas (the auto-generated header trips `/W4` C4505 on its
`static inline` helpers).

## Pose and projection conversion (M2, section 11.9)

`qar_pose_conversion.{hpp,cpp}` turns a Qar view pose + FOV into an Ogre camera
transform and projection matrix. The full derivation, with a worked asymmetric
example, lives in [`docs/coordinate_conventions.md`](../../docs/coordinate_conventions.md);
the essentials:

- **Both frames are identical** — right-handed, +X right, +Y up, −Z forward
  (OpenXR convention on the Qar side, **contractor-confirmed**; the native
  `Ogre::Camera` local frame on the Ogre side). So the pose mapping is the
  **identity mapping**.
- **Position** copies component-for-component (`qar_position_to_ogre`).
- **Orientation** copies component-for-component — the *same rotation* — with
  only a storage reindex from Qar `x,y,z,w` (xyzw) to the Ogre-ordered
  `Quat{w,x,y,z}` (wxyz), plus normalization. Degenerate (near-zero-length)
  input is rejected by the checked `qar_pose_to_ogre` (→ `InvalidArgument`) and
  falls back to identity in the convenience `qar_orientation_to_ogre`.
- **Projection** (`make_projection_from_qar_fov`) builds the classic OpenGL
  off-axis **glFrustum** matrix (row-major, depth range **[−1, +1]**), which
  equals `Ogre::Frustum::setFrustumExtents(...) + getProjectionMatrix()`. It
  rejects degenerate frustums (`near ≤ 0`, `far ≤ near`,
  `angle_right ≤ angle_left`, `angle_up ≤ angle_down`, `|angle| ≥ π/2`).
- Output types (`Vec3`, `Quat`, `Mat4`) are plain PODs, so the public API never
  exposes an Ogre math type; `Mat4` is row-major and byte-compatible with
  `Ogre::Matrix4` for direct `setCustomProjectionMatrix` use. This keeps the
  library and its unit-test executable free of any linked Ogre symbol — the
  projection tests validate the matrix against an in-test **replica** of Ogre's
  `Frustum::updateFrustumImpl()` formula rather than linking OgreMain.

M5 consumes these helpers to place per-eye stereo cameras.

## Single-peer stereo stream (M5, section 11.6)

`OgreQarStereoStream` renders one peer's left + right eye views from the shared
scene and submits them as one **separated-texture** stereo frame.

- **Ownership.** It owns two `OgreStereoRenderTarget`s (left/right), each an
  independent `Ogre::Camera` + color RTT (`PF_R8G8B8A8`) + metric-depth RTT
  (`PF_FLOAT32_R`, the M4 `"qar_linear_depth"` scheme). `initialize_targets()`
  creates both at the configured size; it is idempotent and keeps a both-or-
  neither invariant. `targets_ready()` reports creation state.
- **Per-eye pipeline (each `render_and_submit()`).**
  1. `sender->begin_frame(info)` — the Qar adapter returns per-eye pose + FOV +
     near/far (a declined frame is a valid, accounted skip).
  2. For each eye, `compute_eye_render_params()` runs the **M2** conversions:
     `qar_pose_to_ogre` → `Camera::setPosition/setOrientation(Quaternion(w,x,y,z))`
     and `make_projection_from_qar_fov` → `Camera::setCustomProjectionMatrix(true,
     Matrix4(row-major m))`. Degenerate pose/FOV inputs are flagged and leave the
     camera unchanged (a bad frame never blanks the view).
  3. Both eyes render color + metric depth (`RenderTarget::update(false)`).
  4. `assemble_separated_stereo_frame()` builds the `QarOpenGlStereoFrame`.
  5. `sender->submit_opengl_stereo_frame(frame)`.
- **Separated-texture layout.** `texture[0]=left color`,
  `[1]=right color`, `[2]=left depth`, `[3]=right depth`; the frame's explicit
  `views[]` name each logical view's type (COLOR/DEPTH) + eye (LEFT/RIGHT) +
  backing texture index, mirroring the real Qar `QarVideoFrameLayout`. GL texture
  ids are placeholder Ogre handles until the M7 `OgreOpenGlTextureBridge` extracts
  native GL names; the sync block carries no fence until M7.
- **Threading.** `initialize_targets()`/`render_and_submit()` touch Ogre/GL and
  MUST run on the render thread under the host render lock. The stream never locks
  itself — in RViz the plugin's render tick wraps the call in
  `lockRender()/unlockRender()`; M6's scheduler will batch N streams in one lock.
- **Pure, testable core.** `compute_eye_render_params()` and
  `assemble_separated_stereo_frame()` live in an Ogre-free translation unit so the
  per-eye conversion and the separated-layout/eye-label assembly are unit-tested
  headlessly (the actual Ogre render is validated by the in-RViz scripted stereo
  capture).

`MockQarFrameInfoSource` is the reusable, deterministic, animated stand-in for a
real sender's per-frame query: a head pose orbiting a focal point (Z-up look-at,
matching RViz's orbit view controller), two eyes offset by a configurable **IPD**
(default 64 mm) along the head's right axis (parallel stereo → horizontal parallax
∝ 1/depth), and a per-eye asymmetric, aspect-matched FOV. It is pure math (no Ogre,
no wall-clock), so a frame index reproduces the exact stereo view. The RViz plugin
and the M8 standalone sample both drive streams through this same source.

## Batched multi-peer render scheduler + budget (M6, sections 11.5 / 12.2 / 12.3)

`OgreQarRenderScheduler` renders **all** active peers in ONE coherent batch per
render tick, so every peer samples the same scene snapshot. `render_tick()`
implements the section 12.2 sequence exactly, with a **single** render lock around
the whole batch:

```
collect_streams_that_need_frames()   // budget FPS gate + time-budget shedding (pure)
if none -> return (early-out, NO lock)
lock_render()                         // ONE lock around the whole batch (§12.2/C2)
  begin_all_qar_frames()              //   sender->begin_frame() for every due stream
  update_all_cameras()                //   lazy target init + M2 pose/FOV per eye
  render_all_targets()                //   both eyes of every stream, back-to-back
  fence_all_textures()                //   IGlFenceHook seam — NO-OP until M7
  submit_all_frames()                 //   assemble + submit every stream's frame
unlock_render()
```

- **Thread affinity (C2 / §25.2).** `render_tick()` asserts
  `executor_->is_render_thread()` and returns `RenderThreadRequired` (touching
  neither Ogre nor the lock) otherwise. Qar/mock callbacks never render — they only
  enqueue via `IRenderThreadExecutor::post_to_render_thread`; the panel's render
  QTimer is the sole caller of `render_tick()`.
- **Seams (rviz/Qt-free).** The lock is an injected `IRenderLock` (RViz forwards it
  to `DisplayContext::lockRender()/unlockRender()`); time comes from an injected
  `IClock` (default `SteadyClock`) read **once per tick** and threaded into the
  budget, so the pacing logic holds no wall clock and is deterministically testable;
  the fence step calls an optional `IGlFenceHook` that M7 fills (the M6 default is a
  pure no-op). All three are optional `OgreQarStreamingConfig` fields.
- **Stream phases.** `OgreQarStereoStream` exposes `begin_frame_phase()` /
  `update_cameras_phase()` / `render_targets_phase()` / `fence_phase()` /
  `submit_phase()`; the scheduler calls each across ALL streams in phase order (true
  batch semantics), and `render_and_submit()` composes the same phases for a single
  stream. Per-eye render time is measured separately (left/right) for diagnostics.

### RenderBudget (sections 12.3 / 25.4; C8)

Designed for up to **8 peers**, validated with 3–4. Defaults: target **60 FPS/peer**,
hard floor **30 FPS/peer**, ~**16 ms** per-tick time budget.

- **FPS pacing — next-due accumulator.** Each peer carries a `next_due` time; it is
  due when `now >= next_due`. On render, `next_due = max(next_due + 1/max_fps, now)`.
  The accumulator form achieves the target FPS on average even when the tick rate is
  not an exact multiple of the frame interval (a naive `now + interval` gate aliases,
  e.g. to 50 FPS on a 100 Hz tick vs a 60 FPS target); the `max(…, now)` clamp stops
  a stalled peer from bursting to catch up.
- **Fair shedding + 30 FPS floor.** Due peers are ordered most-overdue-first (by
  `next_due`, then registration order). A due peer that has not rendered within
  `1/floor_fps` is **floor-critical** and admitted unconditionally; the remaining
  *optional* (target-FPS) peers are admitted greedily in fair order while the
  accumulated estimated cost (last measured batch cost, or `default_stream_cost_ms`
  before first measurement) stays within `max_batch_milliseconds` and
  `max_streams_per_tick`. Optional peers that do not fit are shed this tick and
  counted as skipped frames. This guarantees no active peer drops below the 30 FPS
  floor while headroom exists, and sheds *fairly* under load (the shed peers become
  most-overdue and render first next tick — the rotation is self-correcting).
- **Testability.** The budget takes `now` as a parameter (no clock inside), so FPS
  windows, time-budget shedding, fairness rotation and floor protection are all
  covered by fake-clock unit tests (`render_budget_tests.cpp`).

### Diagnostics

Per peer: frames submitted/skipped, per-eye + total render ms, cumulative avg, and
**derived achieved FPS** (over a rolling 120-frame submit window, computed by the
scheduler which owns the tick clock). Per batch: last + rolling-avg batch ms,
streams served/skipped this batch, and the global budget-skipped-frame count. All
surface via the copyable `DiagnosticsSnapshot` and the panel diagnostics text.

**Evidence (scripted, in-RViz).** `run_rviz_with_plugin.ps1 -AutoMulti` adds K mock
peers (each an independent orbit-phase view of the same scene), batch-renders them
via `manager.render_tick()`, and dumps `<prefix>_automulti.txt` (per-peer achieved
FPS, per-batch ms, served/skipped, and `full_batch_ticks` = ticks where ALL active
peers rendered in one batch) plus a per-peer color PNG. A 4-peer run rendered all 4
peers in every batch (~2.5 ms/batch) with each peer ≥30 FPS — validating C8.

## Static vs shared decision

`qar_ogre_streaming` is built as a **STATIC** library. Rationale:

1. Its primary consumer is the ament RViz plugin `qar_rviz_streaming` — itself a
   shared module. Linking a static lib into it avoids shipping and version-matching
   a second runtime DLL.
2. It also links directly into standalone Ogre samples/apps (section 11A) — again a
   link-time relationship.
3. As a skeleton it has **no stable ABI** yet; a static lib avoids annotating every
   class with `__declspec(dllexport)/dllimport` and avoids passing Ogre `SharedPtr`
   and other Ogre types across a DLL boundary (a known source of heap/ABI issues on
   Windows).
4. `POSITION_INDEPENDENT_CODE` is ON so it embeds cleanly into a shared consumer on
   non-Windows platforms.

If a future need arises to share one instance across multiple DLLs in one process,
this can be revisited — the CMake target type is the only thing that changes.

## Using the library from a non-RViz Ogre app (section 11A)

```cpp
#include <qar_ogre_streaming/ogre_qar_streaming_manager.hpp>
using namespace qar_ogre_streaming;

// 1. Your app already owns an Ogre::Root + RenderWindow + SceneManager.
Ogre::SceneManager * scene = /* your scene */;

// 2. Provide the three generic collaborators.
OgreQarStreamingConfig config;
config.scene_manager         = scene;
config.render_thread_executor = std::make_shared<MyExecutor>();   // your render thread
config.qar_sender_factory     = std::make_shared<MyQarFactory>(); // or the mock factory
config.diagnostics            = &my_sink;                          // optional

// 3. Drive it.
OgreQarStreamingManager manager(config);
for (peer : peers) {
  StereoStreamConfig s; s.peer_id = peer; s.width = 1280; s.height = 720; s.max_fps = 60;
  manager.create_stream(s);
  manager.get_stream(peer)->set_active(true);
}
manager.set_streaming_active(true);

// 4. Each render frame, on the render thread (under the host render lock if the
//    host has one). ONE batched call renders + submits ALL active peers (§12.2):
manager.render_tick();

// 5. Read results / diagnostics.
auto * stream = manager.get_stream(peer);
stream->left_target()->readColorRgba(rgba);   // OgreStereoRenderTarget accessors
const DiagnosticsSnapshot snap = manager.diagnostics_snapshot();
```

The RViz plugin uses this exact same public API — it only differs in which
`IRenderThreadExecutor` / `IQarRenderSenderFactory` / `Ogre::SceneManager` /
`IRenderLock` it supplies. A standalone sample proving this (with NO rviz/Qt/ROS)
lives in `examples/ogre_standalone_streaming_sample/` (M8); the full side-by-side
of the plugin's vs the sample's calls is in
[`api_usage_from_non_rviz_ogre_app.md`](api_usage_from_non_rviz_ogre_app.md).

## Build instructions

Ogre is discovered from the ROS 2 Humble Windows binary distro's `rviz_ogre_vendor`
package (Ogre 1.12.1). The distro path is **not hardcoded**; provide it one of three
ways (precedence high→low): `-DQAR_ROS2_INSTALL_ROOT=…`, env `ROS2_INSTALL_ROOT`, or
the detected default `D:/development/ros2_humble/ros2-windows` on the audited machine.

```powershell
# From the repo root, using the primary preset (VS 2019 generator, x64):
cmake --preset vs2019-x64
cmake --build --preset vs2019-x64
ctest --preset vs2019-x64
```

- Primary preset `vs2019-x64` uses the **Visual Studio 16 2019** generator (MSVC
  v142, matching the distro CRT) and builds **RelWithDebInfo** — reliable from any
  shell, no `vcvars` juggling.
- Alternative preset `ninja-msvc-x64` is faster but must be configured from a shell
  with the MSVC x64 environment loaded.
- Compiler flags: `/W4 /permissive-`, C++17.
- GoogleTest is fetched via `FetchContent` pinned to `v1.14.0`.
- The M1 unit tests initialize **no Ogre**: the skeleton stores the
  `Ogre::SceneManager*` for bookkeeping and never dereferences it, so tests pass a
  fake non-null pointer and exercise validation + mock paths only.

Full one-time Windows environment setup (before the above works):
the repository [`README.md`](../../README.md). How to also verify the
GL-dependent parts (offscreen render, stereo, multi-peer, the GL bridge) that
these headless unit tests don't cover: [`docs/testing.md`](../../docs/testing.md).
