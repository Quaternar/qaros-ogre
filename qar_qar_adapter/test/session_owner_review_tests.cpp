// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// An owner thread that was left behind and the commands of a person while it is stuck. The thread
// makes no further QarOS call when its call returns, a command that it never ran stands at the
// replacement (the fresh owner follows it), a command during a stall does not wait for the
// abandon, and the left-behind session reaches nobody. Each held call stands at a gate that the
// end of the test opens, so a failed expectation cannot leave a thread held.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace qar_qar_adapter
{

// The friend of the owner that lets a test hold a post between its two steps (the same
// definition as in the other session tests).
struct QarSessionOwnerTestAccess
{
  static void set_poll_hook(QarSessionOwner & owner, std::function<void()> hook)
  {
    owner.poll_hook_ = std::move(hook);
  }

  static void set_task_post_hook(QarSessionOwner & owner, std::function<void()> hook)
  {
    owner.task_post_hook_ = std::move(hook);
  }
};

}  // namespace qar_qar_adapter

namespace
{

using qar_qar_adapter::QarSessionOwnerTestAccess;
using qar_qar_adapter::SessionEvent;
using qar_qar_adapter::SessionState;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::LeftBehindGuard;
using qar_qar_adapter::test::RecordingListener;
using qar_qar_adapter::test::allow_abandons;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::make_peer;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

void wait_left_behind_end()
{
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (qar_qar_adapter::abandoned_owner_threads_running() > 0 &&
    std::chrono::steady_clock::now() < end)
  {
    std::this_thread::sleep_for(2ms);
  }
}

int calls_on(Harness & h, std::thread::id id)
{
  int n = 0;
  for (const auto & call : h.fake().calls()) {
    if (call.thread == id) {
      ++n;
    }
  }
  return n;
}

void held_query(QarSession * session)
{
  std::size_t count = 0;
  (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
}

// Opens the gate and joins a thread, also when a test fails early.
struct JoinAfterOpen
{
  Gate & gate;
  std::thread thread;
  explicit JoinAfterOpen(Gate & g) : gate(g) {}
  ~JoinAfterOpen()
  {
    gate.open();
    if (thread.joinable()) {
      thread.join();
    }
  }
};

long long ms_since(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();
}

// A task with two calls: the first is held until the owner thread was left behind.
TEST(SessionOwnerReview, AnAbandonedThreadMakesNoFurtherCallInsideTheTaskItWasStuckIn)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  std::atomic<bool> second_done{false};
  std::thread::id owner_id;
  ASSERT_TRUE(h.owner().post_session_task("two-calls", [&](QarSession * session) {
        owner_id = std::this_thread::get_id();
        std::size_t count = 0;
        (void)QAR_CALL(qar_query_app_volumes_count, session, &count);   // held
        (void)QAR_CALL(qar_query_app_volumes_count, session, &count);   // after the abandon
        second_done = true;
      }));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  const int before = calls_on(h, owner_id);
  gate.open();
  wait_left_behind_end();
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), 0)
    << "the left-behind thread ended";
  EXPECT_EQ(calls_on(h, owner_id), before) << "the left-behind thread made a further QarOS call";
  EXPECT_TRUE(second_done.load()) << "the task ran to its end on blocked calls";
}

// A first join whose qar_runtime_create never returns: the thread is left behind. When the held
// call returns, the old thread must not onboard.
TEST(SessionOwnerReview, AnAbandonedThreadInsideRuntimeCreateDoesNotOnboardAfterItReturns)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  hold_first_call(h.fake(), "runtime_create", gate);
  h.owner().connect();
  ASSERT_TRUE(gate.wait_entered());
  std::thread::id old_id;
  for (const auto & call : h.fake().calls()) {
    if (call.name == "runtime_create") {
      old_id = call.thread;
    }
  }
  ASSERT_TRUE(h.wait_for([] {
        return qar_qar_adapter::abandoned_owner_threads_running() > 0;
      }, 5s));
  const int before = calls_on(h, old_id);
  gate.open();
  wait_left_behind_end();
  int onboards = 0;
  for (const auto & call : h.fake().calls()) {
    if (call.thread == old_id && call.name == "runtime_onboard_async") {
      ++onboards;
    }
  }
  EXPECT_EQ(onboards, 0) << "the left-behind thread onboarded after its call returned";
  EXPECT_EQ(calls_on(h, old_id), before) << "the left-behind thread made a further QarOS call";
}

// A disconnect posted while the owner thread is stuck in a call of a task is not undone by the
// replacement.
TEST(SessionOwnerReview, ADisconnectPostedToAStuckOwnerIsNotUndoneByTheReplacement)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("held", &held_query));
  ASSERT_TRUE(gate.wait_entered());
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_for([] {
        return qar_qar_adapter::abandoned_owner_threads_running() > 0;
      }, 5s));
  h.wait_for([] { return false; }, 300ms);
  EXPECT_EQ(h.owner().state(), SessionState::kIdle)
    << "the fresh owner follows the disconnect, it does not rejoin by itself";
  EXPECT_FALSE(h.owner().ready());
}

// "Join again" while the owner thread is stuck: the thread is left behind at once, not after the
// rest of abandon_after.
TEST(SessionOwnerReview, JoinAgainWhileTheOwnerIsStuckAbandonsAtOnce)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("held", &held_query));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 2s));
  const auto asked = std::chrono::steady_clock::now();
  h.owner().declare_lost("the person asked to join again");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  EXPECT_LT(ms_since(asked), 300)
    << "the person's Join again waited for the abandon (abandon_after 600 ms)";
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), 1);
}

// A first connect whose call never returns has no session to rejoin: the fresh owner is failed
// with a plain cause, and the person can connect again.
TEST(SessionOwnerReview, AFirstConnectStuckInTheRuntimeCreateEndsFailedNotLost)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  hold_first_call(h.fake(), "runtime_create", gate);
  h.owner().connect();
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kFailed, 5s));
  EXPECT_FALSE(h.state_event(SessionState::kLost) != nullptr)
    << "a join that never succeeded does not turn into an automatic rejoin";
  const SessionEvent * failed = h.last_state_event();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->state, SessionState::kFailed);
  EXPECT_NE(failed->status.message.find("did not answer while joining"), std::string::npos)
    << failed->status.message;
  EXPECT_NE(failed->status.message.find("Connect again"), std::string::npos);
  h.owner().connect();
  ASSERT_TRUE(h.wait_for([&h] {
        return h.owner().state() == SessionState::kRunning && h.owner().ready();
      }, 10s)) << "the person can connect again";
}

// A cancel while the join is stuck: the thread is left behind at once and the fresh owner is idle.
TEST(SessionOwnerReview, ACancelWhileAFirstConnectIsStuckAbandonsAtOnce)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  hold_first_call(h.fake(), "runtime_create", gate);
  h.owner().connect();
  ASSERT_TRUE(gate.wait_entered());
  h.wait_for([] { return false; }, 220ms);   // the call has run longer than call_stall (150 ms)
  const auto asked = std::chrono::steady_clock::now();
  h.owner().cancel();
  ASSERT_TRUE(h.wait_for([] {
        return qar_qar_adapter::abandoned_owner_threads_running() > 0;
      }, 5s));
  EXPECT_LT(ms_since(asked), 300) << "the cancel waited for the abandon (abandon_after 600 ms)";
  EXPECT_EQ(h.owner().state(), SessionState::kIdle);
}

// A disconnect while a call is stalled does not wait for abandon_after either.
TEST(SessionOwnerReview, ADisconnectDuringAStallAbandonsAtOnce)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("held", &held_query));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 2s));
  const auto asked = std::chrono::steady_clock::now();
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_for([] {
        return qar_qar_adapter::abandoned_owner_threads_running() > 0;
      }, 5s));
  EXPECT_LT(ms_since(asked), 300) << "disconnect waited for the abandon (abandon_after 600 ms)";
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s));
}

// The consumer's listener is cleared when the owner is left behind: peer updates of the old
// session reach nobody.
TEST(SessionOwnerReview, TheListenerOfAnAbandonedOwnerHearsNothing)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  auto listener = std::make_shared<RecordingListener>();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());
  h.fake().deliver_peer(make_peer(4, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  ASSERT_EQ(listener->ups(), 1u) << "the listener hears the target of the QarOS session";

  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("held", &held_query));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  h.fake().deliver_peer(make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_EQ(listener->ups(), 1u) << "a late peer update of the left-behind session was delivered";
  h.owner().set_target_listener(std::weak_ptr<qar_qar_adapter::ITargetListener>());
}

// A task whose post is held between its checks and its queueing while the session is lost never
// runs; its dropped function runs, and a later post is refused.
TEST(SessionOwnerReview, ATaskPostedAroundALossNeverRuns)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarSessionOwnerTestAccess::set_task_post_hook(h.owner(), [&gate] { gate.wait(); });
  std::atomic<bool> ran{false};
  std::atomic<bool> dropped{false};
  JoinAfterOpen poster(gate);
  poster.thread = std::thread([&] {
        h.owner().post_session_task("late", [&ran](QarSession *) { ran = true; },
        [&dropped] { dropped = true; });
      });
  ASSERT_TRUE(gate.wait_entered());
  h.owner().declare_lost("lost around a post");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  gate.open();
  poster.thread.join();
  QarSessionOwnerTestAccess::set_task_post_hook(h.owner(), nullptr);
  ASSERT_TRUE(h.wait_for([&dropped] { return dropped.load(); }, 5s))
    << "the task posted around the loss is dropped";
  EXPECT_FALSE(ran.load());
  EXPECT_FALSE(h.owner().post_session_task("after", [](QarSession *) {}))
    << "no task is accepted in the lost state";
}

// QAR_CALL on a thread whose owner was left behind makes no call and returns an error.
QarResult g_probe_fn(int value)
{
  QarResult result{};
  result.code = QAR_STATUS_SUCCESS;
  result.error_handle = static_cast<std::uint32_t>(value);
  return result;
}

int g_probe_void_calls = 0;
void g_probe_void() { ++g_probe_void_calls; }
int g_probe_int() { return 7; }

TEST(SessionOwnerReview, ACallOnAnAbandonedThreadIsNotMadeAndReturnsAnError)
{
  std::atomic<bool> abandoned{false};
  QarResult first{};
  QarResult second{};
  int plain_after = -1;
  std::thread thread([&] {
        qar_qar_adapter::mark_owner_thread({}, nullptr, &abandoned);
        first = QAR_CALL(g_probe_fn, 5);
        abandoned = true;
        second = QAR_CALL(g_probe_fn, 6);
        QAR_CALL(g_probe_void);
        plain_after = QAR_CALL(g_probe_int);
        qar_qar_adapter::mark_owner_thread({}, nullptr, nullptr);
      });
  thread.join();
  EXPECT_EQ(first.code, QAR_STATUS_SUCCESS);
  EXPECT_EQ(first.error_handle, 5u);
  EXPECT_EQ(second.code, QAR_STATUS_TIMEOUT) << "the result of a call that is not made";
  EXPECT_EQ(second.error_handle, 0u);
  EXPECT_EQ(g_probe_void_calls, 0);
  EXPECT_EQ(plain_after, 0);
  EXPECT_FALSE(qar_qar_adapter::owner_calls_blocked()) << "other threads are not affected";
  EXPECT_EQ(QAR_CALL(g_probe_int), 7);
}

// The clock the supervisor and the command path read to age a call. A reader that sees a call
// must read the start of a call that has really run that long: a start of 0 (a call whose end
// is half written) would age it by the whole uptime and get a healthy owner left behind.
TEST(SessionOwnerReview, TheCallClockNeverShowsACallOlderThanItIs)
{
  qar_qar_adapter::OwnerCallClock clock;
  std::atomic<bool> stop{false};
  std::thread owner([&] {
        qar_qar_adapter::mark_owner_thread({}, &clock, nullptr);
        while (!stop.load()) {
          (void)QAR_CALL(g_probe_int);
        }
        qar_qar_adapter::mark_owner_thread({}, nullptr, nullptr);
      });
  const auto test_start = std::chrono::steady_clock::now();
  const auto end = test_start + 1s;
  long long calls_seen = 0;
  long long torn = 0;
  long long worst_ms = 0;
  while (std::chrono::steady_clock::now() < end) {
    // The two loads, in the order of the supervisor and of the command path.
    const char * call = clock.call.load();
    if (call == nullptr) {
      continue;
    }
    const std::int64_t since_ns = clock.since_ns.load();
    ++calls_seen;
    const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    const long long age_ms = (now_ns - since_ns) / 1000000;
    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - test_start).count();
    if (since_ns <= 0 || age_ms > elapsed_ms + 5) {
      ++torn;
      worst_ms = std::max(worst_ms, age_ms);
    }
  }
  stop = true;
  owner.join();
  EXPECT_GT(calls_seen, 0) << "the reader saw no call at all";
  EXPECT_EQ(torn, 0) << "a call was seen with a start that is not its own (age " << worst_ms
                     << " ms)";
}

// An owner thread is asked to be left behind in a stalled call, its call returns and it runs the
// queued disconnect to the end before the consumer's next poll() replaces it. It then waits for
// a message with no deadline: the replacement wakes it, and it ends.
TEST(SessionOwnerReview, AnOwnerLeftBehindWhileIdleEnds)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("held", [](QarSession * session) {
        std::size_t count = 0;
        (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
      }));
  ASSERT_TRUE(gate.wait_entered());
  std::this_thread::sleep_for(250ms);   // longer than call_stall (150 ms)
  const int running_before = qar_qar_adapter::abandoned_owner_threads_running();
  h.owner().disconnect();               // a stalled call: the abandon is requested at once
  gate.open();                          // ... and the call returns before the next poll()
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (h.owner().state() != SessionState::kIdle && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);   // no poll(): the old owner runs the disconnect
  }
  ASSERT_EQ(h.owner().state(), SessionState::kIdle) << "the old owner ran the disconnect";
  std::this_thread::sleep_for(100ms);   // it waits for a message now
  std::vector<SessionEvent> events;
  h.owner().poll(events);               // the replacement leaves the idle thread behind
  const auto ended_by = std::chrono::steady_clock::now() + 3s;
  while (qar_qar_adapter::abandoned_owner_threads_running() > running_before &&
    std::chrono::steady_clock::now() < ended_by)
  {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), running_before)
    << "the idle thread that was left behind never ended";
}

}  // namespace
