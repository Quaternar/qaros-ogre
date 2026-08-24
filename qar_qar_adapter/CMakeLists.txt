# ===========================================================================
# qar_qar_adapter — producer-side glue: the QarOS C API behind the library seam.
# ===========================================================================
# Implements qar_ogre_streaming's IQarRenderSenderAdapter / IQarRenderSenderFactory
# against the QarOS streaming C API: session lifecycle, peer admission, app
# volumes, geo-anchoring, and per-frame OpenGL submission.
#
# The QarOS runtime is loaded DYNAMICALLY at run time (see below), so this package
# builds with nothing but the vendored header — no SDK is needed to compile or to
# run the unit tests. The SDK binaries are only needed to actually stream.
#
# Buildable standalone, via the repo-root umbrella, or via colcon (package.xml
# declares <build_type>cmake</build_type>). After installation it is consumable
# with find_package(qar_qar_adapter).

cmake_minimum_required(VERSION 3.20)
project(qar_qar_adapter VERSION 0.1.0 LANGUAGES CXX
  DESCRIPTION "QarOS streaming C API adapter for qar_ogre_streaming")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(QAR_BUILD_TESTS "Build the qar_qar_adapter unit tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Standalone/colcon build: resolve the sibling package. Under the umbrella the
# target already exists, and the helper functions are already defined.
if(NOT TARGET qar_ogre_streaming)
  find_package(qar_ogre_streaming REQUIRED)
endif()
if(NOT COMMAND qar_apply_warning_flags)
  function(qar_apply_warning_flags target)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endfunction()
endif()

add_library(qar_qar_adapter STATIC
  src/qar_capi_render_sender_adapter.cpp
  # SDK-free geodetic -> ECEF/ENU maths, split out of the adapter so that it is
  # unit testable without a live QarSession (see test/).
  src/geo_anchor.cpp
)
add_library(qar_qar_adapter::qar_qar_adapter ALIAS qar_qar_adapter)

target_compile_features(qar_qar_adapter PUBLIC cxx_std_17)

target_include_directories(qar_qar_adapter PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# qar_ogre_streaming brings the IQarRenderSenderAdapter seam, the shared QarOS
# value types, and the vendored QarOS header on its public include path.
target_link_libraries(qar_qar_adapter PUBLIC
  qar_ogre_streaming::qar_ogre_streaming
)

# ---------------------------------------------------------------------------
# DYNAMIC LOADING of the QarOS runtime (vendor-recommended).
#
# We deliberately do NOT link qar-streaming-c.lib. With QAR_ENABLE_DYNAMIC_LOADING
# the vendored single header resolves every qar_impl_* symbol from the DLL at run
# time via qar_library_load(), which buys two things:
#   * no import lib and no generated vendor Targets.cmake to keep in sync, so the
#     package compiles anywhere the header is present;
#   * the DLL is loaded with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
#     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which EXCLUDES the host application's own
#     directory. That matters: the ROS 2 distro ships older MSVCP140/VCRUNTIME140
#     next to rviz2.exe, and letting those hijack the SDK's CRT imports crashed
#     qar_library_init with an access violation.
#
# The DLL path is resolved at run time from Config::runtime_binaries_folder_path
# (environment variable QAR_RUNTIME_BINARIES_DIR), so nothing about the SDK is
# baked in here.
#
# PRIVATE: this package is the only translation unit that CALLS qar_* wrappers
# (the library and the plugin use QarOS value types only), and the single
# QAR_IMPLEMENT_DYNAMIC_LOADING() lives in its one .cpp.
# ---------------------------------------------------------------------------
target_compile_definitions(qar_qar_adapter PRIVATE QAR_ENABLE_DYNAMIC_LOADING)

set_target_properties(qar_qar_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)

qar_apply_warning_flags(qar_qar_adapter)

# --- Install + export ------------------------------------------------------
install(TARGETS qar_qar_adapter
  EXPORT qar_qar_adapterTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT qar_qar_adapterTargets
  FILE qar_qar_adapterTargets.cmake
  NAMESPACE qar_qar_adapter::
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qar_qar_adapterConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/qar_qar_adapterConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/qar_qar_adapterConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/qar_qar_adapterConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/qar_qar_adapterConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/cmake"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/package.xml"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}")

# --- Tests -----------------------------------------------------------------
if(QAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
