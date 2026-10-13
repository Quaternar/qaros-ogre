# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT
#
# Unit tests for qar_qar_adapter.
#
# Only the SDK-free parts are testable here: everything else in this package is
# bound to a live QarSession. geo_anchor.cpp and env_config.cpp are kept apart from
# the render adapter for exactly that reason, and render_timing.hpp is header-only.
#
# GoogleTest comes from the top-level qar_require_googletest(); this directory just uses
# its targets.

add_executable(qar_qar_adapter_tests
  geo_anchor_tests.cpp
  env_config_tests.cpp
  # The 3D stream's stage timing (render3d.csv): header-only and QarOS-free.
  render_timing_tests.cpp
)

# The `render` component's pure units. Linking the static library pulls in only the
# objects these tests reference (geo_anchor, env_config): nothing here calls the C API
# or Ogre, so the exe needs neither the QarOS DLL nor OgreMain.dll.
target_link_libraries(qar_qar_adapter_tests PRIVATE
  qar_ogre_adapter::render
  GTest::gtest_main
)
target_compile_features(qar_qar_adapter_tests PRIVATE cxx_std_17)

qar_apply_warning_flags(qar_qar_adapter_tests)

include(GoogleTest)
gtest_discover_tests(qar_qar_adapter_tests DISCOVERY_TIMEOUT 30)
