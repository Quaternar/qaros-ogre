# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Unit tests for qar_qar_adapter.
#
# Two executables. qar_qar_adapter_tests holds the SDK-free parts of the render and URL panel
# adapters: geo_anchor.cpp and env_config.cpp are kept apart from the render adapter for
# exactly that reason, and render_timing.hpp is header-only. qar_qar_adapter_session_tests
# runs the session owner against a fake of the QarOS function tables (fake_qar_api.cpp); it
# links only the `session` component, so it builds without Ogre. No test loads the QarOS
# library.
#
# GoogleTest comes from the top-level qar_require_googletest(); this directory just uses
# its targets.

add_executable(qar_qar_adapter_tests
  geo_anchor_tests.cpp
  env_config_tests.cpp
  # The 3D stream's stage timing (render3d.csv): header-only and QarOS-free.
  render_timing_tests.cpp
  # The log line of a panel state change, the line for the first 3D frame after a stall, and the
  # defaults of the factory configuration: pure values, no QarOS session.
  panel_state_log_tests.cpp
  frame_stall_probe_tests.cpp
  render_config_tests.cpp
  # The render sender factory on the session owner, which runs on the fake QarOS library.
  fake_qar_api.cpp
  render_factory_tests.cpp
  # The URL panel adapter on the owner's thread, on the same fake.
  panel_async_tests.cpp
  # The helpers those tests share.
  panel_test_kit.hpp
)

# The `render` component's pure units. Linking the static library pulls in only the
# objects these tests reference (geo_anchor, the factory's queues): nothing here calls the real
# C API or Ogre, so the exe needs neither the QarOS DLL nor OgreMain.dll.
target_link_libraries(qar_qar_adapter_tests PRIVATE
  qar_ogre_adapter::render
  qar_ogre_adapter::url
  GTest::gtest_main
)
target_compile_features(qar_qar_adapter_tests PRIVATE cxx_std_17)
target_include_directories(qar_qar_adapter_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

qar_apply_warning_flags(qar_qar_adapter_tests)
if(MSVC)
  # A deprecated QarOS C API call is an error here as in the components.
  target_compile_options(qar_qar_adapter_tests PRIVATE /we4996)
endif()

include(GoogleTest)
gtest_discover_tests(qar_qar_adapter_tests DISCOVERY_TIMEOUT 30)

# The session owner on the fake library: joining the Hub of this PC, the targets, the teardown
# and the status texts.
add_executable(qar_qar_adapter_session_tests
  fake_qar_api.cpp
  # Replaces the global operator new and delete so that a test can tell when a block was freed.
  alloc_watch.cpp
  session_owner_tests.cpp
  # The timer of QarOS calls on a GUI thread.
  gui_call_guard_tests.cpp
  onboarding_status_tests.cpp
)
target_link_libraries(qar_qar_adapter_session_tests PRIVATE
  qar_ogre_adapter::session
  GTest::gtest_main
)
target_compile_features(qar_qar_adapter_session_tests PRIVATE cxx_std_17)
target_include_directories(qar_qar_adapter_session_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

qar_apply_warning_flags(qar_qar_adapter_session_tests)
if(MSVC)
  # A deprecated QarOS C API call is an error here as in the components.
  target_compile_options(qar_qar_adapter_session_tests PRIVATE /we4996)
endif()

gtest_discover_tests(qar_qar_adapter_session_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
