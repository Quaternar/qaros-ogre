# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Build services shared by the QarOS integration repositories. The file is identical in
# each of them.
# Including it defines variables, functions and macros and makes MSVC read the sources as
# UTF-8; each top-level CMakeLists.txt calls the services it needs. Further services,
# for builds against a robotics distribution, are in QarRosBuildSupport.cmake.

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

# Single-configuration generators default to RelWithDebInfo: the prebuilt Qt and
# platform binaries are release builds, and a debug C runtime in the same process crashes on
# load.
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
# The folder must exist and contain MARKER when MARKER is given; with several markers, one of
# them is enough.
function(qar_resolve_location var)
  cmake_parse_arguments(PARSE_ARGV 1 _qrl "REQUIRED" "ENV;WHAT" "MARKER")
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
  if(_qrl_MARKER)
    set(_marker_found FALSE)
    foreach(_marker IN LISTS _qrl_MARKER)
      if(EXISTS "${_value}/${_marker}")
        set(_marker_found TRUE)
      endif()
    endforeach()
    if(NOT _marker_found)
      string(REPLACE ";" " or " _marker_text "${_qrl_MARKER}")
      message(FATAL_ERROR "${_qrl_WHAT} at ${_value} (from ${_from}) has no ${_marker_text}.")
    endif()
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

# Qt 5 (5.15 or newer) or Qt 6 (6.5 or newer), found in the calling (top-level) directory: imported
# Qt targets are directory-scoped, and the test directories stage the Qt DLLs only when they can see
# those targets. Targets are named Qt${QAR_QT_MAJOR}::Core, so one CMake file serves both versions.
#   qar_use_qt([OPTIONAL] [MAJOR <5|6>] [ROOT_REQUIRED] [WHAT <text>] COMPONENTS <component>...)
# QAR_QT_ROOT (-D, else the environment variable of that name) is the folder of one Qt kit and goes
# in front of CMAKE_PREFIX_PATH; without it CMAKE_PREFIX_PATH decides. The major version is, in this
# order: the MAJOR argument, -DQAR_QT_MAJOR=<5|6>, the version of the kit in QAR_QT_ROOT, Qt 6 when
# CMake finds one, else Qt 5. The result is the variable QAR_QT_MAJOR. OPTIONAL: a missing Qt is not
# an error. ROOT_REQUIRED: QAR_QT_ROOT must be set.
macro(qar_use_qt)
  cmake_parse_arguments(_qq "OPTIONAL;ROOT_REQUIRED" "MAJOR;WHAT" "COMPONENTS" ${ARGN})
  if(NOT _qq_WHAT)
    set(_qq_WHAT "Qt 5.15 or Qt 6.5 and newer")
  endif()
  set(_qq_major "${QAR_QT_MAJOR}")
  if(_qq_MAJOR)
    if(_qq_major AND NOT _qq_major STREQUAL _qq_MAJOR)
      message(FATAL_ERROR "This build needs Qt ${_qq_MAJOR}, but QAR_QT_MAJOR is ${_qq_major}.")
    endif()
    set(_qq_major "${_qq_MAJOR}")
  endif()
  if(_qq_major AND NOT _qq_major MATCHES "^[56]$")
    message(FATAL_ERROR "QAR_QT_MAJOR is ${_qq_major}; it must be 5 or 6.")
  endif()
  if(_qq_major)
    set(_qq_markers "lib/cmake/Qt${_qq_major}/Qt${_qq_major}Config.cmake")
  else()
    set(_qq_markers "lib/cmake/Qt6/Qt6Config.cmake" "lib/cmake/Qt5/Qt5Config.cmake")
  endif()
  if(_qq_ROOT_REQUIRED AND NOT _qq_OPTIONAL)
    qar_resolve_location(QAR_QT_ROOT ENV QAR_QT_ROOT REQUIRED
      WHAT "${_qq_WHAT}" MARKER ${_qq_markers})
  else()
    qar_resolve_location(QAR_QT_ROOT ENV QAR_QT_ROOT WHAT "${_qq_WHAT}" MARKER ${_qq_markers})
  endif()
  if(QAR_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${QAR_QT_ROOT}")
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    if(NOT _qq_major)
      set(_qq_has6 FALSE)
      set(_qq_has5 FALSE)
      if(EXISTS "${QAR_QT_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
        set(_qq_has6 TRUE)
      endif()
      if(EXISTS "${QAR_QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake")
        set(_qq_has5 TRUE)
      endif()
      if(_qq_has6 AND NOT _qq_has5)
        set(_qq_major 6)
      elseif(_qq_has5 AND NOT _qq_has6)
        set(_qq_major 5)
      endif()
    endif()
  endif()
  if(NOT _qq_major)
    find_package(QT NAMES Qt6 Qt5 QUIET COMPONENTS Core)
    if(QT_VERSION_MAJOR)
      set(_qq_major "${QT_VERSION_MAJOR}")
    elseif(NOT _qq_OPTIONAL)
      message(FATAL_ERROR
        "${_qq_WHAT} not found: configure with -DQAR_QT_ROOT=<Qt kit folder> or set the "
        "environment variable QAR_QT_ROOT (see docs/INSTALL.md).")
    endif()
  endif()
  if(_qq_major)
    if(_qq_OPTIONAL)
      find_package(Qt${_qq_major} QUIET COMPONENTS ${_qq_COMPONENTS})
    else()
      find_package(Qt${_qq_major} REQUIRED COMPONENTS ${_qq_COMPONENTS})
    endif()
    if(Qt${_qq_major}_FOUND)
      set(QAR_QT_MAJOR "${_qq_major}")
      if(_qq_major EQUAL 5)
        set(_qq_least "5.15")
      else()
        set(_qq_least "6.5")
      endif()
      if(NOT _qq_OPTIONAL AND Qt${_qq_major}_VERSION VERSION_LESS _qq_least)
        message(FATAL_ERROR
          "Qt ${Qt${_qq_major}_VERSION} (${Qt${_qq_major}_DIR}) is too old: this build needs "
          "Qt ${_qq_least} or newer of the major version ${_qq_major}.")
      endif()
      message(STATUS "Qt ${Qt${_qq_major}_VERSION}: ${Qt${_qq_major}_DIR}")
    endif()
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

# Used by the repositories that install a package config; the others do not call it.
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

# The services for builds against a robotics distribution (the distribution, its build
# environment, the upstream packages) are in QarRosBuildSupport.cmake. It is included here
# when it sits beside this file, so a CMakeLists.txt that includes only this file has every
# service. A repository that has no use for them does not ship it.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/QarRosBuildSupport.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/QarRosBuildSupport.cmake")
endif()
