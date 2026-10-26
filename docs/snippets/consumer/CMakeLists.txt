# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
#
# Consumer project of the installed qar_ogre_adapter package: configured on its own against
# CMAKE_PREFIX_PATH, never as part of the repository build. It is the install smoke test.
cmake_minimum_required(VERSION 3.22)
project(qar_ogre_adapter_consumer LANGUAGES CXX)

# [qar-snippet:find]
find_package(qar_ogre_adapter 0.1.0 CONFIG REQUIRED
  COMPONENTS qar_ar_core qar_ogre_streaming render panels url gestures)
# [qar-snippet-end:find]

set(consumer_targets
  qar_ogre_adapter::qar_ar_core
  qar_ogre_adapter::qar_ogre_streaming
  qar_ogre_adapter::render
  qar_ogre_adapter::panels
  qar_ogre_adapter::url
  qar_ogre_adapter::gestures)
list(JOIN consumer_targets " " consumer_target_text)

add_executable(qar_ogre_adapter_consumer main.cpp)
target_compile_features(qar_ogre_adapter_consumer PRIVATE cxx_std_17)
target_link_libraries(qar_ogre_adapter_consumer PRIVATE ${consumer_targets})
target_compile_definitions(qar_ogre_adapter_consumer PRIVATE
  "CONSUMER_PACKAGE_VERSION=\"${qar_ogre_adapter_VERSION}\""
  "CONSUMER_TARGETS=\"${consumer_target_text}\"")
