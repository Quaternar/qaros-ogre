# Quick start

## Steps

These nine steps take a Windows 11 machine that has the toolchain of [Installation](INSTALL.md#toolchain) installed to a running example. Every command is explained, with its output, in the document that the step names. Run the steps in one PowerShell 7 window, in this order.

1. Create the folders and set the seven variables that all commands use ([Directory layout and environment variables](INSTALL.md#directory-layout-and-environment-variables)).

```powershell
$layout = [ordered]@{
    QAR_ROOT          = 'C:\qar'
    QAR_DEPS_ROOT     = 'C:\qar\deps'
    ROS2_INSTALL_ROOT = 'C:\qar\deps\ros2_humble\ros2-windows'
    QAR_QT_ROOT       = 'C:\qar\deps\Qt\5.15.2\msvc2019_64'
    QAR_VENV          = 'C:\qar\venv'
    QAR_PREFIX        = 'C:\qar\install'
    QAR_SDK_ROOT      = 'C:\Program Files\Quaternar\QAROS'
}
foreach ($name in $layout.Keys) {
    [Environment]::SetEnvironmentVariable($name, $layout[$name], 'User')
    Set-Item -Path "Env:$name" -Value $layout[$name]
}
New-Item -ItemType Directory -Force -Path $layout.QAR_ROOT, $layout.QAR_DEPS_ROOT, $layout.QAR_PREFIX | Out-Null
```

> Success: the block prints nothing and ends without an error. Afterwards the seven variables are set for your Windows user and in the current window, and these folders exist: `C:\qar`, `C:\qar\deps` and `C:\qar\install`. The folder of QAROS is made by its setup program, not by this block.
>
> This command changes your machine, so no output is shown here.

2. Install QAROS with the setup program that you received from QuaternAR, then check the installation. The command prints the version, the install folder and the hash of the C header ([The QarOS SDK](INSTALL.md#the-qaros-sdk)).

```powershell
$qaros = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Quaternar\QAROS'
'Version        {0}' -f $qaros.Version
'InstallFolder  {0}' -f $qaros.InstallFolder
foreach ($file in 'bin\qar-streaming-c.dll', 'bin\qar-runtime-launcher.exe', 'bin\qar-streaming-viz.exe', 'include\qar_streaming.h') {
    '{0,-30} {1}' -f $file, (Test-Path -LiteralPath (Join-Path $qaros.InstallFolder $file))
}
(Get-FileHash -LiteralPath (Join-Path $qaros.InstallFolder 'include\qar_streaming.h') -Algorithm SHA256).Hash
```
```text
Version        0.9.0-dev.3008+65dc9aeb
InstallFolder  C:\Program Files\Quaternar\QAROS
bin\qar-streaming-c.dll        True
bin\qar-runtime-launcher.exe   True
bin\qar-streaming-viz.exe      True
include\qar_streaming.h        True
55E6E3F48D940CA29331F0528FE475C1B7230FB19A59C31D5525C6E90B097F6C
```

3. Clone this repository and go to the folder of the clone; the remaining steps run there ([Python environment](INSTALL.md#python-environment)).

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

4. Download and unpack the ROS 2 Humble distribution, which provides Ogre ([ROS 2 Humble for Windows](INSTALL.md#ros-2-humble-for-windows)). This repository needs none of the repairs that the reminder at the end of the output mentions: the repair scripts in the folder `scripts` serve qar_rviz_plugin.

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

5. Configure the repository with its preset ([Build and install qar_ogre_adapter](INSTALL.md#build-and-install-qar_ogre_adapter)).

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

6. Build the libraries, the sample and the example.

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

7. Run the tests ([Testing](TESTING.md)).

```powershell
ctest --preset vs2019-x64
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
        Start   1: check_layering_qar_ar_core
  1/610 Test   #1: check_layering_qar_ar_core .................................................................   Passed    0.11 sec
        Start   2: check_layering_qar_ogre_streaming
  2/610 Test   #2: check_layering_qar_ogre_streaming ..........................................................   Passed    0.10 sec
        Start   3: check_layering_qar_qar_adapter
  3/610 Test   #3: check_layering_qar_qar_adapter .............................................................   Passed    0.21 sec
        Start   4: check_layering_examples
...
100% tests passed, 0 tests failed out of 610

Total Test time (real) = 117.66 sec
```

8. Run the headless sample. It renders an Ogre scene for three synthetic peers and writes their stereo images; it needs no QarOS SDK ([Run the headless sample](GETTING_STARTED.md#run-the-headless-sample)).

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

9. Check the connection to QAROS. The example joins the QAROS Hub of this PC as a source app: the Hub asks you once to allow the program, so click Allow in the QAROS window. The example prints the runtime, the mode, its peer and the targets, then exits; it opens no window and streams nothing ([Check the connection to QarOS without a window](GETTING_STARTED.md#check-the-connection-to-qaros-without-a-window)). The command starts the Hub when it is not running, so it is not run for this document and its output is not shown.

```powershell
$env:PATH = "$env:ROS2_INSTALL_ROOT\opt\rviz_ogre_vendor\bin;$env:PATH"
& .\build\vs2019-x64\examples\qaros_live_stream\RelWithDebInfo\qar_ogre_live_stream_example.exe --local --connect-only
```

[Getting started](GETTING_STARTED.md) goes on from here and shows how to use the library from your own Ogre application. The live example that streams the sample scene to a Visualizer is [Stream the sample scene to the Visualizer](GETTING_STARTED.md#stream-the-sample-scene-to-the-visualizer); it needs the installed QAROS, which holds the Hub and the Visualizer, and a receiver that you add in the Visualizer.
