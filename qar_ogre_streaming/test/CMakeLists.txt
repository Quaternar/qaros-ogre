# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Unit tests for qar_ogre_streaming (GoogleTest comes from the top-level qar_require_googletest()).

add_executable(qar_ogre_streaming_tests
  config_validation_tests.cpp
  manager_tests.cpp
  mock_sender_tests.cpp
  mock_frame_source_tests.cpp
  opengl_bridge_tests.cpp
  pose_conversion_tests.cpp
  projection_tests.cpp
  render_budget_tests.cpp
  render_target_support_tests.cpp
  scene_lighting_tests.cpp
  spatial_pick_visual_tests.cpp
  scheduler_tests.cpp
  stereo_stream_tests.cpp
)

target_link_libraries(qar_ogre_streaming_tests PRIVATE
  qar_ogre_adapter::qar_ogre_streaming
  qar_ogre_adapter::qar_ogre_streaming_test_support
  GTest::gtest_main
)

qar_apply_warning_flags(qar_ogre_streaming_tests)

# The reusable library emits real out-of-line Ogre symbols
# (OgreStereoRenderTarget uses TextureManager/RenderTexture/MaterialManager/etc.),
# all of which live in OgreMain.dll. Those objects get transitively linked into
# this test executable via the manager/stereo-stream tests. The tests, however,
# NEVER call any Ogre function (there is no GL context; the real render paths are
# exercised by the standalone sample and in RViz).
#
# So instead of requiring OgreMain.dll (+ its dep DLLs) on PATH just to LOAD the
# test exe, DELAY-LOAD OgreMain.dll on MSVC: the DLL is only loaded on first call
# to one of its functions, which never happens in the tests. The exe therefore
# starts and runs the full suite from a plain shell with no distro env sourced,
# and `ctest --preset` stays green with zero setup. (As a belt-and-suspenders, the
# distro's Ogre bin is also prepended to PATH for the test runs, so if a future
# test ever does exercise a real Ogre path it can still find the DLL.)
if(MSVC)
  target_link_options(qar_ogre_streaming_tests PRIVATE /DELAYLOAD:OgreMain.dll)
  target_link_libraries(qar_ogre_streaming_tests PRIVATE delayimp)
endif()

# Distro root (…/ros2-windows) is a global cache var set by the top-level
# CMakeLists (the only place this test dir is added). Ogre runtime DLLs live under
# opt/rviz_ogre_vendor/bin.
file(TO_NATIVE_PATH "${QAR_ROS2_INSTALL_ROOT}/opt/rviz_ogre_vendor/bin" _ogre_bin_native)

include(GoogleTest)
gtest_discover_tests(qar_ogre_streaming_tests
  DISCOVERY_TIMEOUT 30
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_ogre_bin_native}"
)
