# Testing

Everything in this repository is covered by **108 automated tests** that need no
GPU, no GL context, no QarOS runtime and no ROS environment beyond the distro that
supplies Ogre. They run in about two seconds.

## Running them

```powershell
.\scripts\build_adapter.ps1
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"
colcon test
colcon test-result --all
```

Use `build_adapter.ps1` rather than a bare `colcon build` — on Windows, colcon's
CMake task requires the Visual Studio developer environment already imported
(`VisualStudioVersion` set) and the ROS 2 distro on `CMAKE_PREFIX_PATH` for
*every* package's own configure step (colcon builds each package as a separate
process). The script does both; see its header comment for why.

Expected last line:

```
Summary: 108 tests, 0 errors, 0 failures, 0 skipped
```

For the tighter inner loop on the library alone, configure it directly with the
bundled preset (no colcon, no workspace):

```powershell
cd qar_ogre_streaming
cmake --preset vs2019-x64 -DQAR_ROS2_INSTALL_ROOT=<distro>\ros2-windows
cmake --build --preset vs2019-x64
ctest --preset vs2019-x64
```

That covers 88 of the 108 (the library's own suite plus the dependency-direction
guard). The remaining 20 belong to `qar_qar_adapter`.

## What the 108 are

| Suite | Cases | Scope |
|---|---:|---|
| `qar_ogre_streaming_tests` | 87 | Config validation, the streaming manager, the mock sender and frame sources, the interactive head-pose state machine, the OpenGL texture bridge, pose conversion, projection maths, the render budget, render-target support, scene lighting, the scheduler and the stereo stream. |
| `qar_qar_adapter_tests` | 20 | The geodetic → ECEF/ENU conversion and the re-anchor distance threshold. See [`geolocation.md`](geolocation.md). |
| `check_no_rviz_includes` | 1 | The dependency-direction guard, below. |

### The dependency-direction guard

`qar_ogre_streaming/test/check_no_rviz_includes.ps1` scans every header and source
under `include/`, `src/`, `examples/`, `test_support/` and `test/` for `#include`
directives naming `rviz_*`, Qt, `ament_*`, `rclcpp` or `rcl`. It fails the build if
it finds one.

This is not busywork. The whole value of `qar_ogre_streaming` is that it is
consumable from any Ogre application, not only from RViz2 — and that property is
very easy to lose accidentally, because during development RViz is always on the
include path. Enforcing it on every `ctest` run is the only way it stays true.

`examples/ogre_standalone_streaming_sample` is scanned for the same reason: it is a
plain Ogre app with no rviz, Qt or ROS dependency that drives the library through
its public API, so if the library ever grew a hidden RViz dependency the sample
would stop building.

## What is deliberately NOT covered

The tests never call a real Ogre function. The library emits real out-of-line Ogre
symbols, but the suites exercise the logic around them with test doubles
(`test_support/`) — there is no GL context in a unit test, so a real render call
would crash rather than fail. On MSVC the test executable therefore **delay-loads**
`OgreMain.dll`, which is what lets it run from a plain shell with no distro
environment sourced at all.

That leaves the real render path — Ogre render targets, GL texture extraction, the
actual QarOS submission — verified by running it, not by unit tests:

- `scripts/run_ogre_sample.ps1` runs the standalone sample, which renders a real
  Ogre scene into offscreen stereo targets and writes PNG readbacks.
- The RViz2 side has its own scripted captures, in the `qar_rviz_plugin`
  repository.

Anything in `qar_qar_adapter` other than the geodetic maths needs a live
`QarSession`, so it is not unit tested. That is why `geo_anchor.{hpp,cpp}` was split
out of the adapter in the first place: extracting the pure arithmetic into free
functions over plain doubles made it testable with zero QarOS dependency. **Where
adapter logic is worth testing, this is the pattern to follow** — move it out of
the session-bound class rather than trying to mock the SDK.

## Note on a fresh clone

Clone to a **short path**. GoogleTest is fetched at configure time, and its
sub-build creates paths deep enough that a clone nested a few directories down
overflows the Windows 260-character `MAX_PATH` limit — the failure surfaces as
`ninja: error: Stat(...): Filename longer than 260 characters` during the
googletest populate step, not as anything that mentions your clone.
