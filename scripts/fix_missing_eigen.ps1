# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Provides Eigen3 (headers and CMake config) in the ROS 2 Humble Windows binary
    distribution, so that downstream ament plugins that call find_package(rviz_common) can
    configure.

.DESCRIPTION
    find_package(rviz_common) transitively runs
        rviz_common -> rviz_rendering -> find_package(Eigen3 REQUIRED)
    through eigen3_cmake_module's FindEigen3.cmake, which calls find_package(Eigen3 CONFIG)
    and requires EIGEN3_FOUND. The official release-humble-20260220 Windows archive ships no
    Eigen headers and no Eigen3Config.cmake, so every ament plugin build fails with
        "Could NOT find Eigen3 (missing: EIGEN3_FOUND)".

    Eigen is header-only. The script copies the Eigen 3.4.0 headers into the distribution and
    writes a self-contained Eigen3Config.cmake. It needs no conda installation. It
      1. does not look for a source when <Ros2Root>\opt\eigen3_vendor\include\eigen3 already
         holds the headers;
      2. otherwise takes the headers from -EigenSourceInclude (a folder like .../include/eigen3),
         or from the source archive eigen-3.4.0.tar.bz2, which it takes from -SourceArchive or
         from the first of three download locations (Debian, Ubuntu, GitLab) that answers with a
         file of the SHA-256 that the script pins (the file goes to <DepsRoot>\downloads; a
         download with another hash is deleted and never unpacked);
      3. unpacks the entries Eigen, unsupported and signature_of_eigen3_matrix_library of the
         archive with tar into <DepsRoot>\build_eigen, after it has checked that no member of the
         archive would leave that folder, and copies them to
         <Ros2Root>\opt\eigen3_vendor\include\eigen3;
      4. writes Eigen3Config.cmake and Eigen3ConfigVersion.cmake to
         <Ros2Root>\share\eigen3\cmake.
    Idempotent.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.

.PARAMETER DepsRoot
    Dependency root. The download goes to <DepsRoot>\downloads and the scratch files to
    <DepsRoot>\build_eigen. Default: the environment variable QAR_DEPS_ROOT. Not needed when the
    headers are already installed or -EigenSourceInclude is given.

.PARAMETER SourceArchive
    A local copy of eigen-3.4.0.tar.bz2 (the file of the three download locations, which have
    the same content) for a machine without access to them. It must have the pinned SHA-256.

.PARAMETER EigenSourceInclude
    Path of a source '.../include/eigen3' folder (it contains 'Eigen' and
    'signature_of_eigen3_matrix_library'). It wins over -SourceArchive and the download.

.EXAMPLE
    ./scripts/fix_missing_eigen.ps1

.EXAMPLE
    ./scripts/fix_missing_eigen.ps1 -SourceArchive C:\transfer\eigen-3.4.0.tar.bz2
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [string]$SourceArchive = "",
    [string]$EigenSourceInclude = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'

$archiveName = "eigen-3.4.0.tar.bz2"
$archiveSha256 = "B4C198460EBA6F28D34894E3A5710998818515104D6E74E5CC331CE31E46E626"
$archiveUrls = @(
    "https://deb.debian.org/debian/pool/main/e/eigen3/eigen3_3.4.0.orig.tar.bz2",
    "https://archive.ubuntu.com/ubuntu/pool/universe/e/eigen3/eigen3_3.4.0.orig.tar.bz2",
    "https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.bz2"
)
# The archive has one top folder; these entries of it make up an include/eigen3 folder.
$topFolder = "eigen-3.4.0"
$entries = @("Eigen", "unsupported", "signature_of_eigen3_matrix_library")

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# Runs tar with its output captured; Windows PowerShell 5.1 would turn a line on stderr into an
# error. Returns the exit code, the text goes to $script:nativeText.
function Invoke-Tar([string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & tar @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    $script:nativeText = ($output | Out-String).Trim()
    return $code
}

# The version of the Eigen headers in an include/eigen3 folder, "3.4.0" for example, or $null.
function Get-EigenVersion([string]$Include) {
    $macros = Join-Path $Include "Eigen\src\Core\util\Macros.h"
    if (-not (Test-Path -LiteralPath $macros -PathType Leaf)) { return $null }
    $part = @{}
    foreach ($line in Get-Content -LiteralPath $macros -TotalCount 60) {
        if ($line -match '^#define EIGEN_(WORLD|MAJOR|MINOR)_VERSION\s+(\d+)') {
            $part[$Matches[1]] = $Matches[2]
        }
    }
    if ($part.Count -ne 3) { return $null }
    return "$($part['WORLD']).$($part['MAJOR']).$($part['MINOR'])"
}

# --- Locate a source Eigen 3.4.0 (include/eigen3), unless the distribution has the headers ---
$destInclude = Join-Path $Ros2Root "opt\eigen3_vendor\include\eigen3"
$headersPresent = Test-Path (Join-Path $destInclude "signature_of_eigen3_matrix_library")
if (-not $headersPresent) {
    if ($EigenSourceInclude) {
        if (-not (Test-Path (Join-Path $EigenSourceInclude "Eigen"))) {
            throw ("-EigenSourceInclude holds no 'Eigen' folder: $EigenSourceInclude. " +
                "Pass the '.../include/eigen3' folder of an Eigen 3.4.0 installation.")
        }
    } else {
        $DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' `
            -EnvVar 'QAR_DEPS_ROOT' -What 'Dependency root' `
            -Section 'Directory layout and environment variables' -AllowMissing
        $BuildRoot = Join-Path $DepsRoot "build_eigen"
        $downloads = Join-Path $DepsRoot "downloads"
        New-Item -ItemType Directory -Force -Path $BuildRoot, $downloads | Out-Null
        if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
            throw "tar not found on PATH (see docs/INSTALL.md, section 'Toolchain')."
        }

        # --- the source archive, checked by its SHA-256 ---------------------------------------
        $archive = Join-Path $downloads $archiveName
        if ($SourceArchive) {
            if (-not (Test-Path -LiteralPath $SourceArchive -PathType Leaf)) {
                throw "-SourceArchive file does not exist or cannot be reached: $SourceArchive"
            }
            $archive = (Resolve-Path -LiteralPath $SourceArchive).Path
            $actual = Get-FileSha256 $archive
            if ($actual -ne $archiveSha256) {
                throw "SHA-256 mismatch for ${archive}: expected $archiveSha256, got $actual."
            }
            Write-Host "Using $archive (SHA-256 verified)"
        } elseif ((Test-Path -LiteralPath $archive) -and
            (Get-FileSha256 $archive) -eq $archiveSha256) {
            Write-Host "Already downloaded and SHA-256 verified: $archive"
        } else {
            $verified = $false
            foreach ($url in $archiveUrls) {
                Write-Host "Downloading $url"
                try {
                    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
                } catch {
                    Write-Host "  failed: $($_.Exception.Message)"
                    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
                    continue
                }
                if ((Get-FileSha256 $archive) -eq $archiveSha256) { $verified = $true; break }
                Write-Host "  the SHA-256 of the file is not $archiveSha256; the file is discarded"
                Remove-Item -LiteralPath $archive -Force
            }
            if (-not $verified) {
                throw ("No download location answered with the file (SHA-256 $archiveSha256). " +
                    "Download $archiveName elsewhere and pass it with -SourceArchive, or pass " +
                    "an Eigen 3.4.0 include/eigen3 folder with -EigenSourceInclude.")
            }
            Write-Host "Downloaded and verified: $archive"
        }

        # --- unpack the three entries ----------------------------------------------------------
        Write-Host "Extracting $(Split-Path $archive -Leaf)"
        $code = Invoke-Tar @("-tjf", $archive)
        if ($code -ne 0) { throw "tar could not read ${archive} (status ${code}): $nativeText" }
        foreach ($name in ($nativeText -split "`r?`n")) {
            $segments = $name -split '[\\/]'
            if ($name -match '^([A-Za-z]:|[\\/])' -or $segments -contains '..' -or
                ($segments[0] -ne $topFolder)) {
                throw "The archive holds a member that is not below ${topFolder}: $name"
            }
        }
        $unpacked = Join-Path $BuildRoot $topFolder
        if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
        $members = @($entries | ForEach-Object { "$topFolder/$_" })
        $code = Invoke-Tar (@("-xjf", $archive, "-C", $BuildRoot) + $members)
        if ($code -ne 0) {
            throw "Extracting the source archive failed (status ${code}): $nativeText"
        }
        foreach ($name in $entries) {
            if (-not (Test-Path -LiteralPath (Join-Path $unpacked $name))) {
                throw "The source archive has no $topFolder/$name."
            }
        }
        $eigenVersion = Get-EigenVersion $unpacked
        if ($eigenVersion -ne "3.4.0") {
            throw "The unpacked headers are Eigen '$eigenVersion', not 3.4.0."
        }
        $EigenSourceInclude = $unpacked
    }
    Write-Host "Eigen source: $EigenSourceInclude" -ForegroundColor Cyan
}

# --- Copy headers into the distro ------------------------------------------
if (-not $headersPresent) {
    New-Item -ItemType Directory -Force -Path $destInclude | Out-Null
    Copy-Item -Path (Join-Path $EigenSourceInclude "*") -Destination $destInclude -Recurse -Force
    Write-Host "Copied Eigen headers -> $destInclude" -ForegroundColor Green
} else {
    Write-Host "Eigen headers already present -> $destInclude" -ForegroundColor Green
}

# --- Write a self-contained Eigen3Config.cmake -----------------------------
$cmakeDir = Join-Path $Ros2Root "share\eigen3\cmake"
New-Item -ItemType Directory -Force -Path $cmakeDir | Out-Null

# Forward slashes for CMake.
$incForCMake = ($destInclude -replace '\\', '/')

$config = @"
# Self-contained Eigen3 config generated by scripts/fix_missing_eigen.ps1 to fill
# a packaging gap in the ROS 2 Humble Windows binary distribution (no Eigen shipped).
set(EIGEN3_VERSION_STRING "3.4.0")
set(Eigen3_VERSION "3.4.0")
set(EIGEN3_FOUND TRUE)
set(Eigen3_FOUND TRUE)
set(EIGEN3_INCLUDE_DIR "$incForCMake")
set(EIGEN3_INCLUDE_DIRS "$incForCMake")
set(EIGEN3_ROOT_DIR "$incForCMake")

if(NOT TARGET Eigen3::Eigen)
  add_library(Eigen3::Eigen INTERFACE IMPORTED)
  set_target_properties(Eigen3::Eigen PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "$incForCMake")
endif()
"@
Set-Content -Path (Join-Path $cmakeDir "Eigen3Config.cmake") -Value $config -Encoding UTF8

$version = @"
set(PACKAGE_VERSION "3.4.0")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if(PACKAGE_VERSION VERSION_EQUAL PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
endif()
"@
Set-Content -Path (Join-Path $cmakeDir "Eigen3ConfigVersion.cmake") -Value $version -Encoding UTF8

Write-Host "Wrote Eigen3Config.cmake -> $cmakeDir" -ForegroundColor Green
Write-Host "find_package(Eigen3) will now succeed for downstream ament plugin builds." -ForegroundColor Green
