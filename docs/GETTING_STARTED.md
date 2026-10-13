# Getting started

## What you will run

Two programs of this repository, both built together with the libraries. They render the same Ogre scene: a ground grid, five coloured cubes and one directional light.

| Program | What it does | Needs | Opens a window |
|---|---|---|---|
| `qar_ogre_standalone_streaming_sample` | the headless sample: renders the scene for synthetic peers that live in the same process and writes their stereo images | Ogre only, no QarOS SDK | no |
| `qar_ogre_live_stream_example` with `--connect-only` | opens a QarOS session, prints the app volume that it created and exits | the QarOS SDK | no |
| `qar_ogre_live_stream_example` | invites a QarOS Visualizer and streams the scene to it | the QarOS SDK, a local desktop session | the Visualizer does |

The last section of this document shows, from the source of the example, how to use the library from your own Ogre application.

## Before you start

Finish [Installation](INSTALL.md), up to and including the build of this repository. The commands below run from the clone and use only the environment variables of the layout. Go to the folder of the clone first:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

Both programs use Ogre as a shared library, and Windows finds a DLL of a program through the folders on `PATH`. The Ogre DLLs of the ROS 2 distribution are in the folder `opt\rviz_ogre_vendor\bin`. The script that runs the sample puts that folder on `PATH` for you; for the example you do it yourself, in the shell that runs it. The command also checks that the DLLs of the render systems are there.

```powershell
$ogreBin = Join-Path $env:ROS2_INSTALL_ROOT 'opt\rviz_ogre_vendor\bin'
$env:PATH = "$ogreBin;$env:PATH"
Get-ChildItem -Path "$ogreBin\OgreMain.dll", "$ogreBin\RenderSystem_GL.dll", "$ogreBin\RenderSystem_GL3Plus.dll" -Name
```
```text
OgreMain.dll
RenderSystem_GL.dll
RenderSystem_GL3Plus.dll
```

The QarOS C API currently supports NVIDIA adapters only. On a machine with an integrated and an NVIDIA adapter, tell Windows to run the example on the NVIDIA adapter. The command writes the per-program graphics preference that the page Settings, Display, Graphics of Windows writes, for the current user and for this one program; delete the value to undo it. It is optional on a machine with a single NVIDIA adapter.

```powershell
$exe = Join-Path $env:QAR_ROOT 'src\qar_ogre_adapter\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe'
$key = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
New-ItemProperty -Path $key -Name $exe -Value 'GpuPreference=2;' -PropertyType String -Force | Out-Null
(Get-ItemProperty -Path $key -Name $exe).$exe
```

> Success: the command ends without an error and prints one line, the preference value that it has just stored for the example program: `GpuPreference=2;`. It is stored for the current user in the registry key HKCU:\Software\Microsoft\DirectX\UserGpuPreferences, in a value that is named with the full path of the example program.
>
> This command changes your machine, so no output is shown here.

## Run the headless sample

The sample brings up its own Ogre, renders the scene for three synthetic stereo peers, writes the stereo images of each peer and exercises the conversion of a rendered frame into the OpenGL frame structure of the QarOS C API. It loads no QarOS library and sends nothing anywhere.

The script `scripts\run_ogre_sample.ps1` runs the built sample with its defaults: 120 render ticks, three peers, 1280 by 720 pixels for each eye, output into the folder `qar_ogre_sample` below your `TEMP` folder.

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

The sample exits with 0 when every check passes, with 1 when a check fails and with 2 when an argument is wrong or Ogre cannot start. For each peer it checks that both eyes contain content, that the depth is present and not uniform, that the two eyes differ, and that the nearer part of the scene shifts more between the eyes than the farther part, as parallax requires.

List what the run wrote.

```powershell
Get-ChildItem -LiteralPath (Join-Path $env:TEMP 'qar_ogre_sample') -Name
```
```text
bridge_demo.txt
peer0_left_color.png
peer0_left_depth.f32
peer0_left_depth.png
peer0_right_color.png
peer0_right_depth.f32
peer0_right_depth.png
peer0_stereo.txt
peer1_left_color.png
peer1_left_depth.f32
peer1_left_depth.png
peer1_right_color.png
peer1_right_depth.f32
peer1_right_depth.png
peer1_stereo.txt
peer2_left_color.png
peer2_left_depth.f32
peer2_left_depth.png
peer2_right_color.png
peer2_right_depth.f32
peer2_right_depth.png
peer2_stereo.txt
qar_sample_ogre.log
summary.txt
```

Each peer has a pair of colour images, `peer0_left_color.png` and `peer0_right_color.png`, which show the same scene from the two eyes with a parallax between them; a pair of depth images that show the distance of each pixel; the raw depth values as `.f32` files, in metres as long as `QAR_DEPTH_ENCODING` is not `ndc` (a later step of this document sets it in the shell, so run the sample before that step or in a new window); and a text file `peer0_stereo.txt` with the pose and the field of view of each eye and the numbers behind the checks. The file `bridge_demo.txt` reports the conversion of the last submitted frame into the OpenGL frame structure of the C API, with the number of textures and views, and `summary.txt` has the result of the whole run.

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

## Check the connection to QarOS without a window

The example is built next to the sample: `build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe`. Its option `--sdk-bin` is required and names the `bin` folder of the QarOS SDK, the folder from which QarOS starts its own launcher and Visualizer. With `--connect-only` the example opens a QarOS session through the session factory of the `render` component, checks it and exits; it starts no Ogre, opens no window and invites nobody.

Run it. The first command line sets `PATH` as above, so that the program starts.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --sdk-bin "$env:QAR_SDK_ROOT\bin" --connect-only
```
```text
[qar-adapter] library logging: severity=warn console=off folder=C:\Users\<user>\AppData\Local\Temp\quaternar
[qar-adapter] zenoh logging: off (QAR_ZENOH_LOG not set)
example: connected to QarOS: ready=1 app_volume=051e412fabf14bc6ac4815c694793fec
```

The session factory starts the QarOS runtime: it loads `qar-streaming-c.dll` from the folder, initialises the library, creates a session, joins it and creates one app volume. `ready` means that this bootstrap succeeded. The factory prints lines that start with `[qar-adapter]` about its own state, and QarOS writes its library log into the folder `quaternar` below your `TEMP` folder. When the program ends, the factory destroys the session, the runtime and the library again.

On success the example prints one line with the identifier of the app volume, 32 hexadecimal digits, and exits with 0. The line has this form:

```text
example: connected to QarOS: ready=1 app_volume=
```

On a failure it prints a line that has this form, with the reason that the factory reports, and exits with 1. The reason for a folder that does not hold `qar-streaming-c.dll` starts with `qar_library_load failed for` and names the file that was tried.

```text
example: QarOS connect failed: %s
```

## Stream the sample scene to the Visualizer

Without `--connect-only` the example brings up Ogre with a hidden window, connects as above, and invites a QarOS Visualizer into the session. QarOS starts the Visualizer itself, from the SDK folder that you passed, and the Visualizer opens its own window. The example then renders the scene for the Visualizer for the number of seconds that `--seconds` gives, 60 by default, and prints a line when the stream of the Visualizer opens and when it closes. When QarOS reports the stream of a peer closed, for example because the peer left, the example drops that stream.

Two environment variables decide whether the Visualizer shows the scene correctly. The Visualizer reads the depth texture as the window-space depth value, so set `QAR_DEPTH_ENCODING` to `ndc`; the default is metric distance, which the Visualizer shows as one flat colour. The scene is Z-up, like an RViz scene, while QarOS is Y-up, so set `QAR_POSE_WORLD` to `zup`, which rotates the head poses of QarOS into the Z-up world. Set both before you start the example: the library reads each of them once for each process, the first time that it needs it.

You start the example yourself, and you look at the Visualizer window; nothing in this repository clicks or moves a window. The maintainers have not run this example in a session with a Visualizer: the libraries ran in the live sessions of qar_rviz_plugin, and [What the live sessions showed](VERIFICATION.md#what-the-live-sessions-showed) says what those sessions showed.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
$env:QAR_DEPTH_ENCODING = 'ndc'
$env:QAR_POSE_WORLD = 'zup'
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --sdk-bin "$env:QAR_SDK_ROOT\bin" --seconds 60
```

The example stops by itself when the time is up and then shuts down in the order that the last section describes. The line that it prints when a stream opens has this form, with the peer identifier in hexadecimal:

```text
example: stream open for peer %s
```

The line for a stream that closes has this form:

```text
example: stream closed for peer %s
```

## Use the library from your own Ogre application

The example is a short program, and every step below is a region of its source, `examples/qaros_live_stream/main.cpp`, which is compiled with the repository. Your application replaces only the Ogre scene: the example takes its scene from the sample.

**1. Link the render component.** The target `qar_ogre_adapter::render` pulls in `qar_ogre_streaming`, `qar_ar_core`, the component `capi` and Ogre. The component `capi` carries the compile definition `QAR_ENABLE_DYNAMIC_LOADING` as a usage requirement, because the program's one loader of the SDK library lives in it. A consumer of the installed package asks for the components with `find_package`; [API](API.md#linking-the-package) shows the command.

```cmake
target_link_libraries(qar_ogre_live_stream_example PRIVATE qar_ogre_adapter::render)
```

**2. Include what you use.** The library headers are plain C++17 headers; Ogre headers are not needed by the application unless it uses Ogre itself, because the library forward-declares its Ogre types. The last include is the scene of the sample.

```cpp
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "../ogre_standalone_streaming_sample/src/sample_scene.hpp"
```

**3. Provide the seams.** The library needs three things from the host: the Ogre scene manager, a sender factory, and an object that tells it whether the calling thread is the render thread. The library only asks the executor, with `is_render_thread()`, whether the calling thread is the render thread, and `render_tick()` refuses to render when it is not. The example renders on the main thread, so the main thread is the render thread.

```cpp
// The main thread is the render thread: the library only asks is_render_thread() and posts nothing.
struct MainThreadExecutor : qos::IRenderThreadExecutor
{
  void post_to_render_thread(std::function<void()> work) override { work(); }
  bool is_render_thread() const override { return std::this_thread::get_id() == thread; }
  const std::thread::id thread = std::this_thread::get_id();
};
```

**4. Configure and open the session.** `QarCApiSenderFactory` is the sender factory of QarOS: constructing it runs the whole bootstrap and blocks until it is done, so a GUI application constructs it on a worker thread. The configuration names the SDK folder, the copy hook that is required in render-into mode, and the log settings of the QarOS library. The copy hook copies the textures that Ogre rendered into the textures that QarOS hands out; without it, QarOS shows a black scene.

```cpp
qqa::QarCApiSenderFactory::Config config;
config.runtime_binaries_folder_path = sdk_bin;      // QarOS starts its launcher and Visualizer from here
config.copy_hook = qqa::make_gl_copy_image_hook();  // copies our textures into the ones QarOS hands out
config.log_severity = QAR_LOG_SEVERITY_WARN;
config.enable_console_logging = false;
```

**5. Check the connection.** A failure of the bootstrap is not thrown: `ready()` says whether it worked and `last_error()` says what failed. The app volume is the box that every stream is keyed to; QarOS rejects a stream without one.

```cpp
auto factory = std::make_shared<qqa::QarCApiSenderFactory>(config);
QarAppVolumeId volume{};
if (!factory->ready() || !factory->app_volume(volume)) {
  std::printf("example: QarOS connect failed: %s\n",
    factory->ready() ? "no app volume (see the lines above)" : factory->last_error().c_str());
  return 1;
}
std::printf("example: connected to QarOS: ready=1 app_volume=");
for (const auto byte : volume.data) { std::printf("%02x", byte); }
std::printf("\n");
```

**6. Create the manager.** `OgreQarStreamingManager` owns the render scheduler and one stereo stream for each peer. It keeps its own reference to the factory, and it throws `std::invalid_argument` when the scene manager, the executor or the factory is missing. The example then clears the factory from its configuration object, so that no reference outlives the shutdown in step 9. Streaming is switched on once, after the manager exists. The optional seams, a host render lock, a clock, a GL fence hook, a diagnostics sink and a lighting rig, keep their defaults: a single-threaded program needs none of them.

```cpp
qos::OgreQarStreamingConfig streaming;
streaming.scene_manager = scene.scene_manager();
streaming.render_thread_executor = std::make_shared<MainThreadExecutor>();
streaming.qar_sender_factory = factory;
auto manager = std::make_unique<qos::OgreQarStreamingManager>(streaming);
streaming.qar_sender_factory.reset();  // the manager holds the factory now
manager->set_streaming_active(true);
```

**7. Run the frame loop on the render thread.** Each pass does four things. It latches the OpenGL context that is current on the thread, because the factory creates a sender only after a context is known. It takes the peers that asked for a stream and opens a stream for each; the sender of a peer is created asynchronously, so the first attempts fail with a state error and the example asks again on the next pass, which keeps the loop running. It renders one batch for all streams that are due. And it closes the streams that QarOS reported closed.

```cpp
const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
while (std::chrono::steady_clock::now() < deadline) {
  factory->capture_gl_context();  // latch the GL context that is current on this thread
  std::vector<QarPeerId> requested;
  factory->poll_new_peer_requests(requested);
  for (const QarPeerId & id : requested) {
    qos::StereoStreamConfig stream;
    stream.peer_id = id;
    const qos::Result created = manager->create_stream(stream);
    if (created.ok()) {
      manager->get_stream(id)->set_active(true);
      std::printf("example: stream open for peer %s\n", qos::peer_id_to_hex(id).c_str());
    } else if (created.status != qos::Status::AlreadyExists) {
      factory->on_render_request(id);  // the sender is still being created: ask again next pass
    }
  }
  if (const qos::Result ticked = manager->render_tick(); !ticked.ok()) {
    std::printf("example: render_tick failed: %s\n", ticked.message.c_str());
    return 1;
  }
  std::vector<QarPeerId> closed;
  factory->poll_closed_peers(closed);
  for (const QarPeerId & id : closed) {
    manager->destroy_stream(id);
    factory->forget(id);
    std::printf("example: stream closed for peer %s\n", qos::peer_id_to_hex(id).c_str());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
```

**8. Close streams that QarOS closed.** `poll_closed_peers()` lists the peers whose stream QarOS reported closed. Destroy the stream, then tell the factory to forget the peer; that frees its place under the limit on concurrent peers, so a client that comes back is admitted again.

```cpp
std::vector<QarPeerId> closed;
factory->poll_closed_peers(closed);
for (const QarPeerId & id : closed) {
  manager->destroy_stream(id);
  factory->forget(id);
  std::printf("example: stream closed for peer %s\n", qos::peer_id_to_hex(id).c_str());
}
```

**9. Shut down in this order.** The manager and its streams go first, because their senders belong to the QarOS session; then the factory, which destroys the session, the runtime and the library; then Ogre.

```cpp
manager.reset();   // the streams and their senders go first,
factory.reset();   // then the QarOS session, runtime and library,
scene.shutdown();  // then Ogre.
```

**Environment settings.** The conversion of head poses and the depth encoding are read from the environment, not from the configuration: `QAR_DEPTH_ENCODING=ndc` for the Visualizer, and `QAR_POSE_WORLD=zup` for content that is Z-up. [Configuration](CONFIGURATION.md) lists every variable that the library reads.

## Headless checks and what needs the real runtime

What you can check without QarOS is limited to three things: the headless sample, which runs the whole Ogre side with synthetic peers; the unit tests; and the layering guard. There is no stand-in for QarOS in this repository: the unit tests cover the parts of the adapter that need no session, and everything that talks to QarOS needs the real runtime: `--connect-only` checks the session, and the streaming run is the check of the frames, which the maintainers have not run with a Visualizer. [Testing](TESTING.md#what-the-automated-tests-do-not-cover) lists what the automated tests leave out.

## Next steps

- [Architecture](ARCHITECTURE.md) explains the layers, the seams and the threads of the library.
- [API](API.md) documents the public interface, including the other adapter components: panels, URL panels and gestures.
- [Configuration](CONFIGURATION.md) lists the options and the environment variables, and the settings that the QarOS Visualizer needs.
- [Troubleshooting](TROUBLESHOOTING.md#qaros-stream-problems) explains what to do when the Visualizer stays empty or black.
