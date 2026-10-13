# Installation

## Overview

This document installs everything that you need to build, test and run qar_ogre_adapter on a Windows 11 machine. It goes in this order: the machine and its toolchain; the folders and the environment variables that every command uses; the QarOS SDK; a clone of this repository with the Python environment; the ROS 2 Humble distribution, which provides Ogre; and finally this repository, which you configure, build, test and install, and then check with a small consumer program. Every command runs in PowerShell 7 and uses only the environment variables of the layout, so a different layout needs only different values for the variables.

qar_ogre_adapter needs no Qt and no colcon. It needs the ROS 2 distribution for Ogre only, and it needs Python only for the layering guard that its tests run. The QarOS SDK is not needed to build or test: you need it to connect to QarOS.

When you are done, the libraries of the repository, the CMake package `qar_ogre_adapter` and the QarOS C header are installed below `QAR_PREFIX`, and the headless sample and the live example are built in the build folder of the repository. The command below shows how much disk space each folder uses; run it after the last step.

```powershell
foreach ($folder in $env:QAR_DEPS_ROOT, $env:QAR_VENV, "$env:QAR_ROOT\src", $env:QAR_PREFIX, $env:QAR_SDK_ROOT) {
    $bytes = (Get-ChildItem -LiteralPath $folder -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
    '{0}  {1:N2} GiB' -f $folder, ($bytes / 1GB)
}
```
```text
C:\qar\deps  1.84 GiB
C:\qar\venv  0.24 GiB
C:\qar\src  1.17 GiB
C:\qar\install  0.02 GiB
C:\QarOS\install  0.26 GiB
```

The sizes were measured after the three repositories of the stack had been built on the machine; your folders are smaller when you build fewer of them.

## Machine prerequisites

The installation was developed and run on Windows 11 Pro, 64-bit. Check the machine before you install anything: the commands below only read the system.

```powershell
$os = Get-CimInstance Win32_OperatingSystem
'{0}, version {1}, 64-bit: {2}' -f $os.Caption, $os.Version, [Environment]::Is64BitOperatingSystem
```
```text
Microsoft Windows 11 Pro, version 10.0.26200, 64-bit: True
```

| Requirement | What this stack needs | Why |
|---|---|---|
| Operating system | Windows 11, 64-bit. Other Windows versions and Linux are not built or tested. | The toolchain and the ROS 2 distribution of this documentation are the Windows 64-bit builds. |
| Desktop session | An interactive desktop on the local console of the machine. Remote Desktop and services are not covered. | The QarOS Visualizer opens OpenGL windows, and the documented runs were made from the local console. |
| Graphics | A GPU with a current OpenGL driver; the reference machine has an NVIDIA GPU. This stack states no minimum GPU or driver version. | Ogre renders with OpenGL, and QarOS encodes the frames on the GPU. |
| Administrator rights | Only for the installers of the toolchain. The scripts of this stack and the commands of this documentation run in a normal, non-elevated PowerShell. | The scripts write below the folders you pass them. |
| Paths | A short root folder: this documentation uses `C:\qar` and `C:\QarOS`. | Windows limits a path to 260 characters unless long paths are enabled, and a deep clone path overflows that limit. |

Check the two Windows settings that matter for the layout. The first value is the long-path policy: `0` means long paths are off, which is the Windows default. This stack does not change it and does not need it, because the layout above is short.

```powershell
(Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled).LongPathsEnabled
```
```text
0
```

The second command lists the graphics adapters Windows sees. On a laptop with an integrated and a discrete adapter, Windows may run an application on the integrated one, while QarOS encodes on the NVIDIA GPU; frames then cross between the adapters.

```powershell
Get-CimInstance Win32_VideoController | ForEach-Object { '{0}, driver {1}' -f $_.Name, $_.DriverVersion }
```
```text
Intel(R) UHD Graphics, driver 31.0.101.5187
NVIDIA GeForce RTX 4080 Laptop GPU, driver 32.0.15.7657
```


Plan the disk space before you start. The layout of this documentation puts the dependencies, the virtual environment, the clones, the install prefix and the SDK on one drive; the [Overview](#overview) has a command that measures them, so run that command after the last step of the chain to see what your machine used.

## Toolchain

The stack builds with five tools. Install them in the order of the table; every tool has an install command and a verify command, and the verify commands are the ones this documentation was run with.

| Tool | Version | Used for |
|---|---|---|
| Visual Studio 2019 Build Tools | MSVC v142 (compiler 19.29.30151.0) with a Windows 10 SDK | compiling and linking every C++ module |
| CMake | 3.22 or newer | configuring and building; the presets of every repository need it |
| Git | any recent version | cloning the repositories |
| PowerShell 7 | 7.x (`pwsh`) | running every command and script of this documentation |
| Python | 3.10.11 (any 3.10.x) | the optional virtual environment and the layering guard |

All install commands use `winget`, the Windows package manager that ships with Windows 11 as part of the App Installer. They change the machine (they install software system-wide), so run them in an elevated PowerShell 7 and open a new window afterwards, so that the new `PATH` is read.

### Why Visual Studio 2019 and the v142 toolset

The ROS 2 Humble distribution of this documentation and the Qt 5.15.2 kit `msvc2019_64` are compiled with the Visual Studio 2019 toolset (v142). C++ classes cross DLL boundaries in this stack, between our code and Ogre, RViz and Qt, and the compiled layout of those classes and the C++ runtime they use must match the toolset that built the binaries. The stack therefore pins the toolset that built them instead of trying a newer one: the presets select the generator `Visual Studio 16 2019`, and the scripts find only a Visual Studio 16.x installation that has the C++ x64 tools. Visual Studio 2022 or 2026 may be installed next to it; they are ignored. Always build `RelWithDebInfo` or `Release`: the ROS 2 and Qt binaries are release builds, and a debug C runtime in the same process crashes on load.

### Visual Studio 2019 Build Tools

Install the workload `Microsoft.VisualStudio.Workload.VCTools` (the C++ build tools) together with the components `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` (the MSVC v142 compiler and linker) and `Microsoft.VisualStudio.Component.Windows10SDK.19041` (the Windows 10 SDK 10.0.19041). The full Visual Studio 2019 IDE with the same workload works as well.

```powershell
winget install --id Microsoft.VisualStudio.2019.BuildTools --source winget --accept-package-agreements --accept-source-agreements --override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows10SDK.19041"
```

> Success: `winget` finishes with exit code 0 once the installer has added the C++ build tools workload, the MSVC compiler and linker and the Windows 10 SDK. The options of the command make the installer wait until it is done, work without a window and not restart the machine. The verify command below shows the installation.
>
> This command changes your machine, so no output is shown here.

The scripts of this stack locate the installation with `vswhere`, a tool that the Visual Studio installer puts in a fixed folder. The verify command below runs the same query as `scripts\qar_locations.ps1`, then reads the toolset folder and the compiler version from that installation. The toolset folder starts with `14.2` for v142.

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -version '[16.0,17.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest
$vs
$toolset = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Name | Select-Object -Last 1
$toolset
(Get-Item (Join-Path $vs "VC\Tools\MSVC\$toolset\bin\Hostx64\x64\cl.exe")).VersionInfo.FileVersion
```
```text
C:\Program Files (x86)\Microsoft Visual Studio\2019\Community
14.29.30133
19.29.30151.0 built by: cloudtest
```

The first line is the installation folder (its last part names the edition), the second the toolset folder, the third the compiler version followed by a build annotation. The reference build used the compiler 19.29.30151.0; a later 16.11 servicing release of the same toolset is the same toolset family. If the first command prints nothing, the C++ x64 tools are not installed, and the scripts stop with an error that points here.

### CMake

The presets require CMake 3.22 or newer.

The runs that this documentation shows used CMake 3.29.5 as Visual Studio 2022 version 17.11 bundles it, which prints `cmake version 3.29.5-msvc4`; the compiler of those runs was still the toolset v142 of Visual Studio 2019. Every output of this documentation that shows a version of CMake comes from that build. No run used the build of Kitware that the install command below fetches, and none used a CMake 4 release, so nobody has checked whether a CMake 4 release configures this stack. The command therefore installs the Kitware build of the version number of the recorded runs, 3.29.5, which `winget` lists. A Kitware build and a Microsoft build of one version number are two builds, and this documentation does not claim that they behave alike.

```powershell
winget install --id Kitware.CMake --version 3.29.5 --source winget --accept-package-agreements --accept-source-agreements
```

> Success: `winget` finishes with exit code 0 and CMake 3.29.5 is installed. In a new window the verify command below prints the version of the CMake that comes first on `PATH`.
>
> This command changes your machine, so no output is shown here.

```powershell
cmake --version
```
```text
cmake version 3.29.5-msvc4
...
```

### Git

Git clones the repositories and is the only tool of this chain that talks to a Git host. The CMake configure step of every repository downloads GoogleTest as an archive and does not need Git for it.

```powershell
winget install --id Git.Git --source winget --accept-package-agreements --accept-source-agreements
```

> Success: `winget` finishes with exit code 0 and Git is installed. In a new window the verify command below prints the version of Git.
>
> This command changes your machine, so no output is shown here.

```powershell
git --version
```
```text
git version 2.39.2.windows.1
```

### PowerShell 7

Every command of this documentation was run in PowerShell 7 (`pwsh`), started without a profile, and that is the shell the commands are written for. Windows PowerShell 5.1 (`powershell.exe`) is not used by the documented commands. The helper file `scripts\qar_locations.ps1` states that it works in both editions.

```powershell
winget install --id Microsoft.PowerShell --source winget --accept-package-agreements --accept-source-agreements
```

> Success: `winget` finishes with exit code 0 and PowerShell 7 is installed. In a new PowerShell 7 window (`pwsh`) the verify command below prints a version that starts with 7.
>
> This command changes your machine, so no output is shown here.

```powershell
$PSVersionTable.PSVersion.ToString()
```
```text
7.4.4
```

### Python 3.10

The compiled Python modules of the ROS 2 Humble Windows distribution carry the `cp310` ABI tag, so a Python 3.10 interpreter must be the one that runs them; the distribution ships no interpreter of its own. Install the machine-wide interpreter; it lands in the folder Python310 below Program Files.

```powershell
winget install Python.Python.3.10 --scope machine --source winget --accept-package-agreements --accept-source-agreements
```

> Success: `winget` finishes with exit code 0 and the machine-wide Python 3.10 is installed in the folder Python310 below Program Files. The verify command below calls that interpreter by its full path and prints its version, which starts with 3.10.
>
> This command changes your machine, so no output is shown here.

The verify command calls the interpreter by its full path, because other Python versions may come first on `PATH`. The next section creates the virtual environment from this interpreter.

```powershell
& "$env:ProgramFiles\Python310\python.exe" --version
```
```text
Python 3.10.11
```


### Execution policy and downloaded files

The scripts of this stack are not signed. If the execution policy of your machine blocks local scripts, start the script through `pwsh` and set its `-ExecutionPolicy` option to `Bypass`, which applies to that one process only. Windows marks files that were downloaded from the internet (for example a repository or the SDK saved as a zip file by a browser); the policy `RemoteSigned` refuses an unsigned script that carries the mark, and `Unblock-File` removes it.

The command prints the policy stored for your user and for the machine; `Undefined` means that scope sets nothing.

```powershell
foreach ($scope in 'CurrentUser', 'LocalMachine') { '{0,-13} {1}' -f $scope, (Get-ExecutionPolicy -Scope $scope) }
```
```text
CurrentUser   Undefined
LocalMachine  RemoteSigned
```

## Directory layout and environment variables

This documentation uses one short folder layout and refers to every folder through an environment variable. A different layout needs only different values for the variables, never different commands.

```text
C:\qar                                  QAR_ROOT, the root of the layout
C:\qar\deps                             QAR_DEPS_ROOT: downloads and dependencies
C:\qar\deps\downloads                   archives that the scripts download
C:\qar\deps\ros2_humble\ros2-windows    ROS2_INSTALL_ROOT: the ROS 2 Humble distribution
C:\qar\venv                             QAR_VENV: the Python virtual environment
C:\qar\src\<repository>                 the clones of the repositories
C:\qar\install                          QAR_PREFIX: the install prefix of the repositories
C:\QarOS                                the QarOS SDK (outside the layout root)
C:\QarOS\install                        QAR_SDK_ROOT: the unpacked SDK drop
```

| Variable | Meaning | Value in this documentation |
|---|---|---|
| `QAR_ROOT` | Root of the layout. Documented commands use it to name the clone and working folders; no script and no CMake file reads it. | `C:\qar` |
| `QAR_DEPS_ROOT` | Dependency root: the downloaded archives and the build folders of the distribution repairs. | `C:\qar\deps` |
| `ROS2_INSTALL_ROOT` | The ROS 2 Humble Windows distribution, the folder that holds `local_setup.bat`. | `C:\qar\deps\ros2_humble\ros2-windows` |
| `QAR_QT_ROOT` | The Qt 5.15.2 folder. The block below sets it for qar_qt_streaming and qar_rviz_plugin, which build against Qt; this repository installs no Qt, and its build does not read the variable. | `C:\qar\deps\Qt\5.15.2\msvc2019_64` |
| `QAR_VENV` | The Python 3.10 virtual environment, the folder that holds `Scripts\python.exe`. | `C:\qar\venv` |
| `QAR_PREFIX` | The install prefix: the repositories of this stack install into it, and the others find them there. | `C:\qar\install` |
| `QAR_SDK_ROOT` | The QarOS SDK install tree, the folder that holds `bin` and `include`. | `C:\QarOS\install` |

The outputs shown in this documentation were recorded in another folder layout on a machine of the same kind; the folder names in them were replaced by the names of the layout above.

Every script of this stack that needs one of these folders takes it from its parameter first and from the environment variable second. When both are missing, the script stops with an error that names the parameter, the variable and the section of this document that explains them, in the form `not found: pass -<Parameter> <folder> or set the environment variable <EnvVar>`. When the folder does not exist, or lacks the file that identifies it, the error says so. CMake follows the same order for the locations it needs: a `-D` option wins over the environment variable.

### Create the folders and set the variables

The block below creates the folders and sets the seven variables for your Windows user, so that every new shell has them, and for the current shell, so that you can continue at once. It changes your user environment; [Removing everything again](#removing-everything-again) shows how to undo it. If you chose other folders, change the values at the top.

```powershell
$layout = [ordered]@{
    QAR_ROOT          = 'C:\qar'
    QAR_DEPS_ROOT     = 'C:\qar\deps'
    ROS2_INSTALL_ROOT = 'C:\qar\deps\ros2_humble\ros2-windows'
    QAR_QT_ROOT       = 'C:\qar\deps\Qt\5.15.2\msvc2019_64'
    QAR_VENV          = 'C:\qar\venv'
    QAR_PREFIX        = 'C:\qar\install'
    QAR_SDK_ROOT      = 'C:\QarOS\install'
}
foreach ($name in $layout.Keys) {
    [Environment]::SetEnvironmentVariable($name, $layout[$name], 'User')
    Set-Item -Path "Env:$name" -Value $layout[$name]
}
New-Item -ItemType Directory -Force -Path $layout.QAR_ROOT, $layout.QAR_DEPS_ROOT, $layout.QAR_PREFIX, (Split-Path $layout.QAR_SDK_ROOT) | Out-Null
```

> Success: the block prints nothing and ends without an error. Afterwards the seven variables are set for your Windows user and in the current window, and these folders exist: `C:\qar`, `C:\qar\deps`, `C:\qar\install` and `C:\QarOS`. The check below lists the variables.
>
> This command changes your machine, so no output is shown here.

A variable that is set for your user is visible to programs you start afterwards, not to windows that were already open: open a new PowerShell 7 window, or set the variable in the current window as the block does.

Check the result. Every variable must be set, and the folders you created must exist; the folders of the dependencies appear later in the chain.

```powershell
foreach ($name in 'QAR_ROOT', 'QAR_DEPS_ROOT', 'ROS2_INSTALL_ROOT', 'QAR_QT_ROOT', 'QAR_VENV', 'QAR_PREFIX', 'QAR_SDK_ROOT') {
    '{0,-18} {1}' -f $name, [Environment]::GetEnvironmentVariable($name)
}
```
```text
QAR_ROOT           C:\qar
QAR_DEPS_ROOT      C:\qar\deps
ROS2_INSTALL_ROOT  C:\qar\deps\ros2_humble\ros2-windows
QAR_QT_ROOT        C:\qar\deps\Qt\5.15.2\msvc2019_64
QAR_VENV           C:\qar\venv
QAR_PREFIX         C:\qar\install
QAR_SDK_ROOT       C:\QarOS\install
```

Every command of this documentation that clones, builds or runs something uses only these variables, and a shell that has them needs nothing else.

## The QarOS SDK

The QarOS SDK is QuaternAR's proprietary runtime and is not part of this stack. You obtain a drop of it from QuaternAR as a zip file, unpack it next to the layout, and point the stack at the unpacked folder. The SDK drop dates that this stack was verified against are in [SDK compatibility](SDK_COMPATIBILITY.md); this section only installs a drop.

1. Save the zip file you received from QuaternAR as `C:\QarOS\qaros_sdk.zip`.
2. Unpack it into its parent folder. The archive holds one top-level folder, `install`, with `bin`, `include` and a licence list; unpacking it into `C:\QarOS` therefore yields `C:\QarOS\install`, which is the value of `QAR_SDK_ROOT`.

```powershell
$sdkParent = Split-Path $env:QAR_SDK_ROOT
Expand-Archive -LiteralPath (Join-Path $sdkParent 'qaros_sdk.zip') -DestinationPath $sdkParent -Force
Get-ChildItem -LiteralPath $env:QAR_SDK_ROOT -Name
```
```text
bin
include
third_party_licenses.md
```

3. Remove the download mark from the unpacked programs. Windows marks files that came from the internet, and a process start from a marked SDK program can fail with Windows error 1223.

```powershell
Get-ChildItem -LiteralPath $env:QAR_SDK_ROOT -Recurse -Include *.exe, *.dll | Unblock-File
```

4. Check that the folder holds what the stack needs: the C library, the runtime launcher and the Visualizer that QarOS starts itself, and the C header. The folder holds the program `qar-stereokit-gui.exe` as well, after which the crash reports of the Visualizer are named. The last line is the SHA256 of the header; [Identifying an SDK drop](SDK_COMPATIBILITY.md#identifying-an-sdk-drop) explains what to compare it with.

```powershell
foreach ($file in 'bin\qar-streaming-c.dll', 'bin\qar-runtime-launcher.exe', 'bin\qar-streaming-viz.exe', 'include\qar_streaming.h') {
    '{0,-30} {1}' -f $file, (Test-Path -LiteralPath (Join-Path $env:QAR_SDK_ROOT $file))
}
(Get-FileHash -LiteralPath (Join-Path $env:QAR_SDK_ROOT 'include\qar_streaming.h') -Algorithm SHA256).Hash
```
```text
bin\qar-streaming-c.dll        True
bin\qar-runtime-launcher.exe   True
bin\qar-streaming-viz.exe      True
include\qar_streaming.h        True
5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA
```

### How the stack finds the SDK

Deploy the `bin` folder of the SDK whole and unmodified; do not copy single files out of it into an application folder, and never copy the SDK into a repository. The C header documents this rule, and it matters here because QarOS starts its own launcher and Visualizer from that folder.

Nothing in this stack links against the SDK. The library `qar-streaming-c.dll` is loaded at run time from the folder an application passes in; the C API takes the library file `bin\qar-streaming-c.dll` in one call and the folder `bin` in the runtime initialisation. An empty folder starts no launcher, and peer invitations then never complete.

At configure time, CMake reads `QAR_SDK_ROOT` only to check that the DLL exists below it; when the variable is not set or the DLL is missing, it prints a warning and continues. A build and the whole test suite therefore work without an SDK; only the programs that connect to QarOS need it at run time.

At run time an application passes the folder in the `runtime_binaries_folder_path` field of the QarOS session factory configuration.

## Python environment

### Clone this repository

The next steps use the scripts and the `requirements.txt` of this repository, so clone it first. The command takes the default branch of the repository. This documentation describes version 0.1.0 of the three repositories of this stack; the version of a clone is the number after the word VERSION in the project command of the file `CMakeLists.txt` in its root.

```powershell
git clone https://github.com/Quaternar/qaros-ogre "$env:QAR_ROOT\src\qar_ogre_adapter"
```
```text
Cloning into 'C:\qar\src\qar_ogre_adapter'...
done.
```

Go to the folder of the clone; the next steps run there:

```powershell
Set-Location "$env:QAR_ROOT\src\qar_ogre_adapter"
```

### Create the virtual environment

The virtual environment holds the Python tools of the build in one folder that you can delete. It is created from the Python 3.10 interpreter of the toolchain section and filled from the pinned list `requirements.txt` of the repository. The list contains, among other packages, `aqtinstall`, which installs Qt, and `colcon-common-extensions`, which builds ROS 2 packages; every repository ships the same list.
This repository uses neither of the two tools, and the environment is optional for it: the layering guard that its tests run needs only the Python standard library and runs with the Python 3 interpreter (version 3.8 or newer) that CMake finds, not with this environment. Only the commands in this documentation that run `scripts\check_layering.py` by hand call the interpreter of the environment; if you skip the environment, run them with another Python 3 interpreter. The commands of this section create and check the environment and change nothing else.

```powershell
& "$env:ProgramFiles\Python310\python.exe" -m venv $env:QAR_VENV
```

```powershell
& "$env:QAR_VENV\Scripts\python.exe" -m pip install --disable-pip-version-check -r requirements.txt
```
```text
...
Successfully installed PyYAML-6.0.3 Pygments-2.20.0 aqtinstall-3.3.0 backports.zstd-1.6.0 beautifulsoup4-4.15.0 brotli-1.2.0 bs4-0.0.2 catkin-pkg-1.1.0 certifi-2026.6.17 charset-normalizer-3.4.9 colcon-cmake-0.2.30 colcon-common-extensions-0.3.0 colcon-core-0.21.0 colcon-defaults-0.2.9 colcon-devtools-0.3.0 colcon-library-path-0.2.1 colcon-metadata-0.2.5 colcon-notification-0.3.1 colcon-output-0.2.14 colcon-package-information-0.4.1 colcon-package-selection-0.2.10 colcon-parallel-executor-0.4.0 colcon-pkg-config-0.1.0 colcon-powershell-0.5.0 colcon-python-setup-py-0.2.9 colcon-recursive-crawl-0.2.3 colcon-ros-0.5.0 colcon-test-result-0.3.8 colorama-0.4.6 coloredlogs-15.0.1 coverage-7.15.0 defusedxml-0.7.1 distlib-0.4.3 docutils-0.23 empy-4.2.1 exceptiongroup-1.3.1 humanfriendly-10.0 humanize-4.16.0 idna-3.18 inflate64-1.0.4 iniconfig-2.3.0 lark-1.3.1 multivolumefile-0.2.3 netifaces-0.11.0 numpy-2.2.6 opencv-python-5.0.0.93 packaging-26.2 patch-ng-1.19.1 pluggy-1.6.0 psutil-7.2.2 py7zr-1.1.3 pybcj-1.0.7 pycryptodomex-3.23.0 pyparsing-3.3.2 pyppmd-1.3.1 pyreadline3-3.5.6 pytest-9.1.1 pytest-cov-7.1.0 pytest-repeat-0.9.4 pytest-rerunfailures-16.4 python-dateutil-2.9.0.post0 pywin32-312 requests-2.34.2 semantic-version-2.10.0 six-1.17.0 soupsieve-2.8.4 texttable-1.7.0 tomli-2.4.1 typing_extensions-4.16.0 urllib3-2.7.0 vcstool-0.3.0
```

Check that the pinned tools are installed; this repository uses neither of the two that the command lists, so the check only confirms the environment.

```powershell
& "$env:QAR_VENV\Scripts\python.exe" -m pip list --format=freeze | Select-String -Pattern '^(aqtinstall|colcon-common-extensions)=='
```
```text
aqtinstall==3.3.0
colcon-common-extensions==0.3.0
...
```

## ROS 2 Humble for Windows

This repository needs the ROS 2 distribution for one reason: Ogre. The distribution carries the exact Ogre 1.12.1 that RViz links, in its package `rviz_ogre_vendor`, and the library links that same Ogre, so that the process that later loads the RViz plugin never holds two Ogre runtimes. Nothing else of ROS 2 is used by this repository.

The stack uses the official ROS 2 Humble binary release for Windows, tag `release-humble-20260220`, as it is: the file `ros2-humble-20260220-windows-release-amd64.zip` (395014297 bytes, SHA256 `256ABCFA37F8D97D2CB3FF2A85F53465B016A25632399C99F67DC2BEE18E89C3`) from the ROS 2 releases on GitHub. The script `scripts\fetch_dependencies.ps1` downloads it into `$env:QAR_DEPS_ROOT\downloads`, checks the SHA256, and unpacks it into `$env:QAR_DEPS_ROOT\ros2_humble`; the distribution then lives in the folder `ros2-windows` below that, which is the value of `ROS2_INSTALL_ROOT`. The script is safe to run again: it skips a download whose hash matches and an extraction into a populated folder, unless you pass `-Force`. It also downloads the reference source manifest of the release (`ros2-release-humble-20260220-src.zip`), which nothing in this stack uses.

The download needs access to `github.com`. If your network inspects TLS traffic and the download fails with a certificate error, see the troubleshooting document before you continue.

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

Check that the distribution is where `ROS2_INSTALL_ROOT` points and that it contains the Ogre 1.12.1 of the upstream release.

```powershell
Test-Path -LiteralPath (Join-Path $env:ROS2_INSTALL_ROOT 'local_setup.bat')
(Select-String -LiteralPath (Join-Path $env:ROS2_INSTALL_ROOT 'opt\rviz_ogre_vendor\CMake\OGREConfigVersion.cmake') -Pattern 'set\(PACKAGE_VERSION ').Line.Trim()
```
```text
True
set(PACKAGE_VERSION "1.12.1")
set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")
```

What you build here uses the distribution as it is; it needs none of the repairs that other repositories of this stack apply, so the reminder about repair scripts at the end of the output of the script above does not concern this repository. The repair scripts in the folder `scripts` serve qar_rviz_plugin.

## Build and install qar_ogre_adapter

This repository is the one you cloned in [Python environment](#python-environment). The commands below build it from its clone.

Configure with the preset of the repository. The preset selects the generator `Visual Studio 16 2019` for x64, the build folder `build\vs2019-x64` inside the clone and the configuration `RelWithDebInfo`, and it reads the location variables of the layout. Configuring downloads GoogleTest 1.14.0 as an archive from `github.com` and checks its SHA256, so this step needs network access; without it, pass `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST` with the path of an extracted GoogleTest 1.14.0 folder.

```powershell
cmake --preset vs2019-x64
```
```text
Preset CMake variables:

  QAR_BUILD_TESTS="ON"
  QAR_ROS2_INSTALL_ROOT="C:\qar\deps\ros2_humble\ros2-windows"
  QAR_SDK_ROOT="C:\QarOS\install"

...
-- The CXX compiler identification is MSVC 19.29.30151.0
...
-- ROS 2 distribution: C:/qar/deps/ros2_humble/ros2-windows
-- Found rviz_ogre_vendor: 11.2.25 (C:/qar/deps/ros2_humble/ros2-windows/share/rviz_ogre_vendor/cmake)
...
-- qar_qar_adapter: QarOS SDK backend, DYNAMIC loading of C:\QarOS\install/bin/qar-streaming-c.dll (no import lib linked)
...
-- Configuring done (7.7s)
-- Generating done (0.1s)
-- Build files have been written to: C:/qar/src/qar_ogre_adapter/build/vs2019-x64
```

Build it. The build writes into the build folder of the repository only.

```powershell
cmake --build --preset vs2019-x64
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

Run the tests of the repository. The tests need no QarOS SDK and open no window; [Testing](TESTING.md) explains what they cover.

```powershell
ctest --preset vs2019-x64
```
```text
Test project C:/qar/src/qar_ogre_adapter/build/vs2019-x64
        Start   1: check_layering_qar_ar_core
  1/331 Test   #1: check_layering_qar_ar_core ................................................................   Passed    0.08 sec
        Start   2: check_layering_qar_ogre_streaming
  2/331 Test   #2: check_layering_qar_ogre_streaming .........................................................   Passed    0.09 sec
        Start   3: check_layering_qar_qar_adapter
  3/331 Test   #3: check_layering_qar_qar_adapter ............................................................   Passed    0.10 sec
        Start   4: check_layering_examples
  4/331 Test   #4: check_layering_examples ...................................................................   Passed    0.05 sec
...
100% tests passed, 0 tests failed out of 331

Total Test time (real) =   2.65 sec
```

Install it into the prefix. The install puts the libraries of the three modules `qar_ar_core`, `qar_ogre_streaming` and `qar_qar_adapter` and the vendored QarOS C header below `$env:QAR_PREFIX`, together with the CMake package that the other repositories of this stack find there.

```powershell
cmake --install build\vs2019-x64 --config RelWithDebInfo --prefix $env:QAR_PREFIX
```
```text
-- Installing: C:\qar\install/lib/qar_ar_core.lib
-- Installing: C:\qar\install/lib/qar_ogre_streaming.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_capi.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_render.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_panels.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_url.lib
-- Installing: C:\qar\install/lib/qar_qar_adapter_gestures.lib
-- Installing: C:\qar\install/include
...
-- Installing: C:\qar\install/include/qar_streaming.h
-- Installing: C:\qar\install/share/qar_ogre_adapter/LICENSE.qar_streaming_header
-- Installing: C:\qar\install/share/qar_ogre_adapter/LICENSE
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterTargets.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterTargets-relwithdebinfo.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterConfig.cmake
-- Installing: C:\qar\install/share/qar_ogre_adapter/cmake/qar_ogre_adapterConfigVersion.cmake
```

## Install smoke test

The folder `docs/snippets/consumer` of the repository holds a small project that uses the installed package the way your own project does. It is not part of the repository build; it configures on its own against `QAR_PREFIX`. It asks for the version 0.1.0 of the package and for the components `qar_ar_core`, `qar_ogre_streaming`, `render`, `panels`, `url` and `gestures`, and it links their targets. The installed headers are UTF-8 text without a byte order mark, and some comments contain characters outside ASCII; compile your own code with `/utf-8`, as this repository does.

```cmake
find_package(qar_ogre_adapter 0.1.0 CONFIG REQUIRED
  COMPONENTS qar_ar_core qar_ogre_streaming render panels url gestures)
```

Its program prints the version of the package that `find_package` found and the targets that it was linked with, and it calls three functions that touch neither Ogre nor the QarOS SDK: the check whether the QarOS library is loaded, which the wrapper header of `qar_ar_core` declares and the component `capi` defines, the text of a status from `qar_ogre_streaming`, and the parser of a boolean text from `render`. The program therefore needs neither at run time.

Configure the project against the install prefix, build it, and run it.

```powershell
cmake -S docs\snippets\consumer -B build\consumer -G "Visual Studio 16 2019" -A x64 "-DCMAKE_PREFIX_PATH=$env:QAR_PREFIX"
```
```text
...
-- Found rviz_ogre_vendor: 11.2.25 (C:/qar/deps/ros2_humble/ros2-windows/share/rviz_ogre_vendor/cmake)
...
-- Configuring done (1.7s)
-- Generating done (0.0s)
-- Build files have been written to: C:/qar/src/qar_ogre_adapter/build/consumer
```

```powershell
cmake --build build\consumer --config RelWithDebInfo
```
```text
...
  qar_ogre_adapter_consumer.vcxproj -> C:\qar\src\qar_ogre_adapter\build\consumer\RelWithDebInfo\qar_ogre_adapter_consumer.exe
...
```

```powershell
.\build\consumer\RelWithDebInfo\qar_ogre_adapter_consumer.exe
```
```text
qar_ogre_adapter 0.1.0
targets: qar_ogre_adapter::qar_ar_core qar_ogre_adapter::qar_ogre_streaming qar_ogre_adapter::render qar_ogre_adapter::panels qar_ogre_adapter::url qar_ogre_adapter::gestures
QarOS library loaded: no
status text: Ok
parse_bool_env("on"): true
```

The first line is the version of the package, the second the targets that the program was linked with, and the next three lines are the results of the three calls. If `find_package` reports that it cannot find the package, `QAR_PREFIX` does not point to the folder that the install step wrote to, or the version of the installed package differs from the requested one; [Troubleshooting](TROUBLESHOOTING.md#installation-and-build-problems) lists the installation problems.

## Removing everything again

The installation changes your machine in a few places. This section lists every one of them and how to undo it. The scripts and commands that this documentation tells you to run write only to the folders and registry values listed here, to the build folders inside the clones, and to the files they report. The programs that they call keep caches and logs of their own outside these folders, such as the download cache of pip; this section does not list them.

| Change | Made by | Where it lives | How to undo it |
|---|---|---|---|
| The toolchain: Visual Studio 2019 Build Tools, CMake, Git, PowerShell 7, Python 3.10 | the installers of [Toolchain](#toolchain) | the Windows installation and `C:\Program Files` | Uninstall each program in Windows Settings, or with `winget uninstall` and the package id used to install it. |
| Seven environment variables of your user | the block of [Directory layout and environment variables](#directory-layout-and-environment-variables) | the user environment of Windows | The second block below removes them. |
| The layout folders: the dependencies, the virtual environment, the clones, the install prefix | the steps of this document | `C:\qar` | Delete the folder; the second block below does it. The ROS 2 distribution and the downloads are inside it. |
| The QarOS SDK | [The QarOS SDK](#the-qaros-sdk) | the folder that `QAR_SDK_ROOT` names, `C:\QarOS\install` in this documentation, and the zip file in its parent folder `C:\QarOS` | No block below removes it; the paragraph below the table says how to remove it. |
| The results of the headless sample | `scripts\run_ogre_sample.ps1` | the folder `qar_ogre_sample` in `%TEMP%`, or the folder that you gave as `-OutDir` | Delete the folder. |
| The Windows graphics preference of the live example | the optional command of [Before you start](GETTING_STARTED.md#before-you-start) | a registry value below HKCU:\Software\Microsoft\DirectX\UserGpuPreferences, named with the full path of the program file of `qar_ogre_live_stream_example` | Delete that registry value, or remove the program in Windows Settings under System, Display, Graphics. |
| The logs of the QarOS library | QarOS, when an application connects to it | the folder `quaternar` in `%TEMP%` | Delete the folder. It can grow to gigabytes when trace logging is on. |

The QarOS SDK is a product of QuaternAR that you may use for other work, so no block of this document deletes it. If you unpacked it only for this stack, delete the folder that `QAR_SDK_ROOT` names yourself, before you run the blocks below, because they remove the variable. Never delete the parent of that folder with a command that takes it from the variable: with a layout of your own, the parent can be a whole drive or a system folder. In the layout of this documentation the parent `C:\QarOS` holds the zip file `C:\QarOS\qaros_sdk.zip` next to the folder `C:\QarOS\install`; delete `C:\QarOS` by hand when nothing else is in it.

Close every program that uses the layout first, and go back to your home folder, so that the window does not stand in a folder that the blocks remove:

```powershell
Set-Location "$env:USERPROFILE"
```

The first block lists what the second one deletes and changes nothing: it prints the folder and runs the removal with `-WhatIf`, which only reports what it would do. Read the folder before you go on, because the second block deletes without asking. Both blocks stop with an error when `QAR_ROOT` is empty, does not start with a drive letter, a colon and a backslash (so a network path and every path that starts with two backslashes are refused), is the root of a drive, or is your user profile folder or a folder above it.

```powershell
$folder = $env:QAR_ROOT
if (-not $folder -or $folder -notmatch '^[A-Za-z]:\\') {
    throw 'QAR_ROOT is empty or does not start with a drive letter and a backslash: nothing is removed.'
}
$full = [System.IO.Path]::GetFullPath($folder).TrimEnd('\')
$profileFolder = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\')
if ($full -eq [System.IO.Path]::GetPathRoot($full).TrimEnd('\') -or
    $full -eq $profileFolder -or
    $profileFolder.StartsWith("$full\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove '$full': it is a drive, your user profile folder or a folder above it."
}
"Folder to remove: $full"
foreach ($name in 'QAR_DEPS_ROOT', 'ROS2_INSTALL_ROOT', 'QAR_QT_ROOT', 'QAR_VENV', 'QAR_PREFIX') {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ($value -and -not "$value\".StartsWith("$full\", [System.StringComparison]::OrdinalIgnoreCase)) {
        "Not removed, outside that folder: $name = $value"
    }
}
Remove-Item -LiteralPath $full -Recurse -Force -WhatIf
```

> Success: the block prints one line that starts with "Folder to remove:" and gives the full path of the folder that `QAR_ROOT` names, one line that starts with "Not removed, outside that folder:" for each of the other folder variables that points outside it, and a line from `-WhatIf` that starts with "What if: Performing the operation" and names the folder. It deletes nothing. When `QAR_ROOT` is not a safe folder, it ends with an error that says so.
>
> This block changes nothing and prints your own folders, so no output is shown here.

The second block deletes the folder that `QAR_ROOT` names and then the seven variables, and nothing else. It leaves the QarOS SDK, the other items of the table and the programs of the toolchain alone. If you gave `QAR_DEPS_ROOT`, `ROS2_INSTALL_ROOT`, `QAR_QT_ROOT`, `QAR_VENV` or `QAR_PREFIX` a value outside the folder of `QAR_ROOT`, delete those folders yourself: the first block prints them. Run the second block when the first one shows the folder that you want to remove:

```powershell
$folder = $env:QAR_ROOT
if (-not $folder -or $folder -notmatch '^[A-Za-z]:\\') {
    throw 'QAR_ROOT is empty or does not start with a drive letter and a backslash: nothing is removed.'
}
$full = [System.IO.Path]::GetFullPath($folder).TrimEnd('\')
$profileFolder = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\')
if ($full -eq [System.IO.Path]::GetPathRoot($full).TrimEnd('\') -or
    $full -eq $profileFolder -or
    $profileFolder.StartsWith("$full\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove '$full': it is a drive, your user profile folder or a folder above it."
}
Remove-Item -LiteralPath $full -Recurse -Force
foreach ($name in 'QAR_ROOT', 'QAR_DEPS_ROOT', 'ROS2_INSTALL_ROOT', 'QAR_QT_ROOT', 'QAR_VENV', 'QAR_PREFIX', 'QAR_SDK_ROOT') {
    [Environment]::SetEnvironmentVariable($name, $null, 'User')
}
```

> Success: the command prints nothing and ends without an error. Afterwards the folder `C:\qar` is gone and the seven variables are deleted from the environment of your Windows user: a new PowerShell 7 window shows them as empty, while a window that was already open keeps the values it had. The QarOS SDK is still there.
>
> This command changes your machine, so no output is shown here.

## Next steps

Continue with [Getting started](GETTING_STARTED.md): it runs the headless sample, checks the connection to QarOS, streams the sample scene to a Visualizer and shows how to use the library from your own Ogre application. [Testing](TESTING.md) explains what the tests cover, [Configuration](CONFIGURATION.md) lists every build option and environment variable, and [Troubleshooting](TROUBLESHOOTING.md) helps when a step fails.
