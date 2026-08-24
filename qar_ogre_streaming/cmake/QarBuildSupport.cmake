# ===========================================================================
# QarBuildSupport — small helpers shared by the packages in this repository.
# ===========================================================================
# Both helpers are guarded so that this module is safe to include more than once
# (each package includes it so it can also be configured standalone, and the
# repo-root umbrella includes it too).

# Consistent warning level. RViz Humble builds with C++17; MSVC gets /W4.
if(NOT COMMAND qar_apply_warning_flags)
  function(qar_apply_warning_flags target)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endfunction()
endif()

# ---------------------------------------------------------------------------
# Put the ROS 2 Humble binary distro on CMAKE_PREFIX_PATH so that
# find_package(rviz_ogre_vendor) resolves.
#
# Ogre is NOT vendored here: it is consumed from the ROS 2 distro's
# rviz_ogre_vendor package, which is exactly the Ogre build RViz itself links.
# Using any other Ogre would mix two Ogre runtimes inside one process.
#
# Under colcon the distro is already on CMAKE_PREFIX_PATH (the workspace was
# built from a sourced ROS 2 environment), so this is a no-op. For a plain
# stand-alone CMake build, point it at the distro with any one of:
#   * environment variable ROS2_INSTALL_ROOT
#   * -DQAR_ROS2_INSTALL_ROOT=<distro>/ros2-windows
#   * -DCMAKE_PREFIX_PATH=<distro>/ros2-windows
# ---------------------------------------------------------------------------
if(NOT COMMAND qar_locate_ros2_distro)
  function(qar_locate_ros2_distro)
    if(NOT DEFINED QAR_ROS2_INSTALL_ROOT OR QAR_ROS2_INSTALL_ROOT STREQUAL "")
      if(DEFINED ENV{ROS2_INSTALL_ROOT} AND NOT "$ENV{ROS2_INSTALL_ROOT}" STREQUAL "")
        set(QAR_ROS2_INSTALL_ROOT "$ENV{ROS2_INSTALL_ROOT}" CACHE PATH
          "Root of the extracted ROS 2 Humble Windows distro (.../ros2-windows)")
      endif()
    endif()

    if(DEFINED QAR_ROS2_INSTALL_ROOT AND NOT QAR_ROS2_INSTALL_ROOT STREQUAL "")
      if(NOT EXISTS "${QAR_ROS2_INSTALL_ROOT}")
        message(FATAL_ERROR
          "QAR_ROS2_INSTALL_ROOT does not exist: ${QAR_ROS2_INSTALL_ROOT}\n"
          "Set ROS2_INSTALL_ROOT / -DQAR_ROS2_INSTALL_ROOT to the extracted "
          "ros2-windows folder. See README.md.")
      endif()
      list(PREPEND CMAKE_PREFIX_PATH "${QAR_ROS2_INSTALL_ROOT}")
      set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
      message(STATUS "Using ROS 2 distro for Ogre discovery: ${QAR_ROS2_INSTALL_ROOT}")
    endif()
  endfunction()
endif()

# ---------------------------------------------------------------------------
# GoogleTest, fetched at configure time and pinned to a release tag.
# Idempotent: the second caller in one CMake tree sees the targets already exist.
# ---------------------------------------------------------------------------
if(NOT COMMAND qar_require_googletest)
  function(qar_require_googletest)
    if(TARGET GTest::gtest_main)
      return()
    endif()
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    # Match the shared MSVC runtime the ROS 2 distro / Ogre use (avoids a CRT mix).
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # GoogleTest is a test-only dependency. Without this its own install() rules
    # run as part of ours and dump gtest headers, libs and .pc files into the
    # consumer's install prefix.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endfunction()
endif()
