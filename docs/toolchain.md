# Toolchain

The exact versions this code is developed and tested against, and why each one is
pinned rather than "whatever is installed".

## Versions

| Component | Version | Why this one |
|---|---|---|
| ROS 2 | Humble, Windows binary distro, release tag `release-humble-20260220` | Only used here as the source of Ogre, via its `rviz_ogre_vendor` package. |
| Ogre (via `rviz_ogre_vendor`) | 1.12.1 ("Rhagorthua") | **Not vendored on purpose.** This is the exact Ogre build RViz2 links. A different Ogre would put two Ogre runtimes in one process the moment the downstream plugin loads. |
| C++ standard | C++17 | Matches RViz Humble. |
| VS toolchain | VS2019 Community, MSVC 19.29.30151.0 (v142) | Matches the distro's compiled toolset. Mixing toolsets across a DLL boundary mixes CRTs. |
| CMake | ≥ 3.20 (≥ 3.21 for the bundled presets) | |
| Python | 3.10.11 | Only needed for colcon. The ROS 2 distro's `.pyd` files carry the `cp310` ABI tag — **not** 3.8, despite what the official ROS 2 Windows documentation says. There is no embedded interpreter in the archive, so an external 3.10 matching that ABI tag must be on `PATH`. |
| GoogleTest | v1.14.0 | Fetched at configure time, pinned to a release tag for reproducibility. Built with `gtest_force_shared_crt=ON` to match the distro's shared MSVC runtime. |
| QarOS C API header (`qar_streaming.h`) | SDK v0.1.1 | Vendored under `qar_ogre_streaming/third_party/qar/`, MIT-licensed. Re-vendor with `scripts/update_qar_headers.ps1`. |

## Build configuration

Always **Release** or **RelWithDebInfo**, never Debug. A Debug build mixes CRT and
Qt runtimes with the Release `rviz2.exe` that eventually loads the downstream
plugin, and fails to load.

## Locating the distro

Nothing here hardcodes a machine path. `find_package(rviz_ogre_vendor)` resolves
through `CMAKE_PREFIX_PATH`, which is satisfied by any one of:

* a sourced ROS 2 environment (the colcon case — nothing extra to do),
* the `ROS2_INSTALL_ROOT` environment variable,
* `-DQAR_ROS2_INSTALL_ROOT=<distro>/ros2-windows`,
* `-DCMAKE_PREFIX_PATH=<distro>/ros2-windows`.

## The QarOS runtime is not a build dependency

The adapter is compiled with `QAR_ENABLE_DYNAMIC_LOADING`, so no import library is
linked and every `qar_impl_*` symbol is resolved from `qar-streaming-c.dll` at run
time. Consequences worth knowing:

* This repository builds and its full test suite passes with **no QarOS SDK
  present at all**.
* At run time the DLL is loaded with `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, which deliberately **excludes the host
  application's own directory**. That is not incidental: the ROS 2 distro ships
  older `MSVCP140`/`VCRUNTIME140` next to `rviz2.exe`, and letting those satisfy
  the SDK's CRT imports crashed `qar_library_init` with an access violation.
* Point a running application at the SDK with the `QAR_RUNTIME_BINARIES_DIR`
  environment variable.

## A note on Windows path length

GoogleTest's configure-time fetch creates paths deep enough to overflow the
260-character `MAX_PATH` limit if the repository is cloned somewhere already deeply
nested. Clone to a short path. The failure reads
`ninja: error: Stat(...): Filename longer than 260 characters` during the
googletest populate step and does not mention your clone path.
