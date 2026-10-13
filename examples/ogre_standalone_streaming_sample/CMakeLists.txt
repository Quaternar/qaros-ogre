# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT
#
# ===========================================================================
# examples/ogre_standalone_streaming_sample — reusability proof
# ===========================================================================
# A plain Ogre app that uses qar_ogre_streaming with NO rviz_common / Qt / ROS.
# It links ONLY the reusable library (+ its header-only test_support helpers) and
# Ogre — exactly the dependency set a non-RViz Ogre consumer would use. The
# check_layering_examples CTest (scripts/check_layering.py) scans examples/** to
# keep it that way.

add_executable(qar_ogre_standalone_streaming_sample
  src/main.cpp
  src/sample_scene.cpp
  src/sample_capture.cpp
)

target_compile_features(qar_ogre_standalone_streaming_sample PRIVATE cxx_std_17)

target_include_directories(qar_ogre_standalone_streaming_sample PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  # Vendored stb_image_write.h for the debug CPU-readback PNG path (self-contained;
  # the sample does not link the RViz plugin, so it compiles its own copy).
  "${CMAKE_SOURCE_DIR}/third_party/stb"
)

# Link ONLY the reusable library + its header-only test-support helpers. Ogre comes
# transitively (qar_ogre_streaming links rviz_ogre_vendor::OgreMain PUBLIC). NO
# plugin, NO rviz, NO Qt, NO ament/ROS.
target_link_libraries(qar_ogre_standalone_streaming_sample PRIVATE
  qar_ogre_adapter::qar_ogre_streaming
  qar_ogre_adapter::qar_ogre_streaming_test_support
)

# Bake the rviz_ogre_vendor bin dir (RenderSystem_GL*/OgreMain) as the default Ogre
# plugin dir so the sample can create its own Ogre::Root out of the box. Overridable
# at runtime by --ogre-plugin-dir or the ROS2_INSTALL_ROOT env var.
if(DEFINED QAR_ROS2_INSTALL_ROOT AND NOT QAR_ROS2_INSTALL_ROOT STREQUAL "")
  target_compile_definitions(qar_ogre_standalone_streaming_sample PRIVATE
    "QAR_OGRE_PLUGIN_DIR=\"${QAR_ROS2_INSTALL_ROOT}/opt/rviz_ogre_vendor/bin\"")
endif()

# Warnings: /W4 (MSVC) / -Wall -Wextra. Intentionally NOT /permissive- here — the
# vendored stb_image_write.h implementation is not written to that strictness.
if(MSVC)
  target_compile_options(qar_ogre_standalone_streaming_sample PRIVATE /W4)
else()
  target_compile_options(qar_ogre_standalone_streaming_sample PRIVATE -Wall -Wextra)
endif()
