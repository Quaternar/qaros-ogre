# Changelog

What changes for users of qar_ogre_adapter: the CMake package, its components and headers, the
examples and what they show. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
with a **Breaking** section first, and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):
a minor version adds features (anything that breaks a user is listed under Breaking with its
migration), a patch version only fixes. The version is the file [VERSION](VERSION); how entries are
written and how a version is released is in [AGENTS.md](AGENTS.md).

## [Unreleased]

### Breaking

- The session owner joins only the QAROS Hub of its own PC, as a local app (`QarOnboardLocalAppExt`): to reach the Hub of another PC, make the Hub of this PC a member of it in QAROS; a stored pairing below `%LOCALAPPDATA%\Quaternar\QarOS-ROS\<program>` is no longer read ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- Removed from `qar_session_owner.hpp`: `HubEndpoint`, `submit_code()`, `forget()`, `configure()`, `effective_mode()`, `has_remote_identity()`, the states `kRejoining`, `kNeedsCode`, `kPairing`, the event `kForgetDone` and the fields `SessionOwnerConfig::mode`, `remote`, `instance`, `identity_dir`; drop the calls, the owner joins the local Hub ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- Removed `HubMode`, `resolve_hub_mode` (`target_rule.hpp`), the contexts `kRemote`, `kRejoin`, `kForget` of `OnboardingContext`, `identity_store.hpp` with `default_identity_dir`, and the environment variable `QAR_PANEL_IDENTITY_DIR` ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- `describe_status` takes `(code, context, detail)` without the Hub address, and the status texts no longer mention another PC's Hub or a stored pairing ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- The QarOS C and C++ headers come from the public qaros repository, the git submodule `external/qaros`: clone with `--recurse-submodules`; the cache variable `QAROS_SOURCE_DIR` replaces `QAR_VENDOR_INCLUDE_DIR` ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- `SessionOwnerConfig::app_version`, the program version the Hub is told, defaults to the qar_ogre_adapter version instead of `0.1.0`; set it to keep a fixed value ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).

### Features

- The version of a build comes from the file `VERSION`, the last release, and git tags (`X.Y.Z` on a release tag, the next patch `X.Y.(Z+1)-dev.N+<sha>` on any other commit); `qar_ar_core/version.hpp` reports it as `QAR_OGRE_ADAPTER_VERSION` and `qar_ar_core::kLibraryVersion`, and the live example prints it with `--version` ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- A frame worker, a thread of its own for each sender, makes every QarOS call of the frame path, so that the thread of the host never waits for QarOS; the switch `QAR_FRAME_CALLS` selects `worker`, the default, or `gui`.
- The copy into the textures of the runtime ends with a GL fence and a flush that the frame worker waits for on a context of its own; `QAR_FRAME_FENCE` selects `worker` or `gui`.
- A GPU progress gate, a watch thread that follows a fence of every frame, closes a series of frames while the GPU makes no progress, so that the scheduler skips the batch (`QAR_GPU_GATE`, `QAR_GPU_GATE_TEST`).
- The session owner has the state `kLost` and rejoins by itself after the Hub stopped answering, with a supervisor thread that makes no QarOS call, the calls `release_lost_session`, `rejoin_now` and `declare_lost`, and the reports `health` and `rejoin_info`.
- A release lane lets a sender, a panel adapter or an input source release its QarOS handles on the thread that owns the session (`QAR_ADAPTER_STREAM_RELEASE`).
- The session owner exits within a bound, and an owner thread that is left behind when a QarOS call does not return is replaced by a fresh owner.
- `IQarUrlPanelAdapter` has the call `closed_in_ar()`, true while the last report of the runtime for the panel says closed, so that a host can show that the operator closed a panel in AR.
- The 3D senders are released when the Hub process ends and made again when the new Hub runs, with `QarSessionOwner::hub_process()` and lines of the factory about the Hub and about each render request.
- Screen-space objects, such as the selection box of RViz, are left out of the colour and depth images of the AR eyes, with one log line for each stream.
- A visible URL panel is hidden before its page is changed (`hide_before_swap` of the attach decision), and `log_runtime_record()` of the URL panel adapter logs the state, pose and size of a panel without the URL.
- A breaker pauses the frame calls of a sender after two slow calls, a sender is made again when the runtime asks it for no frames (`QAR_RENDER_RECYCLE_S`), and a creation that gets no answer is cancelled and tried again.

### Enhancements

- The package installs the QarOS C++ headers `qar_streaming_cpp` next to `qar_streaming.h`, and the qaros licence as `share/qar_ogre_adapter/LICENSE.qaros` (was `LICENSE.qar_streaming_header`) ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- The components of `qar_qar_adapter` are compiled with `/we4996`, so a call of a QarOS C API function the header marks deprecated fails the build ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- The line of a failed `wglMakeCurrent` names the thread and the thread that bound the context last.
- The diagnostic copy probe is made on the frame worker path only when `QAR_ADAPTER_PROBE_EVERY` is set.
- No thread of the host waits for QarOS when the Hub or the Visualizer is gone: the calls that can wait run on the thread of the owner or on a frame worker, and `QAR_FRAME_CALLS=gui` keeps the earlier behaviour for comparison.

### Bug fixes

- A 3D sender reads the stream state of the runtime (`qar_render_sender_get_stream_state`) when a begin-frame call finds no frame ready, so that a reconnect, a pause and a closed stream are told apart on QAROS 0.10.0: a state reconnecting for more than 90 seconds makes the sender again, a closed stream is replaced, and a change of the state is logged.

### Removed

- `third_party/qar` (the copy of `qar_streaming.h`) and `scripts/update_qar_headers.ps1` ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).
- The options `--hub HOST:PORT`, `--code CODE` and `--forget` of the live example; it takes `[--local] [--connect-only] [--seconds N]` ([#5](https://github.com/Quaternar/qaros-ogre/pull/5)).

## [0.2.0] - 2026-11-13

Verified with QAROS 0.9.0-dev.3008+65dc9aeb, ROS 2 Humble (Ogre 1.12.1 of `rviz_ogre_vendor`), MSVC v142 and CMake 3.22 or newer; not tagged. Known limitations: [QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations).

### Breaking

- The component `panels` (`qar_ogre_adapter::panels`) is removed with its GUI panel sender, panel input source, panel types and panel sender seam: the panel glue is part of qaros-ros-rviz ([#4](https://github.com/Quaternar/qaros-ogre/pull/4)).
- The input interface of the pointer samples is `IPanelInputSource` in `pointer_input.hpp`; `panel_input_source.hpp` keeps only the mapping from the QarOS enumerations and gesture fields ([#4](https://github.com/Quaternar/qaros-ogre/pull/4)).

### Features

- `qar_ar_core` headers `pointer_input.hpp`, `panel_op_result.hpp` and `panel_state_guard.hpp`: the AR pointer input vocabulary with `IPanelInputSource` and `QueuedPanelInputSource`, the outcome of a queued panel call, and the guard that keeps a panel in the requested state ([#4](https://github.com/Quaternar/qaros-ogre/pull/4)).
- `PanelStateGuard` and the attach decision of `panel_attach.hpp` are plain values and functions usable without QarOS ([#4](https://github.com/Quaternar/qaros-ogre/pull/4)).

## [0.1.0] - 2026-10-26

The first version. Verified with QAROS 0.9.0-dev.3008+65dc9aeb, ROS 2 Humble (Ogre 1.12.1), MSVC v142 and CMake 3.22 or newer; not tagged.

### Features

- Module `qar_ar_core`: pointer types, the input seam, the URL panel seam, the pointing-ray model, spatial frames and picks, a timing log and the one wrapper of the QarOS C header ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- Module `qar_ogre_streaming`: an Ogre stereo stream per peer with per-eye colour and depth textures, a batched scheduler with a render budget, a lighting rig and two depth encodings ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- Module `qar_qar_adapter` with the components `capi`, `session`, `render`, `url` and `gestures` ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- Session owner that loads the installed QAROS, joins its Hub on its own thread, keeps the render targets and runs QAROS calls off the GUI thread ([#3](https://github.com/Quaternar/qaros-ogre/pull/3)).
- Stable common names for QarOS panels and the app volume, so the next start finds them again ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- URL panel adapters with queued calls reported through `drain_results`, hiding and showing without closing, and the list of peers that see a panel ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- Geo anchoring of the app volume from a WGS84 fix or an ECEF frame ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- Diagnostics: skip reasons, a per-frame trace, stage timings as CSV and timestamped panel and stream state lines ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
- The headless sample `qar_ogre_standalone_streaming_sample` and the live example `qar_ogre_live_stream_example` ([#2](https://github.com/Quaternar/qaros-ogre/pull/2)).
