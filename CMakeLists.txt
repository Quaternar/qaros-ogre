# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# qar_ogre_adapter: the QarOS integration layer for Ogre applications - qar_ar_core,
# qar_ogre_streaming and the QarOS C API adapter components capi, session, render, url, gestures.
cmake_minimum_required(VERSION 3.22)
project(qar_ogre_adapter VERSION 0.2.0
  DESCRIPTION "QarOS integration layer for Ogre applications"
  LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/QarBuildSupport.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/QarRosBuildSupport.cmake")

option(QAR_BUILD_TESTS "Build the unit tests and the layering guard tests" ON)
option(QAR_BUILD_EXAMPLES "Build the examples under examples/" ON)
option(QAR_BUILD_DOC_SNIPPETS "Build the compile-checked documentation snippets" ON)
option(QAR_INSTALL "Generate the install rules and the CMake package" ON)

qar_set_default_build_type()
qar_set_default_install_prefix()
qar_use_ros2_distro()

# The QarOS C header (qar-streaming-c) and the C++ headers (qar-streaming-cpp) come from the public
# qaros repository, a git submodule in external/qaros: one qaros commit for both.
set(QAROS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/qaros" CACHE PATH
  "Checkout of the public qaros repository (the git submodule external/qaros)")
qar_add_qaros_headers("${QAROS_SOURCE_DIR}")

add_subdirectory(qar_ar_core)
add_subdirectory(qar_ogre_streaming)
add_subdirectory(qar_qar_adapter)
if(QAR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
# The documentation snippets are built when the documentation folder is present.
if(QAR_BUILD_DOC_SNIPPETS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/snippets/CMakeLists.txt")
  add_subdirectory(docs/snippets)
endif()

if(QAR_BUILD_TESTS)
  enable_testing()
  qar_require_googletest()
  add_subdirectory(qar_ogre_streaming/test)
  add_subdirectory(qar_ar_core/test)
  add_subdirectory(qar_qar_adapter/test)
  qar_add_layering_guard_tests(MODULES qar_ar_core qar_ogre_streaming qar_qar_adapter examples)
endif()

qar_install_enabled(_qar_install)
if(_qar_install)
  install(TARGETS qar_ar_core qar_ogre_streaming
      qar_qar_adapter_capi qar_qar_adapter_session qar_qar_adapter_render
      qar_qar_adapter_url qar_qar_adapter_gestures
    EXPORT qar_ogre_adapterTargets
    ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include)
  install(DIRECTORY qar_ar_core/include/ qar_ogre_streaming/include/ qar_qar_adapter/include/
    DESTINATION include)
  # The QarOS headers of the submodule, so that the repositories built on this package compile
  # against the same qaros commit.
  install(FILES "${QAROS_SOURCE_DIR}/qar-streaming-c/include/qar_streaming.h" DESTINATION include)
  install(DIRECTORY "${QAROS_SOURCE_DIR}/qar-streaming-cpp/include/" DESTINATION include)
  install(FILES "${QAROS_SOURCE_DIR}/LICENSE" DESTINATION share/qar_ogre_adapter
    RENAME LICENSE.qaros)
  install(FILES LICENSE DESTINATION share/qar_ogre_adapter OPTIONAL)
  qar_install_package(qar_ogre_adapter EXPORT qar_ogre_adapterTargets NAMESPACE qar_ogre_adapter::)
endif()
