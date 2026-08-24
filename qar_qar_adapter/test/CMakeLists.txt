# Unit tests for qar_qar_adapter.
#
# Only the SDK-free parts are testable here: everything else in this package is
# bound to a live QarSession. geo_anchor.cpp was split out of the adapter for
# exactly that reason.

# GoogleTest may already have been fetched by a sibling package in the same CMake
# tree (the umbrella build); fetch it here only if it has not.
if(NOT TARGET GTest::gtest_main)
  if(COMMAND qar_require_googletest)
    qar_require_googletest()
  else()
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Test-only dependency: keep its install() rules out of our install prefix.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

add_executable(qar_qar_adapter_tests
  geo_anchor_tests.cpp
)

# Compile the geo unit directly rather than linking the whole adapter: the adapter
# pulls in the QarOS C API surface, and these tests must not need any of it. The
# source is small and has no dependencies beyond <cmath>.
target_sources(qar_qar_adapter_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/geo_anchor.cpp"
)
target_include_directories(qar_qar_adapter_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_link_libraries(qar_qar_adapter_tests PRIVATE GTest::gtest_main)
target_compile_features(qar_qar_adapter_tests PRIVATE cxx_std_17)

qar_apply_warning_flags(qar_qar_adapter_tests)

include(GoogleTest)
gtest_discover_tests(qar_qar_adapter_tests DISCOVERY_TIMEOUT 30)
