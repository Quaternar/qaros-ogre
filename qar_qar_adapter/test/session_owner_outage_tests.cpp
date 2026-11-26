// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The session owner when the Hub goes away: silence, the lost state, the release lane, and the
// automatic rejoin with its back-off. Every limit is scaled down to fractions of a second. A QarOS
// call that does not return is a call of the fake library held at a gate; no test loads the real
// library.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using qar_qar_adapter::HubSilence;
using qar_qar_adapter::QarSessionOwner;
using qar_qar_adapter::SessionEvent;
using qar_qar_adapter::SessionState;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::HintBox;
using qar_qar_adapter::test::LeftBehindGuard;
using qar_qar_adapter::test::Outcome;
using qar_qar_adapter::test::RecordingListener;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::make_peer;
using qar_qar_adapter::test::returns_within;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

void give_hub_peer(Harness & h)
{
  h.fake().set_initial_peers({make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE)});
}

void silence_hub_peer(Harness & h)
{
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_OFFLINE));
}

// The QarOS call a task makes: the fake records it and the call hook may hold it.
void query_once(QarSession * session)
{
  std::size_t count = 0;
  (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
}

Outcome fails_with(int code)
{
  Outcome outcome;
  outcome.code = code;
  return outcome;
}

bool running_and_ready(Harness & h)
{
  return h.owner().state() == SessionState::kRunning && h.owner().ready();
}

// Polls until the state is kReconnecting, then until it is kLost; the time between the two
// sightings, or -1.
long long reconnecting_to_lost_ms(Harness & h)
{
  if (!h.wait_state(SessionState::kReconnecting, 5s)) {
    return -1;
  }
  const auto start = std::chrono::steady_clock::now();
  if (!h.wait_state(SessionState::kLost, 5s)) {
    return -1;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();
}

// The "in <ms> ms" values of the lines `QAROS: rejoin <n> in <ms> ms`, in order.
std::vector<std::pair<int, int>> planned_attempts(const Harness & h)
{
  static const std::regex pattern("QAROS: rejoin (\\d+) in (\\d+) ms");
  std::vector<std::pair<int, int>> out;
  for (const std::string & line : h.log_lines()) {
    std::smatch match;
    if (std::regex_match(line, match, pattern)) {
      out.emplace_back(std::atoi(match[1].str().c_str()), std::atoi(match[2].str().c_str()));
    }
  }
  return out;
}

std::string dump_log(const Harness & h)
{
  std::string out;
  for (const std::string & line : h.log_lines()) {
    out += line;
    out.push_back('\n');
  }
  return out;
}

// declare_lost() and the hand-back: the owner is lost and starts its automatic rejoin.
void lose_and_hand_back(Harness & h, const std::string & reason = "test")
{
  h.owner().declare_lost(reason);
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  h.owner().release_lost_session();
}

// ---------------------------------------------------------------------------------------------
// Silence and lost
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerOutage, AHubWhosePeersStayGoneEndsTheSessionAsLost)
{
  Harness h;
  use_scaled_timings(h);
  give_hub_peer(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  silence_hub_peer(h);

  const long long waited_ms = reconnecting_to_lost_ms(h);
  ASSERT_GE(waited_ms, 0);
  EXPECT_NE(h.state_event(SessionState::kReconnecting), nullptr);
  const SessionEvent * lost = h.state_event(SessionState::kLost);
  ASSERT_NE(lost, nullptr);
  // The owner measures the silence itself ("... for <x> s."); two sightings by the polling
  // thread can be far apart or too close on a loaded machine.
  static const std::regex silent_for(" for ([0-9.]+) s\\.");
  std::smatch silent_match;
  ASSERT_TRUE(std::regex_search(lost->status.message, silent_match, silent_for))
    << lost->status.message;
  EXPECT_GE(std::atof(silent_match[1].str().c_str()), 0.4)
    << "lost_after is 400 ms from the start of the silence: " << lost->status.message;
  EXPECT_EQ(lost->status.code, qar_qar_adapter::kStatusSessionLost);
  EXPECT_EQ(lost->status.symbol, "LOST");
  EXPECT_NE(lost->status.message.find("its peers are gone"), std::string::npos)
    << lost->status.message;

  EXPECT_FALSE(h.owner().ready()) << "a lost session is not the consumer's any more";
  EXPECT_EQ(h.owner().session(), nullptr);
  EXPECT_FALSE(h.owner().post_session_task("late", [](QarSession *) {}));
  const qar_qar_adapter::RejoinInfo info = h.owner().rejoin_info();
  EXPECT_TRUE(info.automatic);
  EXPECT_FALSE(info.lost_cause.empty());

  EXPECT_TRUE(h.logged("QAROS: hub silent (hub-peers-gone) at "));
  EXPECT_TRUE(h.logged("QAROS: state reconnecting"));
  EXPECT_TRUE(h.logged("QAROS: state lost"));
  EXPECT_TRUE(h.logged("QAROS: status LOST: The QAROS Hub stopped answering (its peers are gone)"));
}

TEST(SessionOwnerOutage, ACallThatStallsMarksTheHubSilentAndClearsWhenItReturns)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("stalls", &query_once));
  ASSERT_TRUE(gate.wait_entered());

  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 3s)) << "the call runs 150 ms: silent";
  const qar_qar_adapter::OwnerHealth health = h.owner().health();
  EXPECT_NE(health.silence & qar_qar_adapter::kSilenceCallStalled, 0u);
  EXPECT_EQ(health.stalled_in, "qar_query_app_volumes_count");
  EXPECT_GE(health.silent_for_ms, 0);
  EXPECT_TRUE(h.owner().ready()) << "the session is kept";

  gate.open();
  ASSERT_TRUE(h.wait_state(SessionState::kRunning, 3s)) << dump_log(h);
  EXPECT_EQ(h.owner().health().silence & qar_qar_adapter::kSilenceCallStalled, 0u);
  EXPECT_EQ(h.state_event(SessionState::kLost), nullptr) << "it returned before lost_after";
  EXPECT_TRUE(h.logged("QAROS: hub silent (call-stalled qar_query_app_volumes_count "));
  EXPECT_TRUE(h.logged("QAROS: hub answers again after "));
  EXPECT_TRUE(h.logged("QAROS: watchdog: call qar_query_app_volumes_count running for "));
}

TEST(SessionOwnerOutage, ATaskThatStallsMarksTheHubSilent)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_TRUE(h.owner().post_session_task("long-task", [&gate](QarSession *) { gate.wait(); }));
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 3s)) << "a task over 300 ms: silent";
  EXPECT_EQ(h.owner().health().stalled_in, "task long-task");
  EXPECT_TRUE(h.logged("QAROS: hub silent (task-stalled long-task "));
  gate.open();
  ASSERT_TRUE(h.wait_state(SessionState::kRunning, 3s)) << dump_log(h);
}

TEST(SessionOwnerOutage, LostWaitsForTheHandBackThenReleasesInTheDesignOrder)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.hand_back_wait = 5s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().declare_lost("order");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(h.fake().count("session_handle_destroy"), 0)
    << "the session stays until the consumer handed it back";

  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().count("session_handle_destroy") == 1; }));
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  const int cancel = h.fake().first_index("cancel_token_cancel");
  const int destroy = h.fake().first_index("session_handle_destroy");
  const int second_onboard = h.fake().last_index("runtime_onboard_async");
  EXPECT_GE(cancel, 0);
  EXPECT_LT(cancel, destroy) << "the peer subscription ends before the session is destroyed";
  EXPECT_LT(destroy, second_onboard) << "the rejoin starts after the release";
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), 2);
  EXPECT_TRUE(h.logged("QAROS: lost: the consumer handed the session back in "));
}

TEST(SessionOwnerOutage, LostReleasesAnywayWhenNoHandBackComes)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().declare_lost("nobody answers");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().count("session_handle_destroy") == 1; }, 3s));
  EXPECT_TRUE(h.logged("QAROS: lost: no hand-back within 0.5 s; releasing anyway"));
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s)) << "and it rejoins";
}

TEST(SessionOwnerOutage, TasksOfTheLostSessionAreDroppedAndReleasesStillRun)
{
  Harness h;
  use_scaled_timings(h);
  give_hub_peer(h);
  h.config().timings.hand_back_wait = 5s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran_second{0};
  std::atomic<int> dropped_second{0};
  ASSERT_TRUE(h.owner().post_session_task("first", [](QarSession *) {
        std::this_thread::sleep_for(900ms);   // the Hub is silent for longer than lost_after
      }));
  ASSERT_TRUE(h.owner().post_session_task("second", [&ran_second](QarSession *) { ran_second = 1; },
      [&dropped_second] { dropped_second = 1; }));
  silence_hub_peer(h);
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  ASSERT_TRUE(h.wait_for([&dropped_second] { return dropped_second.load() == 1; }))
    << "its dropped function runs instead";
  EXPECT_EQ(ran_second.load(), 0) << "the task of the lost session never runs";

  std::atomic<int> released_on_owner{0};
  const std::thread::id owner_thread = h.owner_thread();
  const auto lane = h.owner().release_lane();
  ASSERT_NE(lane, nullptr);
  EXPECT_TRUE(lane->post_release("while-lost", [&] {
        released_on_owner = (std::this_thread::get_id() == owner_thread) ? 1 : 2;
      }));
  ASSERT_TRUE(h.wait_for([&released_on_owner] { return released_on_owner.load() != 0; }));
  EXPECT_EQ(released_on_owner.load(), 1) << "the release ran on the owner thread, in kLost";
}

// ---------------------------------------------------------------------------------------------
// The automatic rejoin
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerOutage, ARejoinedSessionIsHandedOverLikeAConnect)
{
  Harness h;
  use_scaled_timings(h);
  h.fake().set_initial_peers({make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  auto listener = std::make_shared<RecordingListener>();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_TRUE(h.wait_for([&listener] { return listener->ups() == 1; }));
  const int sessions_before = h.fake().live_sessions();
  EXPECT_EQ(sessions_before, 1);

  lose_and_hand_back(h, "the picture does not come back");
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  EXPECT_NE(h.owner().session(), nullptr);
  EXPECT_EQ(h.fake().live_sessions(), 1) << "the lost session was destroyed, one new one is open";
  EXPECT_TRUE(h.wait_for([&listener] { return listener->ups() == 2; }))
    << "the target is announced again like after a connect";
  std::atomic<int> ran{0};
  EXPECT_TRUE(h.owner().post_session_task("after", [&ran](QarSession *) { ran = 1; }));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }));

  std::vector<SessionState> states;
  for (const SessionEvent & event : h.events()) {
    if (event.kind == SessionEvent::Kind::kState) {
      states.push_back(event.state);
    }
  }
  const std::vector<SessionState> expected{
    SessionState::kLoading, SessionState::kWaitingForApproval, SessionState::kRunning,
    SessionState::kLost, SessionState::kWaitingForApproval, SessionState::kRunning};
  EXPECT_EQ(states, expected) << "no kLoading state in the automatic attempt";
  const qar_qar_adapter::RejoinInfo info = h.owner().rejoin_info();
  EXPECT_FALSE(info.automatic);
  EXPECT_EQ(info.attempt, 1);
  EXPECT_TRUE(h.logged("QAROS: rejoined after 1 attempt(s), "));
  EXPECT_TRUE(h.logged("QAROS: rejoin 1 started (local)"));
  h.owner().set_target_listener(std::weak_ptr<qar_qar_adapter::ITargetListener>());
}

TEST(SessionOwnerOutage, RejoinBacksOffTwoFourEightAndCaps)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  for (int i = 0; i < 4; ++i) {
    h.fake().script_onboard(fails_with(1802));
  }
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 15s));
  const std::vector<std::pair<int, int>> plan = planned_attempts(h);
  ASSERT_EQ(plan.size(), 5u);
  EXPECT_EQ(plan[0], std::make_pair(1, 100));
  EXPECT_EQ(plan[1], std::make_pair(2, 200));
  EXPECT_EQ(plan[2], std::make_pair(3, 400));
  EXPECT_EQ(plan[3], std::make_pair(4, 400)) << "capped at rejoin_cap";
  EXPECT_EQ(plan[4], std::make_pair(5, 400));
  EXPECT_TRUE(h.logged("QAROS: rejoin 1 failed: QAR_STATUS_ONBOARDING_HUB_UNREACHABLE after "));
  EXPECT_TRUE(h.logged("QAROS: rejoined after 5 attempt(s), "));
}

TEST(SessionOwnerOutage, NoTwoAttemptsStartWithinTheSpacing)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.rejoin_first = 10ms;
  h.config().timings.rejoin_spacing = 250ms;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().script_onboard(fails_with(1802));
  h.fake().script_onboard(fails_with(1802));
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  const std::vector<std::pair<int, int>> plan = planned_attempts(h);
  ASSERT_EQ(plan.size(), 3u);
  EXPECT_LE(plan[0].second, 10);
  EXPECT_GE(plan[1].second, 150) << "the spacing, not the 20 ms back-off, decides";
  EXPECT_LE(plan[1].second, 250);
  EXPECT_GE(plan[2].second, 150);
}

TEST(SessionOwnerOutage, RejoinNowSkipsTheBackOffButKeepsTheSpacing)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.rejoin_first = 5s;
  h.config().timings.rejoin_cap = 10s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("QAROS: rejoin 1 in 5000 ms"); }));
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), 1) << "the back-off holds the attempt";
  EXPECT_EQ(h.owner().state(), SessionState::kLost);
  const qar_qar_adapter::RejoinInfo waiting = h.owner().rejoin_info();
  EXPECT_GT(waiting.next_in_ms, 1000);
  h.owner().rejoin_now();
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 3s));
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), 2);
}

TEST(SessionOwnerOutage, AnUnreachableHubKeepsTheOwnerInLost)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  for (int i = 0; i < 12; ++i) {
    h.fake().script_onboard(fails_with(1802));
  }
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().rejoin_info().attempt >= 4; }, 10s));
  EXPECT_EQ(h.state_event(SessionState::kFailed), nullptr);
  EXPECT_TRUE(h.owner().rejoin_info().automatic);
  const SessionState now_state = h.owner().state();
  EXPECT_TRUE(now_state == SessionState::kLost || now_state == SessionState::kWaitingForApproval)
    << static_cast<int>(now_state);
}

TEST(SessionOwnerOutage, ADeniedRejoinStopsTheAutomaticRejoin)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().script_onboard(fails_with(QAR_STATUS_ONBOARDING_APPROVAL_DENIED));
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_state(SessionState::kFailed, 10s)) << dump_log(h);
  const SessionEvent * failed = h.last_state_event();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status.symbol, "QAR_STATUS_ONBOARDING_APPROVAL_DENIED");
  EXPECT_NE(failed->status.message.find("denied"), std::string::npos);
  EXPECT_FALSE(h.owner().rejoin_info().automatic);
  EXPECT_TRUE(h.logged("QAROS: rejoin stopped (QAR_STATUS_ONBOARDING_APPROVAL_DENIED)"));
  const int calls = h.fake().count("runtime_onboard_async");
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), calls) << "no further attempt";
}

TEST(SessionOwnerOutage, ATimedOutApprovalStopsTheAutomaticRejoin)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().script_onboard(fails_with(QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT));
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_state(SessionState::kFailed, 10s)) << dump_log(h);
  EXPECT_EQ(h.last_state_event()->status.symbol, "QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT");
  EXPECT_FALSE(h.owner().rejoin_info().automatic);
}

TEST(SessionOwnerOutage, AnAttemptWithoutAnswerIsCancelledAfterItsTimeout)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  Outcome never;
  never.wait_for_release = true;
  h.fake().script_onboard(never);
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("QAROS: rejoin 1 timed out after "); }, 5s));
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s)) << "the next attempt joins";
  EXPECT_EQ(h.state_event(SessionState::kFailed), nullptr);
  EXPECT_TRUE(h.logged("QAROS: rejoined after 2 attempt(s), "));
}

TEST(SessionOwnerOutage, TheThirdAttemptMakesAFreshRuntime)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().script_onboard(fails_with(1802));
  h.fake().script_onboard(fails_with(1802));
  EXPECT_EQ(h.fake().count("runtime_create"), 1);
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  EXPECT_EQ(h.fake().count("runtime_create"), 2) << "attempts 1 and 2 reuse the runtime";
  EXPECT_EQ(h.fake().count("runtime_handle_destroy"), 1);
  EXPECT_TRUE(h.logged("QAROS: rejoin 3: fresh runtime"));
  EXPECT_LT(h.fake().first_index("runtime_handle_destroy"), h.fake().last_index("runtime_create"));
}

TEST(SessionOwnerOutage, CancelInLostEndsIdleAndStopsTheTimers)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  for (int i = 0; i < 20; ++i) {
    h.fake().script_onboard(fails_with(1802));
  }
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().rejoin_info().attempt >= 2; }, 10s));
  h.owner().cancel();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s)) << dump_log(h);
  EXPECT_FALSE(h.owner().rejoin_info().automatic);
  const int calls = h.fake().count("runtime_onboard_async");
  std::this_thread::sleep_for(700ms);
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), calls) << "the timers are off";
  EXPECT_EQ(h.owner().state(), SessionState::kIdle);
  EXPECT_TRUE(h.logged("QAROS: rejoin stopped (cancelled)"));
}

TEST(SessionOwnerOutage, CancelBeforeTheHandBackReleasesTheSessionAndEndsIdle)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.hand_back_wait = 10s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().declare_lost("stop it");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  h.owner().cancel();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s)) << dump_log(h);
  EXPECT_EQ(h.fake().count("session_handle_destroy"), 1);
  EXPECT_EQ(h.fake().live_sessions(), 0);
}

TEST(SessionOwnerOutage, DisconnectInLostEndsIdle)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  for (int i = 0; i < 20; ++i) {
    h.fake().script_onboard(fails_with(1802));
  }
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().rejoin_info().attempt >= 1; }, 5s));
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s)) << dump_log(h);
  EXPECT_FALSE(h.owner().rejoin_info().automatic);
  const int calls = h.fake().count("runtime_onboard_async");
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), calls);
}

TEST(SessionOwnerOutage, DeclareLostFromRunningReleasesAndRejoins)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarSession * const first = h.owner().session();
  ASSERT_NE(first, nullptr);
  lose_and_hand_back(h, "no stream came back after the Hub returned");
  const SessionEvent * lost = h.state_event(SessionState::kLost);
  ASSERT_NE(lost, nullptr);
  EXPECT_EQ(lost->status.message, "no stream came back after the Hub returned");
  EXPECT_TRUE(h.logged("QAROS: status LOST: no stream came back after the Hub returned"));
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 10s));
  EXPECT_EQ(h.fake().live_sessions(), 1);
  EXPECT_EQ(h.fake().count("session_handle_destroy"), 1);

  // A declaration in any other state is ignored.
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s)) << dump_log(h);
  h.owner().declare_lost("too late");
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(h.owner().state(), SessionState::kIdle);
  EXPECT_TRUE(h.logged("QAROS: declare_lost ignored (state idle)"));
}

// ---------------------------------------------------------------------------------------------
// The hints
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerOutage, AHintThatTheHubIsGoneShortensTheWaitForLost)
{
  HintBox hints;
  Harness h;
  use_scaled_timings(h);
  h.config().watch_hub_hints = true;
  h.config().hint_source = hints.source();
  give_hub_peer(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_TRUE(h.wait_for([&h] {
        return h.logged("QAROS: process qaros-hub-service running (pid 4242)");
      })) << dump_log(h);
  qar_qar_adapter::HubHints gone;
  gone.endpoint_known = true;
  gone.endpoint_present = false;
  gone.processes_known = true;
  gone.hub_pid = 0;
  hints.set(gone);
  ASSERT_TRUE(h.wait_for([&h] {
        return h.logged("QAROS: hint: hub endpoint file missing") &&
        h.logged("QAROS: process qaros-hub-service gone (pid 4242)");
      })) << dump_log(h);
  std::this_thread::sleep_for(300ms);   // longer than hint_absent: the hints count
  silence_hub_peer(h);
  const long long waited_ms = reconnecting_to_lost_ms(h);
  ASSERT_GE(waited_ms, 0);
  EXPECT_LT(waited_ms, 330) << "lost_after_hint is 150 ms, lost_after is 400 ms";
  EXPECT_NE(h.owner().health().silence, 0u);
  EXPECT_TRUE(h.logged("QAROS: hint: watching 4 process names every "));
}

TEST(SessionOwnerOutage, TheHubProcessIsCountedWhenOneEndsOrIsReplaced)
{
  HintBox hints;
  Harness h;
  use_scaled_timings(h);
  h.config().watch_hub_hints = true;
  h.config().hint_source = hints.source();
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().hub_process().pid == 4242; })) << dump_log(h);
  qar_qar_adapter::HubProcessInfo info = h.owner().hub_process();
  EXPECT_TRUE(info.known);
  EXPECT_EQ(info.gone_count, 0u);
  EXPECT_EQ(info.last_gone_pid, 0u);

  // The Hub process ends.
  qar_qar_adapter::HubHints hub = hints.get();
  hub.hub_pid = 0;
  hints.set(hub);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().hub_process().gone_count == 1; })) << dump_log(h);
  info = h.owner().hub_process();
  EXPECT_EQ(info.pid, 0u);
  EXPECT_EQ(info.last_gone_pid, 4242u);

  // A new one runs: nothing ended.
  hub.hub_pid = 5151;
  hints.set(hub);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().hub_process().pid == 5151; })) << dump_log(h);
  EXPECT_EQ(h.owner().hub_process().gone_count, 1u);

  // A new one that took the place of the old between two snapshots counts as an end.
  hub.hub_pid = 6161;
  hints.set(hub);
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().hub_process().gone_count == 2; })) << dump_log(h);
  info = h.owner().hub_process();
  EXPECT_EQ(info.pid, 6161u);
  EXPECT_EQ(info.last_gone_pid, 5151u);
}

TEST(SessionOwnerOutage, AHintThatTheHubIsBackStartsTheAttemptAtOnce)
{
  HintBox hints;
  Harness h;
  use_scaled_timings(h);
  h.config().watch_hub_hints = true;
  h.config().hint_source = hints.source();
  h.config().timings.rejoin_first = 5s;
  h.config().timings.rejoin_cap = 10s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  lose_and_hand_back(h);
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("QAROS: rejoin 1 in 5000 ms"); }));
  EXPECT_EQ(h.owner().state(), SessionState::kLost);
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("QAROS: hint: watching"); }))
    << "the first reading comes before the change" << dump_log(h);
  qar_qar_adapter::HubHints back = hints.get();
  back.endpoint_mtime += 50;   // the Hub rewrote its endpoint file
  hints.set(back);
  ASSERT_TRUE(h.wait_for([&h] { return running_and_ready(h); }, 3s)) << dump_log(h);
}

// ---------------------------------------------------------------------------------------------
// Nothing blocks the caller
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerOutage, EveryCommandAndQueryReturnsWithinTwentyMillisecondsWhileTheOwnerIsBlocked)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.config().timings.call_stall = 60s;      // the test is about the caller, not the supervisor
  h.config().timings.task_stall = 60s;
  h.config().timings.abandon_after = 60s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("blocked", &query_once));
  ASSERT_TRUE(gate.wait_entered());

  QarSessionOwner & owner = h.owner();
  std::vector<SessionEvent> events;
  const auto lane = owner.release_lane();
  ASSERT_NE(lane, nullptr);
  EXPECT_TRUE(returns_within([&] { (void)owner.state(); })) << "state";
  EXPECT_TRUE(returns_within([&] { (void)owner.targets(); })) << "targets";
  EXPECT_TRUE(returns_within([&] { (void)owner.task_stats(); })) << "task_stats";
  EXPECT_TRUE(returns_within([&] { (void)owner.health(); })) << "health";
  EXPECT_TRUE(returns_within([&] { (void)owner.rejoin_info(); })) << "rejoin_info";
  EXPECT_TRUE(returns_within([&] { (void)owner.ready(); })) << "ready";
  EXPECT_TRUE(returns_within([&] { owner.poll(events); })) << "poll";
  EXPECT_TRUE(returns_within([&] {
        (void)owner.post_session_task("queued", [](QarSession *) {});
      })) << "post_session_task";
  EXPECT_TRUE(returns_within([&] { (void)lane->post_release("queued", [] {}); }))
    << "post_release";
  EXPECT_TRUE(returns_within([&] { owner.declare_lost("blocked"); })) << "declare_lost";
  EXPECT_TRUE(returns_within([&] { owner.rejoin_now(); })) << "rejoin_now";
  EXPECT_TRUE(returns_within([&] { owner.release_lost_session(); })) << "release_lost_session";
  EXPECT_TRUE(returns_within([&] { owner.cancel(); })) << "cancel";
  EXPECT_TRUE(returns_within([&] { owner.connect(); })) << "connect";
  EXPECT_TRUE(returns_within([&] {
        owner.set_target_listener(std::weak_ptr<qar_qar_adapter::ITargetListener>());
      })) << "set_target_listener";
  EXPECT_TRUE(returns_within([&] { owner.disconnect(); })) << "disconnect";
  gate.open();
  EXPECT_TRUE(h.destroy_owner_within(10s));
}

// ---------------------------------------------------------------------------------------------
// The release lane
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerRelease, ReleasesRunOnTheOwnerThreadInPostingOrder)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const auto lane = h.owner().release_lane();
  ASSERT_NE(lane, nullptr);
  std::mutex mutex;
  std::vector<int> order;
  std::vector<std::thread::id> threads;
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(lane->post_release("numbered", [&, i] {
          std::lock_guard<std::mutex> lock(mutex);
          order.push_back(i);
          threads.push_back(std::this_thread::get_id());
        }));
  }
  ASSERT_TRUE(h.wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size() == 3u;
      }));
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  for (const std::thread::id & id : threads) {
    EXPECT_EQ(id, h.owner_thread());
    EXPECT_NE(id, std::this_thread::get_id());
  }
  EXPECT_FALSE(lane->post_release("empty", std::function<void()>())) << "nothing to run";
}

TEST(SessionOwnerRelease, AReleaseThatThrowsDoesNotStopTheOwner)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const auto lane = h.owner().release_lane();
  std::atomic<int> after{0};
  EXPECT_TRUE(lane->post_release("throws", [] { throw std::runtime_error("release failed"); }));
  EXPECT_TRUE(lane->post_release("after", [&after] { after = 1; }));
  ASSERT_TRUE(h.wait_for([&after] { return after.load() == 1; }));
  EXPECT_TRUE(h.logged("QAROS: release 'throws' ended with an exception"));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning);
}

TEST(SessionOwnerRelease, AReleasePostedBeforeTheHandBackRunsBeforeTheSessionIsDestroyed)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.hand_back_wait = 5s;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const auto lane = h.owner().release_lane();
  h.owner().declare_lost("release order");
  ASSERT_TRUE(h.wait_state(SessionState::kLost, 5s)) << dump_log(h);
  std::atomic<int> destroys_at_release{-1};
  ASSERT_TRUE(lane->post_release("handle", [&] {
        destroys_at_release = h.fake().count("session_handle_destroy");
      }));
  h.owner().release_lost_session();
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().count("session_handle_destroy") == 1; }));
  EXPECT_EQ(destroys_at_release.load(), 0) << "the release came first";
}

TEST(SessionOwnerRelease, TheLaneRefusesOnceTheOwnerEnded)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const auto lane = h.owner().release_lane();
  ASSERT_NE(lane, nullptr);
  ASSERT_TRUE(h.destroy_owner_within(10s));
  std::atomic<int> ran{0};
  EXPECT_FALSE(lane->post_release("late", [&ran] { ran = 1; }));
  EXPECT_EQ(ran.load(), 0);
}

}  // namespace
