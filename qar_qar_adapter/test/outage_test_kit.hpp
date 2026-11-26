// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// What the outage tests of the session owner share: timings scaled down to fractions of a second,
// a gate that holds a faked QarOS call, a guard that lets a left-behind owner thread end before
// the fake library goes, and a hint source a test can change.

#ifndef QAR_QAR_ADAPTER__TEST__OUTAGE_TEST_KIT_HPP_
#define QAR_QAR_ADAPTER__TEST__OUTAGE_TEST_KIT_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/owner_watch.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"
#include "session_owner_harness.hpp"

namespace qar_qar_adapter
{
namespace test
{

using namespace std::chrono_literals;

/// The limits of an outage in fractions of a second.
inline OutageTimings scaled_timings()
{
  OutageTimings t;
  t.call_stall = 150ms;
  t.task_stall = 300ms;
  t.lost_after = 400ms;
  t.lost_after_hint = 150ms;
  t.hint_absent = 100ms;
  t.peers_gone_grace = 100ms;
  t.hand_back_wait = 500ms;
  t.abandon_after = 600ms;
  t.rejoin_first = 100ms;
  t.rejoin_cap = 400ms;
  t.rejoin_spacing = 100ms;
  t.attempt_timeout = 600ms;
  t.lost_operation_wait = 200ms;
  t.exit_drain = 300ms;
  t.exit_operation_wait = 200ms;
  t.exit_join = 600ms;
  t.watch_period = 20ms;
  t.hint_period = 30ms;
  return t;
}

/// Gives the owner of @p h the scaled timings and no hints from the machine.
inline void use_scaled_timings(Harness & h)
{
  h.config().timings = scaled_timings();
  h.config().watch_hub_hints = false;
}

/// Lets the owner of @p h leave @p more threads behind, counted from the threads this process has
/// already left behind (earlier tests may have used up the default).
inline void allow_abandons(Harness & h, int more)
{
  h.config().timings.max_abandoned_owners = abandoned_owner_threads() + more;
}

/// Holds the thread that calls wait() until open() is called.
class Gate
{
public:
  void wait()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
  }

  void open()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }

  bool wait_entered(std::chrono::milliseconds timeout = 5s)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  bool entered() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool open_ = false;
};

/// Holds the first call of the fake library named @p name at @p gate.
inline void hold_first_call(FakeQarApi & fake, const std::string & name, Gate & gate)
{
  auto used = std::make_shared<std::atomic<bool>>(false);
  fake.set_call_hook([name, &gate, used](const std::string & called) {
      if (called == name && !used->exchange(true)) {
        gate.wait();
      }
    });
}

/// Declared after the harness: opens the gate and waits for every owner thread that was left
/// behind, so that the fake library is not reset under a thread that still runs.
class LeftBehindGuard
{
public:
  explicit LeftBehindGuard(Gate & gate) : gate_(gate), other_(nullptr) {}
  LeftBehindGuard(Gate & gate, Gate & other) : gate_(gate), other_(&other) {}

  ~LeftBehindGuard()
  {
    gate_.open();
    if (other_ != nullptr) {
      other_->open();
    }
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (abandoned_owner_threads_running() > 0 && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(2ms);
    }
  }

  LeftBehindGuard(const LeftBehindGuard &) = delete;
  LeftBehindGuard & operator=(const LeftBehindGuard &) = delete;

private:
  Gate & gate_;
  Gate * other_;
};

/// A hint source whose answer a test sets.
class HintBox
{
public:
  HintBox()
  {
    hints_.endpoint_known = true;
    hints_.endpoint_present = true;
    hints_.endpoint_mtime = 100;
    hints_.endpoint_written = "10:00:00";
    hints_.processes_known = true;
    hints_.hub_pid = 4242;
  }

  std::function<HubHints()> source()
  {
    return [this] {
             std::lock_guard<std::mutex> lock(mutex_);
             return hints_;
           };
  }

  void set(const HubHints & hints)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hints_ = hints;
  }

  HubHints get() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return hints_;
  }

private:
  mutable std::mutex mutex_;
  HubHints hints_;
};

/// Runs @p fn on the calling thread and tells whether it returned within @p limit.
inline bool returns_within(const std::function<void()> & fn, std::chrono::milliseconds limit = 20ms)
{
  const auto start = std::chrono::steady_clock::now();
  fn();
  return std::chrono::steady_clock::now() - start <= limit;
}

}  // namespace test
}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__TEST__OUTAGE_TEST_KIT_HPP_
