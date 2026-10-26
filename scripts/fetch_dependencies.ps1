# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Idempotently downloads and extracts the ROS 2 Humble Windows binary distribution and its
    reference source archive, verifying the SHA256 of each download.

.DESCRIPTION
    Downloads (to <DepsRoot>\downloads\):
      - ros2-humble-20260220-windows-release-amd64.zip  (the binary distribution)
      - ros2-release-humble-20260220-src.zip             (reference-only manifest repository)
    then extracts them to <DepsRoot>\ros2_humble\ and
    <DepsRoot>\ros2_src_release-humble-20260220\ respectively. The distribution ends up in
    <DepsRoot>\ros2_humble\ros2-windows; that folder is the value for ROS2_INSTALL_ROOT.

    Safe to re-run: when a zip already exists and its SHA256 matches the recorded value, the
    download is skipped. Extraction is skipped when the destination already looks populated
    (pass -Force to extract again).

    The downloads use PowerShell's Invoke-WebRequest (-UseBasicParsing). The distribution has
    gaps that the repair scripts in this folder fill; run them after fetching.

.PARAMETER DepsRoot
    Root folder for downloads and extraction. Default: the environment variable
    QAR_DEPS_ROOT. Required; the folder is created when it does not exist.

.PARAMETER Force
    Extract again even if the destination folder already exists and looks populated.

.EXAMPLE
    ./scripts/fetch_dependencies.ps1 -DepsRoot C:\qar\deps
    ./scripts/fetch_dependencies.ps1 -Force
#>
param(
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' -EnvVar 'QAR_DEPS_ROOT' `
    -What 'Dependency root' -Section 'Directory layout and environment variables' -AllowMissing

$downloads = Join-Path $DepsRoot "downloads"
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

$items = @(
    @{
        Name       = "ROS 2 Humble Windows binary distro"
        Url        = "https://github.com/ros2/ros2/releases/download/release-humble-20260220/ros2-humble-20260220-windows-release-amd64.zip"
        ZipName    = "ros2-humble-20260220-windows-release-amd64.zip"
        Sha256     = "256ABCFA37F8D97D2CB3FF2A85F53465B016A25632399C99F67DC2BEE18E89C3"
        ExtractTo  = Join-Path $DepsRoot "ros2_humble"
        # actual layout after extraction: ros2_humble\ros2-windows\{bin,Lib,include,share,opt,...}
        SentinelPath = Join-Path $DepsRoot "ros2_humble\ros2-windows\local_setup.bat"
    },
    @{
        Name       = "ROS 2 source manifest repo (reference only)"
        Url        = "https://github.com/ros2/ros2/archive/refs/tags/release-humble-20260220.zip"
        ZipName    = "ros2-release-humble-20260220-src.zip"
        Sha256     = "EA72D8921072A824D65E592F01E53A192766CD9AC50CA84D5EAB8F475474A28F"
        ExtractTo  = Join-Path $DepsRoot "ros2_src_release-humble-20260220"
        SentinelPath = Join-Path $DepsRoot "ros2_src_release-humble-20260220\ros2-release-humble-20260220\ros2.repos"
    }
)

function Test-Sha256Matches($path, $expected) {
    if (-not (Test-Path $path)) { return $false }
    $actual = (Get-FileHash $path -Algorithm SHA256).Hash
    return ($actual -eq $expected)
}

foreach ($item in $items) {
    Write-Host "=== $($item.Name) ==="
    $zipPath = Join-Path $downloads $item.ZipName

    if (Test-Sha256Matches $zipPath $item.Sha256) {
        Write-Host "Already downloaded and SHA256 verified: $zipPath"
    } else {
        Write-Host "Downloading $($item.Url)"
        Invoke-WebRequest -Uri $item.Url -OutFile $zipPath -UseBasicParsing
        if (-not (Test-Sha256Matches $zipPath $item.Sha256)) {
            $actual = (Get-FileHash $zipPath -Algorithm SHA256).Hash
            throw "SHA256 mismatch for $($item.ZipName): expected $($item.Sha256), got $actual"
        }
        Write-Host "Downloaded and verified: $zipPath"
    }

    if ((Test-Path $item.SentinelPath) -and -not $Force) {
        Write-Host "Already extracted (sentinel found: $($item.SentinelPath)). Use -Force to re-extract."
    } else {
        Write-Host "Extracting to $($item.ExtractTo)"
        New-Item -ItemType Directory -Force -Path $item.ExtractTo | Out-Null
        Expand-Archive -Path $zipPath -DestinationPath $item.ExtractTo -Force
    }
    Write-Host ""
}

Write-Host "Done. Binary distro: $($items[0].ExtractTo)\ros2-windows"
Write-Host "Reminder: the binary distro has packaging gaps - run the repair scripts next:"
Write-Host "fix_missing_spdlog_dll.ps1, fix_missing_vendor_dlls.ps1, fix_missing_openssl_dll.ps1, fix_qt_conda_imports.ps1."
