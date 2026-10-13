// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Pure parsing helpers for the diagnostics env vars (QAR_ADAPTER_FRAME_TRACE,
// QAR_ADAPTER_PRIME, QAR_PANEL_TICK_MS, QAR_PANEL_LOG_SEVERITY / QAR_PANEL_LOG_CONSOLE /
// QAR_LOG_SEVERITY). Kept apart from qar_capi_render_sender_adapter.cpp and from
// qar_rviz_plugin's qar_streaming_panel.cpp for the reason geo_anchor.cpp is kept apart
// from the adapter: these functions take the raw env string (never call std::getenv themselves),
// so they have no QarOS/Qt dependency and are unit-testable without a QarOS session or a
// QApplication. Call sites (the adapter, and the panel of qar_rviz_plugin) pass
// std::getenv(...) in.

#ifndef QAR_QAR_ADAPTER__ENV_CONFIG_HPP_
#define QAR_QAR_ADAPTER__ENV_CONFIG_HPP_

#include <string>

namespace qar_qar_adapter
{

/// @brief Parse a boolean env value. Recognizes "1"/"true"/"on"/"yes" as true and
/// "0"/"false"/"off"/"no" as false, case-insensitively; anything else (including a
/// null pointer, i.e. the variable is unset) yields @p default_value. Used for
/// QAR_ADAPTER_PRIME (default true: one tick per frame, not two) and, from the
/// panel side, to seed the "Prime next frame" checkbox.
bool parse_bool_env(const char * value, bool default_value);

/// @brief True if @p value is non-null (QAR_ADAPTER_TRACE-style "set to anything
/// enables it" convention, matched here for QAR_ADAPTER_FRAME_TRACE).
bool frame_trace_requested(const char * value);

/// @brief Parse QAR_PANEL_TICK_MS: any finite non-negative (fractional allowed, e.g. "0.1")
/// value becomes the render-timer period in milliseconds; a null, empty, negative,
/// implausibly large or non-finite ("nan", "inf", "1e999") value, or text that does not
/// start with a number, falls back to @p default_ms. Text after a leading number is
/// ignored ("5abc" is 5). The panel's own default is 0.1 ms; values below 1 ms are
/// realized as a Qt zero-interval timer plus a steady-clock throttle (QTimer's own
/// interval cannot represent sub-1ms periods).
double parse_tick_ms_env(const char * value, double default_ms);

// ---------------------------------------------------------------------------
// QarOS library logging (QarLibraryInit) — how a severity/console request becomes
// what the vendor library actually does.
// ---------------------------------------------------------------------------

/// @brief Severity filter, in the SAME order and with the SAME numeric values as the
/// vendored C API's `QarLogSeverity` (third_party/qar/include/qar_streaming.h: TRACE=0,
/// DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5). Declared here so the parsing is
/// SDK-free; qar_capi_render_sender_adapter.cpp static_asserts the two enums agree.
enum class LogSeverity : int
{
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kFatal = 5,
};

/// @brief Parse a severity word: trace | debug | info | warn | warning | error | fatal,
/// case-insensitive, surrounding whitespace ignored — the spellings the QarOS processes
/// themselves accept for QAR_LOG_SEVERITY (plus nothing else: "3" or "critical" are NOT
/// accepted by the vendor parser, so they are not accepted here either).
/// @return false (and leaves @p out untouched) for null, empty or unknown text.
bool parse_log_severity(const char * text, LogSeverity & out);

/// @brief The spelling the vendor parses from QAR_LOG_SEVERITY: "trace", "debug",
/// "info", "warn", "error" or "fatal".
const char * log_severity_name(LogSeverity severity);

/// @brief What the QarOS library logging inside RViz is configured to do, and why.
struct LibraryLogging
{
  /// Severity for BOTH `QarLibraryInit.log_severity` and the QAR_LOG_SEVERITY
  /// environment variable (see @ref resolve_library_logging for why both).
  LogSeverity severity = LogSeverity::kTrace;
  /// `QarLibraryInit.enable_console_logging`: duplicate every library log line to stdout
  /// (which the RViz launcher script captures in its stdout log).
  bool console = true;
  /// Which input decided the severity: "QAR_PANEL_LOG_SEVERITY", "QAR_LOG_SEVERITY" or
  /// "default".
  const char * severity_source = "default";
  /// True when QAR_LOG_SEVERITY was set to something DIFFERENT from @ref severity — the
  /// vendor library gives that variable precedence over the init struct, so the adapter
  /// must overwrite it or the request is silently ignored.
  bool environment_overridden = false;
  /// Non-empty when an input was unusable (e.g. QAR_PANEL_LOG_SEVERITY='warnig'); the
  /// host should show it — silently ignoring a typo is how a "quiet" run stays loud.
  std::string warning;
};

/// @brief Decide the library's log severity and console duplication from the three
/// environment inputs.
///
/// ## Why this exists
///
/// Passing `log_severity = QAR_LOG_SEVERITY_WARN` in `QarLibraryInit` is not enough: a
/// `QAR_LOG_SEVERITY=trace` inherited from the operator's shell wins over the struct, and
/// a "quiet" run then writes hundreds of thousands of TRACE lines to the library's log
/// file AND to RViz's stdout. Measured against the shipped qar-streaming-c.dll (library
/// init + runtime create only, nothing spawned):
///
///   * with NO `QAR_LOG_SEVERITY` in the environment the init struct's severity is
///     honoured: ERROR suppresses the WARN line "Runtime folder path not provided".
///   * with `QAR_LOG_SEVERITY=trace` in the environment and `log_severity = ERROR` in
///     the struct the WARN line is still written. The ENVIRONMENT WINS, in both
///     directions (env=error beats struct=TRACE too).
///   * the variable is read through the C runtime's environment table, so it must be
///     changed with `_putenv_s`. A bare `SetEnvironmentVariable` is not seen.
///   * `enable_console_logging = false` removes the stdout duplicate and keeps the file.
///
/// So the request must be made on BOTH channels, which is what this returns: the adapter
/// passes @ref LibraryLogging::severity in the struct and exports it as QAR_LOG_SEVERITY
/// before `qar_library_init`.
///
/// ## Precedence
///
/// severity: `QAR_PANEL_LOG_SEVERITY` (the variable of the panel of qar_rviz_plugin,
/// specific) > `QAR_LOG_SEVERITY` (QarOS-wide, what the vendor reads) > @p default_severity.
/// An unrecognised value is skipped (and reported in @ref LibraryLogging::warning) rather
/// than falling back silently.
///
/// console: `QAR_PANEL_LOG_CONSOLE` (parse_bool_env words) if recognised; otherwise ON
/// for trace/debug/info — the vendor's bug-report profile — and OFF for warn and above,
/// where it would only duplicate a trickle into a file nobody reads.
///
/// @param panel_severity  getenv("QAR_PANEL_LOG_SEVERITY") (may be null)
/// @param env_severity    getenv("QAR_LOG_SEVERITY") (may be null)
/// @param panel_console   getenv("QAR_PANEL_LOG_CONSOLE") (may be null)
/// @param default_severity used when neither names a severity. The vendor asked that
///        bug reports carry TRACE logs, so callers normally pass kTrace.
LibraryLogging resolve_library_logging(
  const char * panel_severity, const char * env_severity, const char * panel_console,
  LogSeverity default_severity);

/// @brief True when a QAR_ZENOH_LOG request may start zenoh's own (very verbose) logger.
///
/// zenoh at `debug` writes thousands of lines per minute into RViz's stdout on top of the
/// library's own log. It is a diagnostic for "the session will not come up", so it is
/// only honoured when the library severity is trace or debug; at info and above (the
/// quiet profile) the request is ignored and the adapter says so. This is the in-process
/// guard behind the launcher's "quiet means quiet": a QAR_ZENOH_LOG left in the shell
/// cannot switch zenoh logging on under a quiet severity.
bool zenoh_logging_allowed(LogSeverity library_severity);

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__ENV_CONFIG_HPP_
