// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Tests for the diagnostics env-var parsing (QAR_ADAPTER_FRAME_TRACE / QAR_ADAPTER_PRIME /
// QAR_PANEL_TICK_MS) and the QarOS library-logging resolution (QAR_PANEL_LOG_SEVERITY /
// QAR_LOG_SEVERITY / QAR_PANEL_LOG_CONSOLE). Like geo_anchor_tests.cpp, this exercises SDK-free logic directly —
// no QarSession, no Qt — so it compiles the small env_config.cpp unit rather than the
// whole adapter (see the CMakeLists.txt in this directory).

#include <gtest/gtest.h>

#include "qar_qar_adapter/env_config.hpp"

#include <string>

namespace
{
using qar_qar_adapter::frame_trace_requested;
using qar_qar_adapter::LibraryLogging;
using qar_qar_adapter::log_severity_name;
using qar_qar_adapter::LogSeverity;
using qar_qar_adapter::parse_bool_env;
using qar_qar_adapter::parse_log_severity;
using qar_qar_adapter::parse_tick_ms_env;
using qar_qar_adapter::resolve_library_logging;
using qar_qar_adapter::zenoh_logging_allowed;
}  // namespace

// --- parse_bool_env (QAR_ADAPTER_PRIME) -------------------------------------

TEST(EnvConfig, BoolUnsetReturnsDefaultTrue)
{
  EXPECT_TRUE(parse_bool_env(nullptr, true));
}

TEST(EnvConfig, BoolUnsetReturnsDefaultFalse)
{
  EXPECT_FALSE(parse_bool_env(nullptr, false));
}

TEST(EnvConfig, BoolRecognizesZeroAndOne)
{
  EXPECT_FALSE(parse_bool_env("0", true));
  EXPECT_TRUE(parse_bool_env("1", false));
}

TEST(EnvConfig, BoolRecognizesWordsCaseInsensitively)
{
  EXPECT_TRUE(parse_bool_env("On", false));
  EXPECT_TRUE(parse_bool_env("TRUE", false));
  EXPECT_TRUE(parse_bool_env("yes", false));
  EXPECT_FALSE(parse_bool_env("Off", true));
  EXPECT_FALSE(parse_bool_env("FALSE", true));
  EXPECT_FALSE(parse_bool_env("no", true));
}

TEST(EnvConfig, BoolUnrecognizedFallsBackToDefault)
{
  EXPECT_TRUE(parse_bool_env("banana", true));
  EXPECT_FALSE(parse_bool_env("banana", false));
  EXPECT_TRUE(parse_bool_env("", true));
}

// --- frame_trace_requested (QAR_ADAPTER_FRAME_TRACE) ------------------------

TEST(EnvConfig, FrameTraceUnsetIsOff)
{
  EXPECT_FALSE(frame_trace_requested(nullptr));
}

TEST(EnvConfig, FrameTraceAnyValueIsOn)
{
  // Matches the existing QAR_ADAPTER_TRACE convention: presence enables it,
  // regardless of the value's text (a bare `set X=` still defines the variable).
  EXPECT_TRUE(frame_trace_requested("1"));
  EXPECT_TRUE(frame_trace_requested("0"));
  EXPECT_TRUE(frame_trace_requested(""));
  EXPECT_TRUE(frame_trace_requested("anything"));
}

// --- parse_tick_ms_env (QAR_PANEL_TICK_MS) ----------------------------------

TEST(EnvConfig, TickMsUnsetReturnsDefault)
{
  EXPECT_DOUBLE_EQ(parse_tick_ms_env(nullptr, 1.0), 1.0);
}

TEST(EnvConfig, TickMsParsesPositiveInteger)
{
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("8", 1.0), 8.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("4", 1.0), 4.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("33", 1.0), 33.0);  // non-standard value: still honored
}

TEST(EnvConfig, TickMsParsesFractionalValues)
{
  // Sub-1ms periods (the panel realizes these as a zero-interval QTimer + throttle).
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("0.1", 1.0), 0.1);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("1.5", 1.0), 1.5);
}

TEST(EnvConfig, TickMsAcceptsZero)
{
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("0", 1.0), 0.0);
}

TEST(EnvConfig, TickMsRejectsNegative)
{
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("-5", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("-0.1", 1.0), 1.0);
}

TEST(EnvConfig, TickMsRejectsGarbageAndAbsurdValues)
{
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("banana", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("999999999", 1.0), 1.0);
  // strtod reads these as numbers; none of them is a finite period.
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("nan", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("NaN", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("-nan", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("inf", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("-inf", 1.0), 1.0);
  EXPECT_DOUBLE_EQ(parse_tick_ms_env("1e999", 1.0), 1.0);
}

// --- parse_log_severity / log_severity_name ---------------------------------

TEST(LogSeverityParse, EnumValuesMatchTheVendorHeaderOrder)
{
  // qar_streaming.h: TRACE=0 DEBUG=1 INFO=2 WARN=3 ERROR=4 FATAL=5. The adapter also
  // static_asserts this against the real header; this pins the SDK-free copy.
  EXPECT_EQ(static_cast<int>(LogSeverity::kTrace), 0);
  EXPECT_EQ(static_cast<int>(LogSeverity::kDebug), 1);
  EXPECT_EQ(static_cast<int>(LogSeverity::kInfo), 2);
  EXPECT_EQ(static_cast<int>(LogSeverity::kWarn), 3);
  EXPECT_EQ(static_cast<int>(LogSeverity::kError), 4);
  EXPECT_EQ(static_cast<int>(LogSeverity::kFatal), 5);
}

TEST(LogSeverityParse, EveryWordCaseInsensitivelyAndTrimmed)
{
  struct Case { const char * text; LogSeverity expected; };
  const Case cases[] = {
    {"trace", LogSeverity::kTrace}, {"DEBUG", LogSeverity::kDebug},
    {"Info", LogSeverity::kInfo}, {"warn", LogSeverity::kWarn},
    {"WARNING", LogSeverity::kWarn}, {"  error  ", LogSeverity::kError},
    {"Fatal", LogSeverity::kFatal},
  };
  for (const Case & c : cases) {
    // Start from a value that differs from the expectation, so a parser that "succeeds"
    // without writing would be caught by the EXPECT_EQ.
    LogSeverity out =
      (c.expected == LogSeverity::kFatal) ? LogSeverity::kTrace : LogSeverity::kFatal;
    EXPECT_TRUE(parse_log_severity(c.text, out)) << c.text;
    EXPECT_EQ(out, c.expected) << c.text;
  }
}

TEST(LogSeverityParse, RejectsNullEmptyNumbersAndUnknownWordsLeavingOutUntouched)
{
  const char * const bad[] = {nullptr, "", "   ", "3", "critical", "warnig", "off", "none"};
  for (const char * text : bad) {
    LogSeverity out = LogSeverity::kError;
    EXPECT_FALSE(parse_log_severity(text, out)) << (text != nullptr ? text : "(null)");
    EXPECT_EQ(out, LogSeverity::kError);
  }
}

TEST(LogSeverityParse, NamesAreTheSpellingsTheVendorParsesAndRoundTrip)
{
  EXPECT_STREQ(log_severity_name(LogSeverity::kTrace), "trace");
  EXPECT_STREQ(log_severity_name(LogSeverity::kDebug), "debug");
  EXPECT_STREQ(log_severity_name(LogSeverity::kInfo), "info");
  EXPECT_STREQ(log_severity_name(LogSeverity::kWarn), "warn");
  EXPECT_STREQ(log_severity_name(LogSeverity::kError), "error");
  EXPECT_STREQ(log_severity_name(LogSeverity::kFatal), "fatal");
  for (int i = 0; i <= 5; ++i) {
    LogSeverity out = LogSeverity::kTrace;
    ASSERT_TRUE(parse_log_severity(log_severity_name(static_cast<LogSeverity>(i)), out));
    EXPECT_EQ(static_cast<int>(out), i);
  }
}

// --- resolve_library_logging -------------------------------------------------

TEST(LibraryLoggingResolve, QuietProfileIsWarnWithoutConsoleDuplication)
{
  const LibraryLogging l = resolve_library_logging("warn", nullptr, "off", LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kWarn);
  EXPECT_FALSE(l.console);
  EXPECT_STREQ(l.severity_source, "QAR_PANEL_LOG_SEVERITY");
  EXPECT_FALSE(l.environment_overridden);
  EXPECT_TRUE(l.warning.empty());
}

TEST(LibraryLoggingResolve, NothingSetIsTheVendorBugReportProfile)
{
  const LibraryLogging l = resolve_library_logging(nullptr, nullptr, nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kTrace);
  EXPECT_TRUE(l.console);
  EXPECT_STREQ(l.severity_source, "default");
  EXPECT_FALSE(l.environment_overridden);
  EXPECT_TRUE(l.warning.empty());
}

TEST(LibraryLoggingResolve, StaleTraceInTheEnvironmentIsFlaggedForOverwrite)
{
  // The panel asks for warn, a previous launcher run left QAR_LOG_SEVERITY=trace in the
  // shell, and the vendor library obeys the variable over the init struct: hundreds of
  // thousands of TRACE lines. The decision must stay warn, and the caller must be told the
  // variable has to be overwritten.
  const LibraryLogging l = resolve_library_logging("warn", "trace", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kWarn);
  EXPECT_TRUE(l.environment_overridden);
  EXPECT_FALSE(l.console);  // warn and above: no stdout duplicate by default
}

TEST(LibraryLoggingResolve, AgreeingEnvironmentNeedsNoOverwrite)
{
  const LibraryLogging l = resolve_library_logging("warn", "WARN", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kWarn);
  EXPECT_FALSE(l.environment_overridden);
}

TEST(LibraryLoggingResolve, VendorVariableAloneStillDecidesLikeItAlwaysDid)
{
  // Without QAR_PANEL_LOG_SEVERITY the vendor variable is the effective setting (it
  // beats the struct), so it must keep winning over the default.
  const LibraryLogging l = resolve_library_logging(nullptr, "error", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kError);
  EXPECT_STREQ(l.severity_source, "QAR_LOG_SEVERITY");
  EXPECT_FALSE(l.environment_overridden);
}

TEST(LibraryLoggingResolve, UnknownPanelValueIsReportedAndTheNextInputStillCounts)
{
  const LibraryLogging l = resolve_library_logging("warnig", "error", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kError);
  EXPECT_STREQ(l.severity_source, "QAR_LOG_SEVERITY");
  EXPECT_NE(l.warning.find("QAR_PANEL_LOG_SEVERITY='warnig'"), std::string::npos) << l.warning;

  const LibraryLogging d = resolve_library_logging("warnig", nullptr, nullptr, LogSeverity::kTrace);
  EXPECT_EQ(d.severity, LogSeverity::kTrace);
  EXPECT_STREQ(d.severity_source, "default");
  EXPECT_FALSE(d.warning.empty());
}

TEST(LibraryLoggingResolve, UnparseableVendorVariableMustBeOverwritten)
{
  // The panel decided, so the garbled variable only has to be replaced (the adapter says so
  // when it does); nothing further to warn about.
  const LibraryLogging l = resolve_library_logging("warn", "loud", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kWarn);
  EXPECT_TRUE(l.environment_overridden);
  EXPECT_TRUE(l.warning.empty()) << l.warning;
}

TEST(LibraryLoggingResolve, UnparseableVendorVariableWithNoPanelSettingIsReportedAndDefaulted)
{
  const LibraryLogging l = resolve_library_logging(nullptr, "loud", nullptr, LogSeverity::kTrace);
  EXPECT_EQ(l.severity, LogSeverity::kTrace);
  EXPECT_STREQ(l.severity_source, "default");
  EXPECT_TRUE(l.environment_overridden) << "the vendor would reject 'loud'; make it say trace";
  EXPECT_NE(l.warning.find("QAR_LOG_SEVERITY='loud'"), std::string::npos) << l.warning;
}

TEST(LibraryLoggingResolve, ConsoleDefaultFollowsSeverity)
{
  const struct { LogSeverity severity; bool console; } expected[] = {
    {LogSeverity::kTrace, true}, {LogSeverity::kDebug, true}, {LogSeverity::kInfo, true},
    {LogSeverity::kWarn, false}, {LogSeverity::kError, false}, {LogSeverity::kFatal, false},
  };
  for (const auto & e : expected) {
    const LibraryLogging l =
      resolve_library_logging(log_severity_name(e.severity), nullptr, nullptr, LogSeverity::kTrace);
    EXPECT_EQ(l.severity, e.severity);
    EXPECT_EQ(l.console, e.console) << log_severity_name(e.severity);
  }
}

TEST(LibraryLoggingResolve, ExplicitConsoleWordWinsAndAGarbledOneIsReported)
{
  EXPECT_TRUE(resolve_library_logging("warn", nullptr, "ON", LogSeverity::kTrace).console);
  EXPECT_FALSE(resolve_library_logging("trace", nullptr, "off", LogSeverity::kTrace).console);
  EXPECT_FALSE(resolve_library_logging("info", nullptr, " 0 ", LogSeverity::kTrace).console);

  const LibraryLogging l = resolve_library_logging("warn", nullptr, "maybe", LogSeverity::kTrace);
  EXPECT_FALSE(l.console);  // fell back to the severity default (warn => off)
  EXPECT_NE(l.warning.find("QAR_PANEL_LOG_CONSOLE='maybe'"), std::string::npos) << l.warning;
}

TEST(LibraryLoggingResolve, EmptyStringsCountAsUnset)
{
  const LibraryLogging l = resolve_library_logging("", "", "", LogSeverity::kInfo);
  EXPECT_EQ(l.severity, LogSeverity::kInfo);
  EXPECT_STREQ(l.severity_source, "default");
  EXPECT_FALSE(l.environment_overridden);
  EXPECT_TRUE(l.warning.empty());
  EXPECT_TRUE(l.console);
}

// --- zenoh_logging_allowed ---------------------------------------------------

TEST(ZenohLogging, OnlyUnderTraceOrDebugSeverity)
{
  EXPECT_TRUE(zenoh_logging_allowed(LogSeverity::kTrace));
  EXPECT_TRUE(zenoh_logging_allowed(LogSeverity::kDebug));
  EXPECT_FALSE(zenoh_logging_allowed(LogSeverity::kInfo));
  EXPECT_FALSE(zenoh_logging_allowed(LogSeverity::kWarn));
  EXPECT_FALSE(zenoh_logging_allowed(LogSeverity::kError));
  EXPECT_FALSE(zenoh_logging_allowed(LogSeverity::kFatal));
}
