// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/gui_call_guard.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

#include "qar_qar_adapter/log_time.hpp"

namespace qar_qar_adapter
{
namespace
{

constexpr double kDefaultLimitMs = 20.0;
constexpr std::uint64_t kPrintFirst = 5;
constexpr std::uint64_t kPrintEvery = 100;

thread_local bool t_gui_thread = false;
thread_local OwnerCallReport t_owner_report;

std::mutex g_mutex;
GuiCallStats g_stats;
std::map<std::string, std::uint64_t> g_counts;
std::function<void(const std::string &)> g_sink;

double read_limit_ms()
{
  std::string text;
#ifdef _WIN32
  char * buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, "QAR_GUI_CALL_WARN_MS") == 0 && buffer != nullptr) {
    text = buffer;
  }
  std::free(buffer);
#else
  const char * value = std::getenv("QAR_GUI_CALL_WARN_MS");
  if (value != nullptr) {
    text = value;
  }
#endif
  if (!text.empty()) {
    char * end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && value > 0.0) {
      return value;
    }
  }
  return kDefaultLimitMs;
}

void record_slow(const char * call, double took_ms, double limit_ms)
{
  std::string line;
  std::function<void(const std::string &)> sink;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.slow;
    if (took_ms > g_stats.worst_ms) {
      g_stats.worst_ms = took_ms;
      g_stats.worst = call;
    }
    const std::uint64_t count = ++g_counts[call];
    if (count > kPrintFirst && (count % kPrintEvery) != 0) {
      return;
    }
    char text[256] = {0};
    std::snprintf(
      text, sizeof(text) - 1,
      "[qar-gui-call] %s WARNING %s took %.0f ms on the GUI thread (limit %.0f ms)",
      wall_clock_hms_ms().c_str(), call, took_ms, limit_ms);
    line = text;
    sink = g_sink;
  }
  if (sink) {
    sink(line);
  } else {
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
  }
}

}  // namespace

void mark_gui_thread()
{
  t_gui_thread = true;
}

bool on_gui_thread()
{
  return t_gui_thread;
}

double gui_call_limit_ms()
{
  static const double limit = read_limit_ms();
  return limit;
}

void mark_owner_thread(OwnerCallReport report)
{
  t_owner_report = std::move(report);
}

GuiCallTimer::GuiCallTimer(const char * call) noexcept
: call_(call), start_(), armed_(t_gui_thread), owner_armed_(static_cast<bool>(t_owner_report))
{
  if (armed_ || owner_armed_) {
    start_ = std::chrono::steady_clock::now();
  }
}

GuiCallTimer::~GuiCallTimer()
{
  if (!armed_ && !owner_armed_) {
    return;
  }
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start_).count();
  if (armed_) {
    const double limit_ms = gui_call_limit_ms();
    if (took_ms > limit_ms) {
      record_slow(call_, took_ms, limit_ms);
    }
  }
  if (owner_armed_ && took_ms >= kOwnerCallLimitMs && t_owner_report) {
    t_owner_report(call_, took_ms);
  }
}

GuiCallStats gui_call_stats()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_stats;
}

void set_gui_call_sink(std::function<void(const std::string &)> sink)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sink = std::move(sink);
}

void reset_gui_call_stats()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stats = GuiCallStats();
  g_counts.clear();
}

}  // namespace qar_qar_adapter
