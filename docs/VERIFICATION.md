# Verification

## Status

This section gives the result of each level. The results of Level 0 and Level 1 are those of this release, and each comes from a recorded run of the commands that the level names. The result of Level 2 comes from live sessions and live checks, and its line says which build each ran.

- Level 0, automated checks: passed. Every test of the repository ran and none failed ([Run all tests](#run-all-tests) shows the totals), the layering guard and its self-test reported OK ([The layering guard](#the-layering-guard)), and the consumer program of the install smoke test built and ran ([Install and the install smoke test](#install-and-the-install-smoke-test)).
- Level 1, headless checks: the header of the installed QAROS 0.9.0-dev.3008+65dc9aeb equals the vendored header ([The SDK header](#the-sdk-header)) and the headless sample passed ([The headless sample](#the-headless-sample)); the connection check of the live example starts the QAROS Hub, so it is not run for this document ([A QarOS session without a window](#a-qaros-session-without-a-window)).
- Level 2, desktop checks: the authors of qar_ogre_adapter did not run the live example of this repository in a session with a Visualizer ([Desktop Visualizer checks (Level 2)](#desktop-visualizer-checks-level-2)); the libraries of this repository ran in the live checks of qar_rviz_plugin on the desktop Visualizer with the installed QAROS 0.9.0-dev.3008+65dc9aeb, which the maintainers judged as passed, and no session with a headset has run this release ([What the live sessions showed](#what-the-live-sessions-showed)).
- Level 3, device checks: not run for this release; [Device checks (Level 3)](#device-checks-level-3) says why.

## Verification levels

Every check of this document belongs to one of four levels. The level says what a check needs and who can run it, and it is the level of the documented commands that the check uses.

- **Level 0, automated checks.** They need neither the QarOS SDK nor a window and run in a plain PowerShell 7 window: configure, build, run the tests, install, the layering guard and the install smoke test. A script or a person runs them with the commands of this document, and every command has a recorded output.
- **Level 1, headless checks.** They run the real programs of this stack without a window, without a Visualizer, without inviting a peer and without a click. Most of them need the QarOS SDK: they open a real QarOS session and end again, which can leave QarOS background processes running.
A script or a person runs them.
- **Level 2, desktop checks.** They run against a real QarOS Visualizer on the desktop of the machine. A person starts the programs, points and clicks in the Visualizer and judges what it shows, and the logs of the run give the numbers. Nothing in this stack clicks, moves a window or presses a key.
- **Level 3, device checks.** They run against a real device, such as a HoloLens 2. Where no device is available, the command is run in its dry-run form and the outcome on a device is described from the code, with the log lines quoted from it.

A check of a higher level assumes that the lower levels passed. All checks run on Windows 11 on x64. A check that needs the QarOS SDK uses the folder that `QAR_SDK_ROOT` names, as [The QarOS SDK](INSTALL.md#the-qaros-sdk) describes, and a check that leaves QarOS background processes running does not stop them: if a later check cannot create a QarOS session, look for a program that holds UDP port 7446, as [Run-time problems](TROUBLESHOOTING.md#run-time-problems) explains.

## Automated checks (Level 0)

The automated checks need neither QAROS nor a window. Run them in PowerShell 7 from the clone, once the installation has reached [Build and install qar_ogre_adapter](INSTALL.md#build-and-install-qar_ogre_adapter). The commands are those of that section, of [Testing](TESTING.md) and of [Install smoke test](INSTALL.md#install-smoke-test), in the order of a full check. Level 0 passes when every command ends without an error and the test run reports no failed test. Go to the folder of the clone first:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

### Configure and build

Configure with the preset of the repository, then build. The build compiles the three libraries, the headless sample and the live example into the build folder of the repository.

```powershell
cmake --preset vs2019-x64
```
```text
Preset CMake variables:

  QAR_BUILD_TESTS="ON"
  QAR_ROS2_INSTALL_ROOT="C:\qar\deps\ros2_humble\ros2-windows"
  QAR_SDK_ROOT="C:\Program Files\Quaternar\QAROS"

...
-- The CXX compiler identification is MSVC 19.29.30151.0
...
-- ROS 2 distribution: C:/qar/deps/ros2_humble/ros2-windows
-- Found rviz_ogre_vendor: 11.2.25 (C:/qar/deps/ros2_humble/ros2-windows/share/rviz_ogre_vendor/cmake)
...
-- qar_qar_adapter: QarOS SDK backend, DYNAMIC loading of C:\Program Files\Quaternar\QAROS/bin/qar-streaming-c.dll (no import lib linked)
...
-- Configuring done (10.4s)
-- Generating done (0.2s)
-- Build files have been written to: C:/qar/src/qar_ogre_adapter/build/vs2019-x64
```

```powershell
cmake --build --preset vs2019-x64
```
```text
  qar_ar_core.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_ar_core\RelWithDebInfo\qar_ar_core.lib
...
  qar_qar_adapter_session.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_qar_adapter\RelWithDebInfo\qar_qar_adapter_session.lib
...
  qar_ogre_streaming.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_ogre_streaming\RelWithDebInfo\qar_ogre_streaming.lib
...
  qar_qar_adapter_render.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\qar_qar_adapter\RelWithDebInfo\qar_qar_adapter_render.lib
...
  qar_ogre_standalone_streaming_sample.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe
...
  qar_ogre_live_stream_example.vcxproj -> C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe
```

### Run all tests

The test run is the core of Level 0. It runs every test of the repository through the test preset. A pass is a run in which every test passes: the last lines of the output give the number of tests that ran and say that none failed. A test that is reported as skipped does not count as a failure.

```powershell
ctest --preset vs2019-x64
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
        Start   1: check_layering_qar_ar_core
  1/610 Test   #1: check_layering_qar_ar_core .................................................................   Passed    0.13 sec
        Start   2: check_layering_qar_ogre_streaming
  2/610 Test   #2: check_layering_qar_ogre_streaming ..........................................................   Passed    0.11 sec
        Start   3: check_layering_qar_qar_adapter
  3/610 Test   #3: check_layering_qar_qar_adapter .............................................................   Passed    0.22 sec
        Start   4: check_layering_examples
...
100% tests passed, 0 tests failed out of 610

Total Test time (real) = 117.38 sec
```

[Testing](TESTING.md#what-the-automated-tests-do-not-cover) lists what these tests leave out: every call into QarOS, the handover of the pictures through OpenGL and the live desktop. Of the checks of the next levels, only those of Level 1 were run for this release: the headless sample, which runs the Ogre side without QarOS, and `--connect-only`, which covers the bootstrap of a QarOS session. The calls into QarOS beyond that bootstrap and the live desktop were exercised only in the live checks of qar_rviz_plugin on the desktop Visualizer, with a build that differs from this release only in comments and the text of one tool tip, and the check of Level 2 has not been run.

### The layering guard

The guard checks the include rules of every module and of the examples, and its self-test checks the guard itself. It prints one line for each module. [The layering guard](TESTING.md#the-layering-guard) explains the rules. The commands call the interpreter of the virtual environment, which is optional for this repository: any Python 3 interpreter of version 3.8 or newer, such as the one that CMake uses, runs the guard as well, because it needs only the standard library.

```powershell
& "$env:QAR_VENV\Scripts\python.exe" scripts\check_layering.py --all
```
```text
check_layering qar_ar_core: OK (23 files)
check_layering qar_ogre_streaming: OK (54 files)
check_layering qar_qar_adapter: OK (50 files)
check_layering examples: OK (7 files)
```

```powershell
& "$env:QAR_VENV\Scripts\python.exe" scripts\check_layering.py --self-test
```
```text
check_layering self-test: OK
```

### Install and the install smoke test

Install the libraries and the CMake package into the prefix, then build and run a small consumer program against the installed package. A pass is a consumer that prints the version of the package and the targets that it found.

```powershell
cmake --install build\vs2019-x64 --config RelWithDebInfo --prefix $env:QAR_PREFIX
```
```text
-- Installing: C:\qar\install/lib/qar_ar_core.lib
-- Installing: C:\qar\install/lib/qar_ogre_streaming.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_capi.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_session.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_render.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_panels.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_url.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_gestures.lib
-- Installing: C:\qar\install/include
...
-- Installing: C:\qar\install/include/qar_streaming.h
-- Installing: C:\qar\install/share/qar_ogre_adapter/LICENSE.qar_streaming_header
-- Installing: C:\qar\install/share/qar_ogre_adapter/LICENSE
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterTargets.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterTargets-relwithdebinfo.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterConfig.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterConfigVersion.cmake
```

```powershell
cmake -S docs\snippets\consumer -B build\consumer -G "Visual Studio 16 2019" -A x64 "-DCMAKE_PREFIX_PATH=$env:QAR_PREFIX"
```
```text
-- Found rviz_ogre_vendor: 11.2.25 (C:/qar/deps/ros2_humble/ros2-windows/share/rviz_ogre_vendor/cmake)
...
-- Configuring done (2.1s)
-- Generating done (0.0s)
-- Build files have been written to: C:/qar/src/qar_ogre_adapter/build/consumer
```

```powershell
cmake --build build\consumer --config RelWithDebInfo
```
```text
  main.cpp
```

```powershell
.\build\consumer\RelWithDebInfo\qar_ogre_adapter_consumer.exe
```
```text
qar_ogre_adapter 0.1.0
targets: qar_ogre_adapter::qar_ar_core qar_ogre_adapter::qar_ogre_streaming qar_ogre_adapter::render qar_ogre_adapter::panels qar_ogre_adapter::url qar_ogre_adapter::gestures
QarOS library loaded: no
status text: Ok
parse_bool_env("on"): true
```

## Headless checks (Level 1)

The headless checks run the real programs of this repository without a window and without a Visualizer. The sample needs no QAROS. The other checks need the installed QAROS; the check of the header only reads files, and the connection check starts the QAROS Hub.

### The headless sample

The sample runs the whole Ogre side with three synthetic peers in a hidden OpenGL window and writes its result files into the folder `qar_ogre_sample` of `TEMP`. A pass is an exit code of 0, a final line that says that the sample passed, and a summary in which every peer reports pass=1. [Run the headless sample](GETTING_STARTED.md#run-the-headless-sample) describes the files.

```powershell
.\scripts\run_ogre_sample.ps1
```
```text
Running sample: C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe
...
== final per-peer stereo capture ==
  peer 00000000  1280x720  submitted=120  L_px=138158 R_px=137901  diff=5.72  near_disp=12.60px far_disp=9.01px  nearer_more=true  pass=true
  peer 01000000  1280x720  submitted=120  L_px=158699 R_px=159175  diff=6.15  near_disp=13.75px far_disp=8.58px  nearer_more=true  pass=true
  peer 02000000  1280x720  submitted=120  L_px=104406 R_px=104459  diff=5.09  near_disp=9.70px far_disp=9.27px  nearer_more=true  pass=true
...
sample PASSED (exit 0)
Sample exited with code 0
```

```powershell
Get-Content -LiteralPath (Join-Path $env:TEMP 'qar_ogre_sample\summary.txt')
```
```text
# Qar standalone Ogre streaming sample — run summary
render_system=OpenGL Rendering Subsystem
ticks=120 peers=3 size=1280x720 ipd_mm=64.0
batches_rendered=120
avg_batch_ms=0.928
peer 00000000 submitted=120 both_content=1 depth_ok=1 eyes_differ=1 nearer_shifts_more=1 pass=1
peer 01000000 submitted=120 both_content=1 depth_ok=1 eyes_differ=1 nearer_shifts_more=1 pass=1
peer 02000000 submitted=120 both_content=1 depth_ok=1 eyes_differ=1 nearer_shifts_more=1 pass=1
bridge_ran=1
overall_pass=true
```

### A QarOS session without a window

With `--connect-only` the live example joins the QAROS Hub through a session owner, attaches the session factory, which makes one app volume, prints the runtime, the mode, its peer and the targets, and ends again. It starts no Ogre and opens no window. A pass is an exit code of 0 and a line that reports the identifier of the app volume. The command starts the Hub of the installed QAROS when it is not running and the Hub asks its operator to allow the program, so a person runs it and it is not run for this document. [Check the connection to QarOS without a window](GETTING_STARTED.md#check-the-connection-to-qaros-without-a-window) describes the output.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --local --connect-only
```

### The SDK header

The repository vendors the QarOS C header, and the header must be the one of the QAROS that the stack runs against. The command compares the SHA256 of the header of the installed QAROS with that of the vendored copy and prints True when they are equal. It changes nothing; [SDK compatibility](SDK_COMPATIBILITY.md) describes the script that updates the vendored header.

```powershell
$qaros = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Quaternar\QAROS'
$sdk = Join-Path $qaros.InstallFolder 'include\qar_streaming.h'
$vendored = 'third_party\qar\include\qar_streaming.h'
(Get-FileHash -LiteralPath $sdk -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $vendored -Algorithm SHA256).Hash
```
```text
True
```

## Desktop Visualizer checks (Level 2)

One check runs the library against a real Visualizer: the live example in its default mode, which the authors of qar_ogre_adapter have not run. You start the example and you look at the Visualizer window; nothing in this repository clicks or moves a window. The command is the one of [Stream the sample scene to the Visualizer](GETTING_STARTED.md#stream-the-sample-scene-to-the-visualizer), which also explains the two settings that the Visualizer needs, the depth encoding and the world basis.

A pass is a Visualizer window that shows the sample scene upright and in colour for the 60 seconds of the run, after you added a receiver in the Visualizer. Near objects hide far ones, because the depth is the window-space value that the Visualizer read with a QAROS runtime earlier than the verified version. The example prints a line when the stream opens and a line when it closes, and it exits with 0 when the time is up.

### What the live sessions showed

The authors of qar_ogre_adapter did not run the live example of this repository in a session with a Visualizer, so the check above has not been run. The libraries of this repository ran in the live checks of qar_rviz_plugin, which streams the scene of RViz to a Visualizer through them: the plugin joined the QAROS Hub of the PC as a local app, the maintainers added a receiver in the Visualizer, and the scene of RViz and the panels of the plugin reached it. The maintainers judged those checks as passed; the judgement is theirs, and this document gives no measurement of it.

Not checked in a live session: the approval of a program by the Hub being denied or timing out, a Hub that stops while the program is connected, the pairing with the Hub of another PC, forgetting a pairing and pairing again, the invitation of a device, geographic anchoring against a live GPS source, and real multi-user devices. These paths are described as designed, from the code of the session owner.

## Device checks (Level 3)

Not run for this release: no device was available. The authors of qar_ogre_adapter state that an earlier build of this stack worked on a HoloLens 2 with a QAROS runtime earlier than the verified version; that is their statement, and no record of that run exists.

The session owner of this repository asks the Hub of this PC to start the app of a HoloLens or a headset (`invite_device`, with the kind and the host name of the device); that path is described from the code and was not run against a device. [Session bootstrap and peers](ARCHITECTURE.md#session-bootstrap-and-peers) describes it. None of the checks of this repository needs a device.

## Feature coverage

The table lists what this repository provides, the automated suites that exercise it, and the levels whose checks in this document cover it. A suite is the name of a group of tests, the part before the first dot of a test name, and [List and select tests](TESTING.md#list-and-select-tests) shows how to list every test. The table gives names and no counts; the test counts are in the output of [Run all tests](#run-all-tests). Level 0 is the suites of the middle column, and a feature without a suite has only the checks of the higher levels. Level 2 has not been run for this release: an entry for Level 2 names a check of this document that nobody has run, and the libraries ran in the live checks of qar_rviz_plugin on the desktop Visualizer, with a build that differs from this release only in comments and the text of one tool tip ([What the live sessions showed](#what-the-live-sessions-showed)). An entry for Level 1 means that a command of Level 1 exercises the feature.

| Feature | Automated suites (Level 0) | Covered by the checks of levels |
|---|---|---|
| Stereo rendering of the scene for each peer, with colour and depth | `StereoStreamConfig`, `StereoFrameAssembly`, `PoseConversion`, `Projection`, `OpenGlBridge`, `StereoStreamLifecycle` | 0; [1](#headless-checks-level-1); [2](#desktop-visualizer-checks-level-2) (not run for this release) |
| Depth as the window-space value that the Visualizer read with a QAROS runtime earlier than the verified version | `RenderTargetSupport` | 0; [2](#desktop-visualizer-checks-level-2) (not run for this release) |
| World-fixed light rig for the scene of each peer | `SceneLightingMode`, `SceneLightingConfig`, `PeerHeadPosition` | 0 |
| Render budget and scheduler for several peers | `RenderBudget`, `Scheduler`, `ManagerRenderTick`, `ManagerBookkeeping` | 0; [1](#headless-checks-level-1) |
| Configuration from environment variables and the logging of the QarOS library | `EnvConfig`, `LogSeverityParse`, `LibraryLoggingResolve`, `ZenohLogging` | 0 |
| Timing and frame-trace logs | `TimingSink`, `TimingLog`, `RenderTiming` | 0 |
| Skip reasons and diagnostics of a stream | `StereoStreamSkipReason` | 0 |
| Geographic anchor from a GPS fix | `GeoAnchor`, `GeoFixMoved` | 0 |
| Pointer samples from AR gestures | `PointerPhase`, `PointerGestureMapping`, `QueuedInputSource`, `GestureEventMapping`, `PointerSampleFromGestureFields` | 0 |
| Pointing ray and hand joints | `GestureRay`, `PointingRay`, `PointingRaySlot`, `HandJointsProbe`, `NearestHandJointTest`, `HandJointDistance` | 0 |
| Frames of the app volume and placement of panels | `SpatialFrames`, `ScenePlacementTest`, `GesturePointsAreInAppSpace`, `SpatialPointerFromSample` | 0 |
| Picking of points, poses, areas and navigation goals | `ResolvePickPoint`, `PointPickSession`, `PosePickSession`, `AreaPick`, `AreaPickSession`, `NavGoal` | 0 |
| Visual cues of a pick in the 3D scene | `PickVisual`, `PickVisualHandoff` | 0 |
| QarOS session and app volume through the session factory | none | [1](#headless-checks-level-1); [2](#desktop-visualizer-checks-level-2) (not run for this release) |
| Invitation of a Visualizer into the session and its stream | none | [2](#desktop-visualizer-checks-level-2) (not run for this release) |
| The QarOS library behind one wrapper, and the layering of the modules | `check_layering.py` | 0; [1](#headless-checks-level-1) |



## Known issues

No known issue is specific to this repository.

Two lines of the adapter look like errors and are expected. Neither is a failure by itself, and a run is judged by the checks of this document and not by the absence of these lines.

When the streams close at the end of a run, the adapter can fail to make the OpenGL context of a sender current, and it writes a line that has this form:

```text
[qar-adapter] *** wglMakeCurrent FAILED (err=%lu)
```

While a peer is still negotiating with QarOS, the adapter reports that the creation of its sender takes long, with a line that has this form:

```text
[qar-adapter] create_sender: peer %s still creating after %.1f s
```

## Reporting a problem

When a check fails, report it with the facts that let someone repeat it. [What to send us](TROUBLESHOOTING.md#what-to-send-us) lists what every report needs: the facts of your machine, the newest QarOS logs and the steps that lead to the problem. A failed check of this document adds the following.

- **The check.** Its heading in this document, and the command exactly as you ran it, with its complete output.
- **The text of the console** in which the check ran, when a program printed to it.
- **The QarOS logs** that were written during the run, newest first, and every crash report that appeared: the files of the QarOS log folder whose names end in crash.txt, with their dumps, and the report that Windows shows for a program that stops.
- **Pictures.** A screenshot of the Visualizer, and of the window of your program where there is one, at the moment of the failure, when the failure is visible.
- **What you did,** step by step, and what you expected to see.
