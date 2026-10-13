# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT
#
# Compile-checked documentation snippets of qar_ogre_adapter. Every C++ and CMake block of the documentation
# is a region of a file in this folder (or of an example), so the code shown compiles and links.
# The target is built and never run; it registers no test.
file(GLOB _qar_snippet_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(qar_ogre_adapter_doc_snippets ${_qar_snippet_sources})
target_compile_features(qar_ogre_adapter_doc_snippets PRIVATE cxx_std_17)
# The snippets use every component of the package, so the target links all of them.
# [qar-snippet:link]
target_link_libraries(qar_ogre_adapter_doc_snippets PRIVATE
  qar_ogre_adapter::qar_ar_core
  qar_ogre_adapter::qar_ogre_streaming
  qar_ogre_adapter::render
  qar_ogre_adapter::panels
  qar_ogre_adapter::url
  qar_ogre_adapter::gestures
)
# [qar-snippet-end:link]
qar_apply_warning_flags(qar_ogre_adapter_doc_snippets)
