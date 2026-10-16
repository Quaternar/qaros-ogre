# qar_ogre_adapter - the QarOS integration layer

qar_ogre_adapter 0.1.0 is the QarOS integration layer of a stack that streams Ogre scenes, Qt widgets, web pages and RViz into QarOS. It renders an Ogre scene for each eye into OpenGL textures and streams them to QarOS, it holds the one adapter for the QarOS C API (session, peers, panels, URL panels and gestures) that the other repositories of the stack build on, and it carries the C header of the QarOS SDK that all of them compile against. It is a set of C++17 static libraries for Windows 11 and MSVC, delivered as the CMake package `qar_ogre_adapter`, with a headless sample and a live example that you can run.

## Who this is for

This repository is for QuaternAR engineers who integrate with QarOS. It assumes that you know QarOS and its C API, and it never explains them: where QarOS appears, the documentation states only what this stack relies on and how it uses the C API.

What it does explain, where it is used, is the part of the stack that is not QarOS:

- Ogre 1.12.1, the rendering engine under the stream, and how its OpenGL context is handed to QarOS;
- the ROS 2 Humble distribution for Windows, which this repository uses only as the source of that Ogre;
- CMake and the Microsoft C++ toolchain, with the exact versions and the reason for each.

Everything that is not QarOS is installed from a fresh Windows 11 machine, step by step, in the installation document.

## Features

- **A core that calls no QarOS function.** The module `qar_ar_core` holds the Qt-free panel and pointer types, the input seam through which AR pointer samples enter, the seam to QarOS for browser-backed (URL) panels, the pointing-ray model, the spatial frames and the pick model, an opt-in timing log, and the one wrapper of the vendored QarOS C header.
- **An Ogre stereo stream for every peer.** The module `qar_ogre_streaming` renders an Ogre scene into a colour and a depth render texture for each eye, takes the pose and the field of view of each eye from QarOS, renders the peers that are due in one batch under a render budget, can light the scene with a world-fixed rig while it streams, and writes depth either as the window-space value or as metric distance. It reaches QarOS only through the narrow seam of `IQarRenderSenderAdapter` and `IQarRenderSenderFactory`.
- **The QarOS C API adapter, in five components.** The module `qar_qar_adapter` builds `capi` (the program's one loader of the SDK library, the callback-slot arena and the session owner that panels borrow), `render` (the session bootstrap, the app volume, invitations and admission of peers, asynchronous creation of senders, and the copy of the rendered textures into the textures of QarOS through a shared WGL context), `panels`, `url` and `gestures`.
- **Geo anchor.** A WGS84 fix or an ECEF frame can georeference the app volume.
- **Diagnostics.** Streams report why a frame was skipped, and opt-in switches write a per-frame trace and stage timings as CSV files.
- **A headless sample and a live example.** The sample renders the same Ogre scene for synthetic in-process peers, needs no QarOS SDK and writes stereo image pairs; the example streams that scene to a QarOS Visualizer, a mode that the authors of qar_ogre_adapter have not run with a Visualizer; the libraries ran in the live sessions of qar_rviz_plugin, and [What the live sessions showed](docs/VERIFICATION.md#what-the-live-sessions-showed) says what those sessions showed.
- **Known limitations.** What the QarOS SDK does not document or does not do is listed in [Known SDK limitations](docs/QAROS_INTEGRATION_NOTES.md#known-sdk-limitations), and the symptoms of the limits that you may meet are in [Troubleshooting](docs/TROUBLESHOOTING.md#qaros-stream-problems).

## Where this fits

This repository is one of three that together stream an Ogre scene, Qt widgets, web pages and RViz into QarOS. Each repository has the version 0.1.0, and this documentation describes version 0.1.0 of all three: a repository asks for that version of the repositories it builds on, so the three have to be at matching versions. The table lists the three in the order in which they are installed: `qar_ogre_adapter` first, then `qar_qt_streaming`, then `qar_rviz_plugin`.

| Repository | Version | What it provides | Depends on | Also needs |
|---|---|---|---|---|
| **qar_ogre_adapter** (this repository) | 0.1.0 | The QarOS integration layer: renders an Ogre scene for each eye and streams it to QarOS; the adapter for the QarOS C API; the vendored C header | none | The ROS 2 Humble distribution, for Ogre only |
| qar_qt_streaming | 0.1.0 | Qt widgets as QarOS AR GUI panels | qar_ogre_adapter | Qt 5.15.2 |
| qar_rviz_plugin | 0.1.0 | The RViz2 plugin that streams RViz into QarOS, and web AR panels from a UI model and from the ROS 2 graph | qar_ogre_adapter, qar_qt_streaming | The ROS 2 Humble distribution with its repairs, Qt 5.15.2, colcon |

The installation document of each repository installs the repositories it depends on, with the commands for each, so you can start from any of them.

## Quick start

These ten steps take a Windows 11 machine that has the toolchain of [Installation](docs/INSTALL.md#toolchain) installed to a running example. Every command is explained, with its output, in the document that the step names. Run the steps in one PowerShell 7 window, in this order.

1. Create the folders and set the seven variables that all commands use ([Directory layout and environment variables](docs/INSTALL.md#directory-layout-and-environment-variables)).

```powershell
$layout = [ordered]@{
    QAR_ROOT          = 'C:\qar'
    QAR_DEPS_ROOT     = 'C:\qar\deps'
    ROS2_INSTALL_ROOT = 'C:\qar\deps\ros2_humble\ros2-windows'
    QAR_QT_ROOT       = 'C:\qar\deps\Qt\5.15.2\msvc2019_64'
    QAR_VENV          = 'C:\qar\venv'
    QAR_PREFIX        = 'C:\qar\install'
    QAR_SDK_ROOT      = 'C:\QarOS\install'
}
foreach ($name in $layout.Keys) {
    [Environment]::SetEnvironmentVariable($name, $layout[$name], 'User')
    Set-Item -Path "Env:$name" -Value $layout[$name]
}
New-Item -ItemType Directory -Force -Path $layout.QAR_ROOT, $layout.QAR_DEPS_ROOT, $layout.QAR_PREFIX, (Split-Path $layout.QAR_SDK_ROOT) | Out-Null
```

> Success: the block prints nothing and ends without an error. Afterwards the seven variables are set for your Windows user and in the current window, and these folders exist: `C:\qar`, `C:\qar\deps`, `C:\qar\install` and `C:\QarOS`.
>
> This command changes your machine, so no output is shown here.

2. Unpack the QarOS SDK drop that you received from QuaternAR, saved as `C:\QarOS\qaros_sdk.zip` ([The QarOS SDK](docs/INSTALL.md#the-qaros-sdk)).

```powershell
$sdkParent = Split-Path $env:QAR_SDK_ROOT
Expand-Archive -LiteralPath (Join-Path $sdkParent 'qaros_sdk.zip') -DestinationPath $sdkParent -Force
Get-ChildItem -LiteralPath $env:QAR_SDK_ROOT -Name
```
```text
bin
include
third_party_licenses.md
```

3. Remove the download mark from the unpacked programs.

```powershell
Get-ChildItem -LiteralPath $env:QAR_SDK_ROOT -Recurse -Include *.exe, *.dll | Unblock-File
```

4. Clone this repository and go to the folder of the clone; the remaining steps run there ([Python environment](docs/INSTALL.md#python-environment)).

```powershell
git clone https://github.com/Quaternar/qaros-ogre "$env:QAR_ROOT\src\qar_ogre_adapter"
```
```text
Cloning into 'C:\qar\src\qar_ogre_adapter'...
done.
```

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

5. Download and unpack the ROS 2 Humble distribution, which provides Ogre ([ROS 2 Humble for Windows](docs/INSTALL.md#ros-2-humble-for-windows)). This repository needs none of the repairs that the reminder at the end of the output mentions: the repair scripts in the folder `scripts` serve qar_rviz_plugin.

```powershell
.\scripts\fetch_dependencies.ps1
```
```text
=== ROS 2 Humble Windows binary distro ===
Downloading https://github.com/ros2/ros2/releases/download/release-humble-20260220/ros2-humble-20260220-windows-release-amd64.zip
Downloaded and verified: C:\qar\deps\downloads\ros2-humble-20260220-windows-release-amd64.zip
Extracting to C:\qar\deps\ros2_humble

=== ROS 2 source manifest repo (reference only) ===
Downloading https://github.com/ros2/ros2/archive/refs/tags/release-humble-20260220.zip
Downloaded and verified: C:\qar\deps\downloads\ros2-release-humble-20260220-src.zip
Extracting to C:\qar\deps\ros2_src_release-humble-20260220

Done. Binary distro: C:\qar\deps\ros2_humble\ros2-windows
Reminder: the binary distro has packaging gaps - run the repair scripts next:
fix_missing_spdlog_dll.ps1, fix_missing_vendor_dlls.ps1, fix_missing_openssl_dll.ps1, fix_qt_conda_imports.ps1.
```

6. Configure the repository with its preset ([Build and install qar_ogre_adapter](docs/INSTALL.md#build-and-install-qar_ogre_adapter)).

```powershell
cmake --preset vs2019-x64
```
```text
Preset CMake variables:

  QAR_BUILD_TESTS="ON"
  QAR_ROS2_INSTALL_ROOT="C:\qar\deps\ros2_humble\ros2-windows"
  QAR_SDK_ROOT="C:\QarOS\install"

...
-- The CXX compiler identification is MSVC 19.29.30151.0
...
-- ROS 2 distribution: C:/qar/deps/ros2_humble/ros2-windows
-- Found rviz_ogre_vendor: 11.2.25 (C:/qar/deps/ros2_humble/ros2-windows/share/rviz_ogre_vendor/cmake)
...
-- qar_qar_adapter: QarOS SDK backend, DYNAMIC loading of C:\QarOS\install/bin/qar-streaming-c.dll (no import lib linked)
...
-- Configuring done (7.7s)
-- Generating done (0.1s)
-- Build files have been written to: C:/qar/src/qar_ogre_adapter/build/vs2019-x64
```

7. Build the libraries, the sample and the example.

```powershell
cmake --build --preset vs2019-x64
```
```text
...
  qar_ar_core.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_ar_core\RelWithDebInfo\qar_ar_core.lib
...
  qar_ogre_streaming.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_ogre_streaming\RelWithDebInfo\qar_ogre_streaming.lib
...
  qar_qar_adapter_render.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_qar_adapter\RelWithDebInfo\qar_qar_adapter_render.lib
...
  qar_ogre_standalone_streaming_sample.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe
...
  qar_ogre_live_stream_example.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe
...
```

8. Run the tests ([Testing](docs/TESTING.md)).

```powershell
ctest --preset vs2019-x64
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
        Start   1: check_layering_qar_ar_core
  1/331 Test   #1: check_layering_qar_ar_core ................................................................   Passed    0.08 sec
        Start   2: check_layering_qar_ogre_streaming
  2/331 Test   #2: check_layering_qar_ogre_streaming .........................................................   Passed    0.09 sec
        Start   3: check_layering_qar_qar_adapter
  3/331 Test   #3: check_layering_qar_qar_adapter ............................................................   Passed    0.10 sec
        Start   4: check_layering_examples
  4/331 Test   #4: check_layering_examples ...................................................................   Passed    0.05 sec
...
100% tests passed, 0 tests failed out of 331

Total Test time (real) =   2.65 sec
```

9. Run the headless sample. It renders an Ogre scene for three synthetic peers and writes their stereo images; it needs no QarOS SDK ([Run the headless sample](docs/GETTING_STARTED.md#run-the-headless-sample)).

```powershell
.\scripts\run_ogre_sample.ps1
```
```text
Running sample: C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe
  Ogre DLLs from: C:\qar\deps\ros2_humble\ros2-windows\opt\rviz_ogre_vendor\bin
  Output dir    : C:\Users\<user>\AppData\Local\Temp\qar_ogre_sample
...
Ogre up. render system = "OpenGL Rendering Subsystem"
...
created 3 stereo peers; streaming active.

== rendering 120 ticks ==

== final per-peer stereo capture ==
  peer 00000000  1280x720  submitted=120  L_px=138158 R_px=137901  diff=5.72  near_disp=12.60px far_disp=9.01px  nearer_more=true  pass=true
  peer 01000000  1280x720  submitted=120  L_px=158699 R_px=159175  diff=6.15  near_disp=13.75px far_disp=8.58px  nearer_more=true  pass=true
  peer 02000000  1280x720  submitted=120  L_px=104406 R_px=104459  diff=5.09  near_disp=9.70px far_disp=9.27px  nearer_more=true  pass=true
...
== OpenGL zero-copy bridge demo (outside RViz) ==
  active render system: OpenGL Rendering Subsystem
  color target native GL id (render-target accessor): 1
  fill_opengl_frame: ok  textures=4 views=4
  bridge_ran=true bridge_pass=true (report: C:\Users\<user>\AppData\Local\Temp\qar_ogre_sample/bridge_demo.txt)
...
sample PASSED (exit 0)
Sample exited with code 0
```

10. Check the connection to QarOS. The example opens a QarOS session, prints the app volume that it created and exits; it opens no window and invites nobody ([Check the connection to QarOS without a window](docs/GETTING_STARTED.md#check-the-connection-to-qaros-without-a-window)).

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --sdk-bin "$env:QAR_SDK_ROOT\bin" --connect-only
```
```text
[qar-adapter] library logging: severity=warn console=off folder=C:\Users\<user>\AppData\Local\Temp\quaternar
[qar-adapter] zenoh logging: off (QAR_ZENOH_LOG not set)
example: connected to QarOS: ready=1 app_volume=051e412fabf14bc6ac4815c694793fec
```

[Getting started](docs/GETTING_STARTED.md) goes on from here and shows how to use the library from your own Ogre application. The live example that streams the sample scene to a Visualizer is [Stream the sample scene to the Visualizer](docs/GETTING_STARTED.md#stream-the-sample-scene-to-the-visualizer); it needs a drop of the QarOS SDK, which holds the Visualizer, and QarOS starts that Visualizer itself from the `bin` folder of the drop.

## Documentation

| Document | What it covers |
|---|---|
| [Installation](docs/INSTALL.md) | The machine, the toolchain, the dependencies and this repository, step by step, with the commands |
| [Getting started](docs/GETTING_STARTED.md) | The first program that you run, and what you see |
| [Architecture](docs/ARCHITECTURE.md) | How the code is structured, which part owns what, and why |
| [API](docs/API.md) | The public interface, with examples that are compiled with the repository |
| [Configuration](docs/CONFIGURATION.md) | Every environment variable and build option |
| [Testing](docs/TESTING.md) | How to run the tests, what they cover and what they do not |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Symptoms, causes and fixes |
| [SDK compatibility](docs/SDK_COMPATIBILITY.md) | The supported QarOS SDK drops and the QarOS behaviour this repository relies on |
| [Developing](docs/DEVELOPING.md) | The layout of the repository and the conventions for changing it |
| [Verification](docs/VERIFICATION.md) | The checks of each level, how to run them, and their status |
| [QarOS integration notes](docs/QAROS_INTEGRATION_NOTES.md) | What this stack relies on in QarOS, fact by fact |
| [Changelog](CHANGELOG.md) | What each version contains |
| [License](LICENSE) | The license of this repository |

## The QarOS SDK is not included

The QarOS runtime, which consists of `qar-streaming-c.dll`, `qar_streaming.dll`, the zenoh and GStreamer stacks and the Visualizer `qar-streaming-viz.exe`, is QuaternAR's proprietary binary distribution and is not part of this repository. You obtain a drop of it from QuaternAR. The crash reports of the Visualizer carry the name of a second program of the same SDK folder, `qar-stereokit-gui.exe`.

This repository vendors only the C header of the QarOS C API, `third_party/qar/include/qar_streaming.h`, with the MIT licence of the public QarOS repository in `third_party/qar/LICENSE`; the file `third_party/qar/README.md` names where the header and the licence come from.

You do not need the SDK to build this repository or to run its tests. Nothing links against the SDK: the library `qar-streaming-c.dll` is loaded at run time from the folder that a program passes in, and configuring only prints a warning when the folder that `QAR_SDK_ROOT` names does not hold the DLL. You need the SDK to connect to QarOS.

Point a program at the SDK by passing the `bin` folder of the SDK in the `runtime_binaries_folder_path` field of the QarOS session factory configuration.

This stack was verified against the QarOS SDK drop of 2026-09-28 and needs at least the drop of 2026-09-21; the [SDK compatibility](docs/SDK_COMPATIBILITY.md) document says how to identify the drop you have and what changed between drops. At the time of writing, the public QarOS repository <https://github.com/Quaternar/qaros> publishes a later generation of the C API. This stack does not build against it: the session set-up (create, serialise, join and invite), the hand of a gesture, the time points and the init structures of volumes and panels would have to be ported.

## Platform support

The stack runs on Windows 11, 64-bit, and is built with the Microsoft C++ toolset v142 (Visual Studio 2019). It is run from a local desktop session. Linux is not built or tested.

## License

This repository is licensed under the MIT license; the text is in [LICENSE](LICENSE), and every C++ source and header carries the identifier of the license and the copyright line of QuaternAR.

Third-party material in this repository:

- `third_party/qar` holds the QarOS C header and the MIT licence of the public QarOS repository, which publishes it; its README names the origin of both;
- `third_party/stb` holds the single-header PNG writer `stb_image_write.h`; its README names the origin.
- GoogleTest is downloaded when you configure, only for the tests; it is not part of this repository and is not installed with it.
