// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/env_config.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace qar_qar_adapter
{
namespace
{

std::string to_lower(const char * value)
{
  std::string out(value);
  for (char & c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

/// Lower-cased copy of @p value with leading/trailing whitespace removed.
std::string trimmed_lower(const char * value)
{
  std::string out = to_lower(value);
  const char * const kSpace = " \t\r\n";
  const std::size_t first = out.find_first_not_of(kSpace);
  if (first == std::string::npos) {
    return std::string();
  }
  const std::size_t last = out.find_last_not_of(kSpace);
  return out.substr(first, last - first + 1);
}

}  // namespace

bool parse_bool_env(const char * value, bool default_value)
{
  if (value == nullptr) {
    return default_value;
  }
  const std::string v = to_lower(value);
  if (v == "1" || v == "true" || v == "on" || v == "yes") {
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no") {
    return false;
  }
  return default_value;
}

bool frame_trace_requested(const char * value)
{
  return value != nullptr;
}

double parse_tick_ms_env(const char * value, double default_ms)
{
  if (value == nullptr) {
    return default_ms;
  }
  // strtod (not atof), so an empty or all-whitespace string, or text without a number,
  // is detected through endptr and keeps the default instead of atof's silent 0.0. Only a
  // missing number is detected: the text after a leading number is ignored ("5abc" is 5).
  // strtod also reads "nan", "inf" and "1e999" (which overflows to infinity); a NaN passes
  // both comparisons below, so the value must be checked to be finite first.
  char * end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value) {
    return default_ms;  // no digits at all
  }
  if (!std::isfinite(parsed) || parsed < 0.0 || parsed > 100000.0) {
    return default_ms;  // reject non-finite / negative / absurd values rather than misfire
  }
  return parsed;
}

bool parse_log_severity(const char * text, LogSeverity & out)
{
  if (text == nullptr) {
    return false;
  }
  const std::string v = trimmed_lower(text);
  if (v == "trace") { out = LogSeverity::kTrace; return true; }
  if (v == "debug") { out = LogSeverity::kDebug; return true; }
  if (v == "info") { out = LogSeverity::kInfo; return true; }
  if (v == "warn" || v == "warning") { out = LogSeverity::kWarn; return true; }
  if (v == "error") { out = LogSeverity::kError; return true; }
  if (v == "fatal") { out = LogSeverity::kFatal; return true; }
  return false;
}

const char * log_severity_name(LogSeverity severity)
{
  switch (severity) {
    case LogSeverity::kTrace: return "trace";
    case LogSeverity::kDebug: return "debug";
    case LogSeverity::kInfo: return "info";
    case LogSeverity::kWarn: return "warn";
    case LogSeverity::kError: return "error";
    case LogSeverity::kFatal: return "fatal";
  }
  return "trace";
}

LibraryLogging resolve_library_logging(
  const char * panel_severity, const char * env_severity, const char * panel_console,
  LogSeverity default_severity)
{
  LibraryLogging out;
  out.severity = default_severity;
  out.severity_source = "default";

  const auto present = [](const char * s) { return s != nullptr && *s != '\0'; };

  // Severity: the panel's own variable, then the vendor's, then the default. A value that
  // is present but not a severity is reported and skipped, so the next input still counts.
  bool decided = false;
  LogSeverity parsed = default_severity;
  if (present(panel_severity)) {
    if (parse_log_severity(panel_severity, parsed)) {
      out.severity = parsed;
      out.severity_source = "QAR_PANEL_LOG_SEVERITY";
      decided = true;
    } else {
      out.warning = std::string("QAR_PANEL_LOG_SEVERITY='") + panel_severity +
        "' is not a severity (trace|debug|info|warn|error|fatal) - ignored";
    }
  }
  if (!decided && present(env_severity)) {
    if (parse_log_severity(env_severity, parsed)) {
      out.severity = parsed;
      out.severity_source = "QAR_LOG_SEVERITY";
      decided = true;
    } else {
      if (!out.warning.empty()) { out.warning += "; "; }
      out.warning = out.warning + "QAR_LOG_SEVERITY='" + env_severity +
        "' is not a severity (trace|debug|info|warn|error|fatal) - ignored";
    }
  }

  // The vendor library reads QAR_LOG_SEVERITY in preference to the init struct. If the
  // variable is set to anything other than what we decided (including text the vendor
  // would reject), it has to be overwritten.
  if (present(env_severity)) {
    LogSeverity env_value = LogSeverity::kTrace;
    out.environment_overridden =
      !parse_log_severity(env_severity, env_value) || env_value != out.severity;
  }

  // Console: explicit word wins; otherwise the trace/debug/info profile keeps the
  // vendor's bug-report behaviour and warn+ drops the duplicate.
  out.console = out.severity <= LogSeverity::kInfo;
  if (present(panel_console)) {
    const std::string v = trimmed_lower(panel_console);
    if (v == "1" || v == "true" || v == "on" || v == "yes") {
      out.console = true;
    } else if (v == "0" || v == "false" || v == "off" || v == "no") {
      out.console = false;
    } else {
      if (!out.warning.empty()) { out.warning += "; "; }
      out.warning = out.warning + "QAR_PANEL_LOG_CONSOLE='" + panel_console +
        "' is not on|off - ignored";
    }
  }
  return out;
}

bool zenoh_logging_allowed(LogSeverity library_severity)
{
  return library_severity <= LogSeverity::kDebug;
}

}  // namespace qar_qar_adapter
