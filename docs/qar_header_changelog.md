# Vendored `qar_streaming.h` changelog — old public header → SDK v0.1.1

This records the meaningful C API deltas between the previously vendored public
`qar_streaming.h` (GitHub `Quaternar/qaros` @ `37eb9841…`, "no OpenGL, CPU+D3D11
only") and the header shipped in the **QarOS SDK drop v0.1.1**
(`QarOS_binary/install/include/qar_streaming.h`, now vendored at
`third_party/qar/include/qar_streaming.h`). Provenance/version:
`QarOS_binary/install/share/qar-streaming-c/qar-streaming-cConfigVersion.cmake` → `0.1.1`.
Header self-description: *"Auto-generated single header for the qar-streaming C V0 API."*

This file is just the raw old→new API delta the repository had to reconcile.
Behavioural gaps and mismatches are recorded in [`api_gaps.md`](api_gaps.md).

## Enums

| Enum | Old | New (v0.1.1) |
|---|---|---|
| `QarGraphicsAPI` | `CPU=0, D3D11=4` | `CPU=0, D3D11=4, VULKAN=6, **OPENGL=7**` |
| `QarPixelFormat` | R32_FLOAT=2, R8G8B8A8=51, B8G8R8A8=53, R16G16B16A16=55, D32_FLOAT=101 | unchanged |
| `QarFrameLayout` | SIDE_BY_SIDE=0, SEPARATED_TEXTURES=1, LAYERED=2 | unchanged |

> Our retired locally-proposed OpenGL header had guessed `QAR_GRAPHICS_API_OPENGL = 5`
> — the real value is **7**. `QAR_MAX_FRAME_VIEWS=8`, `QAR_MAX_FRAME_TEXTURES=4` unchanged.

## New graphics types (real OpenGL + Vulkan producer paths)

Gated by `QAR_ENABLE_OPENGL` (auto-defined only on **Linux**; on **Windows you must
`#define QAR_ENABLE_OPENGL` before including** — D3D11 is the platform auto-default).
It pulls in no GL headers on Windows.

```c
typedef struct QarVideoTextureOpenGl { QarTextureSize size; uint32_t texture; } QarVideoTextureOpenGl;
typedef struct QarVideoFrameOpenGl {
    QarVideoFrameView    texture_views[QAR_MAX_FRAME_VIEWS];  size_t texture_views_count;
    QarVideoTextureOpenGl textures[QAR_MAX_FRAME_TEXTURES];   size_t textures_count;
} QarVideoFrameOpenGl;                       /* NOTE: no synchronization field */
typedef struct QarStreamParamsOpenGl { ... WGL/EGL/GLX context + callbacks ... } QarStreamParamsOpenGl;
typedef enum   QarOpenGlContextType { EGL=0, WGL=1, GLX=2 };
typedef struct QarOpenGlContextThreadState { ... };
```

Notes vs our retired proposed structs:
- casing is **`OpenGl`** (not `OpenGL`).
- the texture struct has **no `target`** field (proposed had one).
- the frame struct has **no `synchronization`/fence** field at all (proposed had
  `QarSyncFrameOpenGL`). D3D11 (`QarSyncFrameD3D11`, keyed-mutex) and Vulkan
  (`QarSyncFrameVulkan`, semaphore+fence) *do* carry sync; **OpenGL carries none** —
  the GL fence handoff mechanism is unspecified (see [`api_gaps.md`](api_gaps.md)).

Vulkan analogues also added (`QarVideoTextureVulkan`, `QarVideoFrameVulkan`,
`QarSyncFrameVulkan`, `QarStreamParamsVulkan`) — not used here.

## Functions ADDED

- **Session lifecycle (replaces onboarding):** `qar_runtime_create_session`,
  `qar_runtime_join_session`, `qar_session_join`, `qar_session_destroy`,
  `qar_session_get_invite_handle`.
- **Session invite mgmt:** `qar_session_invite_serialize` / `_deserialize` /
  `_handle_is_valid` / `_handle_destroy` / `_get_session_id` / `_to_debug_string`,
  and connection-string builders `qar_session_invite_connection_string_visualizer`
  / `_hololens` / `_quest` / `_zed`.
- **Peer invitation:** `qar_session_invite_peer`, `qar_session_invite_peer_async`.
- **Render sender:** `qar_render_sender_frame_opengl`, `qar_render_sender_frame_vulkan`,
  `qar_render_sender_try_begin_frame` (non-blocking begin-frame poll).
- **`*_default()` initializers:** `qar_runtime_join_init_default`,
  `qar_session_create_init_default`, `qar_session_join_init_default`,
  `qar_peer_spec_init_default`, `qar_session_invite_peer_init_default`,
  `qar_stream_params_opengl_default`, `qar_video_frame_opengl_default`, + Vulkan variants.
- **App/GUI:** `qar_app_volumes_add_volume`, `qar_gui_panels_add_panel`.

## Functions REMOVED

- **Onboarding:** `qar_runtime_onboard`(+`_async`), `qar_runtime_rejoin`(+`_async`),
  `qar_session_request_onboarding_invite`(+`_async`), `qar_runtime_forget`,
  `qar_onboarding_invite_serialize`/`_deserialize`/`_serialized_size`/`_get_method`/
  `_get_expires_unix`, `qar_onboarding_id_default`/`_equals`/`_is_zero`,
  `qar_peer_presentation_default`, `qar_onboard_init_default`,
  `qar_onboard_code_ext_default`/`_host_ext_default`/`_invite_ext_default`,
  `qar_rejoin_init_default`, `qar_forget_init_default`, `qar_request_invite_init_default`.
- **Render-stream requests (peer-driven):** `qar_render_sender_subscribe_requests`,
  `qar_render_request_get_target_peer_id`, `qar_render_request_get_stream_id`,
  `qar_render_sender_change_layout_async`.
- **App/GUI:** `qar_app_volumes_get_or_create`, `qar_gui_panels_get_or_create`.

## Types REMOVED

`QarOnboardInit`, `QarOnboardCodeExt`, `QarOnboardHostExt`, `QarOnboardInviteExt`,
`QarOnboardingId`, `QarOnboardingMethod`, `QarRejoinInit`, `QarForgetInit`,
`QarRequestInviteInit`, `QarPeerPresentation`, `QarActionSeverity`, `QarVector3d`
(replaced by `QarGeoVec3d`). `QarRenderStreamRequest` no longer exists.

## Types ADDED

`QarSessionCreateInit`, `QarRuntimeJoinInit`, `QarSessionJoinInit`,
`QarSessionInvitePeerInit`, `QarPeerSpecInit`, `QarGeoVec3d`, the OpenGL/Vulkan
graphics structs above.

## Consequences for this repo (reconciled in the M10-header pass)

- `QAR_GRAPHICS_API_OPENGL` used directly (=7), no cast; retired the placeholder macro.
- The producer's session/peer flow moved from `runtime_onboard` +
  `render_sender_subscribe_requests` to `runtime_create_session` → invite serialize →
  `session_join` → `qar_peer_subscribe_updates` (see `qar_qar_adapter/`).
- OpenGL frame marshalling now targets the real `QarVideoFrameOpenGl` (no `target`,
  no sync); the library keeps `target`/fence in its own `GlVideoTexture`/`GlFrameSync`
  (see `qar_ogre_streaming/types.hpp`).

## Dispatch model (unchanged shape, relevant for linking)

By default (no `QAR_ENABLE_DYNAMIC_LOADING`) each `qar_<name>()` is a `static inline`
wrapper calling `qar_impl_<name>()`, declared `__declspec(dllimport)` — you **link** the
import lib (`qar-streaming-c.lib`) which exports the
`qar_impl_<name>` symbols. Defining `QAR_ENABLE_DYNAMIC_LOADING` instead grows load
helpers that `LoadLibraryExA("qar-streaming-c.dll")` (`dlopen` on Linux) + `GetProcAddress`
the `qar_impl_<name>` symbols into a per-module table. This repo uses the default link path.
