# Configuration

This page lists what you can set: the options of the build, the environment variables that the code and the scripts of this repository read, the rules by which the library reads them, and the configuration structs of the libraries. [Installation](INSTALL.md) explains how the variables of the layout get their values.

## CMake options and cache variables

The table is generated from the CMake files of the repository. For a variable that names a folder it says which environment variable stands in for it.

| Name | Type | Default | Meaning | Defined in |
|---|---|---|---|---|
| `QAR_BUILD_DOC_SNIPPETS` | BOOL | `ON` | Build the compile-checked documentation snippets | `CMakeLists.txt` |
| `QAR_BUILD_EXAMPLES` | BOOL | `ON` | Build the examples under examples/ | `CMakeLists.txt` |
| `QAR_BUILD_TESTS` | BOOL | `ON` | Build the unit tests and the layering guard tests | `CMakeLists.txt` |
| `QAR_INSTALL` | BOOL | `ON` | Generate the install rules and the CMake package | `CMakeLists.txt` |
| `QAR_PREFIX` | STRING | `$ENV{QAR_PREFIX}` | Install prefix of the upstream repositories | `cmake/QarBuildSupport.cmake` |
| `QAR_QT_ROOT` | PATH | unset: the value of the environment variable `QAR_QT_ROOT` is used; the configure step stops when neither is set in the modules that require it | Qt 5.15.2 (msvc2019_64). The folder must contain `lib/cmake/Qt5/Qt5Config.cmake`. | `cmake/QarBuildSupport.cmake` |
| `QAR_ROS2_INSTALL_ROOT` | PATH | unset: the value of the environment variable `ROS2_INSTALL_ROOT` is used; the configure step stops when neither is set | ROS 2 Humble distribution (ros2-windows). The folder must contain `local_setup.bat`. | `cmake/QarBuildSupport.cmake` |
| `QAR_SDK_ROOT` | PATH | `$ENV{QAR_SDK_ROOT}` | Root of the QarOS SDK install tree (bin, include) | `qar_qar_adapter/CMakeLists.txt` |
| `QAR_VENDOR_INCLUDE_DIR` | PATH | `${CMAKE_CURRENT_SOURCE_DIR}/third_party/qar/include` | Folder with the vendored qar_streaming.h | `CMakeLists.txt`, `cmake/QarBuildSupport.cmake` |

The build type is `RelWithDebInfo`, as [Build conventions](DEVELOPING.md#build-conventions) explains. Two compile definitions are usage requirements and not options: `QAR_ENABLE_OPENGL` comes with `qar_ar_core` and `QAR_ENABLE_DYNAMIC_LOADING` with the component `capi`, so that every target that links the component receives them. Nobody defines them by hand.

## Consumer-side variables

A program that uses the installed package sets one CMake variable, and a developer can set a second one:

| Variable | What it does |
|---|---|
| `CMAKE_PREFIX_PATH` | The folders that `find_package` searches. Add the install prefix of the package, the value of `QAR_PREFIX`; [Linking the package](API.md#linking-the-package) shows the call. |
| `-DQAR_OGRE_ADAPTER_SOURCE_DIR=<checkout>` | A developer override for a project that includes `cmake/QarBuildSupport.cmake` of this repository and finds the package with its command `qar_find_upstream`; a plain `find_package` ignores it. Your build then takes the modules that the requested components need from a checkout of this repository instead of an installed package. It installs nothing in that mode, and the checkout must have the version that you request. |

The installed headers are UTF-8 text without a byte order mark, and some comments contain characters outside ASCII; compile your own code with `/utf-8`, as this repository does.

## Environment variables

The table lists every variable that code or a script of this repository reads or sets. Most variables of the library are diagnostics or conversion switches, and a wrong value of one of them raises no error, so read the table together with [Parsing rules and traps](#parsing-rules-and-traps).

| Variable | Read by | Default | Values and parsing | Effect |
|---|---|---|---|---|
| `QAR_ADAPTER_PRIME` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | `true`. | `1`, `true`, `on` or `yes` (any case) is true; `0`, `false`, `off` or `no` is false; anything else, including an empty value, gives the default. The value is not trimmed. | Initial value of the prime flag of a render sender adapter. When the flag is on, the wait for the next frame starts right after a frame is shown, so a frame takes one tick instead of two. `OgreQarStreamingManager` gives every stream that it creates its own flag, `true` until the host calls its `set_prime_enabled`, so the variable decides only for an adapter that no manager configures. |
| `QAR_LOG_SEVERITY` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Unset. | The words that `parse_log_severity` accepts: `trace`, `debug`, `info`, `warn`, `warning`, `error` and `fatal`, in any case and trimmed; numbers and other words are not accepted. The QarOS library obeys this variable over the severity in its init struct, so the factory overwrites it in the process environment with `_putenv_s`, with the severity of `Config::log_severity`, before the library starts. | QarOS-wide log severity. The runtime, and the launcher, mixer and Visualizer processes it starts, inherit it. |
| `QAR_VIDEO_TIMING_DIR` | `qar_ar_core/include/qar_ar_core/timing_log.hpp` | Unset: no timing files. | A non-empty value is a folder, created lazily by the writer thread; an unset or empty value disables the feature. | Opt-in per-frame stage timing: when the variable is set, a background writer thread writes timing CSV files into the folder. |
| `QAR_ZENOH_LOG` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Unset: off. | A null or empty value logs `zenoh logging: off (QAR_ZENOH_LOG not set)` and does nothing. The request is honoured only while the library severity is `trace` or `debug`; otherwise it is logged as ignored. The value is the tracing filter passed to `zc_init_log_from_env_or`. | Starts the zenoh logger inside the process that hosts the library. The zenoh log is the only place that says why a session fails to come up. Windows only. |
| `QAR_ADAPTER_TRACE` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Off. | Presence test (the value of `getenv` is not null): any value, including `0` and `off`, enables the trace; remove the variable to disable it. | Breadcrumb trace on standard error: peer ids; a line with the elapsed time before and after each main call of the session start-up (library load and init, runtime and session creation, join, own peer spec, app volume, peer subscription), of the creation of a sender, of an invitation, of an anchor change and of the first three begin-frame polls; and the per-frame submit steps (every frame up to the 150th, then one in 300). |
| `QAR_ADAPTER_TRACE_ALL` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Off. | Presence test, as `QAR_ADAPTER_TRACE`. | Defeats the one-in-300 sampling of `QAR_ADAPTER_TRACE`: every frame is traced, with four flushed writes per frame. |
| `QAR_ADAPTER_FRAME_TRACE` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Off. | Presence test, read once per process. | Writes a per-frame CSV, `qar_frame_trace_<yyyyMMdd_HHmmss>_<tag>.csv`, into the QarOS log folder; the tag is derived from the peer id. |
| `QAR_ADAPTER_FLIP_Y` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | Off. | Presence test. | Flips Y in the copy from the Ogre texture to the QarOS texture (diagnostic). |
| `QAR_ADAPTER_PROBE_EVERY` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | `600` frames: the probe of the first copied frame is always written, then one every 600 frames, six probes at most. | Read with `strtol` in base 10 without an error check, when the copy hook sets itself up: a value above 0 replaces 600; anything else, including 0 and garbage, leaves only the probe of the first copied frame. | Writes a copy and depth probe to standard error at the first copied frame and then every N copied frames, six probes at most: a `---- probe #<n> at frame <n> ----` header, a depth histogram and an ASCII depth map. |
| `QAR_POSE_WORLD` | `qar_ar_core/src/spatial_frames.cpp`, `qar_ogre_streaming/src/qar_pose_conversion.cpp` | Unset: identity (the library default). A host with a ROS world needs `zup`. | Case-sensitive: exactly `zup` enables the Y-up to Z-up basis change; anything else is identity. The pose conversion reads it once per process; the spatial-pick code reads it on every call. | Conversion between the QarOS world (Y up) and the ROS world (Z up) for head poses and for spatial picks; the two readers must agree. The library default (identity) differs from the value that a ROS host needs (`zup`), so a host that does not set it sees poses in the wrong basis. |
| `QAR_POSE_YAW_DEG` | `qar_ar_core/src/spatial_frames.cpp`, `qar_ogre_streaming/src/qar_pose_conversion.cpp` | `180`. | Read with `std::stof`: a numeric prefix is used, garbage gives the default, `nan` is accepted as NaN. Degrees. | Yaw about the ROS +Z axis, applied after the basis change (`zup` mode only). |
| `QAR_POSE_HEIGHT_M` | `qar_ar_core/src/spatial_frames.cpp`, `qar_ogre_streaming/src/qar_pose_conversion.cpp` | `0`. | Read with `std::stof`; garbage gives the default. | Eye height in metres added along the ROS +Z axis (`zup` mode only). |
| `QAR_POSE_BACK_M` | `qar_ar_core/src/spatial_frames.cpp`, `qar_ogre_streaming/src/qar_pose_conversion.cpp` | `0`. | Read with `std::stof`; garbage gives the default. | Stand-off in metres behind the facing direction; it follows the yaw (`zup` mode only). |
| `QAR_DEPTH_ENCODING` | `qar_ogre_streaming/src/ogre_stereo_render_target.cpp` | `metric` (the library default). The QarOS Visualizer needs `ndc`. | Case-sensitive: exactly `ndc` selects `gl_FragCoord.z`; anything else writes metric eye-space distance in metres. Read once per process. | Encoding of the depth texture. With an SDK drop earlier than that of 2026-09-28 the QarOS Visualizer expected the raw window-space depth value in the range 0 to 1 and inverted it itself, so metric depth rendered wrongly there. The library default differs from the value that the Visualizer needs, so a host that does not set it sends metric depth. |
| `QAR_DEPTH_SHADER_SCALE` | `qar_ogre_streaming/src/ogre_stereo_render_target.cpp` | `1.0`. | Metric mode only: read with `std::stof` and accepted when it is greater than 0, otherwise 1.0. | Multiplier on the metric depth that the depth shader writes. |
| `TEMP` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp`, `scripts/fix_stale_pixi_paths.ps1`, `scripts/qar_locations.ps1`, `scripts/run_ogre_sample.ps1` | None. | The adapter reads `TEMP`, then `TMP`, and uses `<folder>\quaternar` as the QarOS log folder; with neither set it uses the relative folder `quaternar`. Scripts use `$env:TEMP` for scratch files. | Default QarOS log folder and trace folder when the host passes no log folder; scratch folder of several scripts. |
| `TMP` | `qar_qar_adapter/src/qar_capi_render_sender_adapter.cpp` | None. | Read only when `TEMP` is unset. | Fallback of `TEMP` for the default QarOS log folder. |
| `AMENT_PREFIX_PATH` | `cmake/QarBuildSupport.cmake`, `scripts/env_setup.ps1` | None. | A list separated by `;` (`:` on other systems). A program that loads ROS message type support reads it through `_dupenv_s` (MSVC) or `getenv` and searches it after the prefixes that the host adds explicitly. | The ament index of the ROS 2 distribution, through which the type-support libraries of ROS message packages (`<pkg>__rosidl_typesupport_*`) are found. The shared CMake support module prepends the distribution root to it for the configure process, and `scripts/env_setup.ps1` prints its value after it has imported the environment of the distribution. |
| `SYSTEMROOT` | `scripts/fix_old_vc_runtime.ps1` | None. | The repair of the C++ runtime joins it with `System32` (with `Sysnative` in a 32-bit PowerShell on 64-bit Windows) to find the system folder whose runtime files it copies; Windows sets the variable for every process. | Windows folder. Only the repair of the C++ runtime of the ROS 2 distribution reads it, to find the newer runtime files of the system folder; no product module reads it. |
| `ROS2_INSTALL_ROOT` | `cmake/QarBuildSupport.cmake`, `examples/ogre_standalone_streaming_sample/src/main.cpp`, `scripts/env_setup.ps1`, `scripts/fix_missing_eigen.ps1`, and 11 more files | None: a script or the configure step that needs it stops with an error that names the parameter or cache variable and this variable. | A folder. The shared CMake support module uses it when the cache variable `QAR_ROS2_INSTALL_ROOT` is unset or empty; the folder must exist and contain `local_setup.bat`. Scripts use it when the `-Ros2Root` parameter is omitted. The Ogre sample of qar_ogre_adapter takes a non-empty value as the Ogre plugin folder `<root>/opt/rviz_ogre_vendor/bin`. | Root of the ROS 2 Humble Windows distribution: Ogre through `rviz_ogre_vendor`, the ROS command-line tools and the Ogre DLL folder. |
| `QAR_QT_ROOT` | `cmake/QarBuildSupport.cmake`, `scripts/env_setup.ps1` | None: a script or the configure step that needs it stops with an error that names the parameter or cache variable and this variable. | A folder. The shared CMake support module uses it when the cache variable `QAR_QT_ROOT` is unset or empty; the folder must contain `lib/cmake/Qt5/Qt5Config.cmake`. Scripts use it when the `-QtRoot` parameter is omitted. | Root of the Qt 5.15.2 `msvc2019_64` installation. |
| `QAR_DEPS_ROOT` | `scripts/fetch_dependencies.ps1`, `scripts/fix_missing_eigen.ps1`, `scripts/fix_missing_openssl_dll.ps1`, `scripts/fix_missing_spdlog_dll.ps1`, and 2 more files | None: a script that needs it stops with an error that names the parameter and this variable. | A folder. Scripts use it when the `-DepsRoot` parameter is omitted. | Dependency root of the setup kit: the downloads folder and the build trees of the distribution repairs live below it. |
| `QAR_PREFIX` | `cmake/QarBuildSupport.cmake` | None: the configure step continues without it, and a script that needs it stops with an error that names the parameter and this variable. | One folder, or several separated by semicolons (CMake reads the value as a list); every folder must exist. CMake prepends them to `CMAKE_PREFIX_PATH`. Scripts use it when the `-Prefix` parameter is omitted. | Install prefix of the repositories: the libraries are installed into it, and a repository that depends on them finds them through it. |
| `QAR_SDK_ROOT` | `qar_qar_adapter/CMakeLists.txt`, `scripts/fix_old_vc_runtime.ps1`, `scripts/update_qar_headers.ps1`, `CMakePresets.json` | None. | A folder. CMake (the adapter module) uses it when the cache variable `QAR_SDK_ROOT` is unset or empty; an unset or wrong value only gives a configure warning, because nothing links against the SDK. Scripts use it when the `-SdkRoot` parameter is omitted. | Root of the QarOS SDK install tree (`bin`, `include`). It is used by the configure-time check and by the scripts that need the SDK; at run time the DLL is found through the runtime binaries folder that the host passes (`runtime_binaries_folder_path`). |
| `QAR_VENV` | `scripts/env_setup.ps1`, `scripts/fix_missing_openssl_dll.ps1`, `scripts/fix_stale_pixi_paths.ps1` | None: a script that needs it stops with an error that names the parameter and this variable; `fix_missing_openssl_dll.ps1` tries `tar` instead; `fix_stale_pixi_paths.ps1` prints a warning and goes on without the numpy headers. | A folder. Scripts use it when the `-Venv` parameter is omitted; `fix_missing_openssl_dll.ps1` takes `Scripts\python.exe` below it as the Python that unpacks the OpenSSL package when `-Python` is omitted; `fix_stale_pixi_paths.ps1` takes the numpy headers from the numpy package below it when `-NumpySourceInclude` is omitted. | Python 3.10 virtual environment (the folder with `Scripts\python.exe`). |
| `PYTHONPATH` | `cmake/QarBuildSupport.cmake` | None. | The shared CMake support module puts `<ROS2_INSTALL_ROOT>/Lib/site-packages` in front of the existing value for the configure process when that folder exists. | Lets the ament CMake modules import `ament_package` from the Python of the ROS 2 distribution while a project is configured. |
| `QT_QPA_PLATFORM_PLUGIN_PATH` | `scripts/env_setup.ps1` | None. | Read by Qt, not by repository code; set by `scripts/env_setup.ps1`. | Directory of the Qt platform plugins, the folder `platforms`, for the programs that the shell starts. Set by `scripts/env_setup.ps1`. |
| `QT_PLUGIN_PATH` | - | None. | Read by Qt, not by repository code; set by `scripts/env_setup.ps1`. | Directory of the Qt plugins. Set by `scripts/env_setup.ps1`. |
| `PATH` | `scripts/env_setup.ps1`, `scripts/qar_locations.ps1`, `scripts/run_ogre_sample.ps1` | None. | Read by the Windows loader. | The folders in which Windows looks for the DLLs of a program that it starts: the Qt, Ogre and ROS DLL folders must be on it for a program that imports them. The scripts prepend them. Set by `scripts/env_setup.ps1`, `scripts/qar_locations.ps1`, `scripts/run_ogre_sample.ps1`. |

The functions of `env_config.hpp` take the text of a variable from the host and never read the environment, so the variables that a host maps onto them are the variables of that host.

## Parsing rules and traps

A variable of the library can be wrong in silence, because most of the readers keep their default when the text does not fit. These rules say how each kind of variable is read.

| Rule | Variables | What happens |
|---|---|---|
| Presence flag | `QAR_ADAPTER_TRACE`, `QAR_ADAPTER_TRACE_ALL`, `QAR_ADAPTER_FRAME_TRACE`, `QAR_ADAPTER_FLIP_Y` | Any value switches the flag on, `0` and `off` included. Remove the variable to switch it off. |
| Word list | `QAR_ADAPTER_PRIME` | `1`, `true`, `on` and `yes` are true, and `0`, `false`, `off` and `no` are false, in any case and without trimming. Any other text, an empty one included, gives the default, which is true. |
| Exact word | `QAR_POSE_WORLD`, `QAR_DEPTH_ENCODING` | Case-sensitive: only `zup` and only `ndc` switch the behaviour on. Every other spelling, an upper-case one included, silently keeps the identity world and the metric depth. |
| Lenient number | `QAR_POSE_YAW_DEG`, `QAR_POSE_HEIGHT_M`, `QAR_POSE_BACK_M`, `QAR_DEPTH_SHADER_SCALE` | Read with `std::stof` and without looking at the end of the text, so a value that starts with a number is read as that number (`90deg` is 90). Text without a leading number keeps the default and raises no error. |
| Integer | `QAR_ADAPTER_PROBE_EVERY` | Read with `strtol` in base 10 and no check of the end: a value above 0 sets the period of the probes in frames, and 0, a negative number or text leaves only the probe of the first frame. Unset, the period is 600, and at most six probes are written. |
| Folder | `QAR_VIDEO_TIMING_DIR` | An unset or empty variable switches the timing log off. A folder is created by the writer thread when it first writes. |
| Severity word | `QAR_LOG_SEVERITY` | `trace`, `debug`, `info`, `warn`, `warning`, `error` or `fatal`, in any case, trimmed; numbers and other words are not accepted. The QarOS library obeys the variable over the severity that the program passes, so the factory overwrites the variable with the severity that it decided on. |
| Request under condition | `QAR_ZENOH_LOG` | Honoured only while the library severity is `trace` or `debug`. Under every other severity it is ignored and the log says so. |
| Read once | the pose, depth, trace, frame trace and timing variables | Read when first used and kept for the life of the process, so a change after that has no effect. Two exceptions: `room_to_ros_from_environment` reads the pose variables on every call, so that after a change the picker and the camera disagree, and `QAR_ADAPTER_TRACE` is read again each time the library traces a peer id and each time a session factory is constructed, so a change of it reaches those places at once and the rest of the trace only after a restart. |

## Runtime configuration structs

These are the structs that a host fills in code. Every default below is the value that the struct has when you change nothing.

### The configuration of the session factory

`QarCApiSenderFactory::Config` in `qar_capi_render_sender_adapter.hpp`. [Component render](API.md#component-render) shows how a host fills it.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `near_far` | `QarNearFar` | `{0.1, 500.0}` | Near and far clip distances in metres, reported with every frame. The far distance matters for the window-space depth encoding ([Settings the QarOS Visualizer needs](#settings-the-qaros-visualizer-needs)). |
| `depth_scale` | `float` | `1.0` | Reported with every frame as the depth scale. With an SDK drop earlier than that of 2026-09-28, it did not change what the Visualizer showed. |
| `runtime_binaries_folder_path` | `std::string` | empty | The `bin` folder of the SDK, from which QarOS starts its launcher and the Visualizer. Left empty, no launcher starts and peer invites never complete. |
| `submit_mode` | `GlSubmitMode` | `kRenderIntoQarTextures` | Which side owns the textures of a frame; see [Enumerations](#enumerations). |
| `copy_hook` | `std::shared_ptr<IQarGlTextureCopyHook>` | null | The hook that copies the rendered textures into those of QarOS; set it to `make_gl_copy_image_hook()`. Without it the adapter skips the copy and QarOS shows a black scene. |
| `invite_flow_admission` | `bool` | `true` | Arms the allow-list before the peer subscription, so that only invited peers get a stream. With it off every peer except the own one is admitted. |
| `invite_peers` | `std::string` | empty | Devices to invite as soon as the session is up, as `kind[:hostname]` entries separated by commas, for example `hololens:HOLOLENS-A2B3,quest`. A failing entry is logged and skipped. |
| `auto_invite_visualizers` | `uint32_t` | `0` | Visualizer peers to invite at connect. With 0, connecting opens the session and nothing else. |
| `visualizer_conn_suffix` | `std::string` | `auto` | The payload that follows `visualizer:` in the invite. `auto` makes a new UUID for each invite, so that several Visualizers can run; empty makes every invite resolve to one peer; any other text is used as it is. |
| `admission` | `PeerAdmission` | `kInvitedOnly` | Which peers get a stream; see [Enumerations](#enumerations). |
| `admission_name_filter` | `std::string` | empty | The text that the display name of a peer must contain for `kInvitedOrNamed`, in any case. Empty makes that policy `kInvitedOnly`. |
| `baseline_settle_ms` | `uint32_t` | `5000` | For `kInvitedOrNew`: how long after the session is up the peers that appear still count as already there. |
| `max_peers` | `uint32_t` | `8` | The ceiling on peers that stream at the same time. A peer past it is refused with a logged warning. |
| `launcher_config_json` | `std::string` | `{"enableMixer": true}` | The JSON object that the launcher receives. Empty passes NULL, which leaves the choice to the runtime. |
| `create_app_volume` | `bool` | `true` | Creates the one app volume at connect, which every render stream is keyed to. Turning it off does not give a working stream: QarOS rejects a stream without an app volume. |
| `app_volume_name` | `std::string` | `RViz Scene` | Display name of the volume. |
| `app_volume_pick_gestures` | `bool` | `true` | Logs the gesture configuration of the volume, read back from the runtime, at connect. The volume always keeps the gesture configuration that the runtime sets by itself. |
| `app_volume_width_m` | `float` | `10.0` | Width of the box that holds the scene, in metres. The header of QarOS says that nothing clips the content to the box, but the desktop Visualizer draws only the part of the scene inside it, so a large scene needs a large box or a small `app_volume_scale`. |
| `app_volume_length_m` | `float` | `10.0` | Length of the box, in metres. |
| `app_volume_height_m` | `float` | `10.0` | Height of the box, in metres. |
| `app_volume_center_y_m` | `float` | `1.0` | Height of the centre of the box above the origin of room space, y = 0, in metres. The header of QarOS does not say whether that is the floor or the height of the eyes. |
| `app_volume_scale` | `float` | `1.0` | Metres in the room for each metre of the scene. |
| `log_folder_path` | `std::string` | empty | The folder of the log files of the QarOS library. Empty selects the folder `quaternar` below `TEMP`, or below `TMP` when `TEMP` is not set. |
| `log_severity` | `QarLogSeverity` | `QAR_LOG_SEVERITY_TRACE` | The severity of the QarOS library log. The default is trace because QuaternAR asks for full trace-level logs with every bug report; a trace log is large, so a host in normal operation sets `QAR_LOG_SEVERITY_WARN` or a quieter level, as the examples of this repository do. The factory also exports it as `QAR_LOG_SEVERITY`. |
| `enable_console_logging` | `bool` | `true` | Copies every log line of the library to standard output. |
| `sdk_library_path` | `std::string` | empty | The absolute path of `qar-streaming-c.dll`. Empty selects `qar-streaming-c.dll` in `runtime_binaries_folder_path`. |

### The configuration of a stream

`StereoStreamConfig` in `types.hpp`, one for each peer.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `width` | `uint32_t` | `1024` | Width of the texture of each eye, in pixels. |
| `height` | `uint32_t` | `1024` | Height of the texture of each eye, in pixels. |
| `max_fps` | `uint32_t` | `72` | The target frame rate of the stream; 0 is refused. |
| `color_format` | `QarPixelFormat` | `QAR_PIXEL_FORMAT_R8G8B8A8` | Format of the colour textures. |
| `depth_format` | `QarPixelFormat` | `QAR_PIXEL_FORMAT_D32_FLOAT` | Requested depth format. The depth texture that is produced and sent is always `QAR_PIXEL_FORMAT_R32_FLOAT`. |
| `layout` | `QarFrameLayout` | `QAR_FRAME_LAYOUT_SEPARATED_TEXTURES` | Layout of the frame; no other layout is accepted. |

### The render budget

`RenderBudgetConfig` in `render_budget.hpp`, in `OgreQarStreamingConfig::render_budget`. [Render scheduling and budget](ARCHITECTURE.md#render-scheduling-and-budget) says how the scheduler uses it.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `max_batch_milliseconds` | `double` | `16.0` | The soft time budget of one batch for all peers, in milliseconds; 0 is unlimited. |
| `max_streams_per_tick` | `uint32_t` | `0` | A cap on the streams rendered in one tick, which streams under floor protection can exceed; 0 is unlimited. |
| `floor_fps` | `uint32_t` | `30` | A due stream that has not rendered within 1 / `floor_fps` seconds is always admitted; 0 switches that protection off. |
| `default_stream_cost_ms` | `double` | `2.0` | The cost that is assumed for a stream that has never rendered, in milliseconds; the measured cost replaces it. |

### The lighting rig

`SceneLightingConfig` in `qar_scene_lighting.hpp`, passed to `QarSceneLighting`. [Scene lighting](ARCHITECTURE.md#scene-lighting) describes the modes.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `mode` | `SceneLightingMode` | `kOmni` | Which lights the rig adds: `kOff`, `kOmni`, `kPerPeer` or `kOmniAndPerPeer`. |
| `set_ambient` | `bool` | `true` | Raises the ambient term of the scene, so that surfaces that face away from every light keep some shape. |
| `ambient` | `Rgb` | `0.30, 0.30, 0.30` | The raised ambient colour. |
| `key_diffuse` | `Rgb` | `0.75, 0.75, 0.75` | Brightness of the two lights that shine from above. |
| `fill_ratio` | `float` | `0.55` | The two lights from below are the key light multiplied by this; at least 0. |
| `omni_specular` | `Rgb` | `0.2, 0.2, 0.2` | Specular colour of the world-fixed lights. |
| `peer_diffuse` | `Rgb` | `0.35, 0.35, 0.35` | Diffuse colour of the light of each peer, for `kPerPeer`. |
| `peer_specular` | `Rgb` | `0.15, 0.15, 0.15` | Specular colour of the light of each peer. |
| `peer_offset` | `Vec3` | `0, 0, 0` | Offset added to the head position of a peer before its light is placed. |
| `attenuation_range` | `float` | `0` | A value of 0 or less keeps the Ogre default: no falloff with distance. |
| `suppress_light_name` | `std::string` | empty | The name of a light of the host scene, such as the headlight of the host, that the rig hides while it is applied. |

### Enumerations

| Enumeration | Value | Meaning |
|---|---|---|
| `PeerAdmission` | `kInvitedOnly`, the default | Only the peers that this factory invited. Each invite gives one peer, but a Visualizer that joined the session on its own is not admitted. |
| `PeerAdmission` | `kAny` | Every peer except the own one, the internal peers of the runtime included. It is for discovering what a peer set looks like, not for production. |
| `PeerAdmission` | `kInvitedOrNamed` | The invited peers, and the peers whose display name contains `admission_name_filter`. The display names change between updates of one peer, so the policy is of no use today. |
| `PeerAdmission` | `kInvitedOrNew` | The invited peers, and every peer that first appears after `baseline_settle_ms`: the policy for headsets that onboard themselves. It relies on timing. |
| `GlSubmitMode` | `kRenderIntoQarTextures`, the default | QarOS owns the textures of a frame and the adapter copies the rendered textures into them. |
| `GlSubmitMode` | `kSupplyOwnTextureIds` | The adapter passes the ids of its own textures. Against the SDK it cannot work, because QarOS overwrites them. |

### Smaller structs

The defaults of the structs that a host rarely changes.

| Struct | Field | Default | Meaning |
|---|---|---|---|
| `RoomToRosTransform` | `enabled` | `true` | The struct enables the change of axes; `room_to_ros_from_environment` enables it only for `QAR_POSE_WORLD=zup`. |
| `RoomToRosTransform` | `yaw_rad` | `180 degrees` | The yaw of the viewer anchor; `height_m` and `back_m` are 0. |
| `PointingRayInputs` | `min_separation_m` | `0.15` | The eye and the hand must be at least this far apart, in metres, for an eye-through-hand ray. |
| `PickTarget` | `depth_m` | `2.0` | The distance along the ray for the `Depth` target, in metres; the kind of a default target is `GroundPlane`. |
| `PosePickConfig` | `min_drag_m` | `0.05` | How far the drag must travel before it sets a heading, in metres; `commit_without_heading` is true and `default_yaw_rad` is 0. |
| `NavGoalOptions` | `frame_id` | `map` | The frame of the goal; `flatten_orientation` and `project_to_ground` are true and `ground_height_m` is 0. |
| `TimingLog` | `flush_interval` | `1000 ms` | How often the writer thread writes the rows of all sinks. |
| `TimingSink` | `kMaxPendingRows` | `65536` | The rows that a sink keeps in memory; the rows beyond that are counted as dropped. |
| `QarCApiPanelInputSource` | `set_lifecycle_timeout` | `0.75 s` | Seconds of silence after which an open gesture is cancelled; 0 switches the backstop off. |
| `PickVisualStyle` | `ray_length_m` | `5.0` | Drawn length of a ray that hits nothing, in metres; `marker_size_m` is 0.25, `arrow_length_m` 0.60, `arrow_head_m` 0.15 and `circle_segments` 32. |

## Settings the QarOS Visualizer needs

A stream that is correct for the library can still look wrong in the Visualizer. These settings are the ones that a program has to make, and [QarOS integration notes](QAROS_INTEGRATION_NOTES.md) explains the reason of each.

- **Depth.** Set `QAR_DEPTH_ENCODING=ndc`. With an SDK drop earlier than that of 2026-09-28, the Visualizer read the depth texture as the raw window-space value in the range 0 to 1 and inverted it itself. Metric depth, which is what the library writes when the variable is not set, showed as nothing up close and as one saturated colour for the rest, and neither `depth_scale` nor the clip distances changed that.
- **The far distance.** The window-space value is reciprocal in distance, so keep the far distance, `near_far` of the factory configuration, close to the extent of your content: a far plane that is much wider squeezes a scene of the size of a room into the first per cent of the range, and everything beyond it is clipped. The far distance is 500 m unless you set it.
- **The world.** Set `QAR_POSE_WORLD=zup` when your scene is Z-up, as the scene of a ROS program is. QarOS reports poses in a Y-up world, and without the switch the viewer is tipped on its side. `QAR_POSE_YAW_DEG` turns the viewer about the vertical axis and is 180 unless you set it. Leave `QAR_POSE_HEIGHT_M` and `QAR_POSE_BACK_M` at 0: a translation that is applied only on this side is invisible to QarOS, so the two worlds would disagree about where the viewer stands.
- **The copy hook.** Set `copy_hook` to `make_gl_copy_image_hook()`. QarOS owns the textures of a frame, and without the hook nothing writes them, so the Visualizer shows a black scene.
- **The folder of the SDK.** Set `runtime_binaries_folder_path` to the `bin` folder of the SDK. With an empty folder QarOS starts no launcher and the invites of peers never complete.
- **The launcher.** The factory passes `{"enableMixer": true}` to the launcher. QuaternAR suggests `{"enableMixer": false}` as a starting point for debugging, because it removes the mixer and connects your program directly to the Visualizer. Never start the launcher yourself: the runtime does.
- **The graphics adapter.** The QarOS C API supports NVIDIA adapters only, so on a machine with an integrated and an NVIDIA adapter, run the program on the NVIDIA adapter; [Getting started](GETTING_STARTED.md) shows the setting.

## Command-line options of the sample and the example

Both programs print their options with `--help`. They load Ogre as a shared library, so the folder of the Ogre DLLs of the ROS 2 distribution must be on `PATH`, as in [Getting started](GETTING_STARTED.md). Go to the folder of the clone first; the paths of the commands are relative to it:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe --help
```
```text
Usage: C:\qar\src\qar_ogre_adapter\build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe [options]
  --ticks N            render N batched ticks (default 120)
  --peers K            number of synthetic in-process stereo peers, >=2 (default 3)
  --capture-every M    also capture every M ticks (0 = final only; default 0)
  --width W            per-eye texture width (default 1280)
  --height H           per-eye texture height (default 720)
  --ipd-mm X           inter-pupillary distance in mm (default 64)
  --out-dir DIR        output directory for PNGs/reports (default qar_sample_out)
  --render-system S    auto|gl3plus|gl (default auto: legacy GL, else GL3Plus)
  --ogre-plugin-dir D  dir with RenderSystem_GL*/OgreMain (default: env/baked)
  --help               show this help
```

The sample exits with 0 when every check passes, with 1 when a check fails and with 2 for a wrong argument or when Ogre cannot start.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --help
```
```text
Usage: qar_ogre_live_stream_example --sdk-bin DIR [--connect-only] [--seconds N]
  --sdk-bin DIR   bin folder of the QarOS SDK (required)
  --connect-only  open a QarOS session, print its app volume and exit; no window, no peer
  --seconds N     otherwise: invite a Visualizer (it opens a window) and stream for N s (default 60)
```

The option `--sdk-bin` of the example is required, and the example streams for 60 seconds unless `--seconds` says otherwise.

### Parameters of the scripts

`scripts\run_ogre_sample.ps1` runs the built sample with the Ogre folder on `PATH`:

| Parameter | Alias | Type | Default | Meaning |
|---|---|---|---|---|
| `-BuildDir` | - | string | empty | CMake build folder. Default: <repository>\build\vs2019-x64. |
| `-Config` | - | string | `"RelWithDebInfo"` | Build configuration of the sample (default RelWithDebInfo). |
| `-Ticks` | - | int | `120` | Number of batched render ticks (default 120). |
| `-Peers` | - | int | `3` | Number of synthetic in-process stereo peers, at least 2 (default 3). |
| `-CaptureEvery` | - | int | `0` | Also capture every N ticks (0 = final only; default 0). |
| `-Width` | - | int | `1280` | Per-eye texture width (default 1280). |
| `-Height` | - | int | `720` | Per-eye texture height (default 720). |
| `-RenderSystem` | - | string | `"auto"` | auto\|gl3plus\|gl (default auto, which selects the plain GL render system, not GL3+). |
| `-OutDir` | - | string | empty | Output folder for PNGs and reports. Default: %TEMP%\qar_ogre_sample. |
| `-Ros2Root` | `-Ros2DistroRoot`, `-Ros2InstallRoot` | string | empty | ROS 2 Humble distribution root. Default: the environment variable ROS2_INSTALL_ROOT. |

`scripts\update_qar_headers.ps1` copies the QarOS header of an SDK into the repository; [Updating the QarOS header](SDK_COMPATIBILITY.md#updating-the-qaros-header) says when to run it:

| Parameter | Alias | Type | Default | Meaning |
|---|---|---|---|---|
| `-SdkRoot` | `-QarSdkRoot` | string | none | Root of the QarOS SDK install tree (the folder with include\qar_streaming.h). Default: the environment variable QAR_SDK_ROOT. Required. |
| `-RepoRoot` | - | string | `(Resolve-Path "$PSScriptRoot\..").Path` | Root of the repository that receives the header: the script writes third_party\qar\include\qar_streaming.h below it. Default: the parent of the folder that holds this script. |
