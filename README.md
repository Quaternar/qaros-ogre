# qar_ogre_adapter - the QarOS integration layer

qar_ogre_adapter 0.1.0 is the QarOS integration layer of a stack that streams Ogre scenes, Qt widgets, web pages and RViz into QarOS. It renders an Ogre scene for each eye into OpenGL textures and streams them to QarOS, it holds the one adapter for the QarOS C API (the session owner that joins the QAROS Hub, the targets, panels, URL panels and gestures) that the other repositories of the stack build on, and it carries the C header of QAROS that all of them compile against. It is a set of C++17 static libraries for Windows 11 and MSVC, delivered as the CMake package `qar_ogre_adapter`, with a headless sample and a live example that you can run.

## TL;DR

You need Windows 11, PowerShell 7, the toolchain, the ROS 2 distribution that provides Ogre (on Windows, the Humble binary distribution: [ROS 2 Humble for Windows](docs/INSTALL.md#ros-2-humble-for-windows)) and the layout variables of [Installation](docs/INSTALL.md); the last step also needs QAROS installed ([The QarOS SDK](docs/INSTALL.md#the-qaros-sdk)). Then clone, build, test, install and run:

```powershell
git clone https://github.com/Quaternar/qaros-ogre "$env:QAR_ROOT\src\qar_ogre_adapter"
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
cmake --preset vs2019-x64
cmake --build --preset vs2019-x64
ctest --preset vs2019-x64
cmake --install build\vs2019-x64 --config RelWithDebInfo --prefix $env:QAR_PREFIX
.\scripts\run_ogre_sample.ps1
```

The tests end with the line 100% tests passed and the sample with the line sample PASSED (exit 0).

To use QAROS, run the live example. It joins the QAROS Hub of this PC as a source app: QAROS asks to allow the program once for each build of it (and again after a rebuild), so click Allow in the QAROS window. Then open a Visualizer from QAROS and add a receiver in it if it has none, and the example streams the sample scene to it. A Hub on another PC needs `--hub HOST:PORT --code CODE` instead of `--local`.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --local --seconds 60
```

The nine steps from a clean machine, with every output, are in [Quick start](docs/QUICKSTART.md); the details of each step are in the documents of the [Documentation](#documentation) table, and the web documentation at <https://docs.quaternar.com/> describes the stack in the context of QAROS: [Ogre and Qt in RViz](https://docs.quaternar.com/docs/next/developer-guide/ogre-qt-rviz/), [Ogre streaming](https://docs.quaternar.com/docs/next/developer-guide/ogre-streaming/) and [SDK limits](https://docs.quaternar.com/docs/next/developer-guide/sdk-limits/).

## Who this is for

This repository is for QuaternAR engineers who integrate with QarOS. It assumes that you know QarOS and its C API, and it never explains them: where QarOS appears, the documentation states only what this stack relies on and how it uses the C API.

What it does explain, where it is used, is the part of the stack that is not QarOS:

- Ogre 1.12.1, the rendering engine under the stream, and how its OpenGL context is handed to QarOS;
- the ROS 2 distribution for Windows, which this repository uses only as the source of that Ogre;
- CMake and the Microsoft C++ toolchain, with the exact versions and the reason for each.

Everything that is not QarOS is installed from a fresh Windows 11 machine, step by step, in the installation document.

## Features

- **A core that calls no QarOS function.** `qar_ar_core` holds the Qt-free panel and pointer types, the input seam, the pointing-ray model, the spatial frames and the pick model, and the one wrapper of the vendored QarOS C header.
- **An Ogre stereo stream for every peer.** `qar_ogre_streaming` renders an Ogre scene into a colour and a depth render texture for each eye, with the pose and the field of view of each eye from QarOS, and reaches QarOS only through a narrow seam.
- **The QarOS C API adapter.** `qar_qar_adapter` has six components, `capi`, `session`, `render`, `panels`, `url` and `gestures`; its session owner joins the QAROS Hub as a local app or with a pairing code, on a thread of its own.
- **Geo anchor and diagnostics.** A WGS84 fix or an ECEF frame can georeference the app volume, and opt-in switches write a per-frame trace and stage timings as CSV files.
- **A headless sample and a live example.** The sample renders the same Ogre scene for synthetic peers and needs no QAROS; the example joins the QAROS Hub and streams that scene to the first target that comes up.
- **Known limitations.** What the QarOS SDK does not document or does not do is listed in [Known SDK limitations](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations), and the symptoms of the limits that you may meet are in [Troubleshooting](docs/TROUBLESHOOTING.md#qaros-stream-problems). [What the live sessions showed](docs/VERIFICATION.md#what-the-live-sessions-showed) says what the sessions of qar_rviz_plugin showed.

Each feature is described in [Feature details](docs/ARCHITECTURE.md#feature-details).

## Where this fits

This repository is one of three that together stream an Ogre scene, Qt widgets, web pages and RViz into QarOS. Each repository has the version 0.1.0, and this documentation describes version 0.1.0 of all three: a repository asks for that version of the repositories it builds on, so the three have to be at matching versions. The table lists the three in the order in which they are installed: `qar_ogre_adapter` first, then `qar_qt_streaming`, then `qar_rviz_plugin`.

| Repository | Version | What it provides | Depends on | Also needs |
|---|---|---|---|---|
| **qar_ogre_adapter** (this repository) | 0.1.0 | The QarOS integration layer: renders an Ogre scene for each eye and streams it to QarOS; the adapter for the QarOS C API; the vendored C header | none | The ROS 2 distribution, for Ogre only |
| qar_qt_streaming | 0.1.0 | Qt widgets as QarOS AR GUI panels | qar_ogre_adapter | Qt 5.15.2 |
| qar_rviz_plugin | 0.1.0 | The RViz2 plugin that streams RViz into QarOS, and web AR panels from a UI model and from the ROS 2 graph | qar_ogre_adapter, qar_qt_streaming | The ROS 2 distribution with its repairs, Qt 5.15.2, colcon |

The installation document of each repository installs the repositories it depends on, with the commands for each, so you can start from any of them.

## Documentation

| Document | What it covers |
|---|---|
| [Quick start](docs/QUICKSTART.md) | The steps from a clean machine to a running example, with the output of each |
| [Installation](docs/INSTALL.md) | The machine, the toolchain, the dependencies and this repository, step by step, with the commands |
| [Getting started](docs/GETTING_STARTED.md) | The first program that you run, and what you see |
| [Architecture](docs/ARCHITECTURE.md) | How the code is structured, which part owns what, and why |
| [API](docs/API.md) | The public interface, with examples that are compiled with the repository |
| [Configuration](docs/CONFIGURATION.md) | Every environment variable and build option |
| [Testing](docs/TESTING.md) | How to run the tests, what they cover and what they do not |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Symptoms, causes and fixes |
| [SDK compatibility](docs/SDK_COMPATIBILITY.md) | The QAROS version that was verified and the QarOS behaviour this repository relies on |
| [Developing](docs/DEVELOPING.md) | The layout of the repository and the conventions for changing it |
| [Verification](docs/VERIFICATION.md) | The checks of each level, how to run them, and their status |
| [QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md) | What this stack relies on in QarOS, fact by fact |
| [Changelog](CHANGELOG.md) | What each version contains |
| [License](LICENSE) | The license of this repository |

## The QarOS SDK is not included

QAROS, which consists of `qar-streaming-c.dll`, `qar_streaming.dll`, the zenoh and GStreamer stacks, the Hub and the Visualizer `qar-streaming-viz.exe`, is QuaternAR's proprietary product and is not part of this repository. You obtain its setup program from QuaternAR and install it once on the PC. The crash reports of the Visualizer carry the name of a second program of the same installation, `qar-stereokit-gui.exe`.

This repository vendors only the C header of the QarOS C API, `third_party/qar/include/qar_streaming.h`, with the MIT licence of the public QarOS repository in `third_party/qar/LICENSE`; the file `third_party/qar/README.md` names where the header and the licence come from.

You do not need QAROS to build this repository or to run its tests. Nothing links against QAROS: a program loads the installed library `qar-streaming-c.dll` at run time, and configuring only prints a warning when the folder that `QAR_SDK_ROOT` names does not hold the DLL. You need QAROS to connect to it.

A program finds the installed QAROS by itself: the installer records the install folder in the registry, and the library is loaded from there. How a program joins the Hub is described on the pages of QuaternAR: [Onboarding and sessions](https://docs.quaternar.com/docs/next/developer-guide/onboarding-and-sessions/).

This stack was verified against the installed QAROS 0.9.0-dev.3008+65dc9aeb; the [SDK compatibility](docs/SDK_COMPATIBILITY.md) document says how to identify the version you have.

## Platform support

The stack runs on Windows 11, 64-bit, and is built with the Microsoft C++ toolset v142 (Visual Studio 2019). It is run from a local desktop session. Linux is not built or tested.

## License

This repository is licensed under the MIT license; the text is in [LICENSE](LICENSE), and every C++ source and header carries the identifier of the license and the copyright line of QuaternAR.

Third-party material in this repository:

- `third_party/qar` holds the QarOS C header and the MIT licence of the public QarOS repository, which publishes it; its README names the origin of both;
- `third_party/stb` holds the single-header PNG writer `stb_image_write.h`; its README names the origin.
- GoogleTest is downloaded when you configure, only for the tests; it is not part of this repository and is not installed with it.
