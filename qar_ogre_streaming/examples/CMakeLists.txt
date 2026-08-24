# ===========================================================================
# examples/ — standalone samples that consume qar_ogre_streaming directly.
# ===========================================================================
# These prove the reusable library is usable OUTSIDE RViz2. They depend ONLY on
# qar_ogre_streaming + Ogre — never rviz_common, Qt or ROS. That is enforced by the
# check_no_rviz_includes test, which scans examples/** alongside the library.

add_subdirectory(ogre_standalone_streaming_sample)
