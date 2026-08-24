# API gaps and design findings

This document records places where the available APIs (Ogre 1.12 via
`rviz_ogre_vendor`, the vendored Qar C API) do not directly provide something the
integration needs, together with the mitigation chosen. It is updated as each
milestone discovers a gap. The M4 and M7 findings below are still exactly the
shipped implementation; the M10 section at the end covers the real QarOS SDK v0.1.1.

> **The live round-trip works.** The RViz scene renders in the QarOS Visualizer,
> cross-process, over OpenGL → Vulkan. **Finding 16 is resolved: render-into** —
> `qar_render_sender_frame_opengl` takes an **out**-frame, so QAR owns the textures
> and the producer copies into them.
>
> ### Open limitations in SDK v0.1.1
>
> | Limitation | Impact / mitigation |
> |---|---|
> | `QarTextureSize.array_layers` reports **1** for a destination texture that is really a `GL_TEXTURE_2D_ARRAY` | A client trusting it attaches the texture wrongly; `glBlitFramebuffer` then fails with `GL_INVALID_OPERATION` and the consumer shows a blank frame. Worked around by probing the real GL target. |
> | **Depth encoding is unspecified, and the two fields that imply one are ignored.** The Visualizer needs raw window-space (NDC) z in [0,1], not the metric metres that the format + `depth_scale` + `rendered_near_far` imply; setting either field has no effect on the rendered depth | Producers must write `gl_FragCoord.z` and clear the depth target to 1.0. Setting `depth_scale` / `rendered_near_far` is pointless — do not trust them to mean anything. |
> | **`qar_session_invite_connection_string_visualizer(char*, size_t)` carries no instance discriminator.** It emits a bare `"visualizer:"` and names the *app*, not an instance, so repeat `qar_session_invite_peer` calls are idempotent: four invites resolved to one peer id and opened one window | **Worked around, not fixed in the SDK.** A distinct payload appended after the colon does yield a distinct instance (QarOS spawns one mixer process per GUID), so the adapter appends a fresh UUID per invite — see `Config::visualizer_conn_suffix`. Verified live with two independent Visualizers, each with its own stream. The contrast with the `hololens` builder, which *does* take a hostname argument, is what showed the payload slot is meaningful. **Request:** an instance-aware invite in the API itself. |
> | **No stable way to identify a peer's role.** `qar_peer_subscribe_updates` surfaces every peer — one run showed **5 distinct ids for 1 invited Visualizer** — and nothing in `QarPeerSpec` discriminates: `display_name` is auto-generated **and changes between updates for the same peer id** (`clever_darwin` → `competent_tesla` → `determined_tesla`), `app_version` and `app_custom_peer_info` are empty, `version_id` is `0.1.0` for everyone | A producer that streams to an internal peer builds the sender with a mismatched app id, and the runtime then rejects the *real* Visualizer's frames. Only ids we invited ourselves can be matched, so a Visualizer that joins on its own cannot be admitted at all. **Request:** a peer *role*/*kind* field, or documentation of which field is authoritative and stable. |
>
> **Corrected earlier claims.** Two things previously recorded here as SDK gaps were
> wrong and are retracted:
>
> * *"There is no API to place an app volume — every `qar_app_volume_*` call is a
>   getter."* **False.** The header exposes `qar_app_volumes_add_volume` plus the
>   setters `qar_app_volumes_change_app_pose`, `_change_app_scale`,
>   `_change_app_world_anchor`, `_change_pose`, `_change_size` and
>   `_change_display_name`. The adapter uses `add_volume` and
>   `change_app_world_anchor` today (see [`geolocation.md`](geolocation.md)).
> * The log lines `rejecting backend <InProcessTransfer> … not in the same process`
>   and `selected transfer type VulkanGpuTransfer` are **normal and correct** for a
>   cross-process consumer, not capability gaps.

Environment under test: ROS 2 Humble Windows binary distro, `rviz_ogre_vendor`
**Ogre 1.12.1**, running Ogre's legacy **"OpenGL Rendering Subsystem"** on an
**OpenGL 4.6** context in practice (see the M7 render-system finding below —
GL3Plus is also present in the distro but is not what `rviz2.exe` actually
selects), MSVC v142.

## Summary index

| Gap area | Section | One-line resolution |
|---|---|---|
| RViz/Ogre: metric depth is not a directly sampleable texture | [M4 — C3 depth re-check](#m4--c3-depth-re-check-can-ogre-112-gl3-expose-the-depth-buffer-as-a-sampleable-texture-directly) | Shader-written `PF_FLOAT32_R` RTT via a `"qar_linear_depth"` material-scheme listener. |
| Qar: no OpenGL producer API existed at M1/M4 | [M4 — Qar OpenGL producer API](#m4--qar-opengl-producer-api-carried-forward-from-m1) | Designed against locally-proposed structs; SDK v0.1.1 later shipped the real OpenGL path, which is what the adapter uses. |
| RViz/Ogre: extracting a native GL texture id without an unwanted link-time GL3Plus dependency | [M7 — native id extraction](#native-gl-texture-id-extraction--real-ogre-112-accessor) | `Ogre::Texture::getCustomAttribute("GLID", &id)` (OgreMain-only, virtual dispatch). |
| RViz/Ogre: which render system RViz2 actually uses | [M7 — render-system finding](#m7--opengl-texture-handoff-spike-gate-b-native-id-gl-fence-zero-copy-proof) | Legacy GL, not GL3Plus as originally assumed — harmless, both derive from `GLTextureCommon`. |
| Qar: fence ownership isn't specified anywhere | [M7 — GL fence contract](#gl-fence--ownership--lifetime-contract) | Producer owns the `GLsync`; consumer wait-only, never deletes; documented contract below. |
| Qar: which frame layout to use (C7) | [M7 — frame layout decision](#frame-layout-decision-c7) | Separated-2D shipped now; texture-array seamed as the optimal upgrade. |
| Qar: does the proposed OpenGL struct set suffice, and what should change when the real API ships | [M7 — struct sufficiency](#does-13s-proposed-struct-set-suffice--yes-with-two-clarifications) | Yes, with 2 clarifications for C9/M10 (enum value, fence wait-only semantics). |
| Layout seam for a possible future out-of-process Qar | [M7 — C2 context/thread note](#c2--confirmed-contextthread-assumption-the-thing-that-de-risks-all-of-m7) | Not built (C2 says in-process); documented as the seam to revisit if that assumption ever changes. |

---

## M4 — C3 depth re-check: can Ogre 1.12 GL3+ expose the depth buffer as a sampleable texture directly?

**Verdict: NO — not in a way that yields a *metric*, readable/sampleable depth
texture through Ogre 1.12's public GL3+ API.** We therefore implement depth as a
**shader-written single-channel F32 RTT holding metric linear eye-space distance
(metres)**, per contractor answer C3b/C3d.

This was a genuine re-check against the vendored Ogre headers (not an assumption
carried over). Evidence and the exact APIs inspected:

### What Ogre 1.12 GL3+ *does* offer

- `Ogre::PixelFormat` defines depth-texture formats: `PF_DEPTH16`
  (`= PF_DEPTH`) and `PF_DEPTH32`
  (`OGRE/OgrePixelFormat.h` lines 113-114, 271). `PixelUtil::isDepth(PixelFormat)`
  exists (line 510).
- The GL depth enums exist in the shipped loader
  (`RenderSystems/GL3Plus/GL/glcorearb.h`): `GL_DEPTH_COMPONENT16/24/32`,
  `GL_DEPTH_COMPONENT32F`, `GL_TEXTURE_DEPTH_*`.
- `TextureManager::createManual(..., PF_DEPTH16/PF_DEPTH32, TU_RENDERTARGET)`
  is accepted by the format layer.

### Why direct depth-as-metric-texture is NOT reliably available

1. **The color RTT's depth attachment is a pooled renderbuffer, not a
   sampleable texture.** When you create a color RTT
   (`createManual(..., PF_R8G8B8A8, TU_RENDERTARGET)`), Ogre auto-attaches a
   pooled `GL3PlusDepthBuffer` (a GL *renderbuffer*), not an `Ogre::Texture`.
   There is no public Ogre 1.12 API to obtain that depth attachment as an
   `Ogre::Texture` to bind/sample, nor to `blitToMemory` it:
   `HardwarePixelBuffer::blitToMemory` (`OGRE/OgreHardwarePixelBuffer.h`
   line 160) operates on the buffer it belongs to (the color attachment). No
   equivalent exists for the render target's depth attachment.
2. **A `PF_DEPTH*` render target does not carry metric values and cannot also be
   the scene's color target.** Even creating a dedicated `PF_DEPTH32` RTT yields
   non-linear *window-space* depth in `[0,1]`, not metric eye-space metres
   (contractor requires metric, C3d). Recovering metres requires the projection
   near/far plus a per-pixel inverse — i.e. a shader/CPU post-step anyway — and
   the raw sample still isn't exposed through a portable readback path.
3. **Attaching a depth *texture* (instead of a renderbuffer) to the color FBO so
   it can be sampled afterward is a render-system-internal operation**
   (`GL3PlusFrameBufferObject` / `GL3PlusDepthBuffer`); Ogre 1.12's public
   `RenderTexture` / compositor API does not expose "give me the FBO's depth as a
   sampleable `Ogre::Texture`" portably.

### Chosen implementation (C3b / C3d)

Render the scene a **second time** into a **`PF_FLOAT32_R`** (single-channel
32-bit float) color RTT through a custom Ogre **material scheme**
(`"qar_linear_depth"`). A `MaterialManager::Listener::handleSchemeNotFound`
(`OGRE/OgreMaterialManager.h` line 110) returns, for every renderable, a
technique whose GLSL (`#version 150`) fragment shader writes
`-viewSpacePosition.z` — the linear eye-space distance in **metres** (the camera
looks down its local −Z, so the negated view-space Z is positive in front of the
camera). Vertex shader binds `ACT_WORLDVIEWPROJ_MATRIX` for clip position and
`ACT_WORLDVIEW_MATRIX` to compute the eye-space position
(`OGRE/OgreGpuProgramParams.h` lines 765, 780). The value is read back with
`blitToMemory` into a `PF_FLOAT32_R` `PixelBox` — genuine metric floats.

### QAR pixel-format / channel-semantic mapping (C3c)

Inspected the vendored `third_party/qar/include/qar_streaming.h`:

| Concept | Qar enum | Value | Notes |
|---|---|---|---|
| Our metric depth RTT pixel format | `QAR_PIXEL_FORMAT_R32_FLOAT` | 2 | Single-channel F32 **color** texture (Ogre `PF_FLOAT32_R`). This is what we produce. |
| Depth **channel semantic** | `QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH` | 2 | Carried per-view on `QarVideoFrameView.data_type` — this is how a frame view is *tagged* as depth, independent of pixel format. |
| Hardware depth-buffer format | `QAR_PIXEL_FORMAT_D32_FLOAT` | 101 | A true GPU depth attachment (non-linear, not sampleable per above). **We deliberately do NOT use this** for the transmitted depth. |

**Decision:** the depth texture we create/transmit is `QAR_PIXEL_FORMAT_R32_FLOAT`
tagged with the `QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH` view semantic — a linear metric
single-channel float. `OgreStereoRenderTarget` maps any requested depth format to
Ogre `PF_FLOAT32_R` and reports `QAR_PIXEL_FORMAT_R32_FLOAT` as the produced depth
format. (`StereoStreamConfig::depth_format` still *defaults* to
`QAR_PIXEL_FORMAT_D32_FLOAT` for API symmetry with the Qar structs; the render
target overrides it to the metric-linear R32F it actually produces and logs the
substitution.)

---

## M4 — Qar OpenGL producer API (carried forward from M1)

`third_party/qar/include/qar_streaming.h` `QarGraphicsAPI` exposes only
`QAR_GRAPHICS_API_CPU (0)` and `QAR_GRAPHICS_API_D3D11 (4)` — **no
`QAR_GRAPHICS_API_OPENGL`**, and no `QarVideoFrameOpenGL` / `QarSyncFrameOpenGL`
types. Per contractor answers C1/C9 the OpenGL producer path is THE production
path and the contractor will supply an updated OpenGL-capable Qar variant later
(ties M10). Until then the proposed OpenGL structs live in our clearly-marked
our own locally-proposed OpenGL structs (since superseded by SDK v0.1.1),
and the zero-copy texture-id + fence handoff is spiked in M7 against those
proposed types. The exact Qar-side OpenGL API needs and the fence-ownership
contract will be finalized in M7's update to this file.

---

## M7 — OpenGL texture handoff spike (Gate B): native id, GL fence, zero-copy proof

**Gate B question:** *Can Ogre RenderTexture output be handed to Qar
as OpenGL textures without CPU readback?* **Decision: PASS — zero-copy is viable,
in-process, on the render/main thread, with no shim.** Proven inside live `rviz2.exe`
on this machine; evidence line + file referenced at the end.

> **Render-system finding (important):** RViz2 Humble on this Windows distro runs
> Ogre's **legacy GL render system — `"OpenGL Rendering Subsystem"`**, on an **OpenGL
> 4.6** context — **not** GL3Plus, despite the original design assuming GL3Plus. This
> turned out not to matter for the handoff: both Ogre GL render systems derive their
> texture from `Ogre::GLTextureCommon` (so `getCustomAttribute("GLID")` yields the
> native id on either), and ARB_sync fences + `glGetTexImage` depend on the GL 4.6
> context, not on which Ogre render system wraps it. The M8 standalone sample can
> create its own `Ogre::Root` with GL3Plus; the bridge is agnostic. This is why the
> extraction guard accepts any `"OpenGL"` render system rather than GL3+ specifically.

### C2 — confirmed context/thread assumption (the thing that de-risks all of M7)

Qar's encoder runs **in-process, on the main (render) thread, sharing RViz's GL
context**. This is now the *implemented and required* assumption, not a hope:

- The producer (our library) and the consumer (Qar) share ONE `HGLRC`. A GL texture
  *name* (`GLuint`) is only meaningful within its context's share group, so this is
  exactly what makes a raw `GLuint` handoff legal. **No cross-process transport, no
  `WGL_NV_DX_interop`, no `GL_EXT_external_objects` / opaque-fd is needed.**
- All extraction, validation, fencing and submission happen on the render thread
  inside `DisplayContext::lockRender()/unlockRender()`. Qar callbacks
  must only *enqueue* a render request; they must never touch GL.

*If it ever became out-of-process* (not the plan): a bare `GLuint` would be useless
across the process boundary, and we would need a shared-texture path — on Windows the
practical route is `WGL_NV_DX_interop2` (render into a GL texture backed by a shared
D3D11 texture, hand the D3D11 `HANDLE`/`IDXGIKeyedMutex` to the other process; the Qar
C API already has a D3D11 keyed-mutex struct), or GL external-memory/semaphore
(`GL_EXT_memory_object_win32` + `GL_EXT_semaphore_win32`). That would replace the
`texture_id`/`GLsync` fields with a shared handle + a cross-process semaphore. We
deliberately did **not** build any of that (C2 says in-process).

### Native GL texture id extraction — real Ogre 1.12 accessor

The task anticipated `Ogre::GL3PlusTexture::getGLID()` reached by `dynamic_cast` to the
GL3Plus texture type. That method does exist (`OgreGLTextureCommon.h`:
`uint getGLID() const { return mTextureID; }`), but reaching it by `dynamic_cast` /
directly linking `RenderSystem_GL3Plus.lib` has two costs on this toolchain:

1. `RenderSystem_GL3Plus.dll` is a runtime-loaded **plugin**; taking a link-time
   dependency on its import lib (for `GL3PlusTexture`'s RTTI/type_info) forces a
   load-time dependency (RTTI is a *data* import — not delay-loadable), which would
   break the headless unit-test executable that currently loads with **no** GL/Ogre
   DLL present (`/DELAYLOAD:OgreMain.dll`).
2. Including `OgreGL3PlusTexture.h` transitively pulls `<GL/gl3w.h>`, whose macros
   redirect every `glXxx` to gl3w's *private, per-DLL* function-pointer table (only
   populated inside `RenderSystem_GL3Plus.dll`) — a foot-gun for any GL call in the
   same TU.

**Chosen accessor:** the public, virtual `Ogre::Texture::getCustomAttribute("GLID",
&id)` (documented in `OgreTexture.h`: *"GLID — The OpenGL texture object id"*). It
dispatches through OgreMain's vtable to `GLTextureCommon::getGLID()`, so it needs
**only OgreMain** — no GL3Plus import lib, no cross-DLL RTTI, no gl3w header — and it
keeps the reusable library link-clean and the test exe DLL-free. It is guarded by a
runtime render-system-name check (`Root::getRenderSystem()->getName()` must contain
`"OpenGL"` — matches both the legacy `"OpenGL Rendering Subsystem"` RViz actually uses
and `"OpenGL 3+ Rendering Subsystem"`) so `"GLID"` is never queried on a non-GL
backend. Implemented in `OgreOpenGlTextureBridge::extract_gl_texture`
(`qar_ogre_streaming`), wired into `OgreStereoRenderTarget::colorGlTextureId()` /
`depthGlTextureId()` (which returned the Ogre resource handle as a placeholder before
M7). Validated per frame-of-interest with `glIsTexture(id)==GL_TRUE` and
`glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH/HEIGHT)` == the RTT size.

### GL entry-point loading — what `rviz_ogre_vendor` ships, and what we use

The GL3Plus render system carries its own loader:
`opt/rviz_ogre_vendor/include/OGRE/RenderSystems/GL3Plus/GL/{gl3w.h, glcorearb.h}`
(gl3w + the Khronos core-profile header). We do **not** link gl3w or the render-system
lib (see above). Instead, the single interop TU (`src/gl_interop.cpp`) declares the
minimal GL type/constant/`PFN…PROC` subset it needs (mirroring `glcorearb.h`) and
**resolves entry points at runtime** — exactly how gl3w itself loads them:

- GL 1.1 functions (`glIsTexture`, `glBindTexture`, `glGetTexLevelParameteriv`,
  `glGetTexImage`, `glGetIntegerv`, `glPixelStorei`, `glGetError`) via
  `GetProcAddress(opengl32.dll, …)`.
- GL ≥3.2 / `ARB_sync` (`glFenceSync`, `glClientWaitSync`, `glDeleteSync`) via
  `wglGetProcAddress` (needs a current context — satisfied on the render thread).

This keeps the reusable static library free of any load-time GL dependency (the test
exe never calls these and stays loadable with no GL driver interaction), and isolates
the one platform-specific seam: Linux (Gate C) swaps the loader body for
`glXGetProcAddress`/`eglGetProcAddress` and nothing above `gl_interop.hpp` changes.

### Zero-copy proof WITHOUT QarOS (the crux of Gate B)

Because the real Qar OpenGL consumer does not exist yet (C9), the handoff is proven by
an **independent in-process GL consumer** that never goes through Ogre:
`run_color_texture_roundtrip_spike()` (`ogre_gl_texture_spike.cpp`) renders the live
RViz scene into an offscreen color RTT, extracts the native id, then reads that id back
two independent ways — (a) Ogre's `HardwarePixelBuffer::blitToMemory`, and (b) a raw
`glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE)` on the extracted id — and
requires the pixels to match (≥0.999; direct or vertically-flipped to be robust to row-
origin convention). A match proves the `GLuint` genuinely refers to the rendered
content, i.e. Qar can sample it directly with no CPU copy. The spike also creates a
`glFenceSync`, `glClientWaitSync`s it (observing `GL_ALREADY_SIGNALED` /
`GL_CONDITION_SATISFIED`), deletes it, and marshals a `QarVideoFrameOpenGL`. It runs
inside `rviz2.exe` on the render thread via `QAR_PANEL_GLSPIKE`
(`scripts/run_rviz_with_plugin.ps1 -GlSpike <prefix>`).

### GL fence — ownership / lifetime contract

The producer inserts one `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)` per stream
per batch, in the scheduler's fence step (§12.2 step 7), after that stream's two eye
targets are rendered and before submit, inside the batch render lock. The resulting
`GLsync` is carried as the opaque `QarSyncFrameOpenGL::fence_sync` with
`qar_waits_on_fence = true`. **Ownership rules (in-process, single render thread,
shared context — C2):**

- **The producer owns the `GLsync`.** The consumer (Qar) must only *wait* on it
  (`glWaitSync`/`glClientWaitSync`) before sampling; it must **never** `glDeleteSync`.
- **At most one fence per stream is outstanding.** `OgreQarStereoStream::set_pending_fence`
  deletes the *previous* fence when the next one is inserted — always on the render
  thread inside the batch lock, the only safe place to release GL objects. Because
  producer and consumer share the thread, by the time frame N+1's fence is created the
  consumer has necessarily finished with frame N's.
- **Teardown:** the last per-stream fence (one small `GLsync` object) is reclaimed when
  the GL context is destroyed at process exit. The stream destructor does **not**
  `glDeleteSync` because stream destruction may occur off the render thread (RViz GUI
  thread); calling GL there would be undefined. This is a bounded, non-growing,
  one-object-per-stream release deferred to context teardown — acceptable and explicit.
  (A real integration that destroys streams under `lockRender` could delete it eagerly.)

### Frame layout decision (C7)

**Baseline (implemented): `QAR_FRAME_LAYOUT_SEPARATED_TEXTURES`** — one GL texture per
eye per view: `textures[0]=left color, [1]=right color, [2]=left depth, [3]=right depth`;
`views[]` name `(data_type, eye, texture_index)`. All targets are
**`GL_TEXTURE_2D`** — the extractor rejects non-2D types, and `QarVideoTextureOpenGL::
target` is always `GL_TEXTURE_2D` (0x0DE1). This is the simplest, most debuggable layout
and maps 1:1 onto Ogre's per-eye `RenderTexture`s.

**Optimal option for later:** a **layered / texture-array** layout
(`GL_TEXTURE_2D_ARRAY`, layer 0 = left, layer 1 = right) is the most efficient for a
single-pass stereo encoder and is the natural fit for `QarVideoFrameView::
array_layer_index` (already in the C API). The proposed `QarVideoTextureOpenGL::target`
already reserves `GL_TEXTURE_2D_ARRAY`, so adopting it later is a target-enum + Ogre
`TEX_TYPE_2D_ARRAY` RTT change behind the same struct — no API change. Side-by-side
(one wide texture, two viewports) is possible but adds viewport-packing complexity for
no benefit over an array; not recommended. **Decision: ship separated-2D now; the
array layout is the seamed upgrade if/when the shipped Qar API + encoder prefer it.**

### Does the proposed struct set suffice? — YES (with two clarifications)

The locally-proposed struct set — `QAR_GRAPHICS_API_OPENGL`,
`QarVideoTextureOpenGL {size, texture_id, target}`, `QarSyncFrameOpenGL {fence_sync,
qar_waits_on_fence}`, `QarVideoFrameOpenGL {texture_views[], textures[],
synchronization}`, and `qar_render_sender_frame_opengl(stream, frame)` — is
**sufficient** to carry everything the zero-copy handoff needs; M7 fills all of it
(`OgreOpenGlTextureBridge::fill_opengl_frame`). Precise notes for when Qar ships the
real API (C9 / M10):

1. **Reconcile `QAR_GRAPHICS_API_OPENGL`'s value.** We use `5` as a placeholder
   (real enum is CPU=0, D3D11=4); adopt whatever value Qar publishes.
2. **`fence_sync` is a `GLsync` as `void*`** and **the consumer must treat it as wait-
   only (never delete)** — this ownership rule should be stated in the real API's
   docs. `texture_id` is a `GLuint` in a `uint32_t`; `target` is a GL enum
   (`GL_TEXTURE_2D` today). Confirm Qar samples on the **same shared context** (C2).
   No other fields are required for the in-process path. (Depth is transmitted as a
   normal single-channel `R32F` color texture tagged `QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH`
   per the M4 C3 decision — no special depth-texture API needed.)

### Evidence

- Unit (headless, no GL): `qar_ogre_streaming/test/opengl_bridge_tests.cpp` — the
  `QarVideoFrameOpenGL` marshalling, null-arg validation, interop value helpers
  (part of the green `ctest --preset vs2019-x64`).
- Live in-RViz spike log line + stats file: [`evidence/gl_texture_spike.txt`](evidence/gl_texture_spike.txt) (produced
  by `run_rviz_with_plugin.ps1 -GlSpike`), measured on this machine:
  `ran=1 rs="OpenGL Rendering Subsystem" texture_id=4 glIsTexture=1 dims=1280x720
  expected=1280x720 dims_match=1 roundtrip_match=1 match_fraction=1.0000 flipped=0
  fence_created=1 fence=GL_CONDITION_SATISFIED fence_signaled=1 opengl_frame=1`
  — i.e. 921600/921600 pixels of the independent raw-`glGetTexImage` readback matched
  Ogre's `blitToMemory` readback of the same extracted native id: **zero-copy proven**.

---

## M10 — Real QarOS SDK v0.1.1 wiring (compile+link resolved; two items pending live)

The delivered SDK v0.1.1 (`QarOS_binary/install`, vendored header
`third_party/qar/include/qar_streaming.h`) replaced the onboarding model with a
create/join + invite-peer session flow and added the real OpenGL producer path.
The producer consumes the SDK through `QAR_ENABLE_DYNAMIC_LOADING`: no import lib
is linked, and every `qar_impl_*` symbol is resolved from `qar-streaming-c.dll` at
run time. Findings 11 and 16 below were both settled against a running Visualizer.

### Finding 11 — host session activation — **RESOLVED (empirically, probe stage=session)**

The host path from `qar_runtime_create_session` (which returns a
`QarSessionInvite*`, not a session) to an **active `QarSession*`** is:

1. `qar_library_init`
2. `qar_runtime_create` — **set `QarRuntimeInit.runtime_binaries_folder_path` to
   `<repo>/QarOS_binary/install/bin`** so the runtime can spawn app binaries (the
   Visualizer) in response to `invite_peer`. Leaving it NULL makes the runtime warn
   *"Runtime folder path not provided. Using cwd."* and it cannot spawn the app.
3. `qar_runtime_create_session` → `QarSessionInvite*` (invite JSON embeds MasterURL
   {LAN IP, port 19100, tcp}, session UUID, token).
4. `qar_session_invite_serialize` → the invite blob.
5. `qar_session_join(serialized own invite)` → **ACTIVE `QarSession*`** — i.e. the
   host activates its own session by joining the invite it just minted.
6. `qar_peer_subscribe_updates(session, …)` — peers surface here (replaces the
   removed render-request subscription).
7. **`qar_session_invite_peer`** with the connection string from
   `qar_session_invite_connection_string_visualizer` — invites the QarOS Visualizer.

Implemented in `qar_qar_adapter` (`QarCApiSenderFactory` ctor). Step 2's folder path
is fed from `Config.runtime_binaries_folder_path` (the RViz panel reads env
`QAR_RUNTIME_BINARIES_DIR`; the run script sets it to the SDK `bin`). Step 7 uses
`qar_session_invite_peer` with a connection string from
`qar_session_invite_connection_string_visualizer`.

**RESOLVED live (2026-07-23):** `qar_session_invite_peer` makes the in-process runtime
**auto-spawn `qar-streaming-viz.exe`** — the Visualizer window appears on its own within
a couple of seconds of connecting, and re-spawns if closed. No separate
`qar-runtime-launcher.exe` and no manual launch are needed to make the window appear.
**However**, that in-process session is isolated and *not* the correct architecture —
see the M10-LIVE section below.

### Finding 16 — OpenGL frame ownership (render-into vs supply-own) — **RESOLVED LIVE: render-into**

**RESOLVED (2026-07-23):** with the real-SDK default `kRenderIntoQarTextures`, the live
RViz scene (grid + coloured cubes) **rendered correctly into the real QarOS Visualizer**
on the runs where the peer/consumer id happened to align — so **render-into is correct**
and the OpenGL producer path (`frame_opengl` + `show_frame`, R8G8B8A8 / R32F,
`SEPARATED_TEXTURES`, `graphics_api = QAR_GRAPHICS_API_OPENGL = 7`) is wired correctly
end to end. (The remaining unreliability is the session/peer-id blocker below, not the
GL ownership mode.) Original analysis retained for context:


`qar_render_sender_frame_opengl(stream, QarVideoFrameOpenGl* out_frame)` is named
`out_frame` / documented "get frame write access", which suggests QAR **hands the
producer texture ids to render/copy into** (render-into) — the OPPOSITE of the M4–M8
zero-copy design, which **supplies our own** extracted Ogre GL ids (supply-own, also
what the mock stub CPU-reads back).

We cannot decide this without a connected peer (must not run one here), so the
adapter carries **both**, selected by `GlSubmitMode`:
- Real-SDK build defaults to **`kRenderIntoQarTextures`** (the Finding-16 working
  assumption): call `frame_opengl` to obtain QAR-owned destination ids, then an
  optional `IQarGlTextureCopyHook` copies the Ogre color/depth **into** them, then
  `show_frame`. Marked `// TODO(M10-live)` in `submit_opengl_stereo_frame`.
- Mock build defaults to **`kSupplyOwnTextureIds`** (preserves the M9.5 round-trip).

Flipping to the alternative is a **one-line switch** (the enum on the stream's Config)
— no other change. Layout is `SEPARATED_TEXTURES` first with `SIDE_BY_SIDE` as the
documented fallback; `graphics_api = QAR_GRAPHICS_API_OPENGL` (7).

### M10 LIVE session — three deployment issues, all resolved

Getting the live GUI round-trip to run reliably needed three fixes. They are
recorded here because none of them is documented on the SDK side, and anyone
integrating QarOS on Windows will hit them:

1. **VC++ runtime crash (fixed).** `rviz2.exe` bundles VC++ 14.29 next to it;
   `qar-streaming-c.dll` needs ≥14.40 → access violation in `qar_library_init`
   (`MSVCP140.dll`). Fixed by upgrading the ROS distro's VC++ runtime DLLs to 14.4x
   (forward-compatible). **Deployment requirement, not in the SDK docs.**
2. **Load-time freeze (fixed).** The panel built the QAR session synchronously in
   `onInitialize()` (before the Qt event loop ran) → RViz froze. Now the connection is
   **button-gated** ("Connect to QarOS") and `begin_frame` is polled via
   `qar_render_sender_try_begin_frame` (non-blocking) instead of blocking on the GUI
   thread.
3. **The blocker — RESOLVED by the vendor call of 2026-07-29.** The live run died in
   QAR's transfer-backend selection: the out-of-process Visualizer made
   `InProcessTransfer` inapplicable, no cross-process backend accepted an *OpenGL
   sender* (*"backend feature not supported for OpenGL sender transform"*), the runtime
   fell back to `VulkanGpuTransfer` and both processes crashed. We initially read this
   as "QarOS has no cross-process OpenGL path" — **that was wrong.**

   The actual cause: we never passed QAR our **GL context**. `QarRenderSenderInit`
   accepts a **`QarStreamParamsOpenGl` extension** chained on `init.header.next`
   carrying `context_type = WGL`, `display_or_device_context = wglGetCurrentDC()` and
   `context = wglGetCurrentContext()`. With it, QAR samples our textures *in-process on
   our context* and performs the cross-process transfer itself. Implemented in
   `QarCApiSenderFactory::create_sender()` (render thread, where the context is
   current; `wgl*` resolved dynamically so the adapter keeps no GL link dependency).

   Related corrections from the same call, all implemented:
   the SDK must be consumed via **`QAR_ENABLE_DYNAMIC_LOADING`** (no import lib; the
   DLL search then skips the host app dir, which is the *intended* cure for the VC++
   runtime crash); the session GUID must be **unique and non-zero**
   (`qar_session_unique()`, not the all-zero default we had briefly tried); the QAR
   library **starts `qar-runtime-launcher.exe` itself** (we must not); the launcher
   config `{"enableMixer": false}` gives a direct RViz→Visualizer path for debugging;
   and library logging should run at **TRACE** into `%TEMP%\quaternar`.

## See also

- [`architecture.md`](architecture.md) — where these seams (`IQarRenderSenderAdapter`,
  `IGlFenceHook`) sit relative to the rest of the system.
- [`geolocation.md`](geolocation.md) — the app-volume anchoring API in use.
- [`../qar_ogre_streaming/docs/ogre_library_architecture.md`](../qar_ogre_streaming/docs/ogre_library_architecture.md) —
  the library's public API surface these gaps and mitigations are implemented behind.
- [`../README.md`](../README.md) — how to build and test this repository.
