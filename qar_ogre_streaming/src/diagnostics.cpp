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

const char * to_string(SkipReason reason)
{
  switch (reason) {
    case SkipReason::None:
      return "none";
    case SkipReason::SenderNotReady:
      return "sender_not_ready";
    case SkipReason::SenderError:
      return "sender_error";
    case SkipReason::TargetInitFailed:
      return "target_init_failed";
    case SkipReason::BudgetShed:
      return "budget_shed";
  }
  return "unknown";
}

}  // namespace qar_ogre_streaming
