# Changelog

All notable changes to this repository are listed in this file, newest version first. The format follows Keep a Changelog, and the versions follow Semantic Versioning. While the major version is 0, the minor version marks changes that may not be backward compatible, and the CMake packages of this stack accept a request only for the same minor version.

Each version lists what it adds, the compatibility it was verified with (the QarOS SDK drops, the ROS 2 distribution, Qt, the toolchain and the versions of the repositories it builds on), and its known limitations. The version of a repository is the version of its CMake package, 0.1.0 for this release.

## 0.1.0

### Added

The first version of the repository. It adds the features that the [README](README.md#features) describes:

- the module `qar_ar_core`: panel and pointer types, the input seam, the seam to QarOS for URL panels, the pointing-ray model, spatial frames and picks, a timing log and the one wrapper of the vendored QarOS C header;
- the module `qar_ogre_streaming`: an Ogre stereo stream for every peer with per-eye colour and depth render textures, a batched scheduler with a render budget, a lighting rig and two depth encodings;
- the module `qar_qar_adapter` in the five components `capi`, `render`, `panels`, `url` and `gestures`;
- geo anchoring of the app volume from a WGS84 fix or an ECEF frame;
- diagnostics: skip reasons, a per-frame trace and stage timings as CSV files;
- the headless sample `qar_ogre_standalone_streaming_sample` and the live example `qar_ogre_live_stream_example`;
- the QarOS C header of the verified SDK drop, vendored in `third_party/qar` with the MIT licence of the public QarOS repository and a README that names the origin of both.

### Compatibility

| Item | Version |
|---|---|
| QarOS SDK | verified against the drop of 2026-09-28; needs at least the drop of 2026-09-21 |
| ROS 2 distribution | Humble, release `release-humble-20260220`, used for Ogre only |
| Ogre | 1.12.1, from the package `rviz_ogre_vendor` of the distribution |
| Compiler | Microsoft C++ toolset v142 of Visual Studio 2019 |
| CMake | 3.22 or newer is required; verified with 3.29.5 as bundled with Visual Studio 2022 version 17.11; CMake 4 not tested |
| Python | 3.10.11; the layering guard needs only the Python standard library |
| GoogleTest | 1.14.0, downloaded when you configure, for the tests only |
| Other repositories | none: this repository depends on no other repository of the stack |

### Known limitations

[QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations) lists what the QarOS SDK does not document or does not do, with the evidence for each item. These limits affect what you can build on this repository:

- The live example has not been run in a session with a Visualizer; the libraries ran in the live sessions of qar_rviz_plugin, and no live session has run the code of this release.
- No call of the SDK retires a logical video stream: a receiver that is attached to a destroyed sender keeps retrying until its peer goes away.
- A cancelled gesture subscription may leave its runtime subscription until the session is destroyed.
- The SDK does not document what the runtime intersects a gesture with.
- The page of a URL panel sees a fixed viewport that the SDK does not let you configure.
- The SDK does not document the encoding of the depth values. The QarOS Visualizer expects the window-space value, so set the environment variable `QAR_DEPTH_ENCODING` to `ndc` when you stream to it; [Configuration](docs/CONFIGURATION.md) explains the setting.

[SDK compatibility](docs/SDK_COMPATIBILITY.md#supported-qaros-sdk-drops) explains how to identify the drop you have.
