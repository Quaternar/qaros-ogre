# QarOS SDK compatibility

## Supported QarOS SDK drops

A QarOS SDK drop has no version number: its C header carries no version macro, and its libraries carry no version resource. QuaternAR names a drop by its date, and this stack states the drops it supports by those dates.

| SDK drop | Status | Meaning |
|---|---|---|
| 2026-09-28 | verified | The drop that this stack was built, tested and run against. The vendored C header is the header of this drop. |
| 2026-09-21 | minimum | The earliest drop that provides everything the code calls. The four items below set this minimum. |
| earlier drops | not supported | They lack at least one of the four items below. |

The minimum is set by these parts of the C API, all of which the code uses and which the vendored header declares:

- the function `qar_session_id_unique`, which makes a session id that is unique and not zero;
- the function `qar_gui_panels_subscribe_all_gesture_events`, which subscribes to all gesture events of a GUI panel;
- the parameter structure `QarStreamParamsOpenGlWgl`, which hands the Windows OpenGL context of the host to a render stream;
- the field `app_volume_id` of `QarRenderSenderInit`, which must not be NULL: a NULL id is rejected with the status `QAR_STATUS_APP_VOLUME_INVALID_ID`.

At the time of writing, the public QarOS repository <https://github.com/Quaternar/qaros> publishes a later generation of the C API. This stack does not build against it: the session set-up (create, serialise, join and invite), the hand of a gesture, the time points and the init structures of volumes and panels would have to be ported.

## Identifying an SDK drop

Because a drop has no version number, you identify it by the SHA256 of its C header, the file `include\qar_streaming.h` in the SDK folder. Two drops with the same header have the same C API; the hash does not prove that every binary of the two drops is identical.

The first command prints the hash of the header in your SDK folder.

```powershell
(Get-FileHash -LiteralPath (Join-Path $env:QAR_SDK_ROOT 'include\qar_streaming.h') -Algorithm SHA256).Hash
```
```text
5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
```

The header that this stack compiles against is the header of the verified drop, and the second command prints its hash.
This repository vendors the header as `third_party\qar\include\qar_streaming.h`, so the command reads it from there, relative to the folder of the clone.

Go to the folder of the clone first:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

```powershell
(Get-FileHash -LiteralPath 'third_party\qar\include\qar_streaming.h' -Algorithm SHA256).Hash
```
```text
5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
```

If the two hashes are equal, your SDK has the C API of the verified drop. If they differ, your drop is another one. The third command checks whether it still provides the four items that set the minimum drop; it prints True for each item that the header of your SDK declares.

```powershell
$header = Join-Path $env:QAR_SDK_ROOT 'include\qar_streaming.h'
foreach ($name in 'qar_session_id_unique', 'qar_gui_panels_subscribe_all_gesture_events', 'QarStreamParamsOpenGlWgl', 'app_volume_id') {
    '{0,-46} {1}' -f $name, (Select-String -LiteralPath $header -Pattern $name -SimpleMatch -Quiet)
}
```
```text
qar_session_id_unique                          True
qar_gui_panels_subscribe_all_gesture_events    True
QarStreamParamsOpenGlWgl                       True
app_volume_id                                  True
```

A drop that has all four items and a different header is not verified: the stack may work with it, but nobody has checked. Build the repositories, run the tests, and read the changes listed for each drop before you rely on it.

## Updating the QarOS header

This repository owns the QarOS C header that all repositories of this stack compile against: it vendors the file `qar_streaming.h` in `third_party\qar\include`, next to `third_party\qar\LICENSE` and a README that names where both come from. The header is the only part of the SDK that is copied into a repository.

To move to another drop, unpack the new drop as described in [The QarOS SDK](INSTALL.md#the-qaros-sdk), point `QAR_SDK_ROOT` at it, and run the update script of this repository. The script prints the SHA256 of the vendored header and of the header in the SDK, and copies the SDK's header only when the two differ; for the same drop it prints that nothing changed. It does not touch the licence file, because the SDK ships no licence file for the header, so read the licence list of the SDK when the header changes.

```powershell
.\scripts\update_qar_headers.ps1 -SdkRoot $env:QAR_SDK_ROOT
```
```text
Source        : C:\QarOS\install\include\qar_streaming.h
Vendored copy : C:\qar\src\qar_ogre_adapter\third_party\qar\include\qar_streaming.h
SHA256 before : 5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
SHA256 SDK    : 5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
No change: the vendored header already equals the SDK's header.
third_party\qar\LICENSE is kept as it is. The SDK ships no licence file for the header: check third_party_licenses.md of the SDK.
```

### Rebuild order

After the header has changed, rebuild the repositories in the order of their dependencies: qar_ogre_adapter first, then qar_qt_streaming, and qar_rviz_plugin last. Each repository is built and installed into the prefix as described in its installation document, and the later ones find the new header and libraries there.

Rebuild from scratch rather than incrementally: an incremental build can keep objects that were compiled against the old header.

```powershell
cmake --build --preset vs2019-x64 --clean-first
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

## API changes by SDK drop

The table lists the changes of the C header from the minimum drop to the verified drop, grouped by drop. The rows of the minimum drop say what its header declares and what this stack needs from it; the rows of the later drops are the differences between the headers of the drops. The drop that lies between the minimum drop and the verified drop is called "after the minimum drop". Only the rows that concern this repository are shown. In the third column, the adapter is the QarOS C API adapter in qar_ogre_adapter:

| SDK drop | Change in the header | Effect on this stack |
|---|---|---|
| 2026-09-21 (minimum drop) | Panel input is a gesture stream: the per-kind click and hover subscriptions and their flat pointer event are replaced by `qar_gui_panels_subscribe_all_gesture_events`, which takes every kind through one reader, and by the opaque handle `QarGuiPanelGesture`, which is read through accessors. The kinds are hover, tap, long press and drag, each with a phase. Every panel, URL panels included, reports gestures. | The adapter subscribes once per stream panel and reads the handle at the top of the callback. The adapter of URL panels does not subscribe, because the page handles pointer input itself and reports it over its own WebSocket. |
| 2026-09-21 (minimum drop) | The two kind enumerations disagree about the value 0: it is hover for a panel, `QAR_GUI_PANEL_GESTURE_KIND_HOVER`, and click for an app volume, `QAR_GESTURE_CLICK`. | The adapter converts each enumeration with its own function, and the seam pins the values at compile time, so that a renumbering fails the build. |
| 2026-09-21 (minimum drop) | A panel gesture names its hand, `qar_gui_panel_gesture_get_hand`, and counts its taps, `qar_gui_panel_gesture_get_tap_count`. A position, `QarGuiPanelPoint`, holds signed integers. | Every hand of every peer is its own pointer with its own grab, and the adapter reads the tap count. |
| 2026-09-21 (minimum drop) | The gesture of an app volume is the opaque handle `QarAppVolumeGesture`, read through accessors. The subscription for gesture updates is renamed `qar_app_volumes_subscribe_gesture_events`, and `qar_app_volumes_subscribe_all_gesture_events` takes every kind through one reader. | The adapter subscribes once for the volume and dispatches on the kind. |
| 2026-09-21 (minimum drop) | The function `qar_session_id_unique` makes a unique session id. | The adapter uses it for the session that it creates. |
| 2026-09-21 (minimum drop) | The parameters of an OpenGL stream are one structure for each type of context, `QarStreamParamsOpenGlEgl`, `QarStreamParamsOpenGlWgl` and `QarStreamParamsOpenGlGlx`, with a shared `QarOpenGlContextCallbacks`. The single structure for all three types is retired, and the callbacks capture and restore the binding of the thread instead of unbinding it. | The adapter chains `QarStreamParamsOpenGlWgl` and supplies the four callbacks. |
| 2026-09-21 (minimum drop) | The field `app_volume_id` of `QarRenderSenderInit` is required: a NULL id is rejected with `QAR_STATUS_APP_VOLUME_INVALID_ID`. | The adapter creates its volume before any sender and refuses to create a sender without one. |
| 2026-09-21 (minimum drop) | The header documents the frame of a panel: the origin is the midpoint of the top edge, +X runs to the viewer's left, the front normal is -Z, and panel content space runs backwards along both panel axes. | The pose of a panel is passed to the header unchanged, and the hit test of the Qt library uses this frame. |
| after the minimum drop | A section on coordinate systems defines the spaces of the C API: room space, app volume space, app content space, view space, panel space, panel content space and texture pixels. | This stack uses the spaces by these names. |
| after the minimum drop | The view poses, the hand poses and the start and action points of a mapped gesture are in app content space, in app units. The gesture points were stated to be in room or app volume space, in metres. | The pick code uses the points without the placement of the volume. |
| after the minimum drop | The start point is the hand and the action point is the intersection, or the start point when nothing was intersected, so that the pair is the ray that the user points along. An absent point had meant a default such as the centre of the volume. | The adapter prefers the ray from the start point to the action point, and reconstructs a ray from the hands when the two points are equal. |
| after the minimum drop | The deltas of a gesture are in app volume space and shaped by the mapping rule of the volume. They had been a world-space offset or head-local axes, depending on the kind. | The adapter does not read the deltas. |
| after the minimum drop | The default gesture configuration has four rules: hover with a dotted ray and click with a light green ray come after the two manipulation rules, whose rays are green. It had the two manipulation rules only. | A volume that is created without a configuration delivers hover and click gestures with these rays; this stack passes NULL. |
| after the minimum drop | The accessors `qar_app_volume_gesture_get_hand` and `qar_app_volume_gesture_get_gesture_id` are added. | The adapter does not read them. |
| after the minimum drop | The extension `QarAppVolumeGestureAllowedPeersExt`, the getters `qar_app_volume_get_gesture_allowed_peers` and `qar_app_volume_get_gesture_holder`, and the status `QAR_STATUS_APP_VOLUME_GESTURE_CONFIGURATION_INVALID` are added. | They are present and not used. |
| after the minimum drop | `qar_render_sender_last_hands` returns the last sample again for a second and reports untracked hands after that; the hand poses are in app content space, and the radii and velocities of the joints are not converted. | The adapter polls the hands once per frame. |
| after the minimum drop | `qar_render_frame_info_get_view_pose` returns the pose of a view in app content space and `qar_render_frame_info_get_view_fov` the field of view in view space; `rendered_near_far` is in app units, and the sender folds the app scale into the depth itself. | The adapter reads the pose and the field of view of both eyes and reports the near and far distances and the depth scale with every frame. |
| 2026-09-28 (verified drop) | Gesture modes are added: `QarAppVolumeGestureModeId`, the default mode `QAR_APP_VOLUME_GESTURE_MODE_DEFAULT`, the functions that define, remove, select and read modes, `qar_app_volume_gesture_get_mode` for the mode that mapped a gesture, and the status `QAR_STATUS_APP_VOLUME_GESTURE_MODE_UNKNOWN`. The calls that set and read the configuration of a volume address the default mode. | The modes are present and not used. |
| 2026-09-28 (verified drop) | A stream panel ignores `content_scale`: its video is drawn at the full panel width and scrolled vertically when it is taller than the panel. | This stack sends 1.0 by default, so its panels are not affected. |
| 2026-09-28 (verified drop) | The position of a gesture on a stream panel refers to the video as its sender draws it, at the resolution of the sender, and for a video that the renderer has scrolled it is the pixel under the finger. | The adapter normalises the position against the content size that its caller names. |

The SHA256 of the file `include\qar_streaming.h` tells the three headers apart:

| SDK drop | SHA256 of the header |
|---|---|
| 2026-09-21 (minimum drop) | D26892604A186E5CC0AEC149614410F7DD648570FE28510A9D49C65220034C50 |
| after the minimum drop | BF0C8C2DE23584B02E7CC1E96B66282908792477FB48C7990CF7AE4E1E78D0FE |
| 2026-09-28 (verified drop) | 5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA |

## QarOS behaviour this repo relies on

This stack states the QarOS behaviour it relies on in [QarOS integration notes](QAROS_INTEGRATION_NOTES.md): one section for each group of facts, with the calls, the statements of the header and the way the code uses them. Read the sections of the table when you move to another SDK drop, because each of them names an assumption that a change of the header or of the runtime can break.

| Section | What it covers |
|---|---|
| [App content space](QAROS_INTEGRATION_NOTES.md#app-content-space) | the space of view poses, hand poses and gesture points, and why no placement is applied to them |
| [Depth encoding](QAROS_INTEGRATION_NOTES.md#depth-encoding) | what the Visualizer reads from the depth view and what the SDK leaves open |
| [World basis](QAROS_INTEGRATION_NOTES.md#world-basis) | the change of axes between the QarOS world and a Z-up world |
| [Session bootstrap](QAROS_INTEGRATION_NOTES.md#session-bootstrap) | the calls from loading the library to a joined session, and the two paths |
| [App volume](QAROS_INTEGRATION_NOTES.md#app-volume) | the volume that every render stream is keyed to, and its geographic anchor |
| [Peers, invites and admission](QAROS_INTEGRATION_NOTES.md#peers-invites-and-admission) | how peers enter a session and which of them get a stream |
| [Render-into and the GL context](QAROS_INTEGRATION_NOTES.md#render-into-and-the-gl-context) | who owns the textures of a frame, the shared WGL context and the begin-frame wait |
| [Gesture rules and the default configuration](QAROS_INTEGRATION_NOTES.md#gesture-rules-and-the-default-configuration) | the mapping rules of an app volume and the default that this stack takes |
| [The pointing ray](QAROS_INTEGRATION_NOTES.md#the-pointing-ray) | start point and action point, and the fallback ray |
| [Panel pixel coordinates and content_scale](QAROS_INTEGRATION_NOTES.md#panel-pixel-coordinates-and-content_scale) | the pixel raster of a panel and the zoom that is always 1.0 |
| [Panel space](QAROS_INTEGRATION_NOTES.md#panel-space) | the frame of a panel pose |
| [Panel input lifecycle](QAROS_INTEGRATION_NOTES.md#panel-input-lifecycle) | the gestures of a panel, their lifecycles and the liveness rule |
| [Teardown and destroy order](QAROS_INTEGRATION_NOTES.md#teardown-and-destroy-order) | the order in which panels, subscriptions, senders and the session are retired |
| [Hands and view poses](QAROS_INTEGRATION_NOTES.md#hands-and-view-poses) | the hand sample and the pose of a view |
| [Logging control](QAROS_INTEGRATION_NOTES.md#logging-control) | the severity, the console copy and the log folder of the library |
| [The in-AR browser for URL panels](QAROS_INTEGRATION_NOTES.md#the-in-ar-browser-for-url-panels) | what the SDK does not say about the browser of a URL panel |
| [Known SDK limitations](QAROS_INTEGRATION_NOTES.md#known-sdk-limitations) | the limits of the SDK that this stack works around |

The build catches some changes of the header by itself, as [The vendored header](#the-vendored-header) explains. It cannot catch a change of behaviour: what the Visualizer reads from the depth view, which geometry the runtime intersects a gesture with and how a receiver reacts to a destroyed sender are observations of the authors of qar_ogre_adapter or gaps of the documentation, and only a live run shows whether they still hold.

## The vendored header

The header of the verified drop is vendored without any change as `third_party/qar/include/qar_streaming.h`. `third_party/qar/LICENSE` next to it is the MIT licence of the public QarOS repository <https://github.com/Quaternar/qaros>, which publishes the header, and the SDK drop ships no licence file for it. [The README of the folder](../third_party/qar/README.md) names the origin of both files. The header calls itself auto-generated and says that it must not be edited, so a change of the C API comes with a new SDK drop and is brought in with the update script that [Updating the QarOS header](#updating-the-qaros-header) describes.

Exactly one file of this repository includes the header: the wrapper `qar_ar_core/include/qar_ar_core/qar_streaming_c.hpp`, and the layering guard rejects every other include of it (rule L3). The reason is that the header has one include guard, so its first inclusion in a translation unit fixes which optional parts exist. On Windows the header enables only Direct3D 11 by itself, and the OpenGL types that the component `render` needs exist only when `QAR_ENABLE_OPENGL` is defined before the header is included. The wrapper defines it, and `qar_ar_core` also exports it as a public compile definition, so that no order of includes can produce a translation unit without it. The wrapper also silences the two warnings that the generated header raises at warning level 4, the removal of an unreferenced local function and an unreferenced formal parameter, so that they do not appear in the build of this repository. It does not define `QAR_ENABLE_DYNAMIC_LOADING`: that definition is a usage requirement of the component `capi`, which holds the function tables of the program ([Component capi](API.md#component-capi)).

A header that renumbers an enumeration that this stack maps must not go unnoticed, so the values are pinned at compile time. The input seam checks that the gesture kinds of an app volume, the gesture kinds of a panel and the gesture phases still have the values that its mapping functions were written for, and the session factory checks the same for the log severities. If one of these checks fires, read the new header and change the mapping before you change the pinned number: a stale mapping would turn a hover into a click.

The vendored header declares the four parts of the C API that set the minimum drop. The command looks for each of them in the vendored file, whose path is relative to the folder of the clone, and prints True when it finds it.

```powershell
$header = 'third_party\qar\include\qar_streaming.h'
foreach ($name in 'qar_session_id_unique', 'qar_gui_panels_subscribe_all_gesture_events', 'QarStreamParamsOpenGlWgl', 'app_volume_id') {
    '{0,-46} {1}' -f $name, (Select-String -LiteralPath $header -Pattern $name -SimpleMatch -Quiet)
}
```
```text
qar_session_id_unique                          True
qar_gui_panels_subscribe_all_gesture_events    True
QarStreamParamsOpenGlWgl                       True
app_volume_id                                  True
```

The hash of this file is the hash of the verified drop that [Identifying an SDK drop](#identifying-an-sdk-drop) prints. The installed package carries the header and its licence, so that a program that links the package compiles against the same file as the libraries; [What the package installs](API.md#what-the-package-installs) lists the files.

## Runtime deployment

The QarOS runtime is not a build dependency of this repository. Nothing links an import library, so the libraries build and the tests run without an SDK, and the configure step only warns when it cannot find the library ([CMake warns that the QarOS SDK DLL was not found](TROUBLESHOOTING.md#cmake-warns-that-the-qaros-sdk-dll-was-not-found)). The library is loaded when a program connects, from the SDK folder that you name.

**Deploy the folder whole.** The SDK is one folder, `bin`, with the library, the launcher and every dependency of the two. Deploy it whole and unmodified, and do not copy single files out of it. A program of this repository takes the folder from `QarCApiSenderFactory::Config::runtime_binaries_folder_path`, which the live example fills from its option `--sdk-bin`, and derives the library file `qar-streaming-c.dll` from it unless `Config::sdk_library_path` names a file. The folder matters twice: the runtime starts its launcher from it, and an empty folder means that no peer can ever be invited. A relative path resolves against the current working directory, so derive both paths from the install location. If the program has already loaded the library itself, the factory does not load it again, and it unloads the library only when it was the one that loaded it.

**Where Windows looks for the dependencies.** The loader of the vendored header loads the library by its full path with the flags `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` and `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`. For the dependencies of the library, Windows then searches, in this order, the folder of the library, the folder of the program, the folders that the program added itself and the system folder. It does not search the current folder or the folders of the `PATH` environment variable, so putting the SDK folder on `PATH` achieves nothing. The SDK folder is searched before the folder of the host program, which is why it must be whole: a runtime file that is missing from it is taken from the folder of the program. Microsoft describes the order in [Dynamic-link library search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order).

**The C++ runtime.** QarOS needs a Microsoft C++ runtime of version 14.40 or newer. The authors of qar_ogre_adapter observed that an older copy of the runtime files next to the host program, as the ROS 2 distribution ships beside `rviz2.exe`, crashed `qar_library_init` with an access violation. The header recommends loading the library dynamically so that its dependencies resolve out of the SDK folder instead of out of the application's folder, and this stack loads it that way. That describes the order of the search and is no guarantee: before Windows searches, it also checks the modules that the process already holds, and it uses a module of the same name whichever folder that module came from. If `qar_library_init` crashes inside a `MSVCP140.dll` that does not lie in the SDK folder, see [The QarOS library crashes while it starts](TROUBLESHOOTING.md#the-qaros-library-crashes-while-it-starts).

**The launcher.** The runtime starts `qar-runtime-launcher` from the folder itself, and QuaternAR states that a host never starts it: do not start it by hand. The JSON that the session factory hands to the launcher is `Config::launcher_config_json`, which is `{"enableMixer": true}` unless you change it; [Session bootstrap](QAROS_INTEGRATION_NOTES.md#session-bootstrap) explains the setting.

**The graphics adapter.** The C API supports NVIDIA adapters only, and QarOS encodes the frames on the NVIDIA GPU. On a machine with an integrated and an NVIDIA adapter, give the program the NVIDIA preference as [Getting started](GETTING_STARTED.md#before-you-start) shows; otherwise Windows may render on the integrated adapter and every frame is copied between adapters.

QarOS writes its own log files; [Reading the logs](TROUBLESHOOTING.md#reading-the-logs) says where and how loud.
