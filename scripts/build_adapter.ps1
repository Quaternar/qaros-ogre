<#
.SYNOPSIS
    Build qar_ogre_streaming + qar_qar_adapter as a colcon workspace (Windows/MSVC).

.DESCRIPTION
    colcon's cmake build task refuses to run on Windows unless the Visual Studio
    developer environment is already imported into the shell — it fails immediately
    with "VisualStudioVersion is not set, please run within a Visual Studio Command
    Prompt", regardless of which CMake generator ends up being used. A plain
    `colcon build` from an ordinary PowerShell window always hits this.

    This script imports that environment (VS2019 v142 x64, matching the ROS 2
    Humble Windows distro's compiled toolset) via vswhere + vcvars64.bat, then runs
    `colcon build` with the Ninja generator, RelWithDebInfo.

    Plain, non-colcon builds (`cmake --preset vs2019-x64`, see README.md) do NOT
    need this — the Visual Studio CMake generator drives MSBuild directly and does
    not depend on an imported shell environment. Use THIS script specifically when
    you want the colcon-installed, find_package()-consumable packages: to build
    this repository together with qar_rviz_plugin, or to reproduce exactly what
    that plugin's own colcon build expects to find.

.PARAMETER Ros2InstallRoot
    Root of the extracted ROS 2 Humble Windows distro. Default: environment
    variable ROS2_INSTALL_ROOT, or D:\development\ros2_humble\ros2-windows.
.PARAMETER Clean
    Remove build/ install/ log/ before building.
#>
param(
    [string]$Ros2InstallRoot = $(if ($env:ROS2_INSTALL_ROOT) { $env:ROS2_INSTALL_ROOT } else { "D:\development\ros2_humble\ros2-windows" }),
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

if (-not (Test-Path $Ros2InstallRoot)) {
    throw "ROS 2 distro not found at $Ros2InstallRoot. Pass -Ros2InstallRoot <path> or set ROS2_INSTALL_ROOT."
}

function Import-BatchEnvironment {
    param([string]$BatchFile, [string]$BatchArgs = "")
    if (-not (Test-Path $BatchFile)) { throw "Batch file not found: $BatchFile" }

    # vcvars64.bat (and similar toolchain batch files) fail with "The input line is
    # too long" / "The syntax of the command is incorrect" when the CURRENT PATH is
    # already large: their own internal `set PATH=<additions>;%PATH%` line exceeds
    # cmd.exe's ~8191-character line-length limit. That is a function of how much
    # the calling shell/profile has already accumulated on PATH, not of anything
    # this script does. Run the batch file against a short, machine-baseline PATH,
    # then merge the caller's original PATH back in afterward (after what the batch
    # file just set, so its own additions -- e.g. cl.exe -- still take precedence),
    # so nothing already on it is permanently lost.
    $savedPath = $env:PATH
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine')

    $dump = Join-Path $env:TEMP ("qar_env_dump_{0}.txt" -f ([System.Guid]::NewGuid().ToString('N')))
    cmd /c "call `"$BatchFile`" $BatchArgs && set > `"$dump`"" | Out-Null
    if (-not (Test-Path $dump)) {
        $env:PATH = $savedPath
        throw "Failed to capture environment from $BatchFile"
    }
    Get-Content $dump -Encoding UTF8 | ForEach-Object {
        if ($_ -match '^([A-Za-z_][A-Za-z0-9_\(\)]*)=(.*)$') {
            try { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue } catch {}
        }
    }
    Remove-Item $dump -ErrorAction SilentlyContinue
    $env:PATH = "$env:PATH;$savedPath"
}

# --- VS2019 (v142) x64 dev environment (cl.exe, link.exe, VisualStudioVersion) --
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere" }
$vsPath = & $vswhere -version "[16.0,17.0)" -property installationPath -latest | Select-Object -First 1
if (-not $vsPath) { throw "Visual Studio 2019 (v142) not found via vswhere." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
Write-Host "=== Importing VS2019 x64 dev environment ===" -ForegroundColor Cyan
Write-Host "  vcvars64: $vcvars"
Import-BatchEnvironment -BatchFile $vcvars
$vswhereDir = Split-Path -Parent $vswhere
if ($env:PATH -notlike "*$vswhereDir*") { $env:PATH = "$vswhereDir;$env:PATH" }

if (-not $env:VisualStudioVersion) {
    throw "VisualStudioVersion still not set after importing vcvars64.bat -- something is wrong with the VS install."
}

# --- ROS 2 distro environment --------------------------------------------------
# colcon builds each package in its OWN cmake configure process. -DQAR_ROS2_
# INSTALL_ROOT below only reaches qar_ogre_streaming (the one CMakeLists.txt that
# reads it) -- qar_qar_adapter's separate configure calls find_package(
# qar_ogre_streaming), whose exported Config.cmake calls find_dependency(
# rviz_ogre_vendor), and that needs the distro on CMAKE_PREFIX_PATH too. Sourcing
# local_setup.bat puts it there for every package uniformly -- this is also just
# the standard colcon-overlay workflow (source the underlay before building).
$localSetup = Join-Path $Ros2InstallRoot "local_setup.bat"
if (-not (Test-Path $localSetup)) { throw "ROS 2 local_setup.bat not found at $localSetup" }
Write-Host "=== Sourcing ROS 2 distro environment ===" -ForegroundColor Cyan
Import-BatchEnvironment -BatchFile $localSetup

# --- colcon (from this repo's own venv, if one exists) ------------------------
$venvScripts = Join-Path $RepoRoot ".venv\Scripts"
if ((Test-Path $venvScripts) -and ($env:PATH -notlike "*$venvScripts*")) {
    $env:PATH = "$venvScripts;$env:PATH"
}
if (-not (Get-Command colcon -ErrorAction SilentlyContinue)) {
    throw @"
colcon not found on PATH. Create a venv and install it (see README.md step 3):
    "C:\Program Files\Python310\python.exe" -m venv .venv
    .venv\Scripts\pip install colcon-common-extensions
"@
}

# --- colcon build ------------------------------------------------------------
Set-Location $RepoRoot

if ($Clean) {
    Write-Host "=== Clean: removing build/ install/ log/ ===" -ForegroundColor Yellow
    Remove-Item -Recurse -Force "build", "install", "log" -ErrorAction SilentlyContinue
}

$ninja = (Get-Command ninja -ErrorAction SilentlyContinue)
if (-not $ninja) { throw "ninja not found on PATH after vcvars import." }

Write-Host "=== colcon build (Ninja, RelWithDebInfo) ===" -ForegroundColor Cyan
Write-Host "  cl.exe : $((Get-Command cl -ErrorAction SilentlyContinue).Source)"
Write-Host "  ninja  : $($ninja.Source)"
Write-Host "  ROS 2  : $Ros2InstallRoot"

colcon build `
    --event-handlers console_direct+ `
    --cmake-args `
        -G Ninja `
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo" `
        "-DQAR_ROS2_INSTALL_ROOT=$Ros2InstallRoot" `
        --no-warn-unused-cli

$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "colcon build FAILED (exit $code)." -ForegroundColor Red
    exit $code
}

Write-Host ""
Write-Host "=== BUILD OK ===" -ForegroundColor Green
Write-Host "Install prefixes:" -ForegroundColor Green
Get-ChildItem "$RepoRoot\install" -Directory -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
