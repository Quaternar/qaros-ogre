# Troubleshooting

This page helps when a step of this documentation fails or when a stream does not look right. Start with the symptom index: it names every entry of the page by the symptom that you see. [Reading the logs](#reading-the-logs) says where the logs are and what the lines of this repository mean, and [What to send us](#what-to-send-us) says what to attach to a report.

## Symptom index

| Symptom | Entry |
|---|---|
| Configuring stops with an error about a file name or a path that is too long | [A deep folder breaks the configure step](#a-deep-folder-breaks-the-configure-step) |
| PowerShell says that running scripts is disabled | [PowerShell refuses to run a script](#powershell-refuses-to-run-a-script) |
| A command fails with a syntax error in a window titled Windows PowerShell | [A command of this documentation fails in Windows PowerShell](#a-command-of-this-documentation-fails-in-windows-powershell) |
| `pip`, `aqt`, `git` or `winget` stops with a certificate error | [A download fails with a certificate error](#a-download-fails-with-a-certificate-error) |
| `cmake --preset vs2019-x64` ends with a fatal error about GoogleTest | [Configuring stops because GoogleTest is not available](#configuring-stops-because-googletest-is-not-available) |
| A script says that it cannot find Visual Studio 2019 | [The scripts cannot find Visual Studio 2019](#the-scripts-cannot-find-visual-studio-2019) |
| Configuring warns that `QAR_SDK_ROOT` is not set or that the SDK DLL was not found | [CMake warns that the QarOS SDK DLL was not found](#cmake-warns-that-the-qaros-sdk-dll-was-not-found) |
| Creating a QarOS session fails with an error that names no port | [Creating a QarOS session fails because UDP port 7446 is held](#creating-a-qaros-session-fails-because-udp-port-7446-is-held) |
| The process ends with an access violation inside `qar_library_init` | [The QarOS library crashes while it starts](#the-qaros-library-crashes-while-it-starts) |
| The AR picture stutters when another window covers the source window | [The stream stutters when the source window is covered](#the-stream-stutters-when-the-source-window-is-covered) |
| The frame rate is low on a laptop with two graphics adapters | [The picture is rendered on the wrong graphics adapter](#the-picture-is-rendered-on-the-wrong-graphics-adapter) |
| The log folder is huge, or all streams stall every 15 seconds | [Logs grow very large, or all streams stall every 15 seconds](#logs-grow-very-large-or-all-streams-stall-every-15-seconds) |
| The Visualizer receives frames and shows a black scene | [The Visualizer shows a black scene](#the-visualizer-shows-a-black-scene) |
| The depth shows one saturated colour, or nothing up close | [The depth shows one flat colour](#the-depth-shows-one-flat-colour) |
| The viewer starts above the scene, or the scene lies on its side | [The scene is tipped on its side](#the-scene-is-tipped-on-its-side) |
| The sender log has *Could not load OpenGL function*, or the Visualizer starts its receiver again and again | [The QarOS graphics thread has no OpenGL context](#the-qaros-graphics-thread-has-no-opengl-context) |
| `create_sender` returns null for a long time, or the log says that a peer is still creating | [A sender is not created](#a-sender-is-not-created) |
| The program waits for the Hub, or the Hub does not let it in | [The program does not join the Hub](#the-program-does-not-join-the-hub) |
| A Visualizer or a device never appears as a target | [A target never appears](#a-target-never-appears) |
| A target is in the session and gets no stream | [A target gets no stream](#a-target-gets-no-stream) |
| The log says that no render stream can be created without an app volume | [No render stream without an app volume](#no-render-stream-without-an-app-volume) |
| `last_error()` says that `qar_library_load` failed | [The QarOS library does not load](#the-qaros-library-does-not-load) |
| The log shows the codes 603 or 604 | [The codes 603 and 604](#the-codes-603-and-604) |
| The log of the Visualizer shows the code 507 every ten seconds | [Code 507 every ten seconds](#code-507-every-ten-seconds) |
| The log says that a backend was rejected | [The log says that a backend was rejected](#the-log-says-that-a-backend-was-rejected) |
| The sample or the example ends at once and Windows names a missing DLL | [The program cannot find the Ogre DLLs](#the-program-cannot-find-the-ogre-dlls) |
| The sample or the example reports that Ogre cannot start | [Ogre cannot start](#ogre-cannot-start) |
| The sample ends with the exit code 1 | [The sample reports a failed check](#the-sample-reports-a-failed-check) |
| The linker stops with LNK1194 | [The linker reports LNK1194](#the-linker-reports-lnk1194) |
| The example prints that the connection to QarOS failed | [The example cannot connect to QarOS](#the-example-cannot-connect-to-qaros) |

## Installation and build problems

### A deep folder breaks the configure step

**Symptom.** Configuring fails while it downloads or unpacks GoogleTest, with an error about a file name or path that is too long (the Windows limit is 260 characters). **Cause.** The repository was cloned into a deeply nested folder, so the paths below its build folder exceed the limit. **Fix.** Use the short layout of this documentation, `C:\qar\src\<repository>`. Enabling long paths in Windows also removes the limit, but it is a machine-wide policy that this stack neither needs nor changes.

### PowerShell refuses to run a script

**Symptom.** PowerShell reports that running scripts is disabled, or asks whether you want to run a script that came from the internet. **Cause.** The execution policy of your machine blocks unsigned scripts, or the files carry the mark that Windows puts on downloads. **Fix.** Start the script through `pwsh` with its `-ExecutionPolicy` option set to `Bypass`, or remove the mark with `Unblock-File`; [Toolchain](INSTALL.md#toolchain) explains both.

### A command of this documentation fails in Windows PowerShell

**Symptom.** A command fails with a syntax error, or with an unknown command or parameter, in a window titled Windows PowerShell. **Cause.** Every command of this documentation was run in PowerShell 7 (`pwsh`); Windows PowerShell 5.1 is not what it was written and recorded for. **Fix.** Start `pwsh` and run the command there. The helper file `scripts\qar_locations.ps1` is the one script that states that it works in both editions.

### A download fails with a certificate error

**Symptom.** `pip`, `aqt`, `git` or `winget` stops with a certificate verification error, or CMake cannot download GoogleTest. **Cause.** Your network inspects TLS traffic and re-signs it with a company certificate that these tools do not trust by themselves. On the reference machine behind such a network, the downloads of `scripts\fetch_dependencies.ps1` worked, because they use the Windows certificate store, while the tools below needed the following settings.

- `pip`: point it at a bundle that contains your company root certificate with its `--cert` option.
- `aqt` and other Python programs: set the environment variables `SSL_CERT_FILE` and `REQUESTS_CA_BUNDLE` to the same bundle.
- `git`: tell Git to use the Windows certificate store by setting its option http.sslBackend to schannel. This changes your global Git configuration; undo it by unsetting the option again.
- `winget`: install with `--source winget`, as the commands of [Toolchain](INSTALL.md#toolchain) do. The default search also queries the Microsoft Store source, which can fail with a certificate error on such networks.
- CMake: download the GoogleTest archive in another way and use the offline option of the next entry.

The command below writes the root certificates of the Windows machine store into a bundle in PEM format and prints how many certificates it holds.

```powershell
$pem = foreach ($cert in Get-ChildItem -Path Cert:\LocalMachine\Root) {
    "-----BEGIN CERTIFICATE-----`n" + [Convert]::ToBase64String($cert.RawData, 'InsertLineBreaks') + "`n-----END CERTIFICATE-----"
}
$bundle = Join-Path $env:QAR_ROOT 'ca-bundle.pem'
Set-Content -Path $bundle -Value $pem -Encoding ascii
(Select-String -Path $bundle -Pattern 'BEGIN CERTIFICATE' | Measure-Object).Count
```
```text
76
```

### Configuring stops because GoogleTest is not available

**Symptom.** `cmake --preset vs2019-x64` ends with a fatal error that says GoogleTest 1.14.0 is not available and names network access to GitHub. **Cause.** Every repository downloads GoogleTest 1.14.0 as an archive at configure time and checks its SHA256, `8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7`; without access to `github.com` the download fails. **Fix.** Download `https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz` on a machine that has access, check the SHA256, unpack it, and configure with `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST` set to the unpacked folder `googletest-1.14.0`.

### The scripts cannot find Visual Studio 2019

**Symptom.** A script stops because `vswhere` finds no Visual Studio 2019 with the C++ x64 build tools. **Cause.** Only a newer Visual Studio is installed, or the Build Tools were installed without the C++ workload and the MSVC component; the scripts accept only a 16.x installation. **Fix.** Install the components listed in [Toolchain](INSTALL.md#toolchain) and run its verify command; a newer Visual Studio next to it does no harm.

### CMake warns that the QarOS SDK DLL was not found

**Symptom.** Configuring prints a warning that `QAR_SDK_ROOT` is not set, or that the SDK DLL was not found below it, and then continues. **Cause.** Nothing links against the SDK; CMake only checks that the DLL exists where you point it. **Fix.** None is needed to build and test. To make the warning disappear, unpack the SDK as described in [The QarOS SDK](INSTALL.md#the-qaros-sdk) and set `QAR_SDK_ROOT`.


## Run-time problems

### Creating a QarOS session fails because UDP port 7446 is held

**Symptom.** Creating the session fails with a QarOS error that names neither the port nor the program that holds it.
**Cause.** With a QAROS runtime earlier than the verified version, QarOS used UDP port 7446 for its discovery (zenoh scouting) and needed it free; the installed QAROS does not document a port for it. ROS 2 participants on domain 0, the default domain, take UDP ports in steps of two upward from 7410, so after about nineteen participants, for example test publishers that were never stopped, one of them holds 7446. On the domain with the number N the steps start at 7410 + 250 x N, so only domain 0 reaches 7446. **Fix.** Find the holder and stop that program yourself. The command prints the processes that hold the port, or says that it is free.

```powershell
$holders = Get-NetUDPEndpoint -LocalPort 7446 -ErrorAction SilentlyContinue
if ($holders) {
    $holders | ForEach-Object { 'UDP 7446 is held by process {0} ({1})' -f $_.OwningProcess, (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName }
} else {
    'UDP 7446 is free'
}
```
```text
UDP 7446 is free
```

### The QarOS library crashes while it starts

**Symptom.** The process ends with an access violation inside `qar_library_init`, and the call stack is in a `MSVCP140.dll` that lies next to the program. **Cause.** QarOS needs a Microsoft C++ runtime of version 14.40 or newer, and the process was using an older copy that lies next to the host program. The authors of this stack observed this in qar_rviz_plugin, where the older copy is the one that the ROS 2 distribution ships beside `rviz2.exe`.
**What the loader of this stack does.** The adapter loads `qar-streaming-c.dll` by its full path, with the flags `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and `LOAD_LIBRARY_SEARCH_SYSTEM32` that the loader of the vendored header sets. Windows then searches for the dependencies of the library in the folder of the library first and then in the system folder, and never in the folder of the host program, the current folder or `PATH`. The runtime files in the QAROS `bin` folder therefore come before an older copy in the folder of the host program, and a runtime file that is missing from that folder is not taken from the folder of the host program. This is the order of the search and no guarantee: Windows uses a module of the same name that the process already holds, whichever folder it came from, so a runtime that the host program has already loaded stays in use.
**Fix.** In qar_rviz_plugin the authors of this stack got past the crash by replacing the runtime files next to `rviz2.exe` with the same files of version 14.4x from the system folder. If the crash remains, report it together with the versions of the `MSVCP140.dll` files on your machine.

### The stream stutters when the source window is covered

**Symptom.** The AR picture is smooth while the source window is in front and stutters, with a steady frame rate that is a fraction of the target, when another window, such as a full-screen Visualizer, covers it. **Cause.** From Windows 11 on, a process whose windows are fully covered may no longer get the timer resolution it requested.
**Fix.** Keep the source window visible, for example side by side with the Visualizer.

### The picture is rendered on the wrong graphics adapter

**Symptom.** On a laptop with an integrated and a discrete GPU, the frame rate is low or each frame is copied between adapters. **Cause.** Windows ran the program on the integrated GPU, while QarOS encodes on the NVIDIA GPU.
**Fix.** Set the graphics preference of the program to high performance in Windows Settings, under System, Display, Graphics.

### Logs grow very large, or all streams stall every 15 seconds

**Symptom.** The QarOS log folder holds files of a gigabyte or more, or every stream stops for about a second and a half at a steady 15-second rhythm. **Cause.** The library log severity is trace, which writes hundreds of thousands of lines in a short run, and a real-time virus scanner or a nearly full system drive turns that into stalls.
**Fix.** Run with the severity warn or higher, and use trace only for a log that QuaternAR asks for; [Reading the logs](#reading-the-logs) explains how the severity is decided.
The examples of this repository run at warn. The default of `log_severity` in the configuration of the session factory is trace, so a host of your own sets it to warn or higher, as the examples do.

## Reading the logs

QarOS writes its own logs, separate from the logs of this stack. When something fails between your program and QarOS, the cause is often only in the QarOS logs, so you need to know where they are and how loud they are.

### Where the QarOS library writes its logs

The library writes its log files to the folder `quaternar` inside your `TEMP` folder, `%TEMP%\quaternar`, unless the program that initialises the library names another folder; an empty folder name selects the default. Every QarOS process that the library starts writes its own files, and the file names carry the role of the process and a GUID, for example `mixer_<guid>_stdout.log`. QuaternAR asks for full trace-level library logs with every bug report.

### Which severity applies

The library reads its severity from the environment variable `QAR_LOG_SEVERITY`, through the environment table of the C runtime, and that variable wins over the severity field of the library initialisation, in both directions: the field cannot make a run quieter than the variable, and the variable cannot be overridden by the field. A change made with the Windows function that sets an environment variable is not seen by the library; the adapter of this stack therefore sets the variable with the C runtime function before it initialises the library, and also passes the severity in the initialisation structure. The library accepts the spellings trace, debug, info, warn, warning, error and fatal, in any letter case, and no others; this stack accepts the same spellings.

### How much a trace log writes

A run at trace severity writes hundreds of thousands of lines in a short time. Use trace only for the log that QuaternAR asks for, delete it afterwards, and run at warn or higher otherwise. [Installation and build problems](#installation-and-build-problems) and [Run-time problems](#run-time-problems) describe what a large log does to a machine.

### The log of the transport

QarOS uses zenoh as its transport. When a session cannot be created, the C API reports only a generic transport error; the actual reason, such as a port that cannot be bound, is in zenoh's own log, which stays silent unless it is switched on. The adapter of this stack switches it on when the environment variable `QAR_ZENOH_LOG` holds a tracing filter, such as debug, and only while the library severity is trace or debug; at a quieter severity the request is ignored and the log says so. Setting `RUST_LOG` alone does nothing in these programs.

### The logs of this repository

The libraries of this repository write to standard error. Every line starts with the prefix of its writer:

| Prefix | Written by | What the lines say |
|---|---|---|
| `[qar-adapter]` | the session factory and the render sender | the bootstrap and its failures, the setting of the library log, the peers that are admitted or refused, the creation of senders and the first frame and the first bind of each, the OpenGL contexts, the copy into the QarOS textures, the first frame after a stall with its mean colour, and the first hand data |
| `[qar-panel-adapter]` | the panel sender | the creation of a panel, its subscription to AR input and its retirement, each change of its state, the record that the runtime keeps of it, a transfer that reconnects, its first frame, and failures that start with `***` |
| `[qar-url-panel-adapter]` | the URL panel adapter | each change of the state of a panel, with its identifier, and a failed call |
| `[qar-panel-input]` | the panel input source | one line for each gesture event of a panel, with its position in pixels and the resulting u and v, or the word SKIPPED with the reason |
| `[qar-spatial]` | the app-volume gesture source | the subscription, and for each gesture event the points, the ray and the outcome |
| `[qar-timing]` | the timing log | that a file cannot be created and that its rows are discarded |

**The join.** The session owner stops at the first call that fails. The owner turns the status code of a failed call into a sentence for the person: the event of the state `kFailed` carries it, and a line of the owner that starts with `QAROS: status` records it. With `QAR_ADAPTER_TRACE` set, the main calls of the render adapter are bracketed by two lines that carry the time since the previous line, so that the last line names the call that hangs.

A hardware fault inside `qar_library_init` or `qar_runtime_create` is caught: the owner prints the code, the address and the module that owns the address, and its failure text refers to that line, so that the host survives and the module of the fault is known. After such a fault the owner still tears down what it built: it calls `qar_library_destroy` and `qar_library_unload` when the library was loaded, and neither call is under the guard.

**Slow calls.** A QarOS call that waits for the Hub is timed. On the GUI thread of the host a call over 20 milliseconds prints a line that starts with `[qar-gui-call]`, the first 5 times for each call name and then once in 100. On the thread of the owner a call of 500 milliseconds or more, and a task of one second or more, is logged as a line of the owner that starts with `QAROS:`. `QAR_GUI_CALL_WARN_MS` changes the limit of the GUI thread.

**The setting of the library log.** The session owner prints the severity, the console switch and the folder of the library log, and a note when a variable `QAR_LOG_SEVERITY` of the environment disagreed and was overwritten, because the variable takes precedence over the init struct inside the library.

**Hands.** Once, the factory says whether the device sends hand joints, which is what the pointing ray needs, and when there is no ray, it says why and what the plane targets then do.

**The copy probe.** Unless you change `QAR_ADAPTER_PROBE_EVERY`, the copy hook prints a probe at the first frame that it copies and then one every 600 frames, six at most. A probe starts with `---- probe #<n> at frame <n> ----`, says what kind of texture QarOS handed over, and reads the colour and depth textures back to print their minimum, maximum and mean, a histogram of the depth and an ASCII map of it. The read-back stalls the pipeline.

**A stalled stream.** Read the skip reason and its detail of the stream in the diagnostics snapshot ([Diagnostics](ARCHITECTURE.md#diagnostics)). `SenderNotReady` with the detail `begin-frame pending (no render request from the viewer)` is the normal state while the viewer does not ask for frames; if it stays for ever while the submit counter is frozen, the wait never completes, which parks the trace of the SDK on *RenderingProducer: waiting for available frame*. `SenderError` carries the text of the failed call.

## What to send us

When you report a problem to the maintainers of this repository, send the facts below. They let someone reproduce your setup without a long exchange. The first command writes the facts of your machine into one text file and prints it: the commit of this repository, the Windows and tool versions, the variables of the layout, and the SHA256 of the SDK header, which identifies the QAROS version. The commit is the abbreviated hash, with the suffix "-dirty" when you changed files, or the name of a tag when the commit has one. It prints only the variables of this stack, no others.

Git reads the commit from the clone, so go to its folder first:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

```powershell
$out = Join-Path $env:TEMP 'qar_support_info.txt'
$info = @(
    'repository : ' + (git describe --tags --always --dirty)
    'windows    : ' + (Get-CimInstance Win32_OperatingSystem).Version
    'powershell : ' + $PSVersionTable.PSVersion.ToString()
    'cmake      : ' + ((cmake --version) | Select-Object -First 1)
)
$info += Get-ChildItem Env:QAR_*, Env:ROS2_INSTALL_ROOT | ForEach-Object { '{0} = {1}' -f $_.Name, $_.Value }
$info += 'sdk header  : ' + (Get-FileHash -LiteralPath (Join-Path $env:QAR_SDK_ROOT 'include\qar_streaming.h') -Algorithm SHA256).Hash
$info | Set-Content -Path $out
Get-Content -Path $out
```
```text
repository : 8bc6ea9
windows    : 10.0.26200
powershell : 7.4.4
cmake      : cmake version 3.29.5-msvc4
QAR_VENV = C:\qar\venv
QAR_DEPS_ROOT = C:\qar\deps
QAR_ROOT = C:\qar
QAR_SDK_ROOT = C:\QarOS\install
QAR_QT_ROOT = C:\qar\deps\Qt\5.15.2\msvc2019_64
QAR_PREFIX = C:\qar\install
ROS2_INSTALL_ROOT = C:\qar\deps\ros2_humble\ros2-windows
sdk header  : 5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
```

The line `cmake` shows the CMake of your machine. The output above is the one of the recorded runs, which used the CMake that Visual Studio 2022 version 17.11 bundles; [CMake](INSTALL.md#cmake) says what that means for the CMake that you install.

Then send the logs of the failing run. The QarOS log folder can hold gigabytes, so send only the newest files, those whose time matches the failure. The command lists the five newest files of the folder with their sizes; it prints nothing when QarOS has not written there yet.

```powershell
Get-ChildItem -LiteralPath (Join-Path $env:TEMP 'quaternar') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object { '{0}  {1:N0} bytes' -f $_.Name, $_.Length }
```
```text
qar_streaming.log  818 bytes
qar_launcher_stdout.log  204,402 bytes
mixer_51cca380-fcfb-4c84-907e-8b668a9c9ee2_stdout.log  10,026 bytes
streaming_viz_b829d79d-b8a8-478e-bf23-c0d28ca52ad0_stdout.log  866 bytes
mixer_51cca380-fcfb-4c84-907e-8b668a9c9ee2_stderr.log  0 bytes
```

Send, in addition:

1. The file `$env:TEMP\qar_support_info.txt` that the first command wrote.
2. The newest files of the QarOS log folder, and a log made at trace severity if the problem is between your program and QarOS; [Reading the logs](#reading-the-logs) explains how to switch it on.
3. The steps that lead to the problem, in the order you did them, and what you expected to see. A screenshot helps when the problem is visible.
4. Anything you changed from this documentation: other folders, other versions, a proxy.

## QarOS stream problems

The entries are about a stream that does not reach the Visualizer or does not look right. The log text of this repository is quoted as it is in the code. Where a line comes from QarOS, it is in italics, and the cause rests on what the authors of qar_ogre_adapter observed in live sessions; [Known SDK limitations](QAROS_INTEGRATION_NOTES.md#known-sdk-limitations) lists the limits of the SDK.

### The Visualizer shows a black scene

**Symptom.** The Visualizer receives frames and its frame counter advances, but it shows a black scene. The log of your program has a line, once, that has this form:

```text
[qar-adapter] render-into WITHOUT a copy hook: QAR's textures are never written, so the consumer will show a BLACK scene. Wire make_gl_copy_image_hook().
```

**Cause.** QarOS owns the textures of a frame and hands their ids to the sender to write into. The factory has no copy hook, or its submit mode is `kSupplyOwnTextureIds`, so nothing wrote the textures and the frames arrive black. **Fix.** Set `copy_hook` of the factory configuration to `make_gl_copy_image_hook()` and keep the default submit mode. If the log has the lines `FBO blit entry points unavailable`, `"FBO incomplete view="` or `glBlitFramebuffer FAILED err=` instead, the copy itself failed: it needs OpenGL 3.0 or newer, and the context of Ogre must be current on the render thread.

### The depth shows one flat colour

**Symptom.** The Visualizer shows the depth as one saturated colour, or as nothing close to the viewer, while the colour image is right. **Cause.** The library writes metric depth unless told otherwise, and with a QAROS runtime earlier than the verified version the Visualizer read the window-space value and inverted it itself. **Fix.** Set `QAR_DEPTH_ENCODING=ndc` before the program starts; the variable is read once for each process. [Settings the QarOS Visualizer needs](CONFIGURATION.md#settings-the-qaros-visualizer-needs) has the other settings of this kind.

### The scene is tipped on its side

**Symptom.** The viewer arrives above the scene and looks straight down at it, or the scene lies on its side. **Cause.** QarOS hands out poses in a Y-up world, and a ROS scene is Z-up, so a pose that is fed straight in tips the view over; the library maps the two worlds one to one unless told otherwise. **Fix.** Set `QAR_POSE_WORLD=zup`, exactly in that spelling, before the program starts: the variable is read once for each process.

### The QarOS graphics thread has no OpenGL context

**Symptom.** The Visualizer shows something for about a second, clears, and starts its receiver again, over and over. The log of the sender has the code 704 with the description *EglError* and the message *Could not load OpenGL function: glGetStringi*. **Cause.** QarOS drives the transform of a sender on its own graphics thread and needs an OpenGL context that is current there. The context of Ogre is current on the render thread, and a WGL context can be current on one thread at a time, so it cannot be handed over. The factory therefore makes a second context for each sender, on the device context of Ogre and sharing its objects, and hands that one over. **Fix.** Call `capture_gl_context()` on the render thread on every tick, before `create_sender`, so that a context is latched; the factory builds the parameters of the sender, and you must not hand over your own context. Until a context is latched the log says `create_sender: no WGL context latched yet` and the creation is retried on a later tick. [Render-into and the GL context](QAROS_INTEGRATION_NOTES.md#render-into-and-the-gl-context) gives the reasons.

### A sender is not created

**Symptom.** `create_sender` keeps returning null for a peer, or the log says that the peer is still creating, which the line `create_sender: peer <id> still creating after <seconds> s` reports once after 5 seconds. **Cause.** Creating a sender negotiates a transfer with the receiver and is not guaranteed to finish: an OpenGL sender against a mixer that supports only Vulkan parks in *waiting for receiver borrow signal*. The factory therefore starts the creation asynchronously and the call returns null until it is done, so that your program stays responsive. **Fix.** Nothing cancels the wait; look at the log of the mixer and the Visualizer for the reason, and keep asking with `on_render_request(peer)`. The line `create_sender: async creation FAILED for peer` means that the creation failed; the call that failed is in an earlier `***` line.

### The program does not join the Hub

**Symptom.** The state of the owner stays at `kWaitingForApproval`, `kNeedsCode` or `kFailed`, and the example prints a failure line. **Cause.** The owner turns the status of the failed call into a sentence. The Hub of this PC asks the person at the PC to allow the program: nobody answered in time (the approval waits 120 seconds unless the configuration says otherwise, and the Hub takes at most 600), the person denied it or closed the prompt, or the program does not match what was approved because it was moved, copied or rebuilt. The Hub may not be running and could not be started, or the program that answers is not the installed, signed QAROS. For the Hub of another PC the code was wrong or has expired (a code lives about 10 seconds and works once), no pairing with that Hub is stored, or the stored pairing has expired or was revoked. QAROS may also not be installed, with `QAROS_RUNTIME_DIR` unset. **Fix.** Connect again and click Allow in the QAROS window; after a rebuild the Hub asks again. For another PC type the current code of its Hub, or pair again. These cases are described from the code of the owner (`onboarding_status.cpp`); the denial, the timeout, a Hub that stops while the program is connected and a pairing with another PC were not run in a live session.

### A target never appears

**Symptom.** The session runs, but the example prints no stream and `targets()` stays empty. **Cause.** A target is a peer whose role is target and whose presence is online or unknown. A fresh Visualizer has no receiver, so the Hub lists no target for it. **Fix.** Add a receiver in the Visualizer with its "Add receiver" button; the Hub then lists a target. A device is started with `invite_device`, which works only for a program that joined as a source app on the PC of the Hub and only for a HoloLens (`hololens:<host>`) or a headset (`headset:<host>`); a bad address is refused with the text that the owner returns. [Peers, invites and admission](QAROS_INTEGRATION_NOTES.md#peers-invites-and-admission) describes the rule.

### A target gets no stream

**Symptom.** A headset or a Visualizer is listed as a target, but `poll_new_peer_requests` never lists it. **Cause.** The owner serves at most `max_targets` targets, 8 by default, and a further target is listed as not served with the reason that the cap is reached; the factory refuses a peer past `max_peers` with a log line that says `refused: already streaming to` a number of peers. **Fix.** Stop streaming to a target that is not needed, or raise the cap in the configuration. Calling `forget` for a peer whose stream was destroyed frees its place against `max_peers`, and a target that is served again gets its stream when another one goes away.

### No render stream without an app volume

**Symptom.** At the attach the log says that a call of `qar_app_volumes_get_or_create` failed and that no render stream can be created without an app volume, or `create_sender` logs that no app volume exists on every tick. **Cause.** The factory has no app volume: `create_app_volume` is off, or the call that gets or creates it failed. QarOS rejects a stream that has no app volume with `QAR_STATUS_APP_VOLUME_INVALID_ID`. **Fix.** Keep `create_app_volume` on and read the error of the failed call in the line before.

### The QarOS library does not load

**Symptom.** The owner fails in the state `kLoading`, and the lines of the owner say `no QAROS runtime was found (see the lookup lines above)`, or `qar_library_load failed for '<path>'` when the configuration names a library file. **Cause.** No library is found in the order that the header lists (a library beside the program, the folder of `QAROS_RUNTIME_DIR`, the installed QAROS, `PATH`), or a DLL that the library needs cannot be found. The loader looks for the dependencies in the folder of the library and in the system folder only, never in the folder of the program, in `PATH` or in the current folder. **Fix.** Install QAROS, or give `SessionOwnerConfig::library_path` the path of `qar-streaming-c.dll`, and deploy that folder whole. [Runtime deployment](SDK_COMPATIBILITY.md#runtime-deployment) has the details.

### The codes 603 and 604

**Symptom.** The log shows the status 603 or 604 from the call that begins a frame of a video stream. **Cause.** The header names 603 *stream is reconnecting* and 604 *no active transfers*. A panel sender exists before anything in the headset is attached to its stream, so both are states that pass, not errors, and the panel adapter reports them as not ready and the host polls again. In the log of the mixer, the line *MediaStreamingStreamIsReconnecting* with the code 603 belongs to a receiver of a panel stream that is reconnecting. **Fix.** None. The 3D stream treats any other error of its begin-frame call as a skip with the reason `SenderError` and the text of the call as detail; only 804, a closed producer stream, marks the sender closed, and `poll_closed_peers` then lists the peer.

### Code 507 every ten seconds

**Symptom.** The log of the Visualizer or of the mixer says *connection lost for stream* and *entering reconnecting state*, and then, every ten seconds, that a reconnect attempt failed with *Code: 507 Desc: BackendExhaustedRetriesToCreateStream*. **Cause.** A receiver waits for a stream whose sender was destroyed. No call of the C API withdraws a logical video stream, so the receiver keeps asking until its peer goes away; this is an observation of the authors of qar_ogre_adapter, not a statement of the SDK, and it was not repeated on every drop. **Fix.** Nothing on the producer side stops the loop. Keep the sender of a panel alive for the session and destroy senders in the order that [Teardown and destroy order](QAROS_INTEGRATION_NOTES.md#teardown-and-destroy-order) gives.

### The log says that a backend was rejected

**Symptom.** The log of the runtime has the line *rejecting backend <InProcessTransfer>* with the reason that the sender and the receiver are not in the same process, and the line *Creating sender selected transfer type VulkanGpuTransfer*. **Cause.** Neither line is an error. Your program and the Visualizer are separate processes, so the in-process transfer cannot apply, and the transfer that QarOS selects across processes for an OpenGL producer is *VulkanGpuTransfer*. **Fix.** None.

## Sample and example problems

### The program cannot find the Ogre DLLs

**Symptom.** The sample or the example ends at once, before it prints anything of its own, and Windows names a DLL such as `OgreMain.dll` that it cannot find. **Cause.** The folder with the Ogre DLLs of the ROS 2 distribution is not on `PATH`; Windows searches the folders of `PATH` for the DLLs of a program. **Fix.** The script `scripts\run_ogre_sample.ps1` puts the folder on `PATH` for the sample. For the example, put `opt\rviz_ogre_vendor\bin` of the distribution on `PATH` in the shell that runs it, as [Getting started](GETTING_STARTED.md) shows.

### Ogre cannot start

**Symptom.** The sample prints a line that starts with `FATAL:` and exits with the code 2, and the example prints `example: Ogre failed to start:` and exits with 1. The text is `no suitable OpenGL render system could be loaded from the plugin dir`, or `"Ogre initialization failed:"` with the description of Ogre. **Cause.** The folder with the render system plugins is wrong, or the render system that you asked for did not load. The sample takes the folder from `--ogre-plugin-dir`, else from `ROS2_INSTALL_ROOT`, else from the folder that the build baked in, and it prints the folder as the line `ogre_plugin_dir=`; `--render-system` is `auto`, `gl3plus` or `gl`, and `auto` loads the plain GL system and takes GL3Plus only when that is missing. The example has neither option: it uses the folder that the build baked in, which is the folder of the distribution that the build was configured with, and the render system `auto`. **Fix.** Check that `RenderSystem_GL.dll` and `RenderSystem_GL3Plus.dll` are in that folder (the first command of [Getting started](GETTING_STARTED.md) lists them). For the sample, pass the folder with `--ogre-plugin-dir`, or choose the other render system. For the example, configure and build again with `ROS2_INSTALL_ROOT` set to the distribution, which bakes the right folder in. The sample needs a current OpenGL context and gets it from a hidden window, so it needs a working graphics driver but no visible window.

### The sample reports a failed check

**Symptom.** The sample ends with the exit code 1. **Cause.** For at least one peer, a check failed: content in both eyes, a depth that is present and not uniform, eyes that differ, and a nearer part of the scene that shifts more than a farther one. **Fix.** Read `summary.txt` and the text file of the peer in the output folder: they have the numbers behind each check, and the colour and depth images of the eyes show what went wrong.

### The linker reports LNK1194

**Symptom.** Building a test project of the repository that delays the load of `OgreMain.dll` ends with the linker error LNK1194. **Cause.** A data symbol that `OgreMain.dll` exports, such as `Ogre::Vector3::ZERO`, cannot be loaded late; code that refers to it, even through a default argument, defeats the delayed load. **Fix.** Do not refer to the exported data symbols of Ogre in code of that project; [Ogre build notes](DEVELOPING.md#ogre-build-notes) lists them.

### The example cannot connect to QarOS

**Symptom.** The example prints `example: QAROS connect failed` with the state of the owner, or `example: QAROS connect failed:` with a text, and ends with 1. **Cause.** The session did not run, or the attach failed, and the text is the `last_error()` of the factory. **Fix.** Look the text up in [The program does not join the Hub](#the-program-does-not-join-the-hub) and in [QarOS stream problems](#qaros-stream-problems), and read the lines of the owner above the message.
