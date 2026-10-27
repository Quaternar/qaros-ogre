// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__GUI_CALL_GUARD_HPP_
#define QAR_QAR_ADAPTER__GUI_CALL_GUARD_HPP_

/// @file gui_call_guard.hpp
/// @brief Times the QarOS calls that a GUI thread makes, so that a call which blocks the thread
/// is named in the log instead of showing up as a frozen window.
///
/// A host marks its GUI thread once. Every QarOS call in the adapters stands behind a
/// QAR_GUI_TIMED line; on the marked thread a call that takes longer than the limit prints one
/// warning that names it. The thread that owns the QarOS session is marked with a report
/// function: it receives each call that takes half a second or longer. On every other thread the
/// timer does nothing.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace qar_qar_adapter
{

/// @brief Marks the calling thread as the GUI thread whose QarOS calls are timed. Call once.
void mark_gui_thread();

/// @brief True on the thread that mark_gui_thread() marked.
bool on_gui_thread();

/// @brief The limit in milliseconds: 20, or QAR_GUI_CALL_WARN_MS when that is a positive number.
/// Read once.
double gui_call_limit_ms();

/// @brief The function that receives a slow call of the session's owner thread: the name of the
/// call and the milliseconds it took.
using OwnerCallReport = std::function<void(const char *, double)>;

/// @brief The time in milliseconds from which a call of the session's owner thread is reported.
constexpr double kOwnerCallLimitMs = 500.0;

/// @brief Marks the calling thread as the one that owns the QarOS session: each timed call on it
/// that takes @ref kOwnerCallLimitMs or longer is handed to @p report on that thread. A thread
/// calls this once, before its first call; an empty @p report ends the marking.
void mark_owner_thread(OwnerCallReport report);

/// @brief Times one QarOS call. On the GUI thread a call longer than the limit prints one line
///   `[qar-gui-call] HH:MM:SS.mmm WARNING <call> took <ms> ms on the GUI thread (limit <n> ms)`
/// the first 5 times per call name, then once in 100. On the owner thread a call of
/// @ref kOwnerCallLimitMs or longer goes to the report function. On any other thread it does
/// nothing.
class GuiCallTimer
{
public:
  /// @param call A string literal that names the call; it is kept, not copied.
  explicit GuiCallTimer(const char * call) noexcept;
  ~GuiCallTimer();
  GuiCallTimer(const GuiCallTimer &) = delete;
  GuiCallTimer & operator=(const GuiCallTimer &) = delete;

private:
  const char * call_;
  std::chrono::steady_clock::time_point start_;
  bool armed_;
  bool owner_armed_;
};

/// @brief What the guard saw so far.
struct GuiCallStats
{
  std::uint64_t slow = 0;      ///< Calls over the limit on the GUI thread.
  std::string worst;           ///< Name of the slowest of them; empty when there was none.
  double worst_ms = 0.0;
};

/// @brief A thread-safe copy of the counters.
GuiCallStats gui_call_stats();

/// @brief Receives each warning line instead of stderr. An empty function restores stderr. For
/// tests.
void set_gui_call_sink(std::function<void(const std::string &)> sink);

/// @brief Clears the counters and the per-call print counts. For tests.
void reset_gui_call_stats();

}  // namespace qar_qar_adapter

#define QAR_GUI_CALL_JOIN_(a, b) a##b
#define QAR_GUI_CALL_NAME_(line) QAR_GUI_CALL_JOIN_(qar_gui_call_timer_, line)
/// Times everything that follows in the same scope; @p call is a string literal.
#define QAR_GUI_TIMED(call) \
  ::qar_qar_adapter::GuiCallTimer QAR_GUI_CALL_NAME_(__LINE__)(call)
/// Calls the QarOS function @p fn with the arguments that follow and times that one call: the
/// timer lives to the end of the full expression that holds it.
#define QAR_CALL(fn, ...) (::qar_qar_adapter::GuiCallTimer(#fn), fn(__VA_ARGS__))

#endif  // QAR_QAR_ADAPTER__GUI_CALL_GUARD_HPP_
