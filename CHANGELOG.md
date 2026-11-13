# Changelog

All notable changes to this repository are listed in this file, newest version first. The format follows Keep a Changelog, and the versions follow Semantic Versioning. While the major version is 0, the minor version marks changes that may not be backward compatible, and the CMake packages of this stack accept a request only for the same minor version.

Each version lists what it adds, the compatibility it was verified with (the QAROS version, the ROS 2 distribution, Qt, the toolchain and the versions of the repositories it builds on), and its known limitations. The version of a repository is the version of its CMake package, 0.2.0 for this release.

## Unreleased

### Changed

- The QarOS headers come from the public qaros repository, a git submodule in `external/qaros`, instead of a copy of the C header in `third_party/qar`: the C header (`qar-streaming-c`) and the C++ headers (`qar-streaming-cpp`) from one qaros commit, through the CMake targets of qaros. Clone with `--recurse-submodules`. The cache variable `QAROS_SOURCE_DIR` replaces `QAR_VENDOR_INCLUDE_DIR`. The qaros header targets need no C language, and the examples of qaros stay off (`QAROS_BUILD_EXAMPLES`).
- The package installs the C++ headers `qar_streaming_cpp` next to `qar_streaming.h`, and the licence of qaros as `share/qar_ogre_adapter/LICENSE.qaros` instead of `LICENSE.qar_streaming_header`.
- **Behaviour change: the session owner joins only the QAROS Hub of its own PC.** `QarSessionOwner` joins as a local source app (`QarOnboardLocalAppExt`): the Hub asks once to allow the program, no code is needed, and the program stores nothing. The QAROS header marks the code pairing with the Hub of another PC (`QarOnboardCodeExt`, `QarOnboardHostExt`), the rejoin with a stored onboarding id (`qar_runtime_rejoin`, `qar_runtime_rejoin_async`) and `qar_runtime_forget` deprecated and names no replacement inside an app.
- **Migration for "another PC":** make the Hub of the program's PC a member of the Hub of the other PC in QAROS, and let the program join the Hub of its own PC as before. A stored pairing (`identity.txt` below `%LOCALAPPDATA%\Quaternar\QarOS-ROS\<program>`) is no longer read; delete the folder if you like, and revoke the old device in the Visualizer of the other Hub.
- The components of `qar_qar_adapter` and its tests are compiled with `/we4996`: a call of a QarOS C API function that the header marks deprecated fails the build.
- The status texts for the Hub of another PC are gone. `describe_status` takes `(code, context, detail)` without the Hub address, and the texts of the storage codes no longer speak of a stored pairing.
- The live example takes `[--local] [--connect-only] [--seconds N]`; `--local` is the only mode and is still accepted.

### Removed

- `third_party/qar` (the copy of `qar_streaming.h`, its licence and README) and `scripts/update_qar_headers.ps1`.
- From the session owner (`qar_session_owner.hpp`): `HubEndpoint`, `submit_code()`, `forget()`, `configure()`, `effective_mode()`, `has_remote_identity()`, the states `kRejoining`, `kNeedsCode` and `kPairing`, the event `kForgetDone`, and the fields `SessionOwnerConfig::mode`, `remote`, `instance` and `identity_dir`. The runtime no longer gets a storage folder of the program.
- `HubMode` and `resolve_hub_mode` (`target_rule.hpp`), and the contexts `kRemote`, `kRejoin` and `kForget` of `OnboardingContext`.
- `identity_store.hpp` and `identity_store.cpp` (the stored pairing, `default_identity_dir`), and with them the environment variable `QAR_PANEL_IDENTITY_DIR`.
- The options `--hub HOST:PORT`, `--code CODE` and `--forget` of the live example.

## 0.2.0

### Added

- the headers `pointer_input.hpp`, `panel_op_result.hpp` and `panel_state_guard.hpp` in `qar_ar_core`: the vocabulary of AR pointer input with its interface `IPanelInputSource` and the queue `QueuedPanelInputSource`, the outcome of a queued call on a panel, and the decision that keeps a panel in the state that was asked for;
- the state guard `PanelStateGuard` and the attach decision of `panel_attach.hpp` as plain values and functions that a library which streams a panel can use without QarOS;
- the checks of the layering guard for the new headers and for the examples.

### Changed

- the input interface of the pointer samples is `IPanelInputSource` and its header is `pointer_input.hpp`; `panel_input_source.hpp` keeps the mapping from the QarOS enumerations and gesture fields;
- the spatial gesture source, the URL panel adapter, the session owner and the render sender work as before.

### Removed

- the component `panels` (`qar_ogre_adapter::panels`), with the sender of a GUI panel that streams pixels and the input source of a panel, and the panel types and the panel sender seam of `qar_ar_core`: the panel glue is part of the repository `qar_rviz_plugin`.

### Compatibility

| Item | Version |
|---|---|
| QAROS | verified against the installed QAROS 0.9.0-dev.3008+65dc9aeb |
| ROS 2 distribution | Humble, release `release-humble-20260220`, used for Ogre only |
| Ogre | 1.12.1, from the package `rviz_ogre_vendor` of the distribution |
| Compiler | Microsoft C++ toolset v142 of Visual Studio 2019 |
| CMake | 3.22 or newer is required; verified with 3.29.5 as bundled with Visual Studio 2022 version 17.11; CMake 4 not tested |
| Python | 3.10.11; the layering guard needs only the Python standard library |
| GoogleTest | 1.14.0, downloaded when you configure, for the tests only |
| Other repositories | none: this repository depends on no other repository of the stack |

### Known limitations

[QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations) lists what the QarOS SDK does not document or does not do, with the evidence for each item. These limits affect what you can build on this repository:

- The live example has not been run in a session with a Visualizer; the libraries ran in the live checks of qar_rviz_plugin on the desktop Visualizer with the installed QAROS, and no session with a headset has run this release.
- A cancelled gesture subscription may leave its runtime subscription until the session is destroyed.
- The SDK does not document what the runtime intersects a gesture with.
- The SDK does not document a way to set the viewport of a URL panel; the page sees a viewport that follows the size of the panel.

## 0.1.0

### Added

The first version of the repository. It adds the features that the [README](README.md#features) describes:

- the module `qar_ar_core`: the pointer types, the input seam, the seam to QarOS for URL panels, the pointing-ray model, spatial frames and picks, a timing log and the one wrapper of the vendored QarOS C header;
- the module `qar_ogre_streaming`: an Ogre stereo stream for every peer with per-eye colour and depth render textures, a batched scheduler with a render budget, a lighting rig and two depth encodings;
- the module `qar_qar_adapter` in the five components `capi`, `session`, `render`, `url` and `gestures`;
- the session owner, which loads the installed QAROS and joins its Hub on a thread of its own, either as a local app on the PC of the Hub (the Hub asks once to allow the program) or with a pairing code on another PC, keeps the table of render targets and the stored pairing, and runs the QAROS calls of its consumer in order, so that no call that waits for the Hub runs on a GUI thread;
- stable common names for the QarOS panels and the app volume, so that the next start finds them again, and the decision about a panel that is found;
- URL panel adapters whose calls queue their work and report through `drain_results`, and a guard that asks a panel for its state again when QarOS reports the opposite right after the program set it;
- geo anchoring of the app volume from a WGS84 fix or an ECEF frame;
- diagnostics: skip reasons, a per-frame trace and stage timings as CSV files, the state of every URL panel and the start of every stream as lines with the time of day;
- hiding and showing of a URL panel without closing it, and the list of the peers that see a URL panel;
- the headless sample `qar_ogre_standalone_streaming_sample` and the live example `qar_ogre_live_stream_example`;
- the QarOS C header of the verified QAROS version, vendored in `third_party/qar` with the MIT licence of the public QarOS repository and a README that names the origin of both.

### Compatibility

| Item | Version |
|---|---|
| QAROS | verified against the installed QAROS 0.9.0-dev.3008+65dc9aeb |
| ROS 2 distribution | Humble, release `release-humble-20260220`, used for Ogre only |
| Ogre | 1.12.1, from the package `rviz_ogre_vendor` of the distribution |
| Compiler | Microsoft C++ toolset v142 of Visual Studio 2019 |
| CMake | 3.22 or newer is required; verified with 3.29.5 as bundled with Visual Studio 2022 version 17.11; CMake 4 not tested |
| Python | 3.10.11; the layering guard needs only the Python standard library |
| GoogleTest | 1.14.0, downloaded when you configure, for the tests only |
| Other repositories | none: this repository depends on no other repository of the stack |

### Known limitations

[QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations) lists what the QarOS SDK does not document or does not do, with the evidence for each item. These limits affect what you can build on this repository:

- The live example has not been run in a session with a Visualizer; the libraries ran in the live checks of qar_rviz_plugin on the desktop Visualizer with the installed QAROS, and no session with a headset has run this release.
- No call of the SDK retires a logical video stream: a receiver that is attached to a destroyed sender keeps retrying until its peer goes away.
- A cancelled gesture subscription may leave its runtime subscription until the session is destroyed.
- The SDK does not document what the runtime intersects a gesture with.
- The page of a URL panel sees a fixed viewport that the SDK does not let you configure.
- The SDK does not document the encoding of the depth values. With a QAROS runtime earlier than the verified version, the QarOS Visualizer expected the window-space value, so set the environment variable `QAR_DEPTH_ENCODING` to `ndc` when you stream to it; [Configuration](docs/CONFIGURATION.md) explains the setting.

[SDK compatibility](docs/SDK_COMPATIBILITY.md#supported-qaros-versions) explains how to identify the QAROS version you have.
