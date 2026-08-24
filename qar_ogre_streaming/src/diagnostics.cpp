// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/diagnostics.hpp"

namespace qar_ogre_streaming
{

const char * to_string(DiagnosticSeverity severity)
{
  switch (severity) {
    case DiagnosticSeverity::Info:
      return "Info";
    case DiagnosticSeverity::Warning:
      return "Warning";
    case DiagnosticSeverity::Error:
      return "Error";
  }
  return "Unknown";
}

}  // namespace qar_ogre_streaming
