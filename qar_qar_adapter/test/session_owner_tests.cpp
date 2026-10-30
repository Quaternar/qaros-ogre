// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The session owner on a fake QarOS library: joining the Hub of this PC, joining a Hub on
// another PC, the list of targets, and the teardown. No test loads the real library.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "session_owner_harness.hpp"

namespace qar_qar_adapter
{

// The friend of the owner that lets a test hold poll() between its two steps.
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

using qar_qar_adapter::DeviceKind;
using qar_qar_adapter::QarSessionOwner;
using qar_qar_adapter::QarSessionOwnerTestAccess;
using qar_qar_adapter::HubEndpoint;
using qar_qar_adapter::HubMode;
using qar_qar_adapter::IdentityRecord;
using qar_qar_adapter::SessionEvent;
using qar_qar_adapter::SessionState;
using qar_qar_adapter::TargetInfo;
using qar_qar_adapter::test::FakePeer;
namespace alloc_watch = qar_qar_adapter::test::alloc_watch;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::Outcome;
using qar_qar_adapter::test::RecordingListener;
using qar_qar_adapter::test::make_peer;
using namespace std::chrono_literals;

constexpr const char * kIdText = "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf";

void store_identity(const Harness & harness)
{
  IdentityRecord record;
  record.id = kIdText;
  record.host = "hub.example";
  record.port = 19120;
  record.paired_unix_s = 1700000000;
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(harness.identity_path(), record, error))
    << error;
}

bool has_event(const Harness & h, SessionEvent::Kind kind)
{
  return std::any_of(h.events().begin(), h.events().end(),
    [kind](const SessionEvent & e) { return e.kind == kind; });
}

// ---------------------------------------------------------------------------------------------
// This PC's Hub (local mode)
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerLocal, ApprovalJoinsTheSessionAndHandsItOverAfterTheConsumerDrainedIt)
{
  Harness h;
  h.config().approval_timeout_ms = 45000;
  h.start();
  h.owner().connect();
  // The state reaches kRunning on the owner thread; the session is the consumer's only after
  // the consumer drained the kRunning event.
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (h.owner().state() != SessionState::kRunning && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_EQ(h.owner().state(), SessionState::kRunning);
  EXPECT_FALSE(h.owner().ready());
  EXPECT_EQ(h.owner().session(), nullptr);

  ASSERT_TRUE(h.wait_for([&h] { return h.owner().ready(); }));
  EXPECT_NE(h.owner().session(), nullptr);
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kLocal);

  const auto requests = h.fake().onboard_requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].kind, "local");
  EXPECT_EQ(requests[0].approval_timeout_ms, 45000u);
  EXPECT_EQ(requests[0].display_name, "Test program on host");
  EXPECT_EQ(requests[0].app_version, "0.1.0");
  EXPECT_EQ(requests[0].custom_info, "qaros-ros;program=test-program");
  EXPECT_FALSE(requests[0].has_chained_host);

  const auto states = h.events();
  std::vector<SessionState> seen;
  for (const SessionEvent & event : states) {
    if (event.kind == SessionEvent::Kind::kState) {
      seen.push_back(event.state);
    }
  }
  const std::vector<SessionState> expected{
    SessionState::kLoading, SessionState::kWaitingForApproval, SessionState::kRunning};
  EXPECT_EQ(seen, expected);

  EXPECT_TRUE(h.logged("QAROS: runtime 0.9.0-fake release v0 from "));
  EXPECT_TRUE(h.logged("QAROS: hub mode local ("));
  EXPECT_TRUE(h.logged("QAROS: state waiting_for_approval"));
  EXPECT_TRUE(h.logged("QAROS: state running"));
  EXPECT_TRUE(h.logged("QAROS: joined as peer eeeeeeee (local) in "));
  EXPECT_EQ(h.owner().runtime_description().find("0.9.0-fake release v0 from "), 0u);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())))
    << "the local mode stores nothing";
}

TEST(SessionOwnerLocal, EveryQarOsCallOfACommandRunsOnTheOwnerThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  ASSERT_TRUE(h.destroy_owner_within());

  const auto calls = h.fake().calls();
  ASSERT_FALSE(calls.empty());
  std::set<std::thread::id> threads;
  for (const auto & call : calls) {
    threads.insert(call.thread);
  }
  EXPECT_EQ(threads.size(), 1u) << "all calls came from one thread";
  EXPECT_EQ(threads.count(std::this_thread::get_id()), 0u) << "none from the test thread";
}

TEST(SessionOwnerLocal, DenyIsReportedAsAFailureThatCanBeRetried)
{
  Harness h;
  Outcome denied;
  denied.code = QAR_STATUS_ONBOARDING_APPROVAL_DENIED;
  h.fake().script_onboard(denied);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  const SessionEvent * failed = h.state_event(SessionState::kFailed);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status.code, QAR_STATUS_ONBOARDING_APPROVAL_DENIED);
  EXPECT_TRUE(failed->status.retry_offered);
  EXPECT_NE(failed->status.message.find("Allow"), std::string::npos);
  EXPECT_FALSE(h.owner().ready());

  ASSERT_TRUE(h.connect_and_run()) << "the second try succeeds";
  EXPECT_EQ(h.fake().onboard_requests().size(), 2u);
}

TEST(SessionOwnerLocal, AnApprovalThatTimedOutNamesTheTimeout)
{
  Harness h;
  Outcome timed_out;
  timed_out.code = QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT;
  h.fake().script_onboard(timed_out);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  const SessionEvent * failed = h.state_event(SessionState::kFailed);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status.code, QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT);
  EXPECT_EQ(failed->status.symbol, "QAR_STATUS_ONBOARDING_APPROVAL_TIMEOUT");
  EXPECT_NE(failed->status.message.find("in time"), std::string::npos);
}

TEST(SessionOwnerLocal, AHubThatCannotBeReachedSaysSoForTheLocalMode)
{
  Harness h;
  Outcome unreachable;
  unreachable.code = QAR_STATUS_ONBOARDING_HUB_UNREACHABLE;
  h.fake().script_onboard(unreachable);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  const SessionEvent * failed = h.state_event(SessionState::kFailed);
  ASSERT_NE(failed, nullptr);
  EXPECT_NE(failed->status.message.find("not running"), std::string::npos);
  EXPECT_TRUE(failed->status.retry_offered);
}

TEST(SessionOwnerLocal, CancelWhileWaitingEndsInIdleAndTheResultFiredOnce)
{
  Harness h;
  Outcome waiting;
  waiting.wait_for_release = true;
  waiting.progress = true;
  h.fake().script_onboard(waiting);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kWaitingForApproval));
  ASSERT_TRUE(h.wait_for([&h] { return has_event(h, SessionEvent::Kind::kProgress); }));
  const auto progress = std::find_if(h.events().begin(), h.events().end(),
      [](const SessionEvent & e) { return e.kind == SessionEvent::Kind::kProgress; });
  ASSERT_NE(progress, h.events().end());
  EXPECT_EQ(progress->text, "Waiting for approval in QAROS hub");
  EXPECT_TRUE(h.logged("QAROS: progress Waiting for approval in QAROS hub"));

  h.owner().cancel();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  EXPECT_EQ(h.fake().onboard_results_fired(), 1);
  EXPECT_EQ(h.fake().live_sessions(), 0);
  EXPECT_EQ(h.fake().live_tokens(), 0);
  EXPECT_FALSE(h.owner().ready());
}

TEST(SessionOwnerLocal, ShutdownWhileWaitingForApprovalDoesNotHang)
{
  Harness h;
  Outcome waiting;
  waiting.wait_for_release = true;
  h.fake().script_onboard(waiting);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kWaitingForApproval));
  ASSERT_TRUE(h.destroy_owner_within(5s));
  EXPECT_EQ(h.fake().onboard_results_fired(), 1) << "the cancelled attempt answered";
  EXPECT_EQ(h.fake().live_tokens(), 0);
  EXPECT_EQ(h.fake().count("library_destroy"), 1);
}

TEST(SessionOwnerLocal, ALibraryThatCannotBeLoadedFailsWithTheNoRuntimeCause)
{
  Harness h(HubMode::kLocal, true, /*load_ok=*/false);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  const SessionEvent * failed = h.state_event(SessionState::kFailed);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status.code, qar_qar_adapter::kStatusNoRuntime);
  EXPECT_NE(failed->status.message.find("not installed"), std::string::npos);
  EXPECT_EQ(h.fake().count("library_init"), 0);
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), 0);
}

TEST(SessionOwnerLocal, ALibraryThatFailsToStartFailsBeforeAnyRuntimeIsMade)
{
  Harness h;
  h.fake().set_library_init_code(QAR_STATUS_LOGIC_ERROR);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  EXPECT_EQ(h.fake().count("runtime_create"), 0);
  ASSERT_TRUE(h.destroy_owner_within());
  EXPECT_EQ(h.fake().count("library_destroy"), 0) << "a library that never started is not stopped";
  EXPECT_EQ(h.fake().count("library_unload"), 1);
}

TEST(SessionOwnerLocal, AHubThatIsBusyIsAskedAgainOnce)
{
  Harness h;
  Outcome busy;
  busy.code = QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING;
  h.fake().script_onboard(busy);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.fake().onboard_requests().size(), 2u);
}

TEST(SessionOwnerLocal, ASecondBusyAnswerEndsInAFailure)
{
  Harness h;
  Outcome busy;
  busy.code = QAR_STATUS_HUB_LIFECYCLE_OPERATION_RUNNING;
  h.fake().script_onboard(busy);
  h.fake().script_onboard(busy);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed, 8s));
  EXPECT_EQ(h.fake().onboard_requests().size(), 2u);
}

TEST(SessionOwnerLocal, TheApprovalTimeoutIsClampedToTenMinutes)
{
  Harness h;
  h.config().approval_timeout_ms = 900000;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_EQ(h.fake().onboard_requests().size(), 1u);
  EXPECT_EQ(h.fake().onboard_requests()[0].approval_timeout_ms, 600000u);
}

// ---------------------------------------------------------------------------------------------
// Which mode a start uses
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerMode, AutoChoosesTheLocalModeWhenQarOsIsInstalledAndNothingIsPaired)
{
  Harness h(HubMode::kAuto, /*installed=*/true);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kLocal);
  EXPECT_TRUE(h.logged("QAROS: hub mode local (QAROS is installed on this PC)"));
}

TEST(SessionOwnerMode, AutoAsksForACodeWhenQarOsIsNotInstalled)
{
  Harness h(HubMode::kAuto, /*installed=*/false);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kRemote);
  EXPECT_EQ(h.fake().onboard_requests().size(), 0u);
  EXPECT_TRUE(h.logged("QAROS: hub mode remote (QAROS is not installed on this PC)"));
}

TEST(SessionOwnerMode, AStoredPairingBeatsTheInstalledHub)
{
  Harness h(HubMode::kAuto, /*installed=*/true);
  store_identity(h);
  h.start();
  EXPECT_TRUE(h.owner().has_remote_identity());
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kRemote);
  ASSERT_EQ(h.fake().onboard_requests().size(), 1u);
  EXPECT_EQ(h.fake().onboard_requests()[0].kind, "rejoin");
}

TEST(SessionOwnerMode, ADamagedIdentityFileIsReportedAndTreatedAsAbsent)
{
  Harness h(HubMode::kAuto, /*installed=*/true);
  std::filesystem::create_directories(std::filesystem::u8path(h.dir()));
  {
    std::ofstream file(std::filesystem::u8path(h.identity_path()), std::ios::binary);
    file << "format=1\nonboarding_id=\nhub_host=x\n";
  }
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kLocal);
  EXPECT_TRUE(h.logged("identity file damaged: pair again"));
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())))
    << "the file stays until a pairing replaces it";
}

// ---------------------------------------------------------------------------------------------
// Another PC's Hub (remote mode)
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerRemote, ARejoinWithTheStoredPairingNeedsNoCode)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kRunning));
  const auto requests = h.fake().onboard_requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].kind, "rejoin");
  EXPECT_EQ(requests[0].id_text, kIdText);
  EXPECT_NE(h.state_event(SessionState::kRejoining), nullptr);
  EXPECT_TRUE(h.logged("QAROS: joined as peer eeeeeeee (rejoin) in "));
}

TEST(SessionOwnerRemote, ARejoinThatTheHubDoesNotKnowAsksForACode)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  Outcome unknown;
  unknown.code = QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND;
  h.fake().script_onboard(unknown);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  const SessionEvent * event = h.state_event(SessionState::kNeedsCode);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->status.code, QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND);
  EXPECT_NE(event->status.message.find("not paired"), std::string::npos);
}

TEST(SessionOwnerRemote, ARejoinThatMustBePairedAgainAsksForACode)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  Outcome repair;
  repair.code = QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED;
  h.fake().script_onboard(repair);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_EQ(h.state_event(SessionState::kNeedsCode)->status.code,
    QAR_STATUS_ONBOARDING_RE_PAIR_REQUIRED);
}

TEST(SessionOwnerRemote, ACodeIsSentWithTheHubAndTheIdentityIsStoredBeforeTheSessionRuns)
{
  Harness h(HubMode::kRemote);
  bool stored_at_subscribe = false;
  const std::string path = h.identity_path();
  h.fake().set_call_hook([&stored_at_subscribe, path](const std::string & name) {
      if (name == "peer_subscribe_updates") {
        stored_at_subscribe = std::filesystem::exists(std::filesystem::u8path(path));
      }
    });
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_FALSE(h.owner().has_remote_identity());

  h.owner().submit_code("48151623", HubEndpoint{"hub.example", 19120});
  ASSERT_TRUE(h.wait_state(SessionState::kRunning));
  EXPECT_TRUE(stored_at_subscribe) << "the identity file exists before the session is announced";
  EXPECT_TRUE(h.owner().has_remote_identity());

  const auto requests = h.fake().onboard_requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].kind, "code");
  EXPECT_EQ(requests[0].code, "48151623");
  EXPECT_EQ(requests[0].host, "hub.example");
  EXPECT_EQ(requests[0].port, 19120);
  EXPECT_NE(h.state_event(SessionState::kPairing), nullptr);
  EXPECT_TRUE(h.logged("QAROS: joined as peer eeeeeeee (pairing) in "));

  for (const std::string & line : h.log_lines()) {
    EXPECT_EQ(line.find("48151623"), std::string::npos) << "the code is never logged: " << line;
  }
  IdentityRecord record;
  std::string error;
  ASSERT_EQ(qar_qar_adapter::read_identity(h.identity_path(), record, error),
    qar_qar_adapter::IdentityRead::kOk) << error;
  EXPECT_EQ(record.host, "hub.example");
  EXPECT_EQ(record.port, 19120);
  EXPECT_EQ(record.id.size(), 36u);
  std::ifstream file(std::filesystem::u8path(h.identity_path()), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text.find("48151623"), std::string::npos) << "the file holds no code";
}

TEST(SessionOwnerRemote, AWrongCodeAsksForTheNextOne)
{
  Harness h(HubMode::kRemote);
  Outcome wrong;
  wrong.code = QAR_STATUS_PAKE_ERROR;
  h.fake().script_onboard(wrong);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  h.owner().submit_code("11111111", HubEndpoint{"hub.example", 19120});
  ASSERT_TRUE(h.wait_for([&h] {
        return h.fake().onboard_requests().size() == 1u &&
               h.owner().state() == SessionState::kNeedsCode &&
               std::count_if(h.events().begin(), h.events().end(), [](const SessionEvent & e) {
          return e.kind == SessionEvent::Kind::kState && e.state == SessionState::kNeedsCode &&
          e.status.code == QAR_STATUS_PAKE_ERROR;
        }) == 1;
      }));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())));

  h.owner().submit_code("22222222", HubEndpoint{"hub.example", 19120});
  ASSERT_TRUE(h.wait_state(SessionState::kRunning));
  EXPECT_EQ(h.fake().onboard_requests().size(), 2u);
}

TEST(SessionOwnerRemote, APortOfZeroIsRefusedBeforeAnyCall)
{
  Harness h(HubMode::kRemote);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  h.owner().submit_code("12345678", HubEndpoint{"hub.example", 0});
  ASSERT_TRUE(h.wait_for([&h] {
        return std::any_of(h.events().begin(), h.events().end(), [](const SessionEvent & e) {
          return e.kind == SessionEvent::Kind::kState && e.status.code ==
          QAR_STATUS_ARGUMENT_NOT_SUPPORTED;
        });
      }));
  EXPECT_EQ(h.owner().state(), SessionState::kNeedsCode);
  EXPECT_EQ(h.fake().onboard_requests().size(), 0u);
}

TEST(SessionOwnerRemote, CancelWhilePairingGoesBackToTheCodePrompt)
{
  Harness h(HubMode::kRemote);
  Outcome waiting;
  waiting.wait_for_release = true;
  h.fake().script_onboard(waiting);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  h.owner().submit_code("12345678", HubEndpoint{"hub.example", 19120});
  ASSERT_TRUE(h.wait_state(SessionState::kPairing));
  h.owner().cancel();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_EQ(h.fake().onboard_results_fired(), 1);
  EXPECT_EQ(h.fake().live_sessions(), 0);
}

TEST(SessionOwnerRemote, ForgetReleasesTheSessionFirstThenForgetsAndDeletesTheFile)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().forget();
  ASSERT_TRUE(h.wait_for([&h] { return has_event(h, SessionEvent::Kind::kForgetDone); }));
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));

  const int destroyed = h.fake().first_index("session_handle_destroy");
  const int forgotten = h.fake().first_index("runtime_forget");
  ASSERT_GE(destroyed, 0);
  ASSERT_GE(forgotten, 0);
  EXPECT_LT(destroyed, forgotten) << "the session is released before the slot is erased";
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())));
  EXPECT_FALSE(h.owner().has_remote_identity());
  const auto done = std::find_if(h.events().begin(), h.events().end(),
      [](const SessionEvent & e) { return e.kind == SessionEvent::Kind::kForgetDone; });
  ASSERT_NE(done, h.events().end());
  EXPECT_EQ(done->status.code, 0);
}

TEST(SessionOwnerRemote, ForgetOfAPairingTheHubNoLongerKnowsStillDeletesTheFile)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  h.fake().script_forget(QAR_STATUS_ONBOARDING_SESSION_NOT_FOUND);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().forget();
  ASSERT_TRUE(h.wait_for([&h] { return has_event(h, SessionEvent::Kind::kForgetDone); }));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())));
}

TEST(SessionOwnerRemote, ForgetWithoutAnySessionNeedsOnlyTheRuntime)
{
  Harness h(HubMode::kRemote);
  store_identity(h);
  h.start();
  h.owner().forget();
  ASSERT_TRUE(h.wait_for([&h] { return has_event(h, SessionEvent::Kind::kForgetDone); }));
  EXPECT_EQ(h.fake().count("runtime_forget"), 1);
  EXPECT_EQ(h.fake().count("runtime_onboard_async"), 0);
  EXPECT_EQ(h.fake().count("runtime_rejoin_async"), 0);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(h.identity_path())));
}

TEST(SessionOwnerRemote, ForgetInTheLocalModeIsIgnored)
{
  Harness h(HubMode::kLocal);
  h.start();
  h.owner().forget();
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("forget ignored"); }));
  EXPECT_EQ(h.fake().count("runtime_forget"), 0);
}

// ---------------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerTargets, OnlyOnlineAndUnknownTargetsAreRenderTargets)
{
  Harness h;
  std::vector<FakePeer> peers;
  peers.push_back(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_UNKNOWN));
  peers.push_back(make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  peers.push_back(make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
  peers.push_back(make_peer(4, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_LEAVING));
  peers.push_back(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_RETIRED));
  peers.push_back(make_peer(6, QAR_PEER_ROLE_CONTROL, QAR_PEER_PRESENCE_ONLINE));
  peers.push_back(make_peer(7, QAR_PEER_ROLE_INTERNAL, QAR_PEER_PRESENCE_ONLINE));
  peers.push_back(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE));
  peers.push_back(make_peer(9, QAR_PEER_ROLE_SOURCE, QAR_PEER_PRESENCE_ONLINE));
  FakePeer self = make_peer(0, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE);
  self.id = qar_qar_adapter::test::fake_self_id();
  peers.push_back(self);
  h.fake().set_initial_peers(peers);
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  const std::vector<TargetInfo> targets = h.owner().targets();
  ASSERT_EQ(targets.size(), 2u);
  std::set<int> tags;
  for (const TargetInfo & target : targets) {
    tags.insert(target.id.data[0]);
    EXPECT_TRUE(target.served);
    EXPECT_EQ(target.device_name, "Meta Quest 3");
    EXPECT_EQ(target.category, QAR_DEVICE_CATEGORY_HEADSET);
  }
  EXPECT_EQ(tags, (std::set<int>{1, 2}));
  const auto self_id = qar_qar_adapter::test::fake_self_id();
  EXPECT_EQ(std::memcmp(h.owner().self_peer().data, self_id.data, sizeof(self_id.data)), 0);
  EXPECT_TRUE(h.logged("QAROS: target 01010101 headset 'Meta Quest 3' unknown -> served"));
}

TEST(SessionOwnerTargets, ATargetThatGoesOfflineStopsAndStartsAgainWhenItComesBack)
{
  Harness h;
  auto listener = std::make_shared<RecordingListener>();
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(listener->ups(), 1u);

  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
  EXPECT_EQ(listener->downs(), 1u);
  EXPECT_EQ(listener->down_list()[0].second, QAR_PEER_PRESENCE_OFFLINE);
  EXPECT_TRUE(h.owner().targets().empty());
  EXPECT_TRUE(h.logged("QAROS: target 01010101 offline -> stream stopped"));

  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_EQ(listener->ups(), 2u);
  EXPECT_EQ(h.owner().targets().size(), 1u);
}

TEST(SessionOwnerTargets, ARetiredTargetIsDroppedForGood)
{
  Harness h;
  auto listener = std::make_shared<RecordingListener>();
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());

  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_RETIRED));
  EXPECT_EQ(listener->downs(), 1u);
  EXPECT_TRUE(h.owner().targets().empty());
  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_RETIRED));
  EXPECT_EQ(listener->downs(), 1u) << "a retired peer is not reported twice";
  EXPECT_TRUE(h.logged("QAROS: target 01010101 retired -> stream stopped"));
}

TEST(SessionOwnerTargets, ThePeerTheQueryAndTheSubscriptionBothReportIsHeardOnce)
{
  Harness h;
  auto listener = std::make_shared<RecordingListener>();
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());
  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_UNKNOWN));
  EXPECT_EQ(listener->ups(), 1u);
  EXPECT_EQ(listener->downs(), 0u);
  EXPECT_EQ(h.owner().targets().size(), 1u);
}

TEST(SessionOwnerTargets, TargetsBeyondTheCapAreListedButNotServedUntilASlotIsFree)
{
  Harness h;
  h.config().max_targets = 2;
  auto listener = std::make_shared<RecordingListener>();
  h.fake().set_initial_peers({
      make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE),
      make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE),
      make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());

  std::vector<TargetInfo> targets = h.owner().targets();
  ASSERT_EQ(targets.size(), 3u);
  EXPECT_EQ(std::count_if(targets.begin(), targets.end(),
    [](const TargetInfo & t) { return t.served; }), 2);
  const auto unserved = std::find_if(targets.begin(), targets.end(),
      [](const TargetInfo & t) { return !t.served; });
  ASSERT_NE(unserved, targets.end());
  EXPECT_EQ(unserved->not_served_reason, "cap reached");
  EXPECT_EQ(listener->ups(), 2u);
  EXPECT_TRUE(h.logged("-> not served (cap reached)"));

  h.fake().deliver_peer(make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
  EXPECT_EQ(listener->downs(), 1u);
  EXPECT_EQ(listener->ups(), 3u) << "the third target takes the free slot";
  targets = h.owner().targets();
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_TRUE(targets[0].served);
  EXPECT_TRUE(targets[1].served);
}

TEST(SessionOwnerTargets, AListenerThatArrivesLateHearsOfTheTargetsThatAreHere)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto listener = std::make_shared<RecordingListener>();
  h.owner().set_target_listener(listener);
  EXPECT_EQ(listener->ups(), 1u);
}

TEST(SessionOwnerTargets, APeerSpecThatACallbackBorrowsIsNeverDestroyed)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().deliver_peer(make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  h.fake().deliver_peer(make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
  EXPECT_EQ(h.fake().borrowed_handle_destroys(), 0);
  EXPECT_EQ(h.fake().owned_handles_outstanding(), 0) << "the handles a query returns are released";
}

TEST(SessionOwnerTargets, AHubThatStaysAwayPutsTheSessionIntoReconnectingUntilItIsBack)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_OFFLINE));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning) << "a short drop is not reported";
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 6s));
  EXPECT_TRUE(h.owner().ready()) << "the session stays valid";
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE));
  ASSERT_TRUE(h.wait_state(SessionState::kRunning));
}

TEST(SessionOwnerTargets, AHubThatComesBackWithinTheGraceTimeIsNeverReported)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_OFFLINE));
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_FALSE(h.wait_state(SessionState::kReconnecting, 3500ms));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning);
  EXPECT_EQ(h.state_event(SessionState::kReconnecting), nullptr);
}

// ---------------------------------------------------------------------------------------------
// configure(): the mode of the next connect without a new owner
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerConfigure, AnIdleOwnerUsesTheNewModeAtTheNextConnectAndNeverMakesALibraryCall)
{
  Harness h(HubMode::kLocal);
  h.start();
  h.owner().configure(HubMode::kRemote, HubEndpoint{"hub.example", 19120});
  EXPECT_TRUE(h.fake().calls().empty()) << "configure() calls no QAROS function";
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kRemote);
  EXPECT_TRUE(h.fake().onboard_requests().empty()) << "no local approval was asked for";
  const std::thread::id owner_thread = h.owner_thread();
  for (const auto & call : h.fake().calls()) {
    EXPECT_EQ(call.thread, owner_thread) << call.name;
  }
}

TEST(SessionOwnerConfigure, ARunningOwnerKeepsTheSettingsAndAppliesThemAtTheNextConnect)
{
  Harness h(HubMode::kLocal);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().configure(HubMode::kRemote, HubEndpoint{"hub.example", 19120});
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("settings kept for the next connect"); }));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning) << "a running session is not touched";
  EXPECT_TRUE(h.owner().ready());
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kLocal);

  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kNeedsCode));
  EXPECT_EQ(h.owner().effective_mode(), HubMode::kRemote);
  EXPECT_EQ(h.fake().count("runtime_create"), 1) << "the runtime is made once";
  EXPECT_EQ(h.fake().count("library_init"), 1) << "the library is started once";
  EXPECT_EQ(h.fake().count("runtime_handle_destroy"), 0);
  EXPECT_EQ(h.fake().count("library_unload"), 0);
}

TEST(SessionOwnerConfigure, AFailedOwnerAppliesTheSettingsAtOnce)
{
  Harness h(HubMode::kLocal, /*installed=*/true, /*load_ok=*/false);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  h.owner().configure(HubMode::kRemote, HubEndpoint{"first.example", 1});
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().effective_mode() == HubMode::kRemote; }));
  h.owner().configure(HubMode::kLocal, HubEndpoint{"second.example", 2});
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().effective_mode() == HubMode::kLocal; }));
  EXPECT_FALSE(h.logged("settings kept for the next connect")) << "failed counts as idle";
}

// ---------------------------------------------------------------------------------------------
// Invites
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerInvites, ADeviceIsInvitedByItsKindAndAddressInTheLocalMode)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.owner().invite_device(DeviceKind::kHeadset, "quest-1"), "");
  EXPECT_EQ(h.owner().invite_device(DeviceKind::kHoloLens, " 192.168.1.20 "), "");
  ASSERT_TRUE(h.wait_for([&h] {
        return std::count_if(h.events().begin(), h.events().end(), [](const SessionEvent & e) {
          return e.kind == SessionEvent::Kind::kInviteDone;
        }) == 2;
      }));
  const auto requests = h.fake().invite_requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0], "headset:quest-1");
  EXPECT_EQ(requests[1], "hololens:192.168.1.20");
  EXPECT_TRUE(h.logged("QAROS: invite headset 'quest-1' -> d0d1d2d3"));
  EXPECT_EQ(h.fake().live_tokens(), 1) << "only the subscription token is left";
}

TEST(SessionOwnerInvites, ARefusedInviteReportsTheHubsAnswer)
{
  Harness h;
  Outcome refused;
  refused.code = QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE;
  h.fake().script_invite(refused);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.owner().invite_device(DeviceKind::kHoloLens, "hl-2"), "");
  ASSERT_TRUE(h.wait_for([&h] { return has_event(h, SessionEvent::Kind::kInviteDone); }));
  const auto done = std::find_if(h.events().begin(), h.events().end(),
      [](const SessionEvent & e) { return e.kind == SessionEvent::Kind::kInviteDone; });
  EXPECT_EQ(done->status.code, QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE);
  EXPECT_FALSE(done->status.retry_offered);
}

TEST(SessionOwnerInvites, TheRemoteModeAndAnEmptyAddressAreRefusedWithoutACall)
{
  {
    Harness remote(HubMode::kRemote);
    store_identity(remote);
    remote.start();
    ASSERT_TRUE(remote.connect_and_run());
    EXPECT_NE(remote.owner().invite_device(DeviceKind::kHeadset, "quest-1"), "");
    EXPECT_EQ(remote.fake().count("session_invite_target_app_async"), 0);
  }
  Harness local;
  local.start();
  EXPECT_NE(local.owner().invite_device(DeviceKind::kHeadset, "quest-1"), "")
    << "not connected yet";
  ASSERT_TRUE(local.connect_and_run());
  EXPECT_NE(local.owner().invite_device(DeviceKind::kHeadset, "   "), "");
  EXPECT_EQ(local.fake().count("session_invite_target_app_async"), 0);
}

// ---------------------------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerTeardown, ThingsAreReleasedInTheOrderTheDesignGives)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  EXPECT_FALSE(h.owner().ready());
  EXPECT_EQ(h.owner().session(), nullptr);
  EXPECT_FALSE(h.fake().subscription_active());
  EXPECT_EQ(h.fake().count("runtime_handle_destroy"), 0) << "a disconnect keeps the runtime";
  EXPECT_EQ(h.fake().count("library_destroy"), 0) << "a disconnect keeps the library";
  EXPECT_LT(h.fake().last_index("cancel_token_handle_destroy"),
    h.fake().first_index("session_handle_destroy")) << "the subscription ends first";

  ASSERT_TRUE(h.destroy_owner_within());
  const int session = h.fake().last_index("session_handle_destroy");
  const int runtime = h.fake().last_index("runtime_handle_destroy");
  const int library = h.fake().last_index("library_destroy");
  const int unload = h.fake().last_index("library_unload");
  EXPECT_GE(session, 0);
  EXPECT_LT(session, runtime);
  EXPECT_LT(runtime, library);
  EXPECT_LT(library, unload);
  EXPECT_EQ(h.fake().live_tokens(), 0);
  EXPECT_EQ(h.fake().live_sessions(), 0);
  EXPECT_TRUE(h.logged("QAROS: teardown: peer subscription ended in "));
  EXPECT_TRUE(h.logged("QAROS: teardown: library destroyed in "));
}

TEST(SessionOwnerTeardown, ADisconnectedOwnerConnectsAgainOnTheSameRuntime)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_EQ(h.fake().count("runtime_create"), 1);
  EXPECT_EQ(h.fake().count("library_init"), 1);
  EXPECT_EQ(h.fake().onboard_requests().size(), 2u);
}

TEST(SessionOwnerTeardown, ABlockedPeerCallbackDoesNotDeadlockTheTeardown)
{
  Harness h;
  auto listener = std::make_shared<RecordingListener>();
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());

  std::mutex gate;
  gate.lock();
  listener->set_up_hook([&gate] { std::lock_guard<std::mutex> wait(gate); });
  std::thread library_thread([&h] {
      h.fake().deliver_peer(make_peer(7, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
    });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!listener->entered() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(listener->entered()) << "the callback is inside the listener";

  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kStopping));
  std::this_thread::sleep_for(100ms);
  // The callback sits in the listener and the owner thread waits for it. Nothing else may be
  // stuck behind that wait: the target list answers.
  auto answer = std::async(std::launch::async, [&h] { return h.owner().targets().size(); });
  EXPECT_EQ(answer.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(h.owner().state(), SessionState::kStopping) << "the teardown waits for the callback";

  gate.unlock();
  library_thread.join();
  EXPECT_TRUE(h.wait_state(SessionState::kIdle, 5s));
  EXPECT_EQ(h.fake().live_sessions(), 0);
}

TEST(SessionOwnerTeardown, AnInviteInFlightIsCancelledAndAnswersOnce)
{
  Harness h;
  Outcome waiting;
  waiting.wait_for_release = true;
  h.fake().script_invite(waiting);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_EQ(h.owner().invite_device(DeviceKind::kHeadset, "quest-1"), "");
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().invite_requests().size() == 1u; }));

  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 5s));
  EXPECT_EQ(h.fake().invite_results_fired(), 1);
  EXPECT_EQ(h.fake().live_tokens(), 0);
  EXPECT_EQ(h.fake().live_sessions(), 0);
  const auto done = std::find_if(h.events().begin(), h.events().end(),
      [](const SessionEvent & e) { return e.kind == SessionEvent::Kind::kInviteDone; });
  ASSERT_NE(done, h.events().end());
  EXPECT_NE(done->status.code, 0);
}

TEST(SessionOwnerTeardown, AnOwnerThatNeverConnectedMakesNoLibraryCall)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.destroy_owner_within());
  EXPECT_TRUE(h.fake().calls().empty());
}

// ---------------------------------------------------------------------------------------------
// The hand-over of the listener, the session and the contexts
// ---------------------------------------------------------------------------------------------

TEST(SessionOwnerListener, RemovingTheListenerWaitsForTheDeliveryThatIsInsideIt)
{
  Harness h;
  auto listener = std::make_shared<RecordingListener>();
  h.start();
  h.owner().set_target_listener(listener);
  ASSERT_TRUE(h.connect_and_run());

  std::mutex gate;
  gate.lock();
  listener->set_up_hook([&gate] { std::lock_guard<std::mutex> wait(gate); });
  std::thread library_thread([&h] {
      h.fake().deliver_peer(make_peer(7, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
    });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!listener->entered() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(listener->entered());

  auto removal = std::async(std::launch::async, [&h] {
      h.owner().set_target_listener(std::weak_ptr<qar_qar_adapter::ITargetListener>());
    });
  EXPECT_EQ(removal.wait_for(200ms), std::future_status::timeout)
    << "the removal waits while the listener is running";
  EXPECT_EQ(listener->ups(), 0u);

  gate.unlock();
  library_thread.join();
  ASSERT_EQ(removal.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(listener->ups(), 1u) << "the delivery was over when the removal returned";

  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_EQ(listener->ups(), 1u) << "a removed listener hears nothing more";
}

TEST(SessionOwnerListener, AReplayNeverAnnouncesATargetThatWentDownBehindIt)
{
  Harness h;
  h.fake().set_initial_peers({
      make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE),
      make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  auto listener = std::make_shared<RecordingListener>();
  std::mutex gate;
  gate.lock();
  std::atomic<int> calls{0};
  listener->set_up_hook([&gate, &calls] {
      if (calls.fetch_add(1) == 0) {
        std::lock_guard<std::mutex> wait(gate);
      }
    });
  auto replay = std::async(std::launch::async, [&h, &listener] {
      h.owner().set_target_listener(listener);
    });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!listener->entered() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(listener->entered()) << "the replay is inside the listener";

  std::thread library_thread([&h] {
      h.fake().deliver_peer(make_peer(2, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
    });
  std::this_thread::sleep_for(150ms);
  gate.unlock();
  library_thread.join();
  ASSERT_EQ(replay.wait_for(5s), std::future_status::ready);

  // Target 2 is announced before it goes down, never after.
  const std::vector<std::string> heard = listener->sequence();
  ASSERT_EQ(heard.size(), 3u);
  const auto down = std::find(heard.begin(), heard.end(), "down:2");
  const auto up = std::find(heard.begin(), heard.end(), "up:2");
  ASSERT_NE(down, heard.end());
  ASSERT_NE(up, heard.end());
  EXPECT_LT(up, down) << "the stale announcement came after the target went down";
}

TEST(SessionOwnerListener, ALogSinkThatBlocksDoesNotHoldTheTargetList)
{
  Harness h;
  auto original = h.config().log_sink;
  std::mutex gate;
  gate.lock();
  std::atomic<bool> in_sink{false};
  h.config().log_sink = [original, &gate, &in_sink](const std::string & line) {
      original(line);
      if (line.find("target 07070707") != std::string::npos) {
        in_sink.store(true);
        std::lock_guard<std::mutex> wait(gate);
      }
    };
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  std::thread library_thread([&h] {
      h.fake().deliver_peer(make_peer(7, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
    });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!in_sink.load() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(in_sink.load()) << "the peer thread is inside the log sink";

  auto answer = std::async(std::launch::async, [&h] { return h.owner().targets().size(); });
  const bool answered = answer.wait_for(2s) == std::future_status::ready;
  gate.unlock();
  library_thread.join();
  ASSERT_TRUE(answered) << "targets() waited for the log sink";
  EXPECT_EQ(answer.get(), 1u);
}

TEST(SessionOwnerHandOver, APollThatSawTheSessionRunningCannotMakeTheConsumerReadyAfterItLeft)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  // A poll that has seen the state kRunning is held before it sets the flag of ready().
  std::mutex gate;
  gate.lock();
  std::atomic<bool> held{false};
  QarSessionOwnerTestAccess::set_poll_hook(h.owner(), [&gate, &held] {
      held.store(true);
      std::lock_guard<std::mutex> wait(gate);
    });
  h.fake().deliver_peer(make_peer(3, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  std::vector<SessionEvent> polled;
  auto poll = std::async(std::launch::async, [&h, &polled] { h.owner().poll(polled); });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!held.load() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(held.load()) << "poll() is held";

  // The session is released meanwhile; the state change waits for the held poll or overtakes it.
  h.owner().disconnect();
  std::this_thread::sleep_for(150ms);
  gate.unlock();
  ASSERT_EQ(poll.wait_for(5s), std::future_status::ready);
  QarSessionOwnerTestAccess::set_poll_hook(h.owner(), nullptr);
  // The state is read without a poll, which would set the flag right.
  const auto idle_by = std::chrono::steady_clock::now() + 5s;
  while (h.owner().state() != SessionState::kIdle && std::chrono::steady_clock::now() < idle_by) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_EQ(h.owner().state(), SessionState::kIdle);
  EXPECT_FALSE(h.owner().ready()) << "ready() was set from a state that had moved on";
  EXPECT_EQ(h.owner().session(), nullptr);
}

TEST(SessionOwnerHandOver, TheConsumerIsNeverReadyWithoutASessionAcrossManyCycles)
{
  // A stress guard: the consumer polls on a thread of its own while the owner joins and leaves
  // over many cycles. While the owner is kIdle, no poll may leave ready() set.
  Harness h;
  h.start();
  std::atomic<bool> stop{false};
  std::atomic<bool> idle_phase{false};
  std::atomic<int> violations{0};
  std::thread consumer([&h, &stop, &idle_phase, &violations] {
      std::vector<SessionEvent> sink;
      while (!stop.load()) {
        const bool idle_before = idle_phase.load();
        h.owner().poll(sink);
        if (idle_before && h.owner().ready()) {
          violations.fetch_add(1);
        }
        sink.clear();
      }
    });
  for (int cycle = 0; cycle < 40; ++cycle) {
    h.owner().connect();
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!(h.owner().state() == SessionState::kRunning && h.owner().ready()) &&
      std::chrono::steady_clock::now() < end)
    {
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(h.owner().ready());
    h.owner().disconnect();
    while (h.owner().state() != SessionState::kIdle && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(h.owner().state(), SessionState::kIdle);
    idle_phase.store(true);
    std::this_thread::sleep_for(3ms);
    idle_phase.store(false);
  }
  stop.store(true);
  consumer.join();
  EXPECT_EQ(violations.load(), 0);
  EXPECT_FALSE(h.owner().ready());
  EXPECT_EQ(h.owner().session(), nullptr);
}

TEST(SessionOwnerRefusedStart, TheContextOfAnAttemptThatFailedAtOnceOutlivesItsLateResult)
{
  Harness h;
  Outcome refused;
  refused.sync_error_code = QAR_STATUS_UNCLASSIFIED;
  refused.wait_for_release = true;
  h.fake().script_onboard(refused);
  h.fake().set_sync_failure_hook([](const void * user) { alloc_watch::arm(user); });
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kFailed));
  ASSERT_TRUE(alloc_watch::armed()) << "the fake saw a synchronous failure";
  EXPECT_FALSE(alloc_watch::freed()) << "the library may still call back with the context";

  h.fake().release_pending();
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().onboard_results_fired() == 1; }));
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().live_sessions() == 0; }))
    << "a session that arrives for a dead attempt is released";
  EXPECT_FALSE(alloc_watch::freed());

  ASSERT_TRUE(h.destroy_owner_within());
  EXPECT_TRUE(alloc_watch::freed()) << "the owner releases the context when it goes";
}

TEST(SessionOwnerRefusedStart, TheContextOfAnInviteThatFailedAtOnceOutlivesItsLateResult)
{
  Harness h;
  Outcome refused;
  refused.sync_error_code = QAR_STATUS_HUB_TARGET_APP_DEVICE_NOT_INVITABLE;
  refused.wait_for_release = true;
  h.fake().script_invite(refused);
  h.fake().set_sync_failure_hook([](const void * user) { alloc_watch::arm(user); });
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_NE(h.owner().invite_device(DeviceKind::kHeadset, "quest-1"), "");
  ASSERT_TRUE(alloc_watch::armed());
  EXPECT_FALSE(alloc_watch::freed()) << "the library may still call back with the context";

  h.fake().release_pending();
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().invite_results_fired() == 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(alloc_watch::freed());
  EXPECT_FALSE(has_event(h, SessionEvent::Kind::kInviteDone)) << "the refusal was the answer";

  ASSERT_TRUE(h.destroy_owner_within());
  EXPECT_TRUE(alloc_watch::freed());
}

// ---------------------------------------------------------------------------------------------
// Tasks for the owner thread
// ---------------------------------------------------------------------------------------------

// A task that calls the fake library on the session it is given, so that the order of calls in
// the fake says where it ran, and then holds the owner thread for @p hold.
QarSessionOwner::SessionTask slow_task(
  std::atomic<int> & ran, std::chrono::milliseconds hold)
{
  return [&ran, hold](QarSession * session) {
           std::size_t count = 0;
           (void)qar_query_app_volumes_count(session, &count);
           std::this_thread::sleep_for(hold);
           ran.fetch_add(1);
         };
}

TEST(SessionOwnerTasks, ATaskIsRefusedUntilTheSessionIsHandedOver)
{
  Harness h;
  h.start();
  std::atomic<int> ran{0};
  EXPECT_FALSE(h.owner().post_session_task("early", slow_task(ran, 0ms)));
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_TRUE(h.owner().post_session_task("now", slow_task(ran, 0ms)));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }));
}

TEST(SessionOwnerTasks, TasksRunOnTheOwnerThreadOneAtATimeInTheOrderTheyWerePosted)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::mutex mutex;
  std::vector<int> order;
  std::atomic<int> concurrent{0};
  std::atomic<int> worst{0};
  std::set<std::thread::id> threads;
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(h.owner().post_session_task("ordered", [&, i](QarSession *) {
          const int now_running = concurrent.fetch_add(1) + 1;
          worst.store(std::max(worst.load(), now_running));
          std::this_thread::sleep_for(5ms);
          {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
            threads.insert(std::this_thread::get_id());
          }
          concurrent.fetch_sub(1);
        }));
  }
  ASSERT_TRUE(h.wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size() == 6u;
      }));
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4, 5}));
  EXPECT_EQ(worst.load(), 1);
  ASSERT_EQ(threads.size(), 1u);
  EXPECT_EQ(*threads.begin(), h.owner_thread());
  EXPECT_NE(*threads.begin(), std::this_thread::get_id());
}

TEST(SessionOwnerTasks, ATaskReceivesTheOpenSessionAndMayPostAFollowUp)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<QarSession *> seen_first{nullptr};
  std::atomic<QarSession *> seen_second{nullptr};
  QarSessionOwner * owner = &h.owner();
  ASSERT_TRUE(owner->post_session_task("first", [&, owner](QarSession * session) {
        seen_first.store(session);
        owner->post_session_task("second", [&](QarSession * again) { seen_second.store(again); });
      }));
  ASSERT_TRUE(h.wait_for([&] { return seen_second.load() != nullptr; }));
  EXPECT_EQ(seen_first.load(), h.owner().session());
  EXPECT_EQ(seen_second.load(), h.owner().session());
}

TEST(SessionOwnerTasks, ATaskThatThrowsDoesNotStopTheOwner)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  ASSERT_TRUE(h.owner().post_session_task("throws", [](QarSession *) {
        throw std::runtime_error("a task that fails");
      }));
  ASSERT_TRUE(h.owner().post_session_task("after", slow_task(ran, 0ms)));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }));
  EXPECT_TRUE(h.logged("task 'throws' ended with an exception"));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning);
}

TEST(SessionOwnerTasks, ASlowTaskIsNamedInTheLogAndInTheStats)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  ASSERT_TRUE(h.owner().post_session_task("slow-one", slow_task(ran, 1100ms)));
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().task_stats().running == "slow-one"; }));
  const auto during = h.owner().task_stats();
  EXPECT_GT(during.running_ms, 0.0);
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }, 5s));
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().task_stats().done == 1u; }));
  const auto stats = h.owner().task_stats();
  EXPECT_EQ(stats.slowest, "slow-one");
  EXPECT_GE(stats.slowest_ms, 1000.0);
  EXPECT_EQ(stats.queued, 0u);
  EXPECT_TRUE(stats.running.empty());
  EXPECT_TRUE(h.logged("task 'slow-one' took"));
}

TEST(SessionOwnerTasks, ACallOfHalfASecondOrMoreIsNamedInTheLog)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  ASSERT_TRUE(h.owner().post_session_task("calls", [&ran](QarSession *) {
        {
          QAR_GUI_TIMED("qar_quick_call");
          std::this_thread::sleep_for(20ms);
        }
        {
          QAR_GUI_TIMED("qar_slow_probe_call");
          std::this_thread::sleep_for(560ms);
        }
        ran.store(1);
      }));
  ASSERT_TRUE(h.wait_for([&ran] { return ran.load() == 1; }, 5s));
  ASSERT_TRUE(h.wait_for([&h] { return h.owner().task_stats().done == 1u; }));
  EXPECT_TRUE(h.logged("QAROS: call qar_slow_probe_call took "));
  EXPECT_FALSE(h.logged("qar_quick_call"));
}

TEST(SessionOwnerTasks, TasksQueuedBeforeDisconnectRunAndNoneTouchesTheReleasedSession)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  std::atomic<int> dropped{0};
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(h.owner().post_session_task("slow", slow_task(ran, 2000ms),
        [&dropped] { dropped.fetch_add(1); }));
  }
  h.owner().disconnect();
  std::atomic<bool> late_ran{false};
  std::atomic<bool> late_dropped{false};
  EXPECT_FALSE(h.owner().post_session_task("late", [&late_ran](QarSession *) { late_ran = true; },
      [&late_dropped] { late_dropped = true; }))
    << "no task is taken after the disconnect";

  ASSERT_TRUE(h.wait_state(SessionState::kIdle, 15s));
  EXPECT_EQ(ran.load(), 3);
  EXPECT_EQ(dropped.load(), 0);
  EXPECT_FALSE(late_ran.load());
  EXPECT_FALSE(late_dropped.load()) << "a refused task calls neither function";

  const auto calls = h.fake().calls();
  int last_query = -1;
  int destroy = -1;
  int queries = 0;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (calls[i].name == "query_app_volumes_count") {
      last_query = static_cast<int>(i);
      ++queries;
    }
    if (calls[i].name == "session_handle_destroy" && destroy < 0) {
      destroy = static_cast<int>(i);
    }
  }
  EXPECT_EQ(queries, 3);
  ASSERT_GE(destroy, 0);
  EXPECT_LT(last_query, destroy) << "the session is released after the last task";
}

TEST(SessionOwnerTasks, DrainBudgetDropsLateTasks)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  std::atomic<int> dropped{0};
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(h.owner().post_session_task("slow", slow_task(ran, 2000ms),
        [&dropped] { dropped.fetch_add(1); }));
  }
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(h.destroy_owner_within(14s));
  const double took_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
    .count();
  EXPECT_LT(took_s, 8.0 + 2.0 + 2.0) << "the budget bounds the wait: 8 s plus the running task";
  EXPECT_GE(took_s, 6.0) << "the tasks before the budget ran";
  EXPECT_EQ(ran.load() + dropped.load(), 12) << "every task ran or was dropped";
  EXPECT_GT(dropped.load(), 0);
  EXPECT_GT(ran.load(), 0);

  const auto calls = h.fake().calls();
  int destroy = -1;
  int queries = 0;
  int after_destroy = 0;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (calls[i].name == "session_handle_destroy" && destroy < 0) {
      destroy = static_cast<int>(i);
    }
    if (calls[i].name == "query_app_volumes_count") {
      ++queries;
      if (destroy >= 0) {
        ++after_destroy;
      }
    }
  }
  EXPECT_EQ(queries, ran.load());
  EXPECT_EQ(after_destroy, 0) << "no task ran after the session was destroyed";
  EXPECT_TRUE(h.logged("drain budget 8 s spent"));
}

TEST(SessionOwnerTasks, ABacklogOfTasksDelaysTheHubDownTimerByOneTaskAtMost)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  // The presence message is handled at once and arms the 3 s grace timer; eight tasks of a
  // second each are queued behind nothing but each other.
  h.fake().deliver_peer(make_peer(8, QAR_PEER_ROLE_RUNTIME, QAR_PEER_PRESENCE_OFFLINE));
  std::atomic<int> ran{0};
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(h.owner().post_session_task("backlog", slow_task(ran, 1000ms)));
  }
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(h.wait_state(SessionState::kReconnecting, 6s)) << "the timer is checked per task";
  const double took_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
    .count();
  EXPECT_LT(took_s, 3.0 + 1.0 + 0.5) << "the grace time plus the task that was running";
  EXPECT_LT(ran.load(), 8) << "the backlog was still there";
}

TEST(SessionOwnerTasks, AForgetThatWasIgnoredLeavesNoDrainDeadlineBehind)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.owner().forget();
  ASSERT_TRUE(h.wait_for([&h] { return h.logged("forget ignored"); }));
  EXPECT_EQ(h.owner().state(), SessionState::kRunning) << "the session goes on";

  // The deadline that forget() armed ran out meanwhile; a task of the session that goes on still
  // runs.
  std::this_thread::sleep_for(std::chrono::seconds(9));
  std::atomic<bool> ran{false};
  std::atomic<bool> dropped{false};
  ASSERT_TRUE(h.owner().post_session_task("after", [&ran](QarSession *) { ran = true; },
      [&dropped] { dropped = true; }));
  ASSERT_TRUE(h.wait_for([&] { return ran.load() || dropped.load(); }));
  EXPECT_TRUE(ran.load());
  EXPECT_FALSE(dropped.load());
}

TEST(SessionOwnerTasks, TasksOfAnEarlierSessionNeverRun)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::mutex gate;
  gate.lock();
  std::atomic<bool> armed{true};
  std::atomic<bool> held{false};
  QarSessionOwnerTestAccess::set_task_post_hook(h.owner(), [&] {
        if (armed.load()) {
          held.store(true);
          std::lock_guard<std::mutex> wait(gate);
        }
      });
  std::atomic<bool> old_ran{false};
  std::atomic<bool> old_dropped{false};
  std::thread poster([&h, &old_ran, &old_dropped] {
        h.owner().post_session_task("old", [&old_ran](QarSession *) { old_ran = true; },
        [&old_dropped] { old_dropped = true; });
      });
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!held.load() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(held.load()) << "the post is accepted and held before it is queued";

  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(SessionState::kIdle));
  // A new session is up before the held post reaches the inbox: the task is of the old one.
  ASSERT_TRUE(h.connect_and_run());
  armed.store(false);
  gate.unlock();
  poster.join();
  ASSERT_TRUE(h.wait_for([&old_dropped] { return old_dropped.load(); }))
    << "the task of the released session is dropped";
  EXPECT_FALSE(old_ran.load()) << "it did not run on the new session";

  std::atomic<QarSession *> seen{nullptr};
  ASSERT_TRUE(h.owner().post_session_task("new", [&seen](QarSession * session) {
        seen.store(session);
      }));
  ASSERT_TRUE(h.wait_for([&seen] { return seen.load() != nullptr; }));
  EXPECT_EQ(seen.load(), h.owner().session()) << "a new task runs on the new session";
  EXPECT_FALSE(old_ran.load());
}

}  // namespace
