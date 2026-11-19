// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// A QarOS call of the owner thread that never returns while the program runs: the supervisor asks
// for the thread to be left behind, the next poll() replaces the owner, and the fresh owner joins
// the Hub again on its own thread. The call that never returns is a call of the fake library held
// at a gate; the gate opens at the end of each test, and the test waits for the thread to end
// before the fake library is reset.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using qar_qar_adapter::SessionEvent;
using qar_qar_adapter::SessionState;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::HintBox;
using qar_qar_adapter::test::LeftBehindGuard;
using qar_qar_adapter::test::RecordingListener;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::make_peer;
using qar_qar_adapter::test::allow_abandons;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

void query_once(QarSession * session)
{
  std::size_t count = 0;
  (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
}

bool running_and_ready(Harness & h)
{
  return h.owner().state() == SessionState::kRunning && h.owner().ready();
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

TEST(SessionOwnerAbandon, AStuckCallIsAbandonedAndAFreshOwnerRejoins)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  const int before = qar_qar_adapter::abandoned_owner_threads();
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(gate.wait_entered());

  // After abandon_after the supervisor asks, and the next poll() replaces the owner.
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 1);
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), 1);
  EXPECT_EQ(h.owner().health().abandoned_owners, before + 1);
  const SessionEvent * lost = h.last_state_event();
  ASSERT_NE(lost, nullptr);
  EXPECT_EQ(lost->state, SessionState::kLost);
  EXPECT_NE(lost->status.message.find("QAROS stopped answering inside qar_query_app_volumes_count"),
    std::string::npos) << lost->status.message;
  EXPECT_TRUE(h.logged("QAROS: watchdog: owner thread abandoned in qar_query_app_volumes_count "
    "after "));
  EXPECT_FALSE(h.owner().ready());

  // The consumer hands the (nothing) back; the fresh owner rejoins on a fresh thread.
  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  EXPECT_TRUE(h.fake().library_loaded());
  EXPECT_EQ(h.fake().count("library_destroy"), 0) << "the library is not touched";
  std::atomic<int> ran{0};
  EXPECT_TRUE(h.owner().post_session_task("after", [&ran](QarSession *) { ran = 1; }));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }));
}

TEST(SessionOwnerAbandon, TheOldLaneRefusesAfterTheAbandon)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const auto old_lane = h.owner().release_lane();
  ASSERT_NE(old_lane, nullptr);
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));

  std::atomic<int> ran{0};
  EXPECT_FALSE(old_lane->post_release("old", [&ran] { ran = 1; }))
    << "handles of the dead session are left unreleased, with no call on them";
  const auto fresh_lane = h.owner().release_lane();
  ASSERT_NE(fresh_lane, nullptr);
  EXPECT_NE(fresh_lane.get(), old_lane.get());
  EXPECT_TRUE(fresh_lane->post_release("fresh", [&ran] { ran = 2; }));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 2; }));
  EXPECT_NE(ran.load(), 1);
}

TEST(SessionOwnerAbandon, ALateResultOfTheAbandonedOwnerNeverReachesTheFreshOne)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));

  const std::size_t calls_before = h.fake().calls().size();
  const int sessions = h.fake().live_sessions();
  gate.open();
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (qar_qar_adapter::abandoned_owner_threads_running() > 0 &&
    std::chrono::steady_clock::now() < end)
  {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), 0);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(h.fake().calls().size(), calls_before) << "the old thread makes no further call";
  EXPECT_EQ(h.fake().live_sessions(), sessions);
  EXPECT_TRUE(running_and_ready(h)) << "the fresh owner is untouched";
  h.wait_for([] { return false; }, 100ms);
  EXPECT_EQ(h.owner().state(), SessionState::kRunning);
}

TEST(SessionOwnerAbandon, TheCapEndsInFailedWithRestartRviz)
{
  Gate first_gate;
  Gate second_gate;
  Harness h;
  LeftBehindGuard guard(first_gate, second_gate);
  use_scaled_timings(h);
  const int before = qar_qar_adapter::abandoned_owner_threads();
  h.config().timings.max_abandoned_owners = before + 2;
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  hold_first_call(h.fake(), "query_app_volumes_count", first_gate);
  ASSERT_TRUE(h.owner().post_session_task("first", &query_once));
  ASSERT_TRUE(first_gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 1);

  hold_first_call(h.fake(), "query_app_volumes_count", second_gate);
  ASSERT_TRUE(h.owner().post_session_task("second", &query_once));
  ASSERT_TRUE(second_gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kFailed, 5s));
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 2);
  const SessionEvent * failed = h.last_state_event();
  ASSERT_NE(failed, nullptr);
  EXPECT_NE(failed->status.message.find("restart RViz to stream again"), std::string::npos)
    << failed->status.message;
  EXPECT_NE(failed->status.message.find("qar_query_app_volumes_count"), std::string::npos);
  EXPECT_EQ(failed->status.symbol, "OWNER_ABANDONED");
}

TEST(SessionOwnerAbandon, TheSupervisorMakesNoQarosCall)
{
  HintBox hints;
  Harness h;
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.config().watch_hub_hints = true;
  h.config().hint_source = hints.source();
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  // A task that stalls, then a loss and a rejoin: the supervisor sees all of it.
  ASSERT_TRUE(h.owner().post_session_task("slow", [](QarSession * session) {
        query_once(session);
        std::this_thread::sleep_for(450ms);
      }));
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 3s));
  ASSERT_TRUE(h.wait_state(SessionState::kRunning, 3s));
  h.owner().declare_lost("looked at by the supervisor");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s));
  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  ASSERT_TRUE(h.destroy_owner_within(10s));

  const std::thread::id owner_thread = h.owner_thread();
  int foreign = 0;
  for (const auto & call : h.fake().calls()) {
    if (call.thread != owner_thread) {
      ++foreign;
    }
  }
  EXPECT_EQ(foreign, 0) << "every QarOS call ran on the owner thread";
  EXPECT_GT(h.fake().calls().size(), 10u);
}

TEST(SessionOwnerAbandon, NoDeadlockWhenTheStuckCallIsAPeerTokenCancelWaitingForACallback)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  JoinAfterOpen deliverer(gate);
  use_scaled_timings(h);
  allow_abandons(h, 5);
  h.start();
  auto listener = std::make_shared<RecordingListener>();
  listener->set_up_hook([&gate] { gate.wait(); });
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());
  const int before = qar_qar_adapter::abandoned_owner_threads();
  // The peer callback of the library blocks inside the listener; ending the peer subscription
  // waits for that callback, so the owner thread stands in qar_cancel_token_cancel.
  deliverer.thread = std::thread([&h] {
        h.fake().deliver_peer(make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
      });
  ASSERT_TRUE(gate.wait_entered());
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_for(
      [before] { return qar_qar_adapter::abandoned_owner_threads() == before + 1; }, 5s));
  EXPECT_EQ(h.owner().state(), SessionState::kIdle)
    << "a disconnect that was under way ends idle, not lost";
  EXPECT_TRUE(h.logged("QAROS: watchdog: owner thread abandoned in qar_cancel_token_cancel "));
  h.owner().set_target_listener(std::weak_ptr<qar_qar_adapter::ITargetListener>());
}

}  // namespace
