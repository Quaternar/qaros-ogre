# qar_ogre_adapter

Two C++ packages that stream an **Ogre** scene to the **QarOS** AR runtime as a
per-eye stereo pair. `qar_ogre_streaming` does the rendering — offscreen stereo
targets, OpenGL texture handoff, scheduling — and knows nothing about QarOS beyond
its value types. `qar_qar_adapter` plugs the QarOS streaming C API in behind that
seam.

Neither package depends on RViz, Qt or ROS, so any Ogre application can use them.
The RViz2 panel that does is in a separate repository,
[`qar_rviz_plugin`](#downstream-consumers).

---

## The QarOS SDK is not included

This repository vendors **only the QarOS C API header**
(`qar_ogre_streaming/third_party/qar/include/qar_streaming.h`, MIT-licensed —
see the `LICENSE` beside it). The QarOS runtime itself — `qar-streaming-c.dll`,
`qar_streaming.dll`, the zenoh and GStreamer stacks, `qar-streaming-viz.exe` — is
**QuaternAR's proprietary binary distribution and is not redistributed here**.
Obtain it from QuaternAR.

You do **not** need the SDK to build this repository or to run its tests. The
adapter resolves every `qar_impl_*` symbol from the DLL at run time
(`QAR_ENABLE_DYNAMIC_LOADING`), so only the header is needed to compile. Point a
running application at the SDK with the `QAR_RUNTIME_BINARIES_DIR` environment
variable.

---

## What is in here

```
qar_ogre_streaming/     The reusable Ogre <-> QarOS streaming library (plain CMake).
  include/ src/           Public headers and implementation.
  test/                   87 unit tests + the dependency-direction guard.
  test_support/           Header-only test doubles, reusable by consumers.
  examples/               A standalone Ogre app that uses the library with no RViz.
  third_party/qar/        The vendored QarOS C API header.
  docs/                   Library architecture, and how to consume it from a
                          non-RViz Ogre app.

qar_qar_adapter/        The QarOS C API implementation of the library's seam.
  include/ src/           Session lifecycle, peers, app volumes, geo-anchoring,
                          per-frame OpenGL submission.
  test/                   20 unit tests over the geodetic maths.

docs/                   Architecture, coordinate conventions, geolocation,
                        testing, known API gaps, pinned toolchain.
scripts/                Sample runner; QarOS header re-vendoring helper.
```

The two packages are separate on purpose. `qar_ogre_streaming` must stay free of
any QarOS *runtime* dependency so it can be tested and reused without one, and free
of rviz/Qt/ament so it can be used outside RViz. A test
(`check_no_rviz_includes`) enforces the second half of that on every run.

---

## Setting up a fresh Windows machine

Verified toolchain (from [`docs/toolchain.md`](docs/toolchain.md), which has the
full detail and the reasoning for each pin):

| Component | Version |
|---|---|
| ROS 2 | Humble, Windows binary distro, release tag `release-humble-20260220` |
| Ogre (via `rviz_ogre_vendor`) | 1.12.1 |
| Python | **3.10.11** — not 3.8; the distro's `.pyd` files are `cp310` |
| VS toolchain | VS2019 Community, MSVC 19.29.30151.0 (v142) |
| CMake | 3.20 or newer (3.21+ to use the bundled presets) |

Ogre is **not** vendored: it comes from the ROS 2 distro's `rviz_ogre_vendor`
package, because that is the exact Ogre build RViz links. Using any other Ogre
would put two Ogre runtimes in one process.

### 1. Clone to a short path

```powershell
git clone <url> C:\dev\qar_ogre_adapter
```

Short genuinely matters. GoogleTest is fetched at configure time and its sub-build
creates paths deep enough to overflow the Windows 260-character `MAX_PATH` limit
from a nested clone. The failure looks like
`ninja: error: Stat(...): Filename longer than 260 characters` during the
googletest populate step and says nothing about your clone path.

### 2. Get the ROS 2 Humble Windows distro

Download and extract the official binary archive, e.g. to
`D:\development\ros2_humble\ros2-windows`. Then either set the environment
variable once:

```powershell
[Environment]::SetEnvironmentVariable("ROS2_INSTALL_ROOT", "D:\development\ros2_humble\ros2-windows", "User")
```

or pass `-DQAR_ROS2_INSTALL_ROOT=<path>` to every configure. Nothing here hardcodes
a machine path.

> The official archive has several packaging gaps that must be patched before RViz
> itself will run. They only matter for the downstream plugin, so the fix scripts
> live in the `qar_rviz_plugin` repository. This repository needs only Ogre, which
> the archive ships correctly.

### 3. Python and colcon

```powershell
& "C:\Program Files\Python310\python.exe" -m venv .venv
.\.venv\Scripts\pip install colcon-common-extensions
```

### 4. Build and test

```powershell
.\scripts\build_adapter.ps1
```

This imports the VS2019 x64 dev environment and sources the ROS 2 distro before
calling colcon — **do not run a bare `colcon build` here**: on Windows, colcon's
CMake task refuses to run outside an imported Visual Studio environment
(`VisualStudioVersion is not set, please run within a Visual Studio Command
Prompt`), and even with that imported, `qar_qar_adapter` needs the distro on
`CMAKE_PREFIX_PATH` in its own separate configure step (colcon builds each
package as its own process — a `-DQAR_ROS2_INSTALL_ROOT` cache variable does not
propagate between them). `scripts/build_adapter.ps1` handles both. Override the
distro location with `-Ros2InstallRoot <path>` if it's not at the default.

Then:

```powershell
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"   # colcon test/test-result need it on PATH too
colcon test
colcon test-result --all
```

Expected:

```
Summary: 108 tests, 0 errors, 0 failures, 0 skipped
```

---

## Building without colcon

Each package is an ordinary CMake project. The library builds and tests on its own
with the bundled presets — this is the fast inner loop:

```powershell
cd qar_ogre_streaming
cmake --preset vs2019-x64 -DQAR_ROS2_INSTALL_ROOT=<distro>\ros2-windows
cmake --build --preset vs2019-x64
ctest --preset vs2019-x64          # 88 tests: 87 unit + the include guard
cmake --install build\vs2019-x64 --config RelWithDebInfo --prefix <prefix>
```

`qar_qar_adapter` then resolves it with `find_package`:

```powershell
cd ..\qar_qar_adapter
cmake -S . -B build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo   # 20 tests
```

See [`docs/testing.md`](docs/testing.md) for what the tests cover and what they
deliberately do not.

---

## Downstream consumers

Both packages install a CMake config package, so a consumer writes:

```cmake
find_package(qar_ogre_streaming REQUIRED)
find_package(qar_qar_adapter REQUIRED)

target_link_libraries(my_target
  qar_ogre_streaming::qar_ogre_streaming
  qar_qar_adapter::qar_qar_adapter
)
```

`qar_ogre_streaming::test_support` is also exported, for consumers that want the
header-only test doubles.

Both carry a `package.xml` with `<build_type>cmake</build_type>`, so colcon builds,
orders and installs them like any other package while they stay ament-free. To
build this repository together with the RViz2 plugin, clone both into one
workspace and build once:

```
ws/
  src/
    qar_ogre_adapter/     <- this repository
    qar_rviz_plugin/
```

```powershell
cd ws
colcon build --cmake-args -DQAR_ROS2_INSTALL_ROOT=<distro>\ros2-windows
```

colcon reads the dependencies from the `package.xml` files and builds
`qar_ogre_streaming` → `qar_qar_adapter` → `qar_rviz_streaming` in that order.

Run this from a shell with the Visual Studio developer environment already
imported (an "x64 Native Tools Command Prompt for VS 2019" from the Start menu, or
`vcvars64.bat` dot-sourced by hand) — colcon's CMake task on Windows fails with
`VisualStudioVersion is not set, please run within a Visual Studio Command Prompt`
otherwise. `scripts\build_adapter.ps1` does this automatically when building this
repository on its own (see above); there is no equivalent single-command wrapper
for the combined two-repository workspace shown here, since its layout is up to
you.

---

## Geolocation

The adapter can anchor a QarOS **app volume** to a real position on Earth, so an AR
client sees the streamed scene where it physically belongs:

```cpp
factory.set_geo_anchor_wgs84(50.0755, 14.4378, 200.0);   // lat, lon, ellipsoidal alt
```

That converts WGS84 geodetic coordinates to an ECEF origin plus a local East/North/Up
basis — which is also the ROS convention — and installs it with
`qar_app_volumes_change_app_world_anchor`. The anchor belongs to the **app volume**,
so a session without one cannot be georeferenced at all.

The conversion lives in `qar_qar_adapter/geo_anchor.hpp` as free functions over
plain doubles, deliberately separate from the session so it can be unit tested
without a QarOS runtime — 20 of the 108 tests cover it, including the NaN a GNSS
receiver reports when it has no lock. Full detail, including why RViz's own
`FrameManager` is the wrong source: [`docs/geolocation.md`](docs/geolocation.md).

**Status: built and unit tested, never exercised against a live GPS receiver.**

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | Layering, the seams, threading and lock discipline. |
| [`docs/testing.md`](docs/testing.md) | What the 108 tests cover, and what is verified by running instead. |
| [`docs/geolocation.md`](docs/geolocation.md) | App-volume georeferencing and the geodetic conversion. |
| [`docs/coordinate_conventions.md`](docs/coordinate_conventions.md) | ROS ↔ Ogre ↔ QarOS coordinate and handedness conventions. |
| [`docs/api_gaps.md`](docs/api_gaps.md) | Ogre and QarOS API limitations found, and the mitigations. Worth reading before filing an SDK bug. |
| [`docs/qar_header_changelog.md`](docs/qar_header_changelog.md) | The old → new QarOS API delta this code reconciled. |
| [`docs/toolchain.md`](docs/toolchain.md) | Exact pinned versions and why each one is pinned. |
| [`qar_ogre_streaming/docs/ogre_library_architecture.md`](qar_ogre_streaming/docs/ogre_library_architecture.md) | The library's public API surface, file by file. |
| [`qar_ogre_streaming/docs/api_usage_from_non_rviz_ogre_app.md`](qar_ogre_streaming/docs/api_usage_from_non_rviz_ogre_app.md) | Consuming the library from a plain Ogre application. |

## License

MIT (see individual file headers). The vendored QarOS header carries its own
license alongside it. The QarOS runtime is proprietary and separately licensed by
QuaternAR.
