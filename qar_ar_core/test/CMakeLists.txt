# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Unit tests for qar_ar_core: Qt-free, Ogre-free, SDK-free (GTest::gtest_main only).
#
# GoogleTest comes from the top-level qar_require_googletest(); this directory just uses
# its targets.

add_executable(qar_ar_core_tests
  common_name_tests.cpp
  panel_attach_tests.cpp
  # The seams of the panels: the URL panel and the hand index.
  panel_seam_tests.cpp
  # The decision to ask a panel for its state again, over scripted times.
  panel_state_guard_tests.cpp
  panel_input_source_tests.cpp
  pointer_input_tests.cpp
  pointing_ray_tests.cpp
  spatial_frames_tests.cpp
  spatial_pick_tests.cpp
  timing_log_tests.cpp
  # The version of the build against the file VERSION.
  version_tests.cpp
)

target_link_libraries(qar_ar_core_tests PRIVATE
  qar_ogre_adapter::qar_ar_core
  GTest::gtest_main
)

target_compile_features(qar_ar_core_tests PRIVATE cxx_std_17)
target_compile_definitions(qar_ar_core_tests PRIVATE
  QAR_TEST_VERSION_FILE="${PROJECT_SOURCE_DIR}/VERSION")

qar_apply_warning_flags(qar_ar_core_tests)

include(GoogleTest)
gtest_discover_tests(qar_ar_core_tests DISCOVERY_TIMEOUT 30)
