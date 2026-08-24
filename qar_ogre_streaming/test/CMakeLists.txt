# Unit tests for qar_ogre_streaming (GoogleTest, fetched at configure time).

qar_require_googletest()

add_executable(qar_ogre_streaming_tests
  config_validation_tests.cpp
  manager_tests.cpp
  mock_sender_tests.cpp
  mock_frame_source_tests.cpp
  interactive_frame_source_tests.cpp
  opengl_bridge_tests.cpp
  pose_conversion_tests.cpp
  projection_tests.cpp
  render_budget_tests.cpp
  render_target_support_tests.cpp
  scene_lighting_tests.cpp
  scheduler_tests.cpp
  stereo_stream_tests.cpp
)

target_link_libraries(qar_ogre_streaming_tests PRIVATE
  qar_ogre_streaming::qar_ogre_streaming
  qar_ogre_streaming::test_support
  GTest::gtest_main
)

qar_apply_warning_flags(qar_ogre_streaming_tests)

# The library emits real out-of-line Ogre symbols (the stereo render target uses
# TextureManager/RenderTexture/MaterialManager), all of which live in OgreMain.dll,
# and those objects get transitively linked into this test executable. The tests
# themselves, however, NEVER call an Ogre function: there is no GL context, and the
# real render paths are validated by the in-RViz scripted capture instead (see
# docs/offscreen_rendering.md).
#
# So rather than requiring OgreMain.dll (+ its dependency DLLs) on PATH just to
# LOAD the test exe, DELAY-LOAD OgreMain.dll on MSVC: the DLL is only loaded on the
# first call into it, which never happens here. The exe therefore starts and runs
# the whole suite from a plain shell with no distro environment sourced.
if(MSVC)
  target_link_options(qar_ogre_streaming_tests PRIVATE /DELAYLOAD:OgreMain.dll)
  target_link_libraries(qar_ogre_streaming_tests PRIVATE delayimp)
endif()

# Belt and braces: if a future test ever does exercise a real Ogre path, let it
# find the DLL. Best effort — resolve the distro's Ogre bin dir from
# QAR_ROS2_INSTALL_ROOT when set, otherwise from where rviz_ogre_vendor was found
# (<distro>/share/rviz_ogre_vendor/cmake). Skipped silently if neither resolves.
set(_qar_ogre_bin "")
if(DEFINED QAR_ROS2_INSTALL_ROOT AND NOT QAR_ROS2_INSTALL_ROOT STREQUAL "")
  set(_qar_ogre_bin "${QAR_ROS2_INSTALL_ROOT}/opt/rviz_ogre_vendor/bin")
elseif(DEFINED rviz_ogre_vendor_DIR AND NOT rviz_ogre_vendor_DIR STREQUAL "")
  get_filename_component(_qar_distro "${rviz_ogre_vendor_DIR}/../../.." ABSOLUTE)
  set(_qar_ogre_bin "${_qar_distro}/opt/rviz_ogre_vendor/bin")
endif()

# ---------------------------------------------------------------------------
# Dependency-direction guard. Fails if any source in this package pulls in an
# rviz_*, Qt, ament or rclcpp header. This library must stay usable from a plain
# Ogre application, so the rule is enforced on every `ctest` run rather than left
# to review. examples/ is scanned too: it exists precisely to prove the library
# is consumable with nothing but Ogre.
# ---------------------------------------------------------------------------
find_program(QAR_PWSH NAMES pwsh powershell)
if(QAR_PWSH)
  add_test(
    NAME check_no_rviz_includes
    COMMAND "${QAR_PWSH}" -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/check_no_rviz_includes.ps1"
      -RepoRoot "${CMAKE_CURRENT_SOURCE_DIR}/.."
      -ScanDirs include src examples test_support test
  )
else()
  message(WARNING "PowerShell not found; check_no_rviz_includes CTest not registered.")
endif()

include(GoogleTest)
if(_qar_ogre_bin AND EXISTS "${_qar_ogre_bin}")
  file(TO_NATIVE_PATH "${_qar_ogre_bin}" _qar_ogre_bin_native)
  gtest_discover_tests(qar_ogre_streaming_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qar_ogre_bin_native}"
  )
else()
  gtest_discover_tests(qar_ogre_streaming_tests DISCOVERY_TIMEOUT 30)
endif()
