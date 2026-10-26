# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Builds spdlog.dll from source and installs it into the bin and Lib folders of the ROS 2
    Humble Windows binary distribution, filling a packaging gap of the official release archive.

.DESCRIPTION
    The official ros2-humble-*-windows-release-amd64.zip archive ships the CMake and share
    metadata of the rcl_logging_spdlog and spdlog_vendor packages, but not the spdlog.dll
    binary that rcl_logging_spdlog.dll links against. Without it rcl.dll (used by rclpy and by
    rclcpp / rviz2.exe) fails to load with "DLL load failed ... module could not be found",
    which blocks `ros2 --help`, `import rclpy` and rviz2.exe alike.

    Why spdlog v1.9.2: the mangled C++ symbols that rcl_logging_spdlog.dll imports from
    spdlog.dll (`dumpbin /imports`) contain `fmt::v8::basic_string_view<char>` in the signature
    of `logger::log(...)`. That proves the required spdlog bundles fmt 8 or newer (spdlog 1.9.0
    or newer), which is newer than the 1.8.2 pin in the reference ros2.repos of this release.
    The script builds spdlog v1.9.2 (it bundles fmt 8.0.1) with the same MSVC v142 toolset and
    the RelWithDebInfo configuration as the rest of the distribution, then checks with
    `dumpbin /exports` that the required symbols are exported.

    Idempotent: nothing happens when <Ros2Root>\bin\spdlog.dll already exists. Delete the file
    to force a rebuild.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution (the folder with bin, Lib, share and
    local_setup.bat). Default: the environment variable ROS2_INSTALL_ROOT. Required.

.PARAMETER DepsRoot
    Dependency root. Only used for the default of -BuildRoot. Default: the environment
    variable QAR_DEPS_ROOT.

.PARAMETER BuildRoot
    Scratch folder for downloading and building spdlog. Default: <DepsRoot>\build_spdlog.
    The download goes to the "downloads" folder next to it.

.PARAMETER SpdlogVersion
    spdlog release tag to build. Default: v1.9.2. If a later release of the distribution ships
    a different spdlog, derive the required version again from `dumpbin /imports` of
    rcl_logging_spdlog.dll (look for the fmt::vN inline namespace in the mangled
    `logger::log` symbol) before changing the default.

.EXAMPLE
    ./scripts/fix_missing_spdlog_dll.ps1 -Ros2Root C:\qar\deps\ros2_humble\ros2-windows
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [string]$BuildRoot,
    [string]$SpdlogVersion = "v1.9.2"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
if ([string]::IsNullOrEmpty($BuildRoot)) {
    $DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' -EnvVar 'QAR_DEPS_ROOT' `
        -What 'Dependency root' -Section 'Directory layout and environment variables' -AllowMissing
    $BuildRoot = Join-Path $DepsRoot "build_spdlog"
}

$targetDll = Join-Path $Ros2Root "bin\spdlog.dll"
if (Test-Path $targetDll) {
    Write-Host "spdlog.dll already present at $targetDll - nothing to do. Delete it to force a rebuild."
    exit 0
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$zipPath = Join-Path (Split-Path $BuildRoot) "downloads\spdlog-$SpdlogVersion.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $zipPath) | Out-Null

if (-not (Test-Path $zipPath)) {
    $url = "https://github.com/gabime/spdlog/archive/refs/tags/$SpdlogVersion.zip"
    Write-Host "Downloading $url"
    # The download goes to <name>.part and is renamed when it is complete, so an interrupted
    # run leaves no file that a later run would take for the archive.
    $partPath = "$zipPath.part"
    Remove-Item -LiteralPath $partPath -Force -ErrorAction SilentlyContinue
    $response = Invoke-WebRequest -Uri $url -OutFile $partPath -UseBasicParsing -PassThru
    # Windows PowerShell 5.1 does not fail when the connection ends early, so the size of
    # the file is compared with the length that the server announced (when it announced
    # one and sent the body unencoded).
    $size = (Get-Item -LiteralPath $partPath).Length
    $announced = @($response.Headers['Content-Length'])[0]
    $encoded = @($response.Headers['Content-Encoding'])[0]
    $short = -not $encoded -and $announced -match '^\d+$' -and [int64]$announced -ne $size
    if ($size -eq 0 -or $short) {
        throw "The download of $url is incomplete: $size bytes arrived, $announced announced."
    }
    Move-Item -LiteralPath $partPath -Destination $zipPath -Force
}

$srcDir = Join-Path $BuildRoot "spdlog-$($SpdlogVersion.TrimStart('v'))"
if (-not (Test-Path $srcDir)) {
    Write-Host "Extracting to $BuildRoot"
    try {
        Expand-Archive -Path $zipPath -DestinationPath $BuildRoot -Force
    } catch {
        # a later run must find neither an unreadable archive nor a partly unpacked folder
        Remove-Item -LiteralPath $srcDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue
        throw ("Cannot unpack ${zipPath}: $($_.Exception.Message) The archive and the partly " +
            "unpacked folder were deleted; run the script again to download it again.")
    }
}

$buildDir = Join-Path $BuildRoot "build"
Write-Host "Configuring (VS2019 x64, matching the distro's v142 toolset)"
cmake -S $srcDir -B $buildDir -G "Visual Studio 16 2019" -A x64 `
    -DSPDLOG_BUILD_SHARED=ON `
    -DSPDLOG_BUILD_EXAMPLE=OFF `
    -DSPDLOG_BUILD_TESTS=OFF `
    -DCMAKE_INSTALL_PREFIX="$BuildRoot\install"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building spdlog (RelWithDebInfo, matching the distro's build config)"
cmake --build $buildDir --config RelWithDebInfo --target spdlog
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$builtDll = Join-Path $buildDir "RelWithDebInfo\spdlog.dll"
$builtLib = Join-Path $buildDir "RelWithDebInfo\spdlog.lib"

Write-Host "Verifying required symbols are present (spot-check against rcl_logging_spdlog.dll's import table)"
$dumpbinCandidates = $null
try {
    $dumpbinCandidates = Get-ChildItem (Join-Path (Get-QarVsInstallPath) "VC\Tools\MSVC") -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1
} catch { }
if ($dumpbinCandidates) {
    $exports = & $dumpbinCandidates.FullName /exports $builtDll 2>&1 | Out-String
    if ($exports -notmatch "register_logger" -or $exports -notmatch "basic_file_sink") {
        Write-Warning "Built spdlog.dll does not appear to export expected symbols - ABI mismatch is possible. Re-check SpdlogVersion."
    } else {
        Write-Host "Symbol spot-check OK (register_logger, basic_file_sink found)."
    }
} else {
    Write-Warning "dumpbin.exe not found - skipping symbol verification."
}

Write-Host "Installing into distro: $targetDll"
Copy-Item $builtDll $targetDll -Force
Copy-Item $builtLib (Join-Path $Ros2Root "Lib\spdlog.lib") -Force

$hash = Get-FileHash $targetDll -Algorithm SHA256
Write-Host "Done. spdlog.dll SHA256: $($hash.Hash)"
Write-Host "Verify with: ros2 --help (through ros2-script.py) or rviz2.exe."
