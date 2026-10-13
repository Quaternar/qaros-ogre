# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Provides the missing "*_vendor" CMake configuration shims in the ROS 2 Humble Windows
    binary distribution, so that downstream ament plugins that call find_package(rviz_common)
    can configure. Idempotent.

.DESCRIPTION
    find_package(rviz_common) transitively calls find_package() for several vendored
    third-party packages whose CMake config files the official release-humble-20260220
    Windows archive does not ship (fix_missing_vendor_dlls.ps1 rebuilds only their runtime
    DLLs). Each one fails at configure time:

      rviz_common -> rviz_rendering -> rviz_assimp_vendor   -> find_package(assimp)     [config missing]
      rviz_common -> rviz_rendering -> resource_retriever
                                       -> libcurl_vendor     -> find_package(curl)      [config missing]
      rviz_common -> yaml_cpp_vendor                         -> find_package(yaml-cpp)  [config missing]

    (Eigen3 - also missing - is handled by fix_missing_eigen.ps1, which additionally copies
    headers. tinyxml2_vendor is a private build dependency of rviz_common and is not
    re-exported, so downstream plugins do not need it.)

    Downstream RViz plugins do not link assimp, curl or yaml-cpp directly - those are already
    linked inside the rviz and resource_retriever DLLs - so these shims only need to let
    find_package() succeed and to provide harmless INTERFACE imported targets and the classic
    *_LIBRARIES / *_INCLUDE_DIRS variables that each vendor -extras.cmake reads.

    The console_bridge shim points at the headers and the import library that
    fix_missing_vendor_dlls.ps1 installs into <Ros2Root>\opt\console_bridge_vendor; run that
    script first.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'

function Write-ConfigFile {
    param([string]$Dir, [string]$FileName, [string]$Content)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Set-Content -Path (Join-Path $Dir $FileName) -Value $Content -Encoding UTF8
    Write-Host "  wrote $((Join-Path $Dir $FileName))" -ForegroundColor Green
}

# --- assimp (rviz_assimp_vendor-extras.cmake forces assimp_DIR here) --------
# Consumes assimp 4.1-style ASSIMP_* variables; plugins do not link assimp.
Write-Host "assimp:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "opt\rviz_assimp_vendor\lib\cmake\assimp-4.1") `
    -FileName "assimpConfig.cmake" `
    -Content @'
# Minimal assimp config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
# Version >5.0.1 + the modern assimp::assimp target: rviz_assimp_vendor-extras.cmake
# then takes its modern branch, AND rviz_rendering's exported link interface (which
# hard-references assimp::assimp) resolves. Plugins do not link assimp themselves.
set(assimp_VERSION 5.2.0)
set(ASSIMP_VERSION 5.2.0)
set(assimp_FOUND TRUE)
set(ASSIMP_FOUND TRUE)
set(ASSIMP_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
set(ASSIMP_LIBRARY_DIRS "")
set(ASSIMP_LIBRARIES "")
set(ASSIMP_CXX_FLAGS "")
if(NOT TARGET assimp::assimp)
  add_library(assimp::assimp INTERFACE IMPORTED)
endif()
'@

# --- curl (libcurl_vendor-extras.cmake: find_package(curl NO_MODULE) then
#           set(CURL_LIBRARIES CURL::libcurl)) ---------------------------------
Write-Host "curl:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "opt\libcurl_vendor\lib\CMake") `
    -FileName "curlConfig.cmake" `
    -Content @'
# Minimal curl config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(curl_VERSION 7.81.0)
set(curl_FOUND TRUE)
set(CURL_FOUND TRUE)
set(CURL_VERSION_STRING 7.81.0)
set(CURL_INCLUDE_DIRS "")
set(CURL_LIBRARY_DIRS "")
if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl INTERFACE IMPORTED)
endif()
'@

# --- yaml-cpp (yaml_cpp_vendor-extras.cmake: find_package(yaml-cpp CONFIG) with
#     yaml-cpp_DIR = opt/yaml_cpp_vendor/CMake; reads YAML_CPP_LIBRARIES /
#     YAML_CPP_INCLUDE_DIR and appends target 'yaml-cpp') ----------------------
Write-Host "yaml-cpp:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "opt\yaml_cpp_vendor\CMake") `
    -FileName "yaml-cpp-config.cmake" `
    -Content @'
# Minimal yaml-cpp config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(yaml-cpp_VERSION 0.7.0)
set(YAML_CPP_VERSION 0.7.0)
set(yaml-cpp_FOUND TRUE)
set(YAML_CPP_INCLUDE_DIR "")
set(YAML_CPP_LIBRARIES yaml-cpp)
if(NOT TARGET yaml-cpp)
  add_library(yaml-cpp INTERFACE IMPORTED)
endif()
'@

# --- console_bridge (class_loader exports it: find_package(console_bridge)) --
# Unlike assimp/curl/yaml-cpp, console_bridge HEADERS ARE needed at compile time:
# pluginlib -> class_loader/class_loader.hpp does #include <console_bridge/console.h>.
# PLUGINLIB_EXPORT_CLASS -> class_loader::registerPlugin also calls console_bridge::log, so
# downstream plugins must LINK console_bridge.lib (the import library). The distribution ships
# neither; fix_missing_vendor_dlls.ps1 installs both, and the target below is a real
# SHARED IMPORTED with IMPORTED_IMPLIB.
Write-Host "console_bridge:" -ForegroundColor Cyan
$cbIncludeDir = Join-Path $Ros2Root "opt\console_bridge_vendor\include"
$cbHeaderDir = Join-Path $cbIncludeDir "console_bridge"
$cbLibDst = Join-Path $Ros2Root "opt\console_bridge_vendor\lib\console_bridge.lib"
if (-not (Test-Path (Join-Path $cbHeaderDir "console.h")) -or -not (Test-Path $cbLibDst)) {
    throw "console_bridge headers or import library are missing from the distribution: run scripts\fix_missing_vendor_dlls.ps1 first."
}
$cbIncForCMake = ($cbIncludeDir -replace '\\', '/')
$cbLibForCMake = ($cbLibDst -replace '\\', '/')
$cbDllForCMake = ((Join-Path $Ros2Root "bin\console_bridge.dll") -replace '\\', '/')
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "share\console_bridge\cmake") `
    -FileName "console_bridgeConfig.cmake" `
    -Content @"
# Minimal console_bridge config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(console_bridge_VERSION 1.0.2)
set(console_bridge_FOUND TRUE)
set(console_bridge_INCLUDE_DIRS "$cbIncForCMake")
if(NOT TARGET console_bridge::console_bridge)
  add_library(console_bridge::console_bridge SHARED IMPORTED)
  set_target_properties(console_bridge::console_bridge PROPERTIES
    IMPORTED_IMPLIB "$cbLibForCMake"
    IMPORTED_LOCATION "$cbDllForCMake"
    INTERFACE_INCLUDE_DIRECTORIES "$cbIncForCMake")
endif()
set(console_bridge_TARGETS console_bridge::console_bridge)
set(console_bridge_LIBRARIES console_bridge::console_bridge)
"@

# --- TinyXML2 (pluginlib exports 'TinyXML2'; FindTinyXML2.cmake tries CONFIG
#     mode first, so a config with an INTERFACE tinyxml2::tinyxml2 target wins) -
Write-Host "TinyXML2:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "share\tinyxml2\cmake") `
    -FileName "TinyXML2Config.cmake" `
    -Content @'
# Minimal TinyXML2 config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(TinyXML2_VERSION 9.0.0)
set(TinyXML2_FOUND TRUE)
if(NOT TARGET tinyxml2::tinyxml2)
  add_library(tinyxml2::tinyxml2 INTERFACE IMPORTED)
endif()
set(TinyXML2_TARGETS tinyxml2::tinyxml2)
set(TinyXML2_LIBRARIES tinyxml2::tinyxml2)
set(TinyXML2_INCLUDE_DIRS "")
'@

# --- spdlog (rclcpp -> rcl -> rcl_logging_spdlog: find_package(spdlog)) ------
# fix_missing_spdlog_dll.ps1 installs spdlog.dll but no config; spdlog is linked inside
# rcl_logging_spdlog.dll.
Write-Host "spdlog:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "share\spdlog\cmake") `
    -FileName "spdlogConfig.cmake" `
    -Content @'
# Minimal spdlog config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(spdlog_VERSION 1.9.2)
set(spdlog_FOUND TRUE)
if(NOT TARGET spdlog::spdlog)
  add_library(spdlog::spdlog INTERFACE IMPORTED)
  set_target_properties(spdlog::spdlog PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS "SPDLOG_COMPILED_LIB")
endif()
set(spdlog_TARGETS spdlog::spdlog)
set(spdlog_LIBRARIES spdlog::spdlog)
set(SPDLOG_INCLUDE_DIRS "")
'@

# --- orocos_kdl (tf2_geometry_msgs -> orocos_kdl_vendor: find_package(orocos_kdl)
#     then find_library(orocos-kdl REQUIRED) unless TARGET orocos-kdl exists).
#     KDL is not shipped at all; the shim MUST pre-define target 'orocos-kdl' so the
#     REQUIRED find_library is skipped. tf2_geometry_msgs is header-only for our use. -
Write-Host "orocos_kdl:" -ForegroundColor Cyan
Write-ConfigFile `
    -Dir (Join-Path $Ros2Root "share\orocos_kdl\cmake") `
    -FileName "orocos_kdlConfig.cmake" `
    -Content @'
# Minimal orocos_kdl config shim (see scripts/fix_missing_vendor_cmake_configs.ps1).
set(orocos_kdl_VERSION 1.5.1)
set(orocos_kdl_FOUND TRUE)
set(orocos_kdl_INCLUDE_DIRS "")
set(orocos_kdl_LIBRARIES "")
# Pre-define the target so orocos_kdl_vendor-extras.cmake skips its REQUIRED
# find_library(orocos-kdl) (no KDL library is shipped in this distro).
if(NOT TARGET orocos-kdl)
  add_library(orocos-kdl INTERFACE IMPORTED)
endif()
'@

Write-Host ""
Write-Host "All vendor CMake config shims written. find_package(rviz_common) will now configure." -ForegroundColor Green
Write-Host "(Eigen3 headers+config are handled separately by fix_missing_eigen.ps1.)" -ForegroundColor Yellow
