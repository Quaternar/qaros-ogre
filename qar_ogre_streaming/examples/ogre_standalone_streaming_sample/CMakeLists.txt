# ===========================================================================
# examples/ogre_standalone_streaming_sample — reusability proof
# ===========================================================================
# A plain Ogre app that uses qar_ogre_streaming with NO rviz_common, Qt or ROS. It
# links ONLY the reusable library (+ its header-only test_support helpers) and
# Ogre — exactly the dependency set a non-RViz Ogre consumer would use. The
# include-dependency checker (check_no_rviz_includes) scans examples/** to keep it
# that way. See README.md in this directory for what it renders and how to run it.

add_executable(qar_ogre_standalone_streaming_sample
  src/main.cpp
  src/sample_scene.cpp
  src/sample_capture.cpp
)

target_compile_features(qar_ogre_standalone_streaming_sample PRIVATE cxx_std_17)

target_include_directories(qar_ogre_standalone_streaming_sample PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  # Vendored stb_image_write.h for the debug CPU-readback PNG path. The sample
  # carries its own copy so that it stays self-contained.
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)

# Link ONLY the reusable library + its header-only test-support helpers. Ogre comes
# transitively (qar_ogre_streaming links rviz_ogre_vendor::OgreMain PUBLIC).
target_link_libraries(qar_ogre_standalone_streaming_sample PRIVATE
  qar_ogre_streaming::qar_ogre_streaming
  qar_ogre_streaming::test_support
)

# Bake the rviz_ogre_vendor bin dir (RenderSystem_GL* / OgreMain) as the default
# Ogre plugin dir so the sample can create its own Ogre::Root out of the box.
# Overridable at run time by --ogre-plugin-dir or the ROS2_INSTALL_ROOT env var.
set(_qar_sample_ogre_bin "")
if(DEFINED QAR_ROS2_INSTALL_ROOT AND NOT QAR_ROS2_INSTALL_ROOT STREQUAL "")
  set(_qar_sample_ogre_bin "${QAR_ROS2_INSTALL_ROOT}/opt/rviz_ogre_vendor/bin")
elseif(DEFINED rviz_ogre_vendor_DIR AND NOT rviz_ogre_vendor_DIR STREQUAL "")
  get_filename_component(_qar_sample_distro "${rviz_ogre_vendor_DIR}/../../.." ABSOLUTE)
  set(_qar_sample_ogre_bin "${_qar_sample_distro}/opt/rviz_ogre_vendor/bin")
endif()
if(_qar_sample_ogre_bin)
  target_compile_definitions(qar_ogre_standalone_streaming_sample PRIVATE
    "QAR_OGRE_PLUGIN_DIR=\"${_qar_sample_ogre_bin}\"")
endif()

# Warnings: /W4 (MSVC) / -Wall -Wextra. Intentionally NOT /permissive- here — the
# vendored stb_image_write.h implementation is not written to that strictness,
# which is why qar_apply_warning_flags is not used for this target.
if(MSVC)
  target_compile_options(qar_ogre_standalone_streaming_sample PRIVATE /W4)
else()
  target_compile_options(qar_ogre_standalone_streaming_sample PRIVATE -Wall -Wextra)
endif()
