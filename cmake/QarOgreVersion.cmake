# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# The version of qar_ogre_adapter. Include it before project() (the top-level CMakeLists.txt) and
# in every module that reports the version (qar_ar_core); it sets in the including scope
#   QAR_OGRE_ADAPTER_VERSION_CORE      X.Y.Z, read from VERSION at the repository root, the only copy
#   QAR_OGRE_ADAPTER_VERSION_RESOLVED  the version a build reports:
#     HEAD tagged vX.Y.Z        X.Y.Z
#     HEAD tagged vX.Y.Z-rc.N   X.Y.Z-rc.N
#     any other commit          X.Y.Z-dev.<commits since the last v* tag>+<short sha>
#     no git checkout           X.Y.Z-dev (a source archive)
# A tag whose X.Y.Z differs from VERSION stops the configure. -DQAR_OGRE_ADAPTER_VERSION_FULL=<v>
# replaces the git part (a release build passes the release version); its X.Y.Z must equal
# VERSION as well. The value is fixed when CMake configures: configure again after a new commit
# or tag. The release steps are in AGENTS.md, section "Versions and releases".

include_guard(DIRECTORY)
find_package(Git QUIET)

get_filename_component(_qar_ogre_version_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(_qar_ogre_version_git root out_var)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    WORKING_DIRECTORY "${root}"
    OUTPUT_VARIABLE output
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE result)
  if(result EQUAL 0)
    set(${out_var} "${output}" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

function(_qar_ogre_version_resolve root out_core out_full)
  set(file "${root}/VERSION")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "${file} does not exist: it holds the version X.Y.Z of qar_ogre_adapter.")
  endif()
  file(STRINGS "${file}" core LIMIT_COUNT 1)
  string(STRIP "${core}" core)
  if(NOT core MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "${file} must hold X.Y.Z, found '${core}'.")
  endif()

  if(NOT "${QAR_OGRE_ADAPTER_VERSION_FULL}" STREQUAL "")
    set(full "${QAR_OGRE_ADAPTER_VERSION_FULL}")
    if(NOT full MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)([-+][0-9A-Za-z.+-]+)?$")
      message(FATAL_ERROR "QAR_OGRE_ADAPTER_VERSION_FULL '${full}' is not a version.")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL core)
      message(FATAL_ERROR "QAR_OGRE_ADAPTER_VERSION_FULL ${full} does not match VERSION ${core}.")
    endif()
  elseif(NOT GIT_FOUND OR NOT EXISTS "${root}/.git")
    set(full "${core}-dev")
  else()
    _qar_ogre_version_git("${root}" tag describe --tags --exact-match --match "v[0-9]*" HEAD)
    if(tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-rc\\.[0-9]+)?$")
      if(NOT CMAKE_MATCH_1 STREQUAL core)
        message(FATAL_ERROR "Tag ${tag} does not match VERSION ${core}. Fix VERSION or the tag.")
      endif()
      set(full "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
    else()
      _qar_ogre_version_git("${root}" last describe --tags --abbrev=0 --match "v[0-9]*" HEAD)
      if(last)
        _qar_ogre_version_git("${root}" count rev-list --count "${last}..HEAD")
      else()
        _qar_ogre_version_git("${root}" count rev-list --count HEAD)
      endif()
      if(count STREQUAL "")
        set(count 0)
      endif()
      _qar_ogre_version_git("${root}" sha rev-parse --short=8 HEAD)
      if(sha STREQUAL "")
        set(full "${core}-dev.${count}")
      else()
        set(full "${core}-dev.${count}+${sha}")
      endif()
    endif()
  endif()
  set(${out_core} "${core}" PARENT_SCOPE)
  set(${out_full} "${full}" PARENT_SCOPE)
endfunction()

# Resolved once per configure; every later include reads the same values.
get_property(_qar_ogre_version_done GLOBAL PROPERTY QAR_OGRE_ADAPTER_VERSION_RESOLVED SET)
if(NOT _qar_ogre_version_done)
  _qar_ogre_version_resolve("${_qar_ogre_version_root}" _qar_core _qar_full)
  set_property(GLOBAL PROPERTY QAR_OGRE_ADAPTER_VERSION_CORE "${_qar_core}")
  set_property(GLOBAL PROPERTY QAR_OGRE_ADAPTER_VERSION_RESOLVED "${_qar_full}")
  message(STATUS "qar_ogre_adapter version ${_qar_full} (VERSION ${_qar_core})")
endif()
get_property(QAR_OGRE_ADAPTER_VERSION_CORE GLOBAL PROPERTY QAR_OGRE_ADAPTER_VERSION_CORE)
get_property(QAR_OGRE_ADAPTER_VERSION_RESOLVED GLOBAL PROPERTY QAR_OGRE_ADAPTER_VERSION_RESOLVED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_qar_ogre_version_root}/VERSION")
