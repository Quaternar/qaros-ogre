# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Installs the OpenSSL 1.1.1 x64 runtime DLLs (libssl-1_1-x64.dll, libcrypto-1_1-x64.dll)
    that Fast-DDS (fastrtps-2.6.dll) needs but the official ROS 2 Humble Windows distribution
    omits.

.DESCRIPTION
    The default ROS 2 middleware for Humble is rmw_fastrtps_cpp, which loads fastrtps-2.6.dll.
    That Fast-DDS build links OpenSSL 1.1.1 (DDS-Security / TLS) through the DLL names
    "libssl-1_1-x64.dll" and "libcrypto-1_1-x64.dll". Neither is shipped anywhere in the
    release-humble-20260220 Windows archive (the same kind of gap as spdlog and the vendor
    DLLs). Without them rmw_implementation reports "failed to load any RMW implementations"
    and rcl init throws:
        rclcpp::exceptions::RCLError: Failed to initialize init options:
        failed to load any RMW implementations
    rviz2.exe calls this during start-up; the exception is unhandled, std::terminate runs and
    the process exits with code 0xC0000409 without showing a window. It also breaks
    rclpy.init() and every rclcpp node. (Merely importing rclpy works, because only init()
    loads an RMW.)

    FIX: put OpenSSL 1.1.1 x64 DLLs, named exactly libssl-1_1-x64.dll and libcrypto-1_1-x64.dll,
    into the distribution's bin folder. Any OpenSSL 1.1.1 patch level works - the whole 1.1.1
    series is ABI-stable. Do not use OpenSSL 3.x: its libraries have different names
    (libssl-3-x64.dll) and Fast-DDS 2.6 imports the 1.1 names.

    Source: the package openssl-1.1.1w-hcfcfb64_0.conda of the conda-forge channel (OpenSSL
    1.1.1w built for Windows x64). The script needs no conda installation. It
      1. does nothing, and returns, when both DLLs already exist in <Ros2Root>\bin;
      2. otherwise takes the DLLs from -SourceDir (a folder that already holds both), or the
         package from -SourceArchive, or the package from the download location
         https://conda.anaconda.org/conda-forge/win-64/openssl-1.1.1w-hcfcfb64_0.conda
         (the file goes to <DepsRoot>\downloads). A package must have the SHA-256 that the
         script pins; a download with another hash is deleted and never unpacked;
      3. unpacks only the two DLLs into <DepsRoot>\build_openssl\unpacked. The package is a ZIP
         file that holds a tar archive compressed with Zstandard. The script reads it with the
         Python of the virtual environment (-Python, default $env:QAR_VENV\Scripts\python.exe)
         and its module backports.zstd, which "pip install -r requirements.txt" installs. When
         that Python is missing or cannot read the file, it tries the tar.exe of Windows, which
         must support Zstandard. If both fail, the script stops with a message;
      4. checks that both files exist and are OpenSSL 1.1.1, and copies them to <Ros2Root>\bin.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.

.PARAMETER DepsRoot
    Dependency root. The download goes to <DepsRoot>\downloads and the scratch files to
    <DepsRoot>\build_openssl. Default: the environment variable QAR_DEPS_ROOT. Not needed
    when the DLLs are already installed or -SourceDir is given.

.PARAMETER SourceArchive
    A local copy of openssl-1.1.1w-hcfcfb64_0.conda for a machine without access to the
    download location. It must have the pinned SHA-256.

.PARAMETER SourceDir
    A folder that already holds both DLLs, which must be OpenSSL 1.1.1 x64 files. It wins over
    -SourceArchive and the download.

.PARAMETER Python
    The Python that unpacks the package; it needs the module backports.zstd. Default:
    $env:QAR_VENV\Scripts\python.exe.

.EXAMPLE
    ./scripts/fix_missing_openssl_dll.ps1

.EXAMPLE
    ./scripts/fix_missing_openssl_dll.ps1 -SourceArchive C:\transfer\openssl-1.1.1w-hcfcfb64_0.conda
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [string]$SourceArchive = "",
    [string]$SourceDir = "",
    [string]$Python = ""
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'

$bin = Join-Path $Ros2Root "bin"
$needed = @("libssl-1_1-x64.dll", "libcrypto-1_1-x64.dll")
$packageName = "openssl-1.1.1w-hcfcfb64_0.conda"
$packageSha256 = "6D46986FAB161CB1B62F019C06DFC27F2E15CACCD3943B3282D9E59872FA4AD2"
$packageUrls = @(
    "https://conda.anaconda.org/conda-forge/win-64/openssl-1.1.1w-hcfcfb64_0.conda"
)
# Names of the DLLs inside the tar archive of the package.
$members = @("Library/bin/libssl-1_1-x64.dll", "Library/bin/libcrypto-1_1-x64.dll")

$haveAll = $true
foreach ($n in $needed) { if (-not (Test-Path (Join-Path $bin $n))) { $haveAll = $false } }
if ($haveAll) {
    Write-Host "OpenSSL 1.1.1 x64 DLLs already present in $bin -- nothing to do."
    return
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# Returns $null when the folder holds both DLLs and both are OpenSSL 1.1.1, otherwise the text
# of the problem.
function Get-OpenSslPairProblem([string]$Folder) {
    foreach ($n in $needed) {
        $path = Join-Path $Folder $n
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "$n is not in $Folder." }
        $version = [string]((Get-Item -LiteralPath $path).VersionInfo.ProductVersion)
        if (-not $version.StartsWith("1.1.1")) {
            return "$n in $Folder is OpenSSL '$version', but Fast-DDS needs OpenSSL 1.1.1."
        }
    }
    return $null
}

# The Python that unpacks the package: -Python, else the one of the virtual environment, else
# $null (then the script tries tar).
function Find-UnpackPython {
    if ($Python) {
        if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
            throw "-Python names a file that does not exist: $Python"
        }
        return (Resolve-Path -LiteralPath $Python).Path
    }
    $venv = $env:QAR_VENV
    if ($venv) {
        $venv = $venv.Trim().Trim('"')
        $candidate = Join-Path $venv "Scripts\python.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

# Runs a native command with its output captured; Windows PowerShell 5.1 would turn a line on
# stderr into an error. Returns the exit code, the text goes to $script:nativeText.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    $script:nativeText = ($output | Out-String).Trim()
    return $code
}

# Python helper: opens the package (a ZIP file) with zipfile, decompresses its pkg-*.tar.zst
# with backports.zstd and reads the tar archive as a stream. Only the members whose names are
# exactly the ones on the command line are written, each under its base name into the output
# folder; no name from the archive is ever used as a path.
$unpackScript = @'
import os
import sys
import tarfile
import zipfile

package, out_dir = sys.argv[1], sys.argv[2]
wanted = sys.argv[3:]
try:
    from backports import zstd
except ImportError:
    sys.stderr.write("the module backports.zstd is not installed in %s\n" % sys.executable)
    sys.exit(3)
written = {}
with zipfile.ZipFile(package) as outer:
    inner = [n for n in outer.namelist() if n.startswith("pkg-") and n.endswith(".tar.zst")]
    if len(inner) != 1:
        sys.stderr.write("expected one pkg-*.tar.zst in the package, found %d\n" % len(inner))
        sys.exit(2)
    with outer.open(inner[0]) as raw, zstd.ZstdFile(raw) as stream:
        with tarfile.open(fileobj=stream, mode="r|") as tar:
            for member in tar:
                if member.name not in wanted or not member.isfile():
                    continue
                data = tar.extractfile(member).read()
                with open(os.path.join(out_dir, os.path.basename(member.name)), "wb") as out:
                    out.write(data)
                written[member.name] = len(data)
                print("%s: %d bytes" % (member.name, len(data)))
                if len(written) == len(wanted):
                    break
missing = [n for n in wanted if n not in written]
if missing:
    sys.stderr.write("not in the package: %s\n" % ", ".join(missing))
    sys.exit(2)
'@

# Unpacks the two DLLs with Python. Returns $null on success, otherwise the text of the problem.
function Expand-WithPython([string]$PythonExe, [string]$Package, [string]$OutDir) {
    $helper = Join-Path $BuildRoot "unpack_conda_dlls.py"
    Set-Content -LiteralPath $helper -Value $unpackScript -Encoding Ascii
    $code = Invoke-Native $PythonExe (@("-I", $helper, $Package, $OutDir) + $members)
    if ($code -ne 0) { return "$PythonExe ended with status ${code}: $script:nativeText" }
    return $null
}

# Unpacks the two DLLs with tar: first the ZIP file, then the Zstandard archive inside it.
# Returns $null on success, otherwise the text of the problem.
function Expand-WithTar([string]$Package, [string]$OutDir) {
    if (-not (Get-Command tar -ErrorAction SilentlyContinue)) { return "tar was not found on PATH" }
    $outer = Join-Path $BuildRoot "package"
    $files = Join-Path $BuildRoot "files"
    foreach ($folder in $outer, $files) {
        if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
    }
    $code = Invoke-Native "tar" @("-xf", $Package, "-C", $outer)
    if ($code -ne 0) {
        return "tar could not read the package (status ${code}): $script:nativeText"
    }
    $inner = @(Get-ChildItem -LiteralPath $outer -Filter "pkg-*.tar.zst" -File)
    if ($inner.Count -ne 1) { return "the package does not hold exactly one pkg-*.tar.zst" }
    $code = Invoke-Native "tar" (@("-xf", $inner[0].FullName, "-C", $files) + $members)
    if ($code -ne 0) {
        return "tar could not read the archive (status ${code}): $script:nativeText"
    }
    foreach ($n in $needed) {
        $path = Join-Path $files "Library\bin\$n"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "tar did not write $n" }
        Copy-Item -LiteralPath $path -Destination $OutDir -Force
    }
    return $null
}

# --- where the two DLLs come from ------------------------------------------------------------
if ($SourceDir) {
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
        throw "-SourceDir folder does not exist: $SourceDir"
    }
    $src = (Resolve-Path -LiteralPath $SourceDir).Path
} else {
    $DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' -EnvVar 'QAR_DEPS_ROOT' `
        -What 'Dependency root' -Section 'Directory layout and environment variables' -AllowMissing
    $BuildRoot = Join-Path $DepsRoot "build_openssl"
    $downloads = Join-Path $DepsRoot "downloads"
    New-Item -ItemType Directory -Force -Path $BuildRoot, $downloads | Out-Null

    # --- the package, checked by its SHA-256 -------------------------------------------------
    $archive = Join-Path $downloads $packageName
    if ($SourceArchive) {
        if (-not (Test-Path -LiteralPath $SourceArchive -PathType Leaf)) {
            throw "-SourceArchive file does not exist or cannot be reached: $SourceArchive"
        }
        $archive = (Resolve-Path -LiteralPath $SourceArchive).Path
        $actual = Get-FileSha256 $archive
        if ($actual -ne $packageSha256) {
            throw "SHA-256 mismatch for ${archive}: expected $packageSha256, got $actual."
        }
        Write-Host "Using $archive (SHA-256 verified)"
    } elseif ((Test-Path -LiteralPath $archive) -and (Get-FileSha256 $archive) -eq $packageSha256) {
        Write-Host "Already downloaded and SHA-256 verified: $archive"
    } else {
        $verified = $false
        foreach ($url in $packageUrls) {
            Write-Host "Downloading $url"
            try {
                Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
            } catch {
                Write-Host "  failed: $($_.Exception.Message)"
                Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
                continue
            }
            if ((Get-FileSha256 $archive) -eq $packageSha256) { $verified = $true; break }
            Write-Host "  the SHA-256 of the file is not $packageSha256; the file is discarded"
            Remove-Item -LiteralPath $archive -Force
        }
        if (-not $verified) {
            throw ("No download location answered with the file (SHA-256 $packageSha256). " +
                "Download $packageName elsewhere and pass it with -SourceArchive, or pass a " +
                "folder that holds both DLLs with -SourceDir.")
        }
        Write-Host "Downloaded and verified: $archive"
    }

    # --- unpack only the two DLLs --------------------------------------------------------------
    $src = Join-Path $BuildRoot "unpacked"
    if (Test-Path -LiteralPath $src) { Remove-Item -LiteralPath $src -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    Write-Host "Extracting $(Split-Path $archive -Leaf)"
    $unpackPython = Find-UnpackPython
    $problem = "no Python of the virtual environment was found (set QAR_VENV or pass -Python)"
    if ($unpackPython) {
        $problem = Expand-WithPython $unpackPython $archive $src
    }
    if ($problem) {
        Write-Host "  not unpacked with Python: $problem"
        Write-Host "  trying tar"
        $problem = Expand-WithTar $archive $src
        if ($problem) {
            throw ("Cannot unpack ${archive}: $problem. Install the pinned packages into the " +
                "virtual environment (pip install -r requirements.txt), or take both DLLs from " +
                "another OpenSSL 1.1.1 x64 distribution and pass their folder with -SourceDir.")
        }
    }
}

$problem = Get-OpenSslPairProblem $src
if ($problem) { throw $problem }

# --- install ---------------------------------------------------------------------------------
foreach ($n in $needed) {
    try {
        Copy-Item -LiteralPath (Join-Path $src $n) -Destination $bin -Force
    } catch {
        throw ("Cannot write $(Join-Path $bin $n): $($_.Exception.Message) Close RViz and every " +
            "program of the distribution, then run the script again.")
    }
    $v = (Get-Item -LiteralPath (Join-Path $bin $n)).VersionInfo.ProductVersion
    Write-Host "Installed $n (OpenSSL $v) from $src"
}
Write-Host "Done. Fast-DDS (rmw_fastrtps_cpp) can now load; rcl/rclpy init and rviz2 will start."
