# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT
#
# Build services shared by the three QarOS integration repositories (qar_ogre_adapter,
# qar_qt_streaming, qar_rviz_plugin). The file is identical in all three.
# Including it defines variables, functions and macros and makes MSVC read the sources as
# UTF-8; each top-level CMakeLists.txt calls the services it needs.

include_guard(GLOBAL)

# URL downloads extract with the current time as file timestamps (no developer warning, and a
# re-extracted archive always rebuilds).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# The sources are UTF-8 text without a byte order mark, which MSVC would read in the code
# page of the machine. /utf-8 sets the source and the execution character set to UTF-8 for
# every target created after this file is included, so a comment or a string literal means
# the same on every machine.
if(MSVC)
  add_compile_options(/utf-8)
endif()

set(QAR_GOOGLETEST_URL "https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz")
set(QAR_GOOGLETEST_SHA256 "8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7")

# Single-configuration generators default to RelWithDebInfo: the ROS 2 and Qt binaries are
# release builds, and a debug C runtime in the same process crashes on load.
macro(qar_set_default_build_type)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
endmacro()

# `cmake --install` without --prefix installs into <build>/install, never into Program Files.
macro(qar_set_default_install_prefix)
  if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install prefix" FORCE)
  endif()
endmacro()

# qar_resolve_location(<var> ENV <name> WHAT <text> [REQUIRED] [MARKER <relative path>])
# Precedence: -D<var> (a preset passes an unset environment variable as an empty value, which
# counts as unset) > environment variable <name> > unset (a fatal error when REQUIRED).
# The folder must exist and contain MARKER when MARKER is given.
function(qar_resolve_location var)
  cmake_parse_arguments(PARSE_ARGV 1 _qrl "REQUIRED" "ENV;WHAT;MARKER" "")
  set(_value "${${var}}")
  set(_from "-D${var}")
  if("${_value}" STREQUAL "" AND NOT "$ENV{${_qrl_ENV}}" STREQUAL "")
    set(_value "$ENV{${_qrl_ENV}}")
    set(_from "environment variable ${_qrl_ENV}")
  endif()
  if("${_value}" STREQUAL "")
    if(_qrl_REQUIRED)
      message(FATAL_ERROR
        "${_qrl_WHAT} not set: configure with -D${var}=<folder> or set the environment "
        "variable ${_qrl_ENV} (see docs/INSTALL.md).")
    endif()
    return()
  endif()
  file(TO_CMAKE_PATH "${_value}" _value)
  if(NOT IS_DIRECTORY "${_value}")
    message(FATAL_ERROR "${_qrl_WHAT} folder does not exist: ${_value} (from ${_from}).")
  endif()
  if(_qrl_MARKER AND NOT EXISTS "${_value}/${_qrl_MARKER}")
    message(FATAL_ERROR "${_qrl_WHAT} at ${_value} (from ${_from}) has no ${_qrl_MARKER}.")
  endif()
  set(${var} "${_value}" CACHE PATH "${_qrl_WHAT}" FORCE)
endfunction()

function(qar_apply_warning_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# The ROS 2 Humble distribution on CMAKE_PREFIX_PATH (Ogre comes from its rviz_ogre_vendor).
macro(qar_use_ros2_distro)
  qar_resolve_location(QAR_ROS2_INSTALL_ROOT ENV ROS2_INSTALL_ROOT REQUIRED
    WHAT "ROS 2 Humble distribution (ros2-windows)" MARKER "local_setup.bat")
  list(PREPEND CMAKE_PREFIX_PATH "${QAR_ROS2_INSTALL_ROOT}")
  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  message(STATUS "ROS 2 distribution: ${QAR_ROS2_INSTALL_ROOT}")
endmacro()

# ament's CMake configurations import the Python module ament_package and read the ament index
# from AMENT_PREFIX_PATH; both are set for this configure process so a plain shell works.
function(qar_use_ament_environment)
  if(WIN32)
    set(_sep ";")
  else()
    set(_sep ":")
  endif()
  set(ENV{AMENT_PREFIX_PATH} "${QAR_ROS2_INSTALL_ROOT}${_sep}$ENV{AMENT_PREFIX_PATH}")
  if(EXISTS "${QAR_ROS2_INSTALL_ROOT}/Lib/site-packages")
    set(ENV{PYTHONPATH} "${QAR_ROS2_INSTALL_ROOT}/Lib/site-packages${_sep}$ENV{PYTHONPATH}")
  endif()
endfunction()

# Qt 5 found in the calling (top-level) directory: imported Qt targets are directory-scoped, and
# the test directories stage the Qt DLLs only when they can see those targets.
macro(qar_use_qt5)
  cmake_parse_arguments(_qq "OPTIONAL" "" "COMPONENTS" ${ARGN})
  if(_qq_OPTIONAL)
    qar_resolve_location(QAR_QT_ROOT ENV QAR_QT_ROOT
      WHAT "Qt 5.15.2 (msvc2019_64)" MARKER "lib/cmake/Qt5/Qt5Config.cmake")
  else()
    qar_resolve_location(QAR_QT_ROOT ENV QAR_QT_ROOT REQUIRED
      WHAT "Qt 5.15.2 (msvc2019_64)" MARKER "lib/cmake/Qt5/Qt5Config.cmake")
  endif()
  if(QAR_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${QAR_QT_ROOT}")
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  endif()
  if(_qq_OPTIONAL)
    find_package(Qt5 QUIET COMPONENTS ${_qq_COMPONENTS})
  else()
    find_package(Qt5 REQUIRED COMPONENTS ${_qq_COMPONENTS})
  endif()
endmacro()

# GoogleTest v1.14.0 for this build only (never installed with the project). Without network
# access: -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<extracted googletest-1.14.0>.
function(qar_require_googletest)
  if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
    return()
  endif()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL "${QAR_GOOGLETEST_URL}"
    URL_HASH "SHA256=${QAR_GOOGLETEST_SHA256}")
  set(gtest_force_shared_crt ON CACHE BOOL "GoogleTest uses the shared MSVC runtime" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "GoogleTest is not installed with this project" FORCE)
  FetchContent_MakeAvailable(googletest)
  if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR
      "GoogleTest v1.14.0 is not available: configuring needs network access to github.com, "
      "or -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<extracted googletest-1.14.0> (see docs/INSTALL.md).")
  endif()
endfunction()

# One CTest per module (scripts/check_layering.py --module <m>) plus the guard's self-test.
function(qar_add_layering_guard_tests)
  cmake_parse_arguments(PARSE_ARGV 0 _qlg "" "" "MODULES")
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  foreach(_m IN LISTS _qlg_MODULES)
    add_test(NAME check_layering_${_m}
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_layering.py"
        --repo-root "${PROJECT_SOURCE_DIR}" --module ${_m})
  endforeach()
  add_test(NAME check_layering_selftest
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_layering.py" --self-test)
endfunction()

# QAR_PREFIX (-D, else the environment variable): the install prefix of the upstream repositories.
macro(_qar_use_prefix)
  if("${QAR_PREFIX}" STREQUAL "" AND NOT "$ENV{QAR_PREFIX}" STREQUAL "")
    set(QAR_PREFIX "$ENV{QAR_PREFIX}" CACHE STRING "Install prefix of the upstream repositories" FORCE)
  endif()
  foreach(_qup_dir IN LISTS QAR_PREFIX)
    if(NOT IS_DIRECTORY "${_qup_dir}")
      message(FATAL_ERROR "QAR_PREFIX names a folder that does not exist: ${_qup_dir}")
    endif()
    file(TO_CMAKE_PATH "${_qup_dir}" _qup_dir)
    list(PREPEND CMAKE_PREFIX_PATH "${_qup_dir}")
  endforeach()
  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
endmacro()

# Source mode: add the upstream modules a component list needs, from a checkout of the upstream
# repository, into this build. Install rules are then disabled (qar_install_enabled).
function(_qar_add_upstream_sources pkg dir version)
  set(_components ${ARGN})
  file(TO_CMAKE_PATH "${dir}" dir)
  string(TOUPPER "${pkg}" _upper)
  if(NOT EXISTS "${dir}/CMakeLists.txt")
    message(FATAL_ERROR "${_upper}_SOURCE_DIR: ${dir} is not a source checkout (no CMakeLists.txt).")
  endif()
  # The checkout's top-level project() version must have the requested major.minor.
  file(READ "${dir}/CMakeLists.txt" _top)
  if(_top MATCHES "project\\([^)]*VERSION[ \t\r\n]+([0-9]+\\.[0-9]+)")
    set(_have "${CMAKE_MATCH_1}")
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _want "${version}")
    if(NOT _have STREQUAL _want)
      message(FATAL_ERROR "${_upper}_SOURCE_DIR: ${dir} is version ${_have}, this build needs ${_want}.")
    endif()
  endif()
  set(_modules)
  if(pkg STREQUAL "qar_ogre_adapter")
    if(NOT _components)
      set(_components qar_ar_core qar_ogre_streaming capi render panels url gestures)
    endif()
    list(APPEND _modules qar_ar_core)
    foreach(_c IN LISTS _components)
      if(_c MATCHES "^(capi|render|panels|url|gestures)$")
        list(APPEND _modules qar_ogre_streaming qar_qar_adapter)
      elseif(_c STREQUAL "qar_ogre_streaming")
        list(APPEND _modules qar_ogre_streaming)
      endif()
    endforeach()
    if("${QAR_VENDOR_INCLUDE_DIR}" STREQUAL "")
      set(QAR_VENDOR_INCLUDE_DIR "${dir}/third_party/qar/include" CACHE PATH
        "Folder with the vendored qar_streaming.h" FORCE)
    endif()
  elseif(pkg STREQUAL "qar_qt_streaming")
    set(_modules qar_qt_streaming)
  else()
    message(FATAL_ERROR "qar_find_upstream: unknown package ${pkg}")
  endif()
  list(REMOVE_DUPLICATES _modules)
  foreach(_m IN LISTS _modules)
    set(_target ${_m})
    if(_m STREQUAL "qar_qar_adapter")
      set(_target qar_qar_adapter_capi)
    endif()
    if(NOT TARGET ${_target})
      if(NOT EXISTS "${dir}/${_m}/CMakeLists.txt")
        message(FATAL_ERROR "${dir} has no module ${_m} (needed for ${pkg} ${_components}).")
      endif()
      add_subdirectory("${dir}/${_m}" "${CMAKE_BINARY_DIR}/${_m}")
    endif()
  endforeach()
  set_property(GLOBAL PROPERTY QAR_SOURCE_MODE ON)
  message(STATUS "${pkg}: modules ${_modules} from the source tree ${dir}")
endfunction()

# qar_find_upstream(<package> VERSION <x.y.z> [COMPONENTS <c>...])
# Default: find_package(<package> <x.y.z> CONFIG) on CMAKE_PREFIX_PATH with QAR_PREFIX in front.
# Developer override: -D<PACKAGE>_SOURCE_DIR=<checkout> (e.g. QAR_OGRE_ADAPTER_SOURCE_DIR) builds the needed upstream modules
# from that checkout instead (no install of the upstream package needed).
macro(qar_find_upstream _qfu_pkg)
  cmake_parse_arguments(_qfu "" "VERSION" "COMPONENTS" ${ARGN})
  string(TOUPPER "${_qfu_pkg}" _qfu_upper)
  _qar_use_prefix()
  if(NOT "${${_qfu_upper}_SOURCE_DIR}" STREQUAL "")
    _qar_add_upstream_sources(${_qfu_pkg} "${${_qfu_upper}_SOURCE_DIR}" ${_qfu_VERSION}
      ${_qfu_COMPONENTS})
  else()
    set(_qfu_args)
    if(_qfu_COMPONENTS)
      set(_qfu_args COMPONENTS ${_qfu_COMPONENTS})
    endif()
    find_package(${_qfu_pkg} ${_qfu_VERSION} CONFIG QUIET ${_qfu_args})
    if(NOT ${_qfu_pkg}_FOUND)
      message(FATAL_ERROR
        "${_qfu_pkg} ${_qfu_VERSION} was not found. ${${_qfu_pkg}_NOT_FOUND_MESSAGE}\n"
        "Install the ${_qfu_pkg} repository into the prefix named by QAR_PREFIX "
        "(now '${QAR_PREFIX}'; see docs/INSTALL.md), or build against a source checkout with "
        "-D${_qfu_upper}_SOURCE_DIR=<folder>.")
    endif()
    message(STATUS "${_qfu_pkg} ${${_qfu_pkg}_VERSION}: ${${_qfu_pkg}_DIR}")
  endif()
endmacro()

# Install rules are generated when QAR_INSTALL is ON and no upstream package is built from
# source (a package built against source trees cannot be installed consistently).
function(qar_install_enabled out)
  get_property(_source_mode GLOBAL PROPERTY QAR_SOURCE_MODE)
  if(QAR_INSTALL AND NOT _source_mode)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
    if(QAR_INSTALL)
      message(STATUS "Install rules disabled: upstream modules are built from source trees.")
    endif()
  endif()
endfunction()

# Used by qar_ogre_adapter and qar_qt_streaming; qar_rviz_plugin ships no package config.
# qar_install_package(<package> EXPORT <export set> NAMESPACE <ns>::)
# <package>Config.cmake (from cmake/<package>Config.cmake.in), ConfigVersion and Targets files
# under <prefix>/share/<package>/cmake. Versions are compatible within one minor version while
# the major version is 0.
function(qar_install_package pkg)
  cmake_parse_arguments(PARSE_ARGV 1 _qip "" "EXPORT;NAMESPACE" "")
  include(CMakePackageConfigHelpers)
  set(_dest "share/${pkg}/cmake")
  install(EXPORT ${_qip_EXPORT} NAMESPACE ${_qip_NAMESPACE} FILE ${pkg}Targets.cmake
    DESTINATION ${_dest})
  configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/${pkg}Config.cmake.in"
    "${PROJECT_BINARY_DIR}/${pkg}Config.cmake" INSTALL_DESTINATION ${_dest})
  write_basic_package_version_file("${PROJECT_BINARY_DIR}/${pkg}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES "${PROJECT_BINARY_DIR}/${pkg}Config.cmake" "${PROJECT_BINARY_DIR}/${pkg}ConfigVersion.cmake"
    DESTINATION ${_dest})
endfunction()
