# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# ===========================================================================
# qar_qar_adapter — the QarOS C API side of the library seams, as five static
# libraries. They are components of the qar_ogre_adapter package; EXPORT_NAME is
# the name the package exports and the alias consumers use (qar_ogre_adapter::<name>):
#
#   capi      qar_qar_adapter_capi      the program's one QarOS C API loader
#                                       (QAR_IMPLEMENT_DYNAMIC_LOADING), the
#                                       callback-slot arena, IQarSessionOwner.
#                                       Every other component links it.
#   render    qar_qar_adapter_render    3D stereo streams (IQarRenderSenderAdapter)
#                                       and the session bootstrap
#                                       (QarCApiSenderFactory). Needs Ogre, through
#                                       qar_ogre_streaming's seam headers.
#   panels    qar_qar_adapter_panels    GUI-panel sender + per-panel input.
#   url       qar_qar_adapter_url       browser-backed (URL) panels.
#   gestures  qar_qar_adapter_gestures  app-volume (spatial) gestures.
#
# panels, url and gestures need only qar_ar_core and capi: no Ogre, no Qt.
# The QarOS SDK DLL is loaded at RUN time; nothing links an import library.
# ===========================================================================

# ---------------------------------------------------------------------------
# The QarOS SDK. DYNAMIC LOADING (the vendor-recommended model): no qar-streaming-c.lib,
# no find_package(qar-streaming-c). With QAR_ENABLE_DYNAMIC_LOADING the vendored single
# header resolves every qar_impl_* symbol from the DLL at run time (qar_library_load()),
# loaded by its full path with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR:
# its own folder is searched first for its dependencies, then the application directory,
# the directories the program added and System32, never PATH or the current directory.
#
# The DLL path is resolved at run time from Config::runtime_binaries_folder_path, which
# the host fills (qar_rviz_plugin reads QAR_RUNTIME_BINARIES_DIR); QAR_SDK_ROOT is only
# a configure-time check.
# ---------------------------------------------------------------------------
# QAR_SDK_ROOT is only checked here: -D (or a preset; an empty value counts as unset), else the
# environment variable QAR_SDK_ROOT.
if("${QAR_SDK_ROOT}" STREQUAL "" AND NOT "$ENV{QAR_SDK_ROOT}" STREQUAL "")
  set(QAR_SDK_ROOT "$ENV{QAR_SDK_ROOT}" CACHE PATH "Root of the QarOS SDK install tree (bin, include)" FORCE)
endif()
if("${QAR_SDK_ROOT}" STREQUAL "")
  set(_qar_dll "qar-streaming-c.dll (QAR_SDK_ROOT not set)")
  message(WARNING
    "QAR_SDK_ROOT is not set. The build continues - nothing links against the SDK, the DLL is "
    "loaded at run time - but nothing can connect to QarOS until qar-streaming-c.dll can be loaded. "
    "Set the environment variable QAR_SDK_ROOT or -DQAR_SDK_ROOT=<QarOS install>.")
else()
  set(_qar_dll "${QAR_SDK_ROOT}/bin/qar-streaming-c.dll")
  if(NOT EXISTS "${_qar_dll}")
    message(WARNING
      "QarOS SDK DLL not found at ${_qar_dll}. "
      "The build continues - nothing links against the SDK, the DLL is loaded at run time - "
      "but nothing can connect to QarOS until qar-streaming-c.dll can be loaded. "
      "Set -DQAR_SDK_ROOT=<QarOS install> to the SDK's install tree.")
  endif()
endif()
message(STATUS
  "qar_qar_adapter: QarOS SDK backend, DYNAMIC loading of ${_qar_dll} "
  "(no import lib linked)")

# One static library per component, all sharing include/ (installed as
# include/qar_qar_adapter/...). /W4 only (no /permissive-).
function(qar_adapter_component target export_name)
  add_library(${target} STATIC ${ARGN})
  add_library(qar_ogre_adapter::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES
    EXPORT_NAME ${export_name}
    POSITION_INDEPENDENT_CODE ON)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  endif()
endfunction()

qar_adapter_component(qar_qar_adapter_capi capi
  src/qar_capi_loader.cpp
  src/qar_capi_input_slots.cpp
)
target_link_libraries(qar_qar_adapter_capi PUBLIC
  qar_ogre_adapter::qar_ar_core
)
# A usage requirement, not a private flag: every translation unit that calls the
# C API (the four components, and any consumer that calls it directly) must
# dispatch through the tables qar_capi_loader.cpp defines. There is no import lib.
target_compile_definitions(qar_qar_adapter_capi PUBLIC QAR_ENABLE_DYNAMIC_LOADING)

qar_adapter_component(qar_qar_adapter_render render
  src/qar_capi_render_sender_adapter.cpp
  # SDK-free geodetic -> ECEF math and env-var parsing, kept separate so they are unit
  # testable without a live QarSession (qar_qar_adapter/test).
  src/geo_anchor.cpp
  src/env_config.cpp
)
target_link_libraries(qar_qar_adapter_render PUBLIC
  qar_ogre_adapter::capi
  qar_ogre_adapter::qar_ar_core
  qar_ogre_adapter::qar_ogre_streaming
)

qar_adapter_component(qar_qar_adapter_panels panels
  src/qar_capi_panel_sender_adapter.cpp
  src/qar_capi_panel_input_source.cpp
)
target_link_libraries(qar_qar_adapter_panels PUBLIC
  qar_ogre_adapter::capi
  qar_ogre_adapter::qar_ar_core
)

qar_adapter_component(qar_qar_adapter_url url
  src/qar_capi_url_panel_adapter.cpp
)
target_link_libraries(qar_qar_adapter_url PUBLIC
  qar_ogre_adapter::capi
  qar_ogre_adapter::qar_ar_core
)

qar_adapter_component(qar_qar_adapter_gestures gestures
  src/qar_capi_spatial_gesture_source.cpp
)
target_link_libraries(qar_qar_adapter_gestures PUBLIC
  qar_ogre_adapter::capi
  qar_ogre_adapter::qar_ar_core
)
