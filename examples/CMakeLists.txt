# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# ===========================================================================
# examples/ - the programs that use the libraries of this repository
# ===========================================================================
# ogre_standalone_streaming_sample proves that qar_ogre_streaming works outside RViz2: it depends only
# on qar_ogre_streaming and Ogre, with synthetic in-process peers instead of QarOS. qaros_live_stream
# streams the same scene to a real QarOS Visualizer and therefore also needs the render component of
# the adapter. Neither uses rviz_common, Qt or ROS. Each example is added only when everything it links
# is built (the check_layering_examples CTest runs scripts/check_layering.py on examples/**).

add_subdirectory(ogre_standalone_streaming_sample)

if(TARGET qar_qar_adapter_render)
  add_subdirectory(qaros_live_stream)
endif()
