# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Build services for the repositories that build against the ROS 2 Humble distribution:
# the distribution on CMAKE_PREFIX_PATH, the ament environment of a plain shell, and the
# upstream QarOS integration packages (an installed package, or a source checkout).
# QarBuildSupport.cmake includes this file when it sits beside it; the helpers it uses
# (qar_resolve_location, qar_use_qt) come from that file.

include_guard(GLOBAL)

# Qt for the RViz build. RViz 2 of the ROS 2 Humble distribution is built against Qt 5.15, and a
# plugin must run on the same Qt, so this pins Qt 5 and requires QAR_QT_ROOT (the kit folder).
# The arguments are those of qar_use_qt (OPTIONAL, COMPONENTS).
macro(qar_use_qt5)
  qar_use_qt(MAJOR 5 ROOT_REQUIRED WHAT "Qt 5.15.2 (msvc2019_64)" ${ARGN})
endmacro()

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
      set(_components qar_ar_core qar_ogre_streaming capi session render url gestures)
    endif()
    list(APPEND _modules qar_ar_core)
    foreach(_c IN LISTS _components)
      if(_c MATCHES "^(capi|session|render|url|gestures)$")
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
    # The consumers here (the RViz plugin) use the seam and the Qt core; the backend over the QarOS C++
    # SDK is left out, so no SDK headers are needed to build from source.
    if("qaros" IN_LIST _components)
      message(FATAL_ERROR "${_upper}_SOURCE_DIR: the component qaros is not available in source mode; "
        "use an installed qar_qt_streaming package for it.")
    endif()
    set(QAR_QT_STREAMING_WITH_QAROS OFF)
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
