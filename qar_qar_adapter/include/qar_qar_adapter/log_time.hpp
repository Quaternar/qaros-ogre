// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__LOG_TIME_HPP_
#define QAR_QAR_ADAPTER__LOG_TIME_HPP_

/// @file log_time.hpp
/// @brief The wall-clock stamp, the thread number and the once-only guard that the adapters use
/// for their log lines.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <thread>

namespace qar_qar_adapter
{

/// @brief Local wall-clock time of @p moment as `HH:MM:SS.mmm`. It is the clock of the host
/// panel's own `[qar-panel]` lines and of the crash dumps the QarOS runtime writes, so an adapter
/// line can be lined up against both.
inline std::string
hms_ms_of(std::chrono::system_clock::time_point moment)
{
  const auto now = moment;
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  const auto millis =
    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  char buf[16] = {0};
  std::snprintf(
    buf, sizeof(buf) - 1, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
    static_cast<int>(millis));
  return std::string(buf);
}

/// @brief The wall-clock time now, as `HH:MM:SS.mmm` (see hms_ms_of()).
inline std::string
wall_clock_hms_ms()
{
  return hms_ms_of(std::chrono::system_clock::now());
}

/// @brief The number of the calling thread as a log line prints it (`tid=<n>`). With the
/// Microsoft C++ library it is the operating-system thread id, the number that a debugger, a
/// process viewer and a crash dump name the thread by. 0 if the library prints the id as
/// something else than a number.
inline unsigned long long
thread_number()
{
  std::ostringstream text;
  text << std::this_thread::get_id();
  return std::strtoull(text.str().c_str(), nullptr, 10);
}

/// @brief Lets exactly one caller through, however many threads ask: guards a log line that is
/// printed once per object (for example the first frame of a sender). A lock-free atomic
/// exchange, so asking costs nothing on a path that runs every frame.
class LogOnce
{
public:
  LogOnce() = default;
  LogOnce(const LogOnce &) = delete;
  LogOnce & operator=(const LogOnce &) = delete;

  /// @return true for the first call over the lifetime of the object, false for every later one.
  bool first() { return !done_.exchange(true, std::memory_order_acq_rel); }

  /// @return true once first() has been called.
  bool done() const { return done_.load(std::memory_order_acquire); }

private:
  std::atomic<bool> done_{false};
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__LOG_TIME_HPP_
