# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Builds tinyxml.dll (TinyXML 2.6.2, the STL variant) from the source archive and installs it into
    the bin folder of the ROS 2 Humble Windows binary distribution, which imports it but does not
    ship it (companion to fix_missing_vendor_dlls.ps1).

.DESCRIPTION
    The libraries urdfdom_model.dll, urdfdom_world.dll, urdfdom_model_state.dll and
    urdfdom_sensor.dll of the distribution import the TinyXML 2.6.2 functions from tinyxml.dll, and
    the release archive does not contain that file. RViz loads the URDF parser plugin
    (urdf_xml_parser.dll, which imports urdfdom_model.dll) when the first robot description
    arrives. Without tinyxml.dll that load fails with the Windows error 126, and RViz ends with
    the exit code 0xC0000409, because nothing catches the exception of the plugin loader. Nothing
    else in the distribution imports the file.

    The script
      1. does nothing, and exits with 0, when <Ros2Root>\bin\tinyxml.dll exists and the stock
         program check_urdf.exe of the distribution parses a one-link robot with it;
      2. otherwise takes the source archive tinyxml_2.6.2.orig.tar.gz from -SourceArchive or from
         the first of three download locations that answers with a file of the expected SHA-256
         (a file with another hash is discarded and never unpacked);
      3. unpacks it into a fresh folder, applies the two fixes for CVE-2021-42260 and CVE-2023-34194
         that the Debian and conda-forge packages of the same version carry (each fix must match
         the source text exactly once, otherwise the script stops), writes a CMakeLists.txt that
         builds the three source files as a shared library with the macro TIXML_USE_STL and every
         symbol exported, and builds it with the generator "Visual Studio 16 2019" in the
         RelWithDebInfo configuration, the toolset and configuration of the rest of the
         distribution. The C++ runtime is the dynamic one (/MD), the msvcp140.dll and
         vcruntime140.dll that the distribution ships next to rviz2.exe;
      4. copies the built tinyxml.dll to <Ros2Root>\bin and runs check_urdf.exe again. The script
         stops with exit status 1 and a message when that fails.
    Unlike the other repairs the installed file is not byte-for-byte reproducible (a build records
    its time); the pinned input is the hash of the source archive.

    Idempotent: a tinyxml.dll that works is never replaced. One that does not work is replaced
    only when -Force is given. The script needs console_bridge.dll and the C++ runtime in the bin
    folder, so run fix_missing_vendor_dlls.ps1 first.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.

.PARAMETER DepsRoot
    Dependency root. Only used for the default of -BuildRoot. Default: the environment variable
    QAR_DEPS_ROOT.

.PARAMETER BuildRoot
    Scratch folder for unpacking and building. Default: <DepsRoot>\build_tinyxml. The download goes
    to the "downloads" folder next to it.

.PARAMETER SourceArchive
    A local copy of tinyxml_2.6.2.orig.tar.gz (the tinyxml_2_6_2.tar.gz of SourceForge is the same
    file) for a machine without access to the download locations. It must have the expected SHA-256.

.PARAMETER Force
    Build and install even when tinyxml.dll exists and works, and replace an existing tinyxml.dll.

.EXAMPLE
    ./scripts/fix_missing_tinyxml_dll.ps1 -Ros2Root C:\qar\deps\ros2_humble\ros2-windows
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [string]$BuildRoot,
    [string]$SourceArchive,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
if ([string]::IsNullOrEmpty($BuildRoot)) {
    $DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' -EnvVar 'QAR_DEPS_ROOT' `
        -What 'Dependency root' -Section 'Directory layout and environment variables' -AllowMissing
    $BuildRoot = Join-Path $DepsRoot "build_tinyxml"
}

$destDir = Join-Path $Ros2Root "bin"
$targetDll = Join-Path $destDir "tinyxml.dll"
$archiveName = "tinyxml_2.6.2.orig.tar.gz"
$archiveSha256 = "15BDFDCEC58A7DA30ADC87AC2B078E4417DBE5392F3AFB719F9BA6D062645593"
$archiveUrls = @(
    "https://deb.debian.org/debian/pool/main/t/tinyxml/tinyxml_2.6.2.orig.tar.gz",
    "https://archive.ubuntu.com/ubuntu/pool/main/t/tinyxml/tinyxml_2.6.2.orig.tar.gz",
    "https://downloads.sourceforge.net/project/tinyxml/tinyxml/2.6.2/tinyxml_2_6_2.tar.gz"
)

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# Runs check_urdf.exe of the distribution on a one-link robot. The program loads urdfdom_model.dll
# and with it tinyxml.dll, parses the text and prints "Successfully Parsed XML". Returns $null when
# it does, otherwise the text of the problem. It starts nothing when a library that it needs
# besides tinyxml.dll is missing, because the Windows loader would stop it with a dialog.
function Test-TinyXmlWorks {
    foreach ($name in "check_urdf.exe", "urdfdom_model.dll", "console_bridge.dll", "msvcp140.dll",
        "vcruntime140.dll") {
        if (-not (Test-Path -LiteralPath (Join-Path $destDir $name))) {
            return "$name is missing in $destDir (run fix_missing_vendor_dlls.ps1 first)"
        }
    }
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    $robot = Join-Path $BuildRoot "probe_robot.urdf"
    Set-Content -LiteralPath $robot -Encoding Ascii `
        -Value '<?xml version="1.0"?><robot name="probe"><link name="base"/></robot>'
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns native stderr into errors
    try {
        $output = & (Join-Path $destDir "check_urdf.exe") $robot 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    $text = ($output | Out-String).Trim()
    if ($exitCode -eq 0 -and $text -match "Successfully Parsed XML") { return $null }
    $message = ("check_urdf.exe ended with status 0x{0:X8} (0xC0000135: a library is missing, " +
        "0xC0000139: a function is not exported)") -f $exitCode
    if ($text) { $message += ": $text" }
    return $message
}

# Replaces $Old by $New in the file and stops when $Old is not there exactly once.
function Edit-SourceOnce([string]$Path, [string]$Old, [string]$New) {
    $text = [System.IO.File]::ReadAllText($Path)
    $ordinal = [System.StringComparison]::Ordinal
    $first = $text.IndexOf($Old, $ordinal)
    $again = if ($first -ge 0) { $text.IndexOf($Old, $first + 1, $ordinal) } else { -1 }
    if ($first -lt 0 -or $again -ge 0) {
        throw "Cannot apply the source fix to ${Path}: the expected text is not there exactly once."
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $text.Replace($Old, $New), $encoding)
}

# --- is the repair needed? --------------------------------------------------------------------
if ((Test-Path -LiteralPath $targetDll) -and -not $Force) {
    $problem = Test-TinyXmlWorks
    if ($null -eq $problem) {
        Write-Host ("tinyxml.dll is already installed at $targetDll and check_urdf.exe parses a " +
            "robot with it - nothing to do.")
        exit 0
    }
    throw ("tinyxml.dll exists at $targetDll, but the check fails: $problem. " +
        "Delete the file or pass -Force to build and install a new one.")
}

# --- the tools --------------------------------------------------------------------------------
$vsPath = Get-QarVsInstallPath
foreach ($tool in 'cmake', 'tar') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found on PATH (see docs/INSTALL.md, section 'Toolchain')."
    }
}
Write-Host "Visual Studio 2019 tools: $vsPath"

# --- the source archive, checked by its SHA-256 ----------------------------------------------
$downloads = Join-Path (Split-Path $BuildRoot) "downloads"
New-Item -ItemType Directory -Force -Path $BuildRoot, $downloads | Out-Null
$archive = Join-Path $downloads $archiveName
if ($SourceArchive) {
    $archive = (Resolve-Path -LiteralPath $SourceArchive).Path
    $actual = Get-FileSha256 $archive
    if ($actual -ne $archiveSha256) {
        throw "SHA-256 mismatch for ${archive}: expected $archiveSha256, got $actual."
    }
    Write-Host "Using $archive (SHA-256 verified)"
} elseif ((Test-Path -LiteralPath $archive) -and (Get-FileSha256 $archive) -eq $archiveSha256) {
    Write-Host "Already downloaded and SHA-256 verified: $archive"
} else {
    $verified = $false
    foreach ($url in $archiveUrls) {
        Write-Host "Downloading $url"
        try {
            Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
        } catch {
            Write-Host "  failed: $($_.Exception.Message)"
            continue
        }
        if ((Get-FileSha256 $archive) -eq $archiveSha256) { $verified = $true; break }
        Write-Host "  the SHA-256 of the file is not $archiveSha256; the file is discarded"
        Remove-Item -LiteralPath $archive -Force
    }
    if (-not $verified) {
        throw ("No download location answered with the file (SHA-256 $archiveSha256). " +
            "Download $archiveName elsewhere and pass it with -SourceArchive.")
    }
    Write-Host "Downloaded and verified: $archive"
}

# --- unpack, fix, build -----------------------------------------------------------------------
$unpacked = Join-Path $BuildRoot "tinyxml"
if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
Write-Host "Extracting $(Split-Path $archive -Leaf)"
tar -xzf $archive -C $BuildRoot
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $unpacked "tinyxml.h"))) {
    throw "Extracting the source archive failed."
}

$t = "`t"
$t2 = $t * 2
$t3 = $t * 3
$t5 = $t * 5
$t6 = $t * 6
$parser = Join-Path $unpacked "tinyxmlparser.cpp"
# CVE-2021-42260: an unfinished UTF-8 lead byte must advance the pointer (endless loop otherwise).
$anchor = "{ p +=3; ++col; }$t// A normal character.`n${t5}}`n"
$added = "${t5}else`n${t5}{`n" +
    "${t6}// The lead byte of a 3 byte sequence without its followers: advance.`n" +
    "${t6}++p;`n${t5}}`n"
Edit-SourceOnce $parser $anchor ($anchor + $added)
# CVE-2023-34194: a 0 byte after white space inside <?xml ... must end the declaration.
$skip = "${t2}p = SkipWhiteSpace( p, _encoding );`n"
$next = "${t2}if ( StringEqual( p, `"version`", true, _encoding ) )`n"
$added = "${t2}if ( !p || !*p )`n${t2}{`n${t3}break;`n${t2}}`n"
Edit-SourceOnce $parser ($skip + $next) ($skip + $added + $next)

$cmakeLists = @'
cmake_minimum_required(VERSION 3.5)
project(tinyxml CXX)
add_definitions(-DTIXML_USE_STL)
add_library(tinyxml SHARED tinyxml.cpp tinyxmlerror.cpp tinyxmlparser.cpp)
set_target_properties(tinyxml PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
'@
Set-Content -LiteralPath (Join-Path $unpacked "CMakeLists.txt") -Value $cmakeLists -Encoding Ascii

$b = Join-Path $unpacked "_build"
Write-Host "Configuring (VS2019 x64, matching the distro's v142 toolset)"
cmake -S $unpacked -B $b -G "Visual Studio 16 2019" -A x64
if ($LASTEXITCODE -ne 0) { throw "tinyxml configure failed" }
Write-Host "Building tinyxml (RelWithDebInfo, matching the distro's build config)"
cmake --build $b --config RelWithDebInfo --target tinyxml
if ($LASTEXITCODE -ne 0) { throw "tinyxml build failed" }
$builtDll = Join-Path $b "RelWithDebInfo\tinyxml.dll"
if (-not (Test-Path -LiteralPath $builtDll)) {
    throw "Could not locate the built tinyxml.dll at $builtDll"
}

# --- install and check ------------------------------------------------------------------------
try {
    Copy-Item -LiteralPath $builtDll -Destination $targetDll -Force
} catch {
    throw ("Cannot write ${targetDll}: $($_.Exception.Message) Close RViz and every program of " +
        "the distribution, then run the script again.")
}
Write-Host "Installed: $targetDll  (from $builtDll)"
$problem = Test-TinyXmlWorks
if ($null -ne $problem) {
    throw "tinyxml.dll is installed, but the check fails: $problem."
}
Write-Host "Check: check_urdf.exe parses a robot with the new tinyxml.dll."
Write-Host ""
Write-Host "Done. tinyxml.dll SHA256: $(Get-FileSha256 $targetDll)"
Write-Host "Start RViz again; its Robot model display can load a robot description now."
