// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ar_core/panel_types.hpp"

namespace qar_ar_core
{

const char *
to_string(Status status)
{
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::InvalidArgument:
      return "InvalidArgument";
    case Status::InvalidState:
      return "InvalidState";
    case Status::AlreadyExists:
      return "AlreadyExists";
    case Status::NotFound:
      return "NotFound";
    case Status::GuiThreadRequired:
      return "GuiThreadRequired";
    case Status::SenderFailed:
      return "SenderFailed";
  }
  return "Unknown";
}

const char *
to_string(DiagnosticSeverity severity)
{
  switch (severity) {
    case DiagnosticSeverity::Info:
      return "INFO";
    case DiagnosticSeverity::Warning:
      return "WARN";
    case DiagnosticSeverity::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace qar_ar_core
