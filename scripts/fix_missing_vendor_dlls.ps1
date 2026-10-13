# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Builds console_bridge, tinyxml2, yaml-cpp, assimp and libcurl from source and installs them
    into the bin folder of the ROS 2 Humble Windows binary distribution, filling packaging gaps
    of the official release archive (companion to fix_missing_spdlog_dll.ps1).

.DESCRIPTION
    A recursive `dumpbin /dependents` walk of rviz2.exe's DLL dependency closure shows that
    the official release-humble-20260220 archive lacks these five third-party DLLs entirely
    (the same kind of gap as spdlog.dll):

      - tinyxml2.dll        (required by rviz_common.dll - XML/URDF parsing)
      - console_bridge.dll  (required by rviz_common.dll, tf2.dll, class_loader.dll - logging shim)
      - yaml-cpp.dll        (required by rviz_common.dll - YAML; distinct from libyaml/yaml.dll)
      - assimp-vc141-mt.dll (required by rviz_rendering.dll - 3D mesh/asset import)
      - libcurl.dll         (required by resource_retriever.dll - HTTP(S)/package:// fetching)

    The versions follow the upstream source pinned by each ROS 2 vendor package:

      - console_bridge_vendor (1.4.1) -> ros/console_bridge tag 1.0.2
      - tinyxml2_vendor (0.7.7) -> no vendor-built pin (find_package only); tinyxml2 9.0.0
        (leethomason/tinyxml2) - low ABI risk, the only symbols rviz_common.dll imports are a
        small, long-stable subset (XMLDocument/XMLElement/XMLNode basics).
      - yaml_cpp_vendor (8.0.2) -> jbeder/yaml-cpp tag yaml-cpp-0.7.0
      - rviz_assimp_vendor (part of the ros2/rviz monorepo) -> assimp tag v4.1.0. Note: assimp's
        own CMake naming logic (as of 4.1.0) emits "assimp-vc140-mt.dll" even when built with the
        VS2019 (v142) generator - the script copies and renames it to the "assimp-vc141-mt.dll"
        filename rviz_rendering.dll expects. This is safe: Windows resolves DLL dependencies by
        filename only, and the MSVC C/C++ runtime ABI (VCRUNTIME140/MSVCP140) is compatible across
        the v140-v143 toolset family, so a VS2019-built DLL satisfies a "vc141"-named import.
      - libcurl_vendor (in github.com/ros/resource_retriever, branch humble, subdirectory
        libcurl_vendor/) -> curl tag curl-7_81_0 (7.81.0), built with CURL_USE_SCHANNEL=ON
        (the Windows TLS stack) so no separate OpenSSL dependency is required.

    All builds use the VS2019 (v142) generator and the RelWithDebInfo configuration, like the
    rest of the distribution.

    For console_bridge the script also installs the headers (<console_bridge/console.h> is
    included by pluginlib's class_loader) and the import library into
    <Ros2Root>\opt\console_bridge_vendor. fix_missing_vendor_cmake_configs.ps1 reads them from
    there, so run this script first.

    Idempotent: a library that is already installed is skipped.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.

.PARAMETER DepsRoot
    Dependency root. Only used for the default of -BuildRoot. Default: the environment
    variable QAR_DEPS_ROOT.

.PARAMETER BuildRoot
    Scratch folder for downloading and building. Default: <DepsRoot>\build_vendor_dlls. The
    downloads go to the "downloads" folder next to it.

.EXAMPLE
    ./scripts/fix_missing_vendor_dlls.ps1 -Ros2Root C:\qar\deps\ros2_humble\ros2-windows
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('DestinationRoot')]
    [string]$DepsRoot,
    [string]$BuildRoot
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
if ([string]::IsNullOrEmpty($BuildRoot)) {
    $DepsRoot = Resolve-QarLocation -Value $DepsRoot -Parameter 'DepsRoot' -EnvVar 'QAR_DEPS_ROOT' `
        -What 'Dependency root' -Section 'Directory layout and environment variables' -AllowMissing
    $BuildRoot = Join-Path $DepsRoot "build_vendor_dlls"
}

$destDir = Join-Path $Ros2Root "bin"
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$downloads = Join-Path (Split-Path $BuildRoot) "downloads"
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

function Get-AndExtract($url, $archiveName, $extractedDirName) {
    $archivePath = Join-Path $downloads $archiveName
    if (-not (Test-Path $archivePath)) {
        Write-Host "Downloading $url"
        # The download goes to <name>.part and is renamed when it is complete, so an
        # interrupted run leaves no file that a later run would take for the archive.
        $partPath = "$archivePath.part"
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
        Move-Item -LiteralPath $partPath -Destination $archivePath -Force
    }
    $destPath = Join-Path $BuildRoot $extractedDirName
    if (-not (Test-Path $destPath)) {
        Write-Host "Extracting $archiveName"
        tar -xzf $archivePath -C $BuildRoot
        if ($LASTEXITCODE -ne 0) {
            # a later run must find neither an unreadable archive nor a partly unpacked folder
            Remove-Item -LiteralPath $destPath -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
            throw ("tar could not unpack $archivePath (exit code $LASTEXITCODE). The archive " +
                "and the partly unpacked folder were deleted; run the script again to " +
                "download it again.")
        }
    }
    return $destPath
}

function Install-BuiltDll($srcPath, $destName) {
    $dst = Join-Path $destDir $destName
    Copy-Item $srcPath $dst -Force
    Write-Host "Installed: $dst  (from $srcPath)"
}

# --- console_bridge 1.0.2 (DLL, headers and import library) ---
$cbRoot = Join-Path $Ros2Root "opt\console_bridge_vendor"
$cbHeaderDir = Join-Path $cbRoot "include\console_bridge"
$cbImportLib = Join-Path $cbRoot "lib\console_bridge.lib"
if (-not ((Test-Path (Join-Path $destDir "console_bridge.dll")) -and (Test-Path (Join-Path $cbHeaderDir "console.h")) -and (Test-Path $cbImportLib))) {
    $src = Get-AndExtract "https://github.com/ros/console_bridge/archive/1.0.2.tar.gz" "console_bridge-1.0.2.tar.gz" "console_bridge-1.0.2"
    $b = Join-Path $src "_build"
    cmake -S $src -B $b -G "Visual Studio 16 2019" -A x64 -DCMAKE_INSTALL_PREFIX="$src\_install"
    if ($LASTEXITCODE -ne 0) { throw "console_bridge configure failed" }
    cmake --build $b --config RelWithDebInfo --target console_bridge
    if ($LASTEXITCODE -ne 0) { throw "console_bridge build failed" }
    if (-not (Test-Path (Join-Path $destDir "console_bridge.dll"))) {
        Install-BuiltDll "$b\bin\RelWithDebInfo\console_bridge.dll" "console_bridge.dll"
    }
    New-Item -ItemType Directory -Force -Path $cbHeaderDir | Out-Null
    Copy-Item "$src\include\console_bridge\console.h" -Destination $cbHeaderDir -Force
    Copy-Item "$b\console_bridge_export.h" -Destination $cbHeaderDir -Force
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cbImportLib) | Out-Null
    Copy-Item "$b\lib\RelWithDebInfo\console_bridge.lib" -Destination $cbImportLib -Force
    Write-Host "Installed: console_bridge headers and import library under $cbRoot"
} else { Write-Host "console_bridge.dll, headers and import library already installed - skipping" }

# --- tinyxml2 9.0.0 ---
if (-not (Test-Path (Join-Path $destDir "tinyxml2.dll"))) {
    $src = Get-AndExtract "https://github.com/leethomason/tinyxml2/archive/refs/tags/9.0.0.tar.gz" "tinyxml2-9.0.0.tar.gz" "tinyxml2-9.0.0"
    $b = Join-Path $src "_build"
    cmake -S $src -B $b -G "Visual Studio 16 2019" -A x64 -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$src\_install"
    if ($LASTEXITCODE -ne 0) { throw "tinyxml2 configure failed" }
    cmake --build $b --config RelWithDebInfo --target tinyxml2
    if ($LASTEXITCODE -ne 0) { throw "tinyxml2 build failed" }
    Install-BuiltDll "$b\RelWithDebInfo\tinyxml2.dll" "tinyxml2.dll"
} else { Write-Host "tinyxml2.dll already installed - skipping" }

# --- yaml-cpp 0.7.0 ---
if (-not (Test-Path (Join-Path $destDir "yaml-cpp.dll"))) {
    $src = Get-AndExtract "https://github.com/jbeder/yaml-cpp/archive/yaml-cpp-0.7.0.tar.gz" "yaml-cpp-0.7.0.tar.gz" "yaml-cpp-yaml-cpp-0.7.0"
    $b = Join-Path $src "_build"
    cmake -S $src -B $b -G "Visual Studio 16 2019" -A x64 -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_BUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$src\_install"
    if ($LASTEXITCODE -ne 0) { throw "yaml-cpp configure failed" }
    cmake --build $b --config RelWithDebInfo --target yaml-cpp
    if ($LASTEXITCODE -ne 0) { throw "yaml-cpp build failed" }
    Install-BuiltDll "$b\RelWithDebInfo\yaml-cpp.dll" "yaml-cpp.dll"
} else { Write-Host "yaml-cpp.dll already installed - skipping" }

# --- assimp 4.1.0 ---
if (-not (Test-Path (Join-Path $destDir "assimp-vc141-mt.dll"))) {
    $src = Get-AndExtract "https://github.com/assimp/assimp/archive/v4.1.0.tar.gz" "assimp-4.1.0.tar.gz" "assimp-4.1.0"
    $b = Join-Path $src "_build"
    cmake -S $src -B $b -G "Visual Studio 16 2019" -A x64 -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DCMAKE_CXX_FLAGS="/w /EHsc" -DCMAKE_C_FLAGS="/w" -DCMAKE_INSTALL_PREFIX="$src\_install"
    if ($LASTEXITCODE -ne 0) { throw "assimp configure failed" }
    cmake --build $b --config RelWithDebInfo --target assimp
    if ($LASTEXITCODE -ne 0) { throw "assimp build failed" }
    # assimp's own CMake naming emits "assimp-vc140-mt.dll" even under the VS2019 generator -
    # rename to the "vc141" filename rviz_rendering.dll's import table actually expects.
    Install-BuiltDll "$b\code\RelWithDebInfo\assimp-vc140-mt.dll" "assimp-vc141-mt.dll"
} else { Write-Host "assimp-vc141-mt.dll already installed - skipping" }

# --- curl 7.81.0 (schannel TLS backend, no OpenSSL dependency) ---
if (-not (Test-Path (Join-Path $destDir "libcurl.dll"))) {
    $src = Get-AndExtract "https://github.com/curl/curl/releases/download/curl-7_81_0/curl-7.81.0.tar.gz" "curl-7.81.0.tar.gz" "curl-7.81.0"
    $b = Join-Path $src "_build"
    cmake -S $src -B $b -G "Visual Studio 16 2019" -A x64 -DENABLE_MANUAL=OFF -DBUILD_TESTING=OFF -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DBUILD_SHARED_LIBS=ON -DCURL_STATICLIB=OFF -DCMAKE_INSTALL_PREFIX="$src\_install"
    if ($LASTEXITCODE -ne 0) { throw "curl configure failed" }
    cmake --build $b --config RelWithDebInfo --target libcurl
    if ($LASTEXITCODE -ne 0) { throw "curl build failed" }
    $dll = Get-ChildItem $b -Recurse -Filter "libcurl*.dll" | Where-Object { $_.FullName -match "RelWithDebInfo" } | Select-Object -First 1
    if (-not $dll) { throw "Could not locate built libcurl DLL" }
    Install-BuiltDll $dll.FullName "libcurl.dll"
} else { Write-Host "libcurl.dll already installed - skipping" }

Write-Host ""
Write-Host "Done. Verify with ros2 --help (through ros2-script.py) or rviz2.exe: no DLL load errors should remain."
