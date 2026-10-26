# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Runs the standalone Ogre streaming sample (examples/ogre_standalone_streaming_sample), the
    reusability proof for qar_ogre_streaming: it needs Ogre only, no RViz, Qt or ROS.

.DESCRIPTION
    The sample links only qar_ogre_streaming and Ogre. At run time it needs the Ogre runtime
    DLLs (OgreMain.dll, RenderSystem_GL*.dll and their dependencies), which live in the
    rviz_ogre_vendor bin folder of the ROS 2 Humble distribution. The script puts that folder
    on PATH and runs the sample.

    Build the sample first with the repository's CMake project (the examples are on by default;
    option QAR_BUILD_EXAMPLES):
        cmake --preset vs2019-x64
        cmake --build --preset vs2019-x64

.PARAMETER BuildDir
    CMake build folder. Default: <repository>\build\vs2019-x64.
.PARAMETER Config
    Build configuration of the sample (default RelWithDebInfo).
.PARAMETER Ticks
    Number of batched render ticks (default 120).
.PARAMETER Peers
    Number of synthetic in-process stereo peers, at least 2 (default 3).
.PARAMETER CaptureEvery
    Also capture every N ticks (0 = final only; default 0).
.PARAMETER Width
    Per-eye texture width (default 1280).
.PARAMETER Height
    Per-eye texture height (default 720).
.PARAMETER RenderSystem
    auto|gl3plus|gl (default auto, which selects the plain GL render system, not GL3+).
.PARAMETER OutDir
    Output folder for PNGs and reports. Default: %TEMP%\qar_ogre_sample.
.PARAMETER Ros2Root
    ROS 2 Humble distribution root. Default: the environment variable
    ROS2_INSTALL_ROOT.
#>
param(
    [string]$BuildDir = "",
    [string]$Config = "RelWithDebInfo",
    [int]$Ticks = 120,
    [int]$Peers = 3,
    [int]$CaptureEvery = 0,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$RenderSystem = "auto",
    [string]$OutDir = "",
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

if ([string]::IsNullOrEmpty($BuildDir)) { $BuildDir = Join-Path $RepoRoot "build\vs2019-x64" }
$exe = Join-Path $BuildDir "examples\ogre_standalone_streaming_sample\$Config\qar_ogre_standalone_streaming_sample.exe"
if (-not (Test-Path $exe)) {
    throw "Cannot run: sample executable not found: $exe`nBuild it: cmake --preset vs2019-x64 && cmake --build --preset vs2019-x64"
}

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
$ogreBin = Join-Path $Ros2Root "opt\rviz_ogre_vendor\bin"
if (-not (Test-Path $ogreBin)) {
    throw "Ogre bin folder not found: $ogreBin (the distribution has no rviz_ogre_vendor)"
}

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path $env:TEMP "qar_ogre_sample"
}

$env:PATH = "$ogreBin;$env:PATH"

Write-Host "Running sample: $exe"
Write-Host "  Ogre DLLs from: $ogreBin"
Write-Host "  Output dir    : $OutDir"

& $exe --ticks $Ticks --peers $Peers --capture-every $CaptureEvery `
    --width $Width --height $Height --render-system $RenderSystem `
    --out-dir $OutDir --ogre-plugin-dir $ogreBin
$code = $LASTEXITCODE
Write-Host "Sample exited with code $code"
exit $code
