# Testing

## Running the tests

All tests of this repository are registered with CTest and run through the test preset `vs2019-x64`. The preset runs the configuration `RelWithDebInfo` and prints the output of every test that fails. Configure and build first, as described in [Build and install qar_ogre_adapter](INSTALL.md#build-and-install-qar_ogre_adapter); the tests are built with the rest.

The tests need no QarOS SDK and open no window, and you do not have to prepare the shell: a plain PowerShell 7 window is enough, because each test executable finds its libraries by itself.

- The Ogre tests never call a real Ogre function; their test property puts the Ogre `bin` folder of the ROS 2 distribution in front of `PATH`.

### Run all tests

Every command of this document runs in the folder of the clone; go there first:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

```powershell
ctest --preset vs2019-x64
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
        Start   1: check_layering_qar_ar_core
  1/331 Test   #1: check_layering_qar_ar_core ................................................................   Passed    0.10 sec
        Start   2: check_layering_qar_ogre_streaming
  2/331 Test   #2: check_layering_qar_ogre_streaming .........................................................   Passed    0.08 sec
        Start   3: check_layering_qar_qar_adapter
  3/331 Test   #3: check_layering_qar_qar_adapter ............................................................   Passed    0.10 sec
        Start   4: check_layering_examples
  4/331 Test   #4: check_layering_examples ...................................................................   Passed    0.06 sec
...
100% tests passed, 0 tests failed out of 331

Total Test time (real) =   2.60 sec
```

The last lines are the summary: how many tests passed, and the names of those that failed. A test that is skipped is reported as skipped and does not count as a failure.

### List and select tests

Every GoogleTest test is registered as one CTest test named `Suite.Test`, and the guard tests of the repository have names that start with `check_layering`.
The first command lists the registered tests without running them; the second runs only the tests whose name matches a regular expression, here the self-test of the layering guard.

```powershell
ctest --preset vs2019-x64 -N
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
  Test   #1: check_layering_qar_ar_core
  Test   #2: check_layering_qar_ogre_streaming
  Test   #3: check_layering_qar_qar_adapter
  Test   #4: check_layering_examples
  Test   #5: check_layering_selftest
  Test   #6: StereoStreamConfig.DefaultsAreValid
  Test   #7: StereoStreamConfig.RejectsZeroDimensions
...
  Test #329: RenderTiming.AFailedSubmitIsRecordedWithOkZero
  Test #330: RenderTiming.ASubmitWithoutAConsumedFrameWritesNothingAndANullStateIsInert
  Test #331: RenderTiming.ATryBeginCallThatItselfBlocksIsFlaggedAsASlowPoll

Total Tests: 331
```

```powershell
ctest --preset vs2019-x64 -R check_layering_selftest
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
    Start 5: check_layering_selftest
1/1 Test #5: check_layering_selftest ..........   Passed    0.05 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.07 sec
```

Use `-R` with a part of a suite name to run one suite, and add `-j` followed by a number to run tests in parallel. When a test fails, its output is printed by the preset; add the option `--rerun-failed` to the command to run only the tests that failed in the last run.

## What the tests cover

The unit tests are three GoogleTest executables, one for each module that has tests, and the layering guard adds its own tests. The list command above prints every registered test by name, with the number of tests at the end.

| Executable | Suites | What the tests assert |
|---|---|---|
| `qar_ar_core_tests` | `PointerGestureMapping`, `GestureEventMapping`, `PointerSampleFromGestureFields`, `QueuedInputSource`, `PointerPhase` | how a QarOS gesture kind, phase and hand map onto the pointer vocabulary: only a click commits and a hover never does, manipulation gestures are no pointer samples, and an action point equal to the start point is no scene hit |
| `qar_ar_core_tests` | `GestureRay`, `PointingRay`, `PointingRaySlot`, `HandJointsProbe`, `NearestHandJointTest`, `HandJointDistance` | the pointing ray: it runs from the start point of a gesture through its action point, equal points mean no intersection and not a zero ray, bad input is refused with an explanation, and a ray from the eye through the hand needs a tracked hand |
| `qar_ar_core_tests` | `SpatialFrames`, `ScenePlacementTest`, `GesturePointsAreInAppSpace` | the change of axes between the QarOS space and the ROS world (up, forward, right, anchor yaw, height and stand-off, round trips) and the placement of the scene |
| `qar_ar_core_tests` | `ResolvePickPoint`, `PointPickSession`, `PosePickSession`, `AreaPick`, `AreaPickSessionTest`, `PlaneBasis`, `NavGoal`, `SpatialPointerFromSample`, `YawQuaternion`, `AreaPickSession` | the resolution of a pick ray against a plane or at a depth, the state machines of point, pose and area picks (press, update, release, cancel; a hover never commits) and navigation goals |
| `qar_ar_core_tests` | `TimingSink`, `TimingLog` | the timing CSV writer: header, row width, bounded queue, concurrent appenders, an unwritable folder, and the opt-in environment variable |
| `qar_ogre_streaming_tests` | `StereoStreamConfig`, `PeerId`, `ManagerConstruction`, `ManagerBookkeeping`, `ManagerRenderTick` | the validation of a stream configuration (zero size, zero frame rate and a layout that is not separated textures are rejected), peer identifiers, and the manager: null seams are rejected, streams are created, looked up and destroyed, a failing sender factory surfaces, and a render tick off the render thread is refused |
| `qar_ogre_streaming_tests` | `Scheduler`, `RenderBudget` | the batch: one render lock around the whole batch, no lock for an empty tick, the frame-rate gate, the time budget that sheds optional frames and protects the frame-rate floor, and the cap on streams for each tick |
| `qar_ogre_streaming_tests` | `EyeRenderParams`, `StereoFrameAssembly`, `StereoStreamLifecycle`, `StereoStreamSkipReason`, `SenderFrameSource` | the per-eye camera parameters, the assembly of a stereo frame (eye labels, textures, views), the early-outs of a stream, and the skip reasons, which are named and add up |
| `qar_ogre_streaming_tests` | `PoseConversion`, `Projection` | the conversion of a QarOS pose and field of view into an Ogre camera, and projection matrices that equal the standard perspective and the Ogre frustum |
| `qar_ogre_streaming_tests` | `OpenGlBridge`, `GlInterop`, `RenderTargetSupport` | the marshalling of a stereo frame into the OpenGL frame structure of the C API, errors for a bad view index or a null texture, and a depth output that is always a 32-bit float colour texture |
| `qar_ogre_streaming_tests` | `SceneLightingMode`, `SceneLightingConfig`, `PeerHeadPosition`, `PickVisual`, `PickVisualHandoff` | the parsing and validation of the lighting rig, the position of a peer head, and the geometry of the spatial-pick visuals |
| `qar_qar_adapter_tests` | `EnvConfig`, `LogSeverityParse`, `LibraryLoggingResolve`, `ZenohLogging` | the parse rules of the environment values and the precedence between the settings of the QarOS library log |
| `qar_qar_adapter_tests` | `GeoAnchor`, `GeoFixMoved`, `RenderTiming` | the conversion of a geodetic fix to ECEF and the local east, north and up axes, the threshold for re-anchoring, and the stage-timing CSV of the 3D stream |

## Test doubles

The tests replace the collaborators of the library with small stand-ins that run in the same process. All of them stand in for a seam of `qar_ogre_streaming`. Four of them live in its `test_support` folder, which the repository builds as an interface library for its tests and for the headless sample; the fifth, `MockQarFrameInfoSource`, is a header of the library itself.

| Test double | What it stands in for |
|---|---|
| `MockQarRenderSenderAdapter` | the QarOS render sender: it records every call to begin a frame and to submit one, with all arguments, and returns scripted results |
| `MockQarFrameInfoSource` | the pose and field of view that a QarOS sender hands out at the start of a frame: a deterministic, animated head pose that orbits a focal point; it is a header of the library itself, and the sample uses it for its synthetic peers |
| `ManualClock` | the clock of the scheduler: its time advances by hand, so frame pacing and the time budget are tested without a wall clock |
| `RecordingRenderLock` | the render lock of the host: it records locking, so a test can assert that a batch takes the lock exactly once |
| `ImmediateRenderThreadExecutor` | the render-thread executor of the host: it runs posted work at once and reports that the caller is the render thread |

There is no stand-in for the QarOS SDK or for its C API. The tests that exercise the adapter cover its pure units only. The code that calls the C API has no automated test: the check `--connect-only` of the live example runs its bootstrap against the real runtime, and the rest of it was exercised only in the live sessions of qar_rviz_plugin, with builds that were made before the last changes of the code.

## What the automated tests do not cover

- **Calls to QarOS.** Only the parts of the adapter that need no session are unit tested: the geodetic arithmetic, the parsing of environment values and the stage-timing writer. The session bootstrap, invitations, sender creation, panels, URL panels and gestures need a live QarOS session. Where logic of the adapter is worth testing, move it out of the class that is bound to the session into free functions over plain values, as the geodetic arithmetic and the parsing of the environment were, and test that; do not try to fake the SDK.
- **Ogre and the GL hand-off.** The tests never call a real Ogre function, because a unit test has no GL context. The test executable of `qar_ogre_streaming` therefore delay-loads `OgreMain.dll`, which lets it run from a plain shell. Ogre render targets and the extraction of GL textures are checked by running the headless sample, and the copy of the textures into the ones of QarOS needs the runtime, which the live example is meant to check.
- **A live session with a viewer.** What the QarOS Visualizer shows and how depth and poses look there cannot be tested without a Visualizer. They are Level 2 checks of [Verification](VERIFICATION.md#desktop-visualizer-checks-level-2) with the live example, and the authors of qar_ogre_adapter have not run that example with a Visualizer. The example invites one Visualizer, so the behaviour with several peers is not part of it. The libraries ran with two Visualizers in the live sessions of qar_rviz_plugin, and [What the live sessions showed](VERIFICATION.md#what-the-live-sessions-showed) says what those sessions showed.

## The layering guard

The layering guard is a small Python program, `scripts\check_layering.py`, that reads the `#include` lines of the C++ sources and enforces the dependency table of the QarOS integration code: which module may include which. It exists because the structure of this stack (a core that calls no QarOS function, an adapter, and the GUI and ROS 2 layers above them) decays quietly when one convenient include crosses a layer. It needs only the Python standard library.

CTest registers one guard test per module that this repository holds, named `check_layering_<module>`, and the test `check_layering_selftest`, which proves that the guard still reports a violation of every rule. The guard checks every module of its table whose folder exists, because the table describes the whole family of repositories and a repository holds only some of the modules.

| Rule | What the guard enforces |
|---|---|
| L1, include classes | A module includes only the foreign header families listed for it: the standard library, Qt, Ogre, ROS 2, RViz, platform headers, GoogleTest, stb. |
| L2, project edges | A module includes only the project modules listed for it, and some edges only through named headers: no upward and no sideways edge. Every example folder has its own entry. |
| L3, vendored header | Only the wrapper header of the core includes the vendored QarOS C header. |
| L4, core calls no API | The core calls no QarOS C function; it uses the types of the C API only, so it never needs the SDK DLL. |
| L5, adapter components | Every file of the adapter belongs to one component and includes only headers of its own component and of the components it may use. |
| L6, loader | The dynamic-loading implementation macro of the C header appears exactly once, in the loader file of the adapter, and nothing defines the dynamic-loading switch except CMake. |

Comments and string literals are ignored; every `#include` line is classified.

### Run the guard by hand

The first command checks all modules of this repository and prints one summary line per module; the second runs the self-test. Both exit with 0 when everything is clean, with 1 when a rule is violated and with 2 when the arguments are wrong or no module folder exists.
The commands call the interpreter of the virtual environment, which is optional for this repository: any Python 3 interpreter of version 3.8 or newer, such as the one that CMake uses, runs the guard as well, because it needs only the standard library.

```powershell
& "$env:QAR_VENV\Scripts\python.exe" scripts\check_layering.py --all
```
```text
check_layering qar_ar_core: OK (17 files)
check_layering qar_ogre_streaming: OK (54 files)
check_layering qar_qar_adapter: OK (23 files)
check_layering examples: OK (7 files)
```

```powershell
& "$env:QAR_VENV\Scripts\python.exe" scripts\check_layering.py --self-test
```
```text
check_layering self-test: OK
```

### Read a failure

For a clean module the guard prints the module name, the word OK and the number of files it read. For a module with violations it prints the number of violations and the number of files, and then one indented line per violation in this form: the file and line of the include, the rule that it breaks and what is not allowed there.

```text
path:line: [rule] detail
```

Fix the include rather than the table: move the code to the layer that may use that header, or pass the dependency in through an interface. Change the table in the script only when the architecture itself changes, and then change the documentation of the layers with it.

For this repository the guard checks four modules: `qar_ar_core`, `qar_ogre_streaming`, `qar_qar_adapter` and `examples`. CTest registers one test for each of them, `check_layering_qar_ar_core`, `check_layering_qar_ogre_streaming`, `check_layering_qar_qar_adapter` and `check_layering_examples`, and the test `check_layering_selftest`. What the rules mean here:

- `qar_ar_core` includes only the standard library, one platform header and its own headers, and it calls no function of the QarOS C API. Only its wrapper header includes the vendored C header.
- `qar_ogre_streaming` includes Ogre and `qar_ar_core`, and it reaches the C types through the wrapper header only.
- `qar_qar_adapter` includes `qar_ar_core` and the two seam headers of `qar_ogre_streaming`, and only the component `render` may include them. Each of the five components includes only its own headers and those of the components that it may use.
- Every folder under `examples` has its own entry. The headless sample may include `qar_ogre_streaming` and `qar_ar_core`; the live example may in addition include the headers of the component `render`.

No module of this repository includes a Qt, ROS or RViz header.

## Adding a test

[Developing](DEVELOPING.md#adding-code-and-tests) explains where a new test goes, how to register it and how to name it.
