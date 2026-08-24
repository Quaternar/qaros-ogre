# ===========================================================================
# qar_ogre_streaming — reusable Ogre <-> QarOS stereo streaming library.
# ===========================================================================
# Plain CMake (deliberately NOT ament). Zero dependency on rviz_common,
# rviz_rendering, Qt, ament or ROS. Depends only on: Ogre (via rviz_ogre_vendor),
# the vendored QarOS C header, and the C++17 standard library. That constraint is
# enforced by the check_no_rviz_includes test — see docs/ogre_library_architecture.md.
#
# Buildable two ways, both supported:
#   * standalone:  cmake --preset vs2019-x64 (from this directory)
#   * via colcon:  package.xml declares <build_type>cmake</build_type>
# After installation it is consumable with find_package(qar_ogre_streaming).

cmake_minimum_required(VERSION 3.20)
project(qar_ogre_streaming VERSION 0.1.0 LANGUAGES CXX
  DESCRIPTION "Reusable Ogre <-> QarOS stereo streaming library")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(QarBuildSupport)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(QAR_BUILD_TESTS "Build the qar_ogre_streaming unit tests" ON)
option(QAR_BUILD_EXAMPLES "Build the standalone Ogre sample under examples/" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

qar_locate_ros2_distro()

# rviz_ogre_vendorConfig.cmake is self-contained: it invokes no ament macros at
# find-time, it only locates Ogre and defines rviz_ogre_vendor::OgreMain (Ogre 1.12.1).
find_package(rviz_ogre_vendor REQUIRED)

# --- Vendored QarOS C header ----------------------------------------------
# Only the HEADER is vendored (it is redistributable). The QarOS runtime itself is
# a separate proprietary binary drop and is NOT part of this repository; it is
# loaded dynamically at run time. See README.md.
set(QAR_VENDOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qar/include"
  CACHE PATH "Directory containing the vendored qar_streaming.h")
if(NOT EXISTS "${QAR_VENDOR_INCLUDE_DIR}/qar_streaming.h")
  message(FATAL_ERROR "Vendored QarOS header not found at ${QAR_VENDOR_INCLUDE_DIR}/qar_streaming.h")
endif()

# ---------------------------------------------------------------------------
# The library — STATIC: it is consumed by an RViz plugin module and by standalone
# Ogre apps through link. Static avoids per-class dllexport and a second runtime
# DLL, and keeps Ogre SharedPtr/types off a DLL boundary.
# ---------------------------------------------------------------------------
add_library(qar_ogre_streaming STATIC
  src/types.cpp
  src/diagnostics.cpp
  src/render_budget.cpp
  src/qar_pose_conversion.cpp
  src/qar_scene_lighting.cpp
  src/qar_scene_lighting_support.cpp
  src/qar_render_sender_adapter.cpp
  src/mock_qar_frame_info_source.cpp
  src/interactive_frame_info_source.cpp
  src/ogre_stereo_render_target.cpp
  src/ogre_stereo_render_target_support.cpp
  src/gl_interop.cpp
  src/ogre_opengl_texture_bridge.cpp
  src/ogre_gl_texture_spike.cpp
  src/ogre_qar_stereo_stream.cpp
  src/ogre_qar_stereo_stream_support.cpp
  src/ogre_qar_render_scheduler.cpp
  src/ogre_qar_streaming_manager.cpp
)
add_library(qar_ogre_streaming::qar_ogre_streaming ALIAS qar_ogre_streaming)

target_compile_features(qar_ogre_streaming PUBLIC cxx_std_17)

target_include_directories(qar_ogre_streaming PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  # The vendored QarOS header is PUBLIC: our value types reuse QarOS types, so
  # consumers of our headers need it too. It is installed into the same include
  # root, hence a single INSTALL_INTERFACE entry covers both.
  "$<BUILD_INTERFACE:${QAR_VENDOR_INCLUDE_DIR}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# Ogre is PUBLIC because public headers forward-declare Ogre types and consumers
# that pass a real Ogre::SceneManager need its headers.
target_link_libraries(qar_ogre_streaming PUBLIC rviz_ogre_vendor::OgreMain)

# A static lib linked into a shared consumer (the RViz plugin module) on
# non-Windows must be position-independent.
set_target_properties(qar_ogre_streaming PROPERTIES POSITION_INDEPENDENT_CODE ON)

qar_apply_warning_flags(qar_ogre_streaming)

# ---------------------------------------------------------------------------
# Header-only test support: a mock QarOS sender adapter, a manual clock, an
# immediate render-thread executor and a recording render lock. A separate
# INTERFACE target so tests and standalone samples can reuse them without them
# becoming part of the shipped library.
# ---------------------------------------------------------------------------
add_library(qar_ogre_streaming_test_support INTERFACE)
add_library(qar_ogre_streaming::test_support ALIAS qar_ogre_streaming_test_support)
target_include_directories(qar_ogre_streaming_test_support INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test_support/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(qar_ogre_streaming_test_support INTERFACE qar_ogre_streaming)

# --- Install + export ------------------------------------------------------
install(TARGETS qar_ogre_streaming qar_ogre_streaming_test_support
  EXPORT qar_ogre_streamingTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY test_support/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${QAR_VENDOR_INCLUDE_DIR}/qar_streaming.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qar/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}"
  RENAME LICENSE.qar_streaming_header)

install(EXPORT qar_ogre_streamingTargets
  FILE qar_ogre_streamingTargets.cmake
  NAMESPACE qar_ogre_streaming::
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qar_ogre_streamingConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/qar_ogre_streamingConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/qar_ogre_streamingConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/qar_ogre_streamingConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/qar_ogre_streamingConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)

# Make colcon/ament index-aware tooling see this as an installed package even
# though it is a plain-CMake (non-ament) package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/package.xml"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}")

# --- Standalone reusability sample -----------------------------------------
# A plain Ogre app (no rviz, no Qt, no ROS) that consumes this library through
# the same public API the RViz plugin uses. It lives inside this package so that
# the package is self-contained and so that the dependency-direction guard covers
# it: examples/ is part of the tree check_no_rviz_includes scans.
if(QAR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# --- Tests -----------------------------------------------------------------
if(QAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
