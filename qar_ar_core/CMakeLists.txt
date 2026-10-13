# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT
#
# qar_ar_core — the Qt-free core every QarOS integration layer shares: the GUI-panel
# seam types (panel_types, qar_panel_sender), the pointer-input seam
# (panel_input_source), the AR pointing ray, room/scene/ROS frames, the spatial pick
# model, the per-frame timing log, the URL-panel seam, and THE one wrapper of the
# vendored QarOS C header (qar_streaming_c.hpp).
#
# Plain CMake. The C++17 standard library and the vendored qar_streaming.h only: no Qt,
# Ogre, ROS or RViz, and no call into the QarOS C API (it uses the C API's TYPES only, so
# it never needs the SDK DLL or the dynamic-loading tables). Enforced by the layering guard.

add_library(qar_ar_core STATIC
  src/panel_types.cpp
  src/spatial_frames.cpp
  src/spatial_pick.cpp
)
add_library(qar_ogre_adapter::qar_ar_core ALIAS qar_ar_core)

target_compile_features(qar_ar_core PUBLIC cxx_std_17)

target_include_directories(qar_ar_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
  # The vendored QarOS C header; only include/qar_ar_core/qar_streaming_c.hpp includes it.
  "$<BUILD_INTERFACE:${QAR_VENDOR_INCLUDE_DIR}>"
)

# One OpenGL policy for every translation unit of every consumer, whatever its include
# order: the vendored header has ONE include guard, and its first inclusion fixes whether
# the OpenGL types and the OpenGL function-table slots exist (see qar_streaming_c.hpp).
target_compile_definitions(qar_ar_core PUBLIC QAR_ENABLE_OPENGL)

# Static lib linked into a shared consumer (the RViz plugin) must be position-independent.
set_target_properties(qar_ar_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

qar_apply_warning_flags(qar_ar_core)
