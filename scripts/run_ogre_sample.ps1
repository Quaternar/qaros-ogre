<#
.SYNOPSIS
    Run the standalone Ogre streaming sample (examples/ogre_standalone_streaming_sample),
    the reusability proof for qar_ogre_streaming.

.DESCRIPTION
    The sample links ONLY qar_ogre_streaming + Ogre (NO rviz/Qt/ROS). At runtime it
    needs the Ogre runtime DLLs (OgreMain.dll + RenderSystem_GL*.dll and their deps),
    which live in the rviz_ogre_vendor bin dir of the ROS 2 Humble distro. This
    script prepends that dir to PATH and runs the sample.

    Build the sample first with the repo-root CMake project (examples are ON by
    default; QAR_BUILD_EXAMPLES):
        cmake --preset vs2019-x64
        cmake --build --preset vs2019-x64

.PARAMETER Ticks        Number of batched render ticks (default 120).
.PARAMETER Peers        Number of mock stereo peers, >=2 (default 3).
.PARAMETER CaptureEvery Also capture every M ticks (0 = final only; default 0).
.PARAMETER Width        Per-eye texture width (default 1280).
.PARAMETER Height       Per-eye texture height (default 720).
.PARAMETER RenderSystem auto|gl3plus|gl (default auto -> legacy GL, the M8 default).
.PARAMETER OutDir       Output directory for PNGs/reports (default build/qar_sample_out).
.PARAMETER Ros2Root     ROS 2 Humble distro root (…/ros2-windows). Default: env
                        ROS2_INSTALL_ROOT or D:\development\ros2_humble\ros2-windows.
#>
param(
    [int]$Ticks = 120,
    [int]$Peers = 3,
    [int]$CaptureEvery = 0,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$RenderSystem = "auto",
    [string]$OutDir = "",
    [string]$Ros2Root = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path

if ([string]::IsNullOrEmpty($Ros2Root)) {
    $Ros2Root = if ($env:ROS2_INSTALL_ROOT) { $env:ROS2_INSTALL_ROOT } else { "D:\development\ros2_humble\ros2-windows" }
}
$ogreBin = Join-Path $Ros2Root "opt\rviz_ogre_vendor\bin"
if (-not (Test-Path $ogreBin)) {
    throw "Ogre bin dir not found: $ogreBin (set -Ros2Root or ROS2_INSTALL_ROOT)"
}

$exe = Join-Path $RepoRoot "build\vs2019-x64\examples\ogre_standalone_streaming_sample\RelWithDebInfo\qar_ogre_standalone_streaming_sample.exe"
if (-not (Test-Path $exe)) {
    throw "Sample exe not found: $exe`nBuild it: cmake --preset vs2019-x64 && cmake --build --preset vs2019-x64"
}

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path $RepoRoot "build\qar_sample_out"
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
