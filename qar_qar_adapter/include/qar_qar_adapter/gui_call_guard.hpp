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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

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

/// @brief The QarOS call the owner thread is inside right now, readable from another thread.
struct OwnerCallClock
{
  /// The name of the call (a string literal); null when the thread is in no timed call.
  std::atomic<const char *> call{nullptr};
  /// Steady-clock nanoseconds at which that call started.
  std::atomic<std::int64_t> since_ns{0};
};

/// @brief Marks the calling thread as the one that owns the QarOS session: each timed call on it
/// that takes @ref kOwnerCallLimitMs or longer is handed to @p report on that thread. A thread
/// calls this once, before its first call; an empty @p report ends the marking.
void mark_owner_thread(OwnerCallReport report);

/// @brief The same, and every timed call of the thread also writes its name and start into
/// @p clock, so that a supervisor thread can tell how long the thread has been inside one call.
/// @p clock must outlive the marking; a null @p clock writes nothing. An empty @p report and a
/// null @p clock end the marking.
void mark_owner_thread(OwnerCallReport report, OwnerCallClock * clock);

/// @brief The same, and @p abandoned is the flag that says the owner of the thread was left
/// behind. From the moment it is true, every QAR_CALL on the thread returns at once with an error
/// status (a result with code @ref kBlockedCallStatus, a zero value for any other type) and calls
/// nothing: a thread that was left behind in a stalled call makes no further QarOS call when that
/// call returns. @p abandoned must outlive the marking; a null pointer blocks nothing.
void mark_owner_thread(
  OwnerCallReport report, OwnerCallClock * clock, const std::atomic<bool> * abandoned);

/// @brief Marks the calling thread as the frame worker of one sender: the thread that makes its
/// QarOS frame calls. It behaves like an owner thread (see the three-argument mark_owner_thread):
/// each timed call that takes @p report_limit_ms or longer is handed to @p report on that thread,
/// @p clock shows from another thread which call it is inside, and from the moment @p abandoned is
/// true every QAR_CALL on the thread returns at once with an error status and calls nothing. A
/// thread calls this once, before its first call; an empty @p report, a null @p clock and a null
/// @p abandoned end the marking. @p clock and @p abandoned must outlive the marking.
void mark_frame_worker_thread(
  OwnerCallReport report, OwnerCallClock * clock, const std::atomic<bool> * abandoned,
  double report_limit_ms = kOwnerCallLimitMs);

/// @brief True on a thread whose owner was left behind (see the three-argument mark_owner_thread),
/// and on a frame worker whose sender was left behind (see mark_frame_worker_thread).
bool owner_calls_blocked();

/// @brief True on a frame worker thread (mark_frame_worker_thread) that was left behind.
bool frame_worker_calls_blocked();

/// @brief The status code of the result that QAR_CALL returns instead of a call that is blocked:
/// the value of QAR_STATUS_TIMEOUT. It is not an answer of the Hub: it exists only on a thread
/// that was left behind, whose results nobody acts on. The owner's rejoin logic runs on the owner
/// thread alone, and that thread checks its abandoned flag before it uses a result; a frame worker
/// never takes part in it, and the worker jobs record nothing from such a result (the sender's
/// skip detail and log lines ignore it).
constexpr int kBlockedCallStatus = 6;

namespace guard_detail
{

template <class T, class = void>
struct HasStatusCode : std::false_type
{
};

template <class T>
struct HasStatusCode<T, std::void_t<decltype(std::declval<T &>().code)>> : std::true_type
{
};

/// What a blocked call returns: nothing for void, a result with an error code for a result
/// struct, else a zero value.
template <class T>
T blocked_value()
{
  if constexpr (std::is_void_v<T>) {
    return;
  } else {
    T value{};
    if constexpr (HasStatusCode<T>::value) {
      value.code = static_cast<decltype(value.code)>(kBlockedCallStatus);
    }
    return value;
  }
}

}  // namespace guard_detail

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
  OwnerCallClock * clock_;
  const char * outer_call_;
  std::int64_t outer_since_ns_;
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
/// timer lives to the end of the full expression that holds it. On the thread of an owner that was
/// left behind the call is not made (see owner_calls_blocked()).
#define QAR_CALL(fn, ...) \
  (::qar_qar_adapter::owner_calls_blocked() ? \
  ::qar_qar_adapter::guard_detail::blocked_value<decltype(fn(__VA_ARGS__))>() : \
  (::qar_qar_adapter::GuiCallTimer(#fn), fn(__VA_ARGS__)))

#endif  // QAR_QAR_ADAPTER__GUI_CALL_GUARD_HPP_
