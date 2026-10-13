# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# ===========================================================================
# examples/qaros_live_stream - the Ogre sample scene, streamed to a real QarOS Visualizer
# ===========================================================================
# A plain Ogre program that opens a QarOS session through the render component of the adapter and
# renders the scene of ../ogre_standalone_streaming_sample for an invited Visualizer. It links only
# the public targets of this repository (the render component pulls in qar_ogre_streaming, Ogre and
# the C API loader); there is no Qt, no ROS and no RViz. check_layering.py scans examples/** to keep
# it that way. Nothing links the QarOS SDK: the DLL is loaded at run time from --sdk-bin.

add_executable(qar_ogre_live_stream_example
  main.cpp
  ../ogre_standalone_streaming_sample/src/sample_scene.cpp
)

target_compile_features(qar_ogre_live_stream_example PRIVATE cxx_std_17)

# [qar-snippet:link]
target_link_libraries(qar_ogre_live_stream_example PRIVATE qar_ogre_adapter::render)
# [qar-snippet-end:link]

# The folder with RenderSystem_GL.dll and OgreMain.dll, from the distribution this build was
# configured with; empty when the build did not use one (the Ogre start then fails with a message).
set(_qar_ogre_plugin_dir "")
if(DEFINED QAR_ROS2_INSTALL_ROOT AND NOT QAR_ROS2_INSTALL_ROOT STREQUAL "")
  set(_qar_ogre_plugin_dir "${QAR_ROS2_INSTALL_ROOT}/opt/rviz_ogre_vendor/bin")
endif()
target_compile_definitions(qar_ogre_live_stream_example PRIVATE
  "QAR_OGRE_PLUGIN_DIR=\"${_qar_ogre_plugin_dir}\"")

if(MSVC)
  target_compile_options(qar_ogre_live_stream_example PRIVATE /W4)
else()
  target_compile_options(qar_ogre_live_stream_example PRIVATE -Wall -Wextra)
endif()
