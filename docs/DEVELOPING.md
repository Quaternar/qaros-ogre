# Developing

## Repository layout

The code of this repository is three modules, each in a folder of its own, plus two examples and the vendored files. [Layers and components](ARCHITECTURE.md#layers-and-components) shows how the modules link.

| Path | What it holds |
|---|---|
| `qar_ar_core/` | the Qt-free core: headers in `qar_ar_core/include/qar_ar_core/`, sources in `qar_ar_core/src/`, tests in `qar_ar_core/test/` |
| `qar_ogre_streaming/` | the Ogre stereo streaming library: headers in `qar_ogre_streaming/include/qar_ogre_streaming/`, sources in `qar_ogre_streaming/src/`, tests in `qar_ogre_streaming/test/`, and the header-only test doubles in `qar_ogre_streaming/test_support/` |
| `qar_qar_adapter/` | the QarOS C API adapter in five components: headers in `qar_qar_adapter/include/qar_qar_adapter/`, sources in `qar_qar_adapter/src/` (with one private header), tests in `qar_qar_adapter/test/` |
| `examples/ogre_standalone_streaming_sample/` | the headless sample, which needs no QarOS SDK |
| `examples/qaros_live_stream/` | the live example, which streams the scene of the sample to a QarOS Visualizer |
| `third_party/qar/` | the vendored QarOS header, its licence, and a README that names where both come from |
| `third_party/stb/` | the single header with which the sample writes its PNG files, and its notice |
| `scripts/` | `update_qar_headers.ps1`, `run_ogre_sample.ps1`, the layering guard `check_layering.py`, `fetch_dependencies.ps1`, which [Installation](INSTALL.md) runs, and `qar_locations.ps1`, which the PowerShell scripts share; the repair scripts whose names start with fix_ and `env_setup.ps1` serve qar_rviz_plugin, and this repository needs none of them |
| `cmake/` | `QarBuildSupport.cmake`, the build services that the top-level project calls |
| `docs/` | these documents, `docs/snippets/` with the compile-checked snippets, and `docs/snippets/consumer/`, the install smoke test |

The headers that a consumer includes are the ones in the `include` folders. A header in a `src` folder is private: the only one in this repository is `qar_qar_adapter/src/qar_capi_input_internal.hpp`, which the components `panels` and `gestures` share.

## Layering rules

The layering guard, `scripts/check_layering.py`, enforces the rules of this section ([The layering guard](TESTING.md#the-layering-guard) shows how to run it). The first table lists, for each module, the project modules and the kinds of foreign headers that its production code may include; test code may add GoogleTest.

| Module | Project modules it may include | Foreign headers it may include |
|---|---|---|
| `qar_ar_core` | itself | the standard library, the vendored QarOS header through the wrapper `qar_streaming_c.hpp` only, `share.h` |
| `qar_ogre_streaming` | itself, and `qar_ar_core` through the wrapper only | the standard library, Ogre, `windows.h` |
| `qar_qar_adapter` | itself, `qar_ar_core`, and `qar_ogre_streaming` through the two seam headers `qar_render_sender_adapter.hpp` and `types.hpp`, in the component `render` only | the standard library, `windows.h` |
| `examples/ogre_standalone_streaming_sample` | `qar_ogre_streaming`, `qar_ar_core` | the standard library, Ogre, stb |
| `examples/qaros_live_stream` | the same, and the headers of the component `render` of `qar_qar_adapter` | the standard library, Ogre, stb |

Every file of `qar_qar_adapter` belongs to exactly one component, and a file may include the headers of its own component and of the components in the next table. A file that is in no component fails the guard, so a new file needs an entry (see [Adding an adapter component](#adding-an-adapter-component)).

| Component | May include the headers of |
|---|---|
| `capi` | `capi` |
| `render` | `capi`, `render` |
| `panels` | `capi`, `panels`, the private input header |
| `url` | `capi`, `url` |
| `gestures` | `capi`, `gestures`, the private input header |
| tests of the adapter | `capi`, `render` |

Three further rules hold the picture together: the core calls no function of the QarOS C API and uses its types only, the program has exactly one definition of the loader tables, and no source file but the wrapper header defines `QAR_ENABLE_OPENGL`; the build adds the same macro as a compile definition of `qar_ar_core`, as the usage requirements below say. The reasons are practical. The core and its tests then run without an SDK. The loader tables are a translation unit of their own, so that every component links without the others. And `panels`, `url` and `gestures` need neither Ogre nor Qt, so a host that streams only panels does not pull Ogre in.

## Build conventions

- **Language level and warnings.** The libraries require C++17 as a compile feature, and the warning flags `/W4 /permissive-` come from `qar_apply_warning_flags`. The five adapter components set `/W4` alone.
- **Static libraries.** Every module and every component is a static library with position-independent code, so that a consumer links it into a program or a shared library without a runtime DLL of its own and without Ogre types crossing a DLL boundary.
- **Names.** Every library has an alias of the form `qar_ogre_adapter::<name>`, and a consumer links the aliases only: `qar_ar_core`, `qar_ogre_streaming`, and the components `capi`, `render`, `panels`, `url` and `gestures`. The component targets carry the export name that the alias repeats.
- **Usage requirements.** `QAR_ENABLE_OPENGL` is a public compile definition of `qar_ar_core`, and `QAR_ENABLE_DYNAMIC_LOADING` is a public compile definition of `capi`; every consumer that links the target receives them. Ogre is a public dependency of `qar_ogre_streaming`, because its public headers forward-declare Ogre types.
- **Build type.** Build `RelWithDebInfo`: the build and test presets name that configuration, and a single-configuration generator defaults to it. The reason is the release runtime of the ROS 2 and Qt binaries, which a debug runtime in the same process cannot share.
- **Tests.** GoogleTest 1.14.0 is downloaded at configure time and checked against a hash, and each test executable registers its cases with CTest through `gtest_discover_tests`.

The options and the variables of the configure step are in [CMake options and cache variables](CONFIGURATION.md#cmake-options-and-cache-variables).

## Coding conventions

These conventions apply to every C++ file of this repository. Keep them when you add code; the layering guard and the tests catch some violations, the rest is up to review.

**Language level and warnings.** The code is C++17. Targets are compiled with the warning level `/W4`, and with `/permissive-` where the module sets it.

**Configuration.** Build `RelWithDebInfo`, never Debug. The ROS 2 and Qt binaries that this code links or loads are release builds, and a debug C runtime in the same process crashes on load.

**File header.** Every C++ source and header starts with these two lines, and the year and owner are the same in every file:

```text
// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
```

**Names.** The namespace of a module is the name of the module. Header guards follow the ROS 2 style, the module name in capitals, two underscores, the file name in capitals and a trailing underscore, for example `QAR_QAR_ADAPTER__ENV_CONFIG_HPP_` for the file `env_config.hpp` of the module `qar_qar_adapter`.

**Comments and documentation.** Write in English. A comment states what the code does and why it does it that way, as the code is now. It contains no dates, no milestone or ticket tags, no names of people or tools and no account of how the code came to be. Documentation comments describe the contract of a function or type: what it expects, what it returns, who owns what, and on which thread it may be called.

**Logic that talks to another system.** Code that reads the environment, Qt, ROS 2 or QarOS is kept thin; the decisions it makes are put into functions that take plain values, so that they can be tested without a QarOS session, a window or a ROS 2 graph. For example, the helpers that parse environment variables take the raw text and never read the environment themselves; their callers pass it in. There is no fake of the QarOS SDK.

Four rules of the threads apply to the code of this repository. Everything that touches Ogre or OpenGL runs on the render thread, under the render lock of the host if there is one. The callbacks that QarOS makes on its own threads convert what they receive, queue it and return: they do not block and they do not call a subscribe function. The state that such a callback may see lives in a slot of a process-lifetime arena that is never freed. The one exception is the subscription to peer updates of the session factory: it passes the factory itself and has no slot, and it relies on the destruction of the session and of the library in the destructor of the factory to end its callbacks. And the libraries start no thread of their own except the writer of the timing log, which never touches Ogre or QarOS.

## Adding code and tests

- **A new source file** goes into the `src` folder of its module, and its header into the `include` folder of the module. List the source in the `CMakeLists.txt` of the module; the headers need no entry.
- **A new test** goes into the `test` folder of its module and is listed in the `CMakeLists.txt` there. A case has the form `TEST(Suite, Name)`, and the name says what the case asserts.
- **A decision that depends on QarOS, Ogre or the environment** goes into a function over plain values, in a translation unit that calls no QarOS or Ogre function, and the test calls that function. The libraries have four such units, `ogre_qar_stereo_stream_support.cpp`, `ogre_stereo_render_target_support.cpp`, `qar_scene_lighting_support.cpp` and `spatial_pick_visual_support.cpp`; in the adapter `geo_anchor.cpp` and `env_config.cpp` are such units. The tests of those modules therefore need neither the SDK nor a GL context.
- **There is no fake of the QarOS SDK.** Code that calls the C API has no automated test and is exercised only against the real runtime, as [What the automated tests do not cover](TESTING.md#what-the-automated-tests-do-not-cover) explains.
- **After a change that touches an include,** run the layering guard. A new adapter file, a new module and a new example folder each need an entry in the tables of `check_layering.py`: `ADAPTER_COMPONENT`, `MODULES`, `PROJECT_ROOTS` and `POLICY`, and `EXAMPLE_POLICY`.

## Documentation and snippets

Every block of C++ or CMake in these documents is a copy of a region of a compiled file. The regions are in the files `docs/snippets/*.cpp` (the target `qar_ogre_adapter_doc_snippets`), in `docs/snippets/consumer/CMakeLists.txt`, and in the example `examples/qaros_live_stream/main.cpp`. A region begins at a comment line that holds the word qar-snippet and the name of the region in square brackets, and it ends at a comment line with qar-snippet-end and the same name. The target is built by default, and `-DQAR_BUILD_DOC_SNIPPETS=OFF` skips it. It is never run: the build proves that the code of the documents compiles and links against the current headers.

When you change a signature, build with the snippets on and fix the regions that no longer compile; then update the block of the document by hand, because nothing compares the Markdown with the region. To find the region of a block, search the text of the block in `docs/snippets` and in `examples`. The consumer project `docs/snippets/consumer` is not part of the build of this repository: it needs the installed package, and [Install smoke test](INSTALL.md#install-smoke-test) configures it on its own.

## Ogre build notes

Ogre 1.12.1 comes with the ROS 2 distribution, as the package `rviz_ogre_vendor`, which the module `qar_ogre_streaming` finds with `find_package`. The build locates the distribution through the cache variable `QAR_ROS2_INSTALL_ROOT`, which it takes from the environment variable `ROS2_INSTALL_ROOT` when you do not set it.

The unit tests of `qar_ogre_streaming` never call an Ogre function, because they have no GL context. The test executable is therefore linked with `/DELAYLOAD:OgreMain.dll`, so that it starts without the Ogre DLLs on `PATH`. Two consequences follow for the code of the library:

- **Pure code stays apart from Ogre code.** The parts that need no Ogre function are in the `*_support.cpp` units of the previous section, because a test that references an object file of the library pulls in everything that file references, and with it the import of `OgreMain.dll`.
- **Do not import Ogre data symbols.** An exported data symbol of `OgreMain.dll` defeats the delayed load and fails the link with LNK1194. `Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME`, `Ogre::Vector3::ZERO` and `Ogre::Quaternion::IDENTITY` are such symbols, and some of them hide in defaulted arguments. The code therefore writes the literal group name `"General"` and passes every default argument explicitly.

RViz2 on the distribution runs Ogre's legacy OpenGL render system, not the GL3+ one; [Background: Ogre 1.12 for QarOS engineers](ARCHITECTURE.md#background-ogre-112-for-qaros-engineers) explains what that means for the texture handoff.

## QarOS format and view ids

The numbers of the C API that the code writes into a frame description, as the vendored header defines them:

| Name | Value | Used for |
|---|---|---|
| `QAR_PIXEL_FORMAT_R8G8B8A8` | 51 | the colour views |
| `QAR_PIXEL_FORMAT_B8G8R8A8` | 53 | the alternative colour format that the render target also accepts |
| `QAR_PIXEL_FORMAT_R32_FLOAT` | 2 | the depth views: the depth is written as a colour value of one 32-bit float channel |
| `QAR_PIXEL_FORMAT_D32_FLOAT` | 101 | the default of `StereoStreamConfig::depth_format`, which the render target replaces by `QAR_PIXEL_FORMAT_R32_FLOAT` |
| `QAR_VIDEO_FRAME_VIEW_TYPE_COLOR` | 1 | the data type of a colour view |
| `QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH` | 2 | the data type of a depth view |
| `QAR_VIDEO_FRAME_VIEW_EYE_LEFT` | 10 | the eye of the left views |
| `QAR_VIDEO_FRAME_VIEW_EYE_RIGHT` | 20 | the eye of the right views |
| `QAR_FRAME_LAYOUT_SEPARATED_TEXTURES` | 1 | the only layout that this stack supports |

The eye values are labels of a view, not its position. The sender of this stack declares four views, in this order: left colour, right colour, left depth and right depth, so that the textures have the indices 0 to 3 in that order; it reads the pose and the field of view of the views 0 and 1. A frame can hold at most 8 views and 4 textures.

## Adding an adapter component

A component is a static library of `qar_qar_adapter` that a host can link on its own. To add one:

1. Write the sources in `qar_qar_adapter/src/` and the public headers in `qar_qar_adapter/include/qar_qar_adapter/`.
2. Create the target with the function `qar_adapter_component(<target> <export name> <sources...>)` in the `CMakeLists.txt` of the module. It makes the static library with position-independent code, the alias `qar_ogre_adapter::<export name>`, the export name, the C++17 feature, the public include path and the `/W4` flag.
3. Link `qar_ogre_adapter::capi` publicly, because the loader tables and the slot arena live there, and `qar_ogre_adapter::qar_ar_core`. Link `qar_ogre_adapter::qar_ogre_streaming` only if the component implements a seam of the Ogre library, as `render` does.
4. Enter every file of the component in `ADAPTER_COMPONENT` of `scripts/check_layering.py`, and the components whose headers it may include in `COMPONENT_MAY_INCLUDE`. Without the entry, rule L5 fails.
5. Add the component to the components that the installed package offers, and extend the consumer project in `docs/snippets/consumer` so that the install smoke test links it.
