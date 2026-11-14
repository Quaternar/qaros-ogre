# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# qar_ar_core — the Qt-free core every QarOS integration layer shares: the panel decisions
# (the naming rule, the attach decision, the panel state guard, the outcome of a panel
# operation), the pointer-input vocabulary (pointer_input, panel_input_source), the AR
# pointing ray, room/scene/ROS frames, the spatial pick model, the per-frame timing log, the
# URL-panel seam, and THE one wrapper of the QarOS C header of the qaros submodule (qar_streaming_c.hpp).
#
# Plain CMake. The C++17 standard library and qar_streaming.h (qaros submodule) only: no Qt,
# Ogre, ROS or RViz, and no call into the QarOS C API (it uses the C API's TYPES only, so
# it never needs the SDK DLL or the dynamic-loading tables). Enforced by the layering guard.

add_library(qar_ar_core STATIC
  src/common_name.cpp
  src/panel_attach.cpp
  src/spatial_frames.cpp
  src/spatial_pick.cpp
)
add_library(qar_ogre_adapter::qar_ar_core ALIAS qar_ar_core)

target_compile_features(qar_ar_core PUBLIC cxx_std_17)

# qar_ar_core/version.hpp: the version of this build (cmake/QarOgreVersion.cmake), generated into
# the build folder and installed with the other headers.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/QarOgreVersion.cmake")
set(QAR_AR_CORE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include"
  CACHE INTERNAL "Generated headers of qar_ar_core")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in"
  "${QAR_AR_CORE_GENERATED_INCLUDE_DIR}/qar_ar_core/version.hpp" @ONLY)

target_include_directories(qar_ar_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${QAR_AR_CORE_GENERATED_INCLUDE_DIR}>"
  "$<INSTALL_INTERFACE:include>"
)
# The QarOS headers of the qaros submodule (qar_streaming.h and qar_streaming_cpp/); only
# include/qar_ar_core/qar_streaming_c.hpp includes the C header. The package installs both
# header sets into its own include folder, so the installed target needs no qaros target.
target_link_libraries(qar_ar_core PUBLIC "$<BUILD_INTERFACE:qar-streaming-cpp>")

# One OpenGL policy for every translation unit of every consumer, whatever its include
# order: the QarOS C header has ONE include guard, and its first inclusion fixes whether
# the OpenGL types and the OpenGL function-table slots exist (see qar_streaming_c.hpp).
target_compile_definitions(qar_ar_core PUBLIC QAR_ENABLE_OPENGL)

# Static lib linked into a shared consumer (the RViz plugin) must be position-independent.
set_target_properties(qar_ar_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

qar_apply_warning_flags(qar_ar_core)
