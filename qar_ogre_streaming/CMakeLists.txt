# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# qar_ogre_streaming — reusable Ogre<->Qar stereo streaming library.
#
# Plain CMake (NOT ament). Zero dependency on rviz_common, rviz_rendering, Qt,
# ament or ROS. Depends only on: Ogre (via rviz_ogre_vendor), the vendored Qar C
# header, and the C++17 standard library. See docs/ARCHITECTURE.md.

# ---------------------------------------------------------------------------
# Ogre discovery via the ROS 2 Humble binary distro's rviz_ogre_vendor package.
# The distro root must be on CMAKE_PREFIX_PATH (set by the top-level project from
# QAR_ROS2_INSTALL_ROOT). rviz_ogre_vendorConfig.cmake is self-contained: it does
# not invoke ament macros at find-time, it only locates Ogre and defines the
# IMPORTED target rviz_ogre_vendor::OgreMain (Ogre 1.12.1).
# ---------------------------------------------------------------------------
find_package(rviz_ogre_vendor REQUIRED)

# ---------------------------------------------------------------------------
# The reusable library — STATIC (consumed by the ament RViz plugin and by
# standalone Ogre samples through link; static avoids per-class dllexport and a
# separate runtime DLL, and keeps Ogre SharedPtr/types off a DLL boundary).
# ---------------------------------------------------------------------------
add_library(qar_ogre_streaming STATIC
  src/types.cpp
  src/diagnostics.cpp
  src/render_budget.cpp
  src/qar_pose_conversion.cpp
  src/qar_scene_lighting.cpp
  src/qar_scene_lighting_support.cpp
  src/spatial_pick_visual.cpp
  src/spatial_pick_visual_support.cpp
  src/qar_render_sender_adapter.cpp
  src/mock_qar_frame_info_source.cpp
  src/ogre_stereo_render_target.cpp
  src/ogre_stereo_render_target_support.cpp
  src/gl_interop.cpp
  src/ogre_opengl_texture_bridge.cpp
  src/ogre_qar_stereo_stream.cpp
  src/ogre_qar_stereo_stream_support.cpp
  src/ogre_qar_render_scheduler.cpp
  src/ogre_qar_streaming_manager.cpp
)
add_library(qar_ogre_adapter::qar_ogre_streaming ALIAS qar_ogre_streaming)

target_compile_features(qar_ogre_streaming PUBLIC cxx_std_17)

target_include_directories(qar_ogre_streaming PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)

# qar_ar_core is PUBLIC: types.hpp reuses the QarOS C types through its wrapper.
# Ogre is PUBLIC because public headers forward-declare Ogre types and consumers
# (RViz plugin, samples) that pass a real Ogre::SceneManager need its headers.
target_link_libraries(qar_ogre_streaming PUBLIC
  qar_ogre_adapter::qar_ar_core
  rviz_ogre_vendor::OgreMain
)

# Static lib built into a shared consumer (the ament RViz plugin) on non-Windows
# must be position-independent.
set_target_properties(qar_ogre_streaming PROPERTIES POSITION_INDEPENDENT_CODE ON)

qar_apply_warning_flags(qar_ogre_streaming)

# ---------------------------------------------------------------------------
# Header-only test support: recording Qar sender adapter (a test double) +
# immediate render executor.
# A separate INTERFACE target so tests and the standalone sample can reuse them
# without them becoming part of the shipped library.
# ---------------------------------------------------------------------------
add_library(qar_ogre_streaming_test_support INTERFACE)
add_library(qar_ogre_adapter::qar_ogre_streaming_test_support ALIAS qar_ogre_streaming_test_support)
target_include_directories(qar_ogre_streaming_test_support INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test_support/include>"
)
target_link_libraries(qar_ogre_streaming_test_support INTERFACE
  qar_ogre_streaming
)
