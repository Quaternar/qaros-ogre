// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The supervisor's policy and its hints, as pure values; and the clock that tells the supervisor
// which QarOS call the owner thread is inside. No thread of the owner, no QarOS call.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/onboarding_status.hpp"
#include "qar_qar_adapter/owner_watch.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

namespace
{

using qar_qar_adapter::HubHints;
using qar_qar_adapter::OutagePolicy;
using qar_qar_adapter::OutageTimings;
using qar_qar_adapter::WatchInput;
using qar_qar_adapter::WatchVerdict;
using namespace std::chrono_literals;

OutageTimings timings()
{
  OutageTimings t;
  t.call_stall = 10000ms;
  t.task_stall = 30000ms;
  t.lost_after = 20000ms;
  t.lost_after_hint = 5000ms;
  t.abandon_after = 45000ms;
  return t;
}

WatchInput joined_input()
{
  WatchInput in;
  in.joined = true;
  return in;
}

TEST(OutagePolicy, BelowTheStallLimitNothingHappens)
{
  const OutagePolicy policy(timings());
  WatchInput in = joined_input();
  in.call = "qar_query_gui_panels";
  in.call_ms = 9999;
  in.task = "url-create:QAR control";
  in.task_ms = 29999;
  const WatchVerdict verdict = policy.judge(in);
  EXPECT_FALSE(verdict.call_stalled);
  EXPECT_FALSE(verdict.lost_due);
  EXPECT_FALSE(verdict.abandon_due);
  EXPECT_FALSE(verdict.rejoin_now);
  EXPECT_TRUE(verdict.stalled_in.empty());

  const WatchVerdict idle = policy.judge(joined_input());
  EXPECT_FALSE(idle.call_stalled) << "no call and no task: nothing stalls";
}

TEST(OutagePolicy, TheCallStallSetsTheBitAndNamesTheCall)
{
  const OutagePolicy policy(timings());
  WatchInput in = joined_input();
  in.call = "qar_query_gui_panels";
  in.call_ms = 10000;
  WatchVerdict verdict = policy.judge(in);
  EXPECT_TRUE(verdict.call_stalled);
  EXPECT_EQ(verdict.stalled_in, "qar_query_gui_panels");
  EXPECT_EQ(verdict.stalled_ms, 10000);

  WatchInput by_task = joined_input();
  by_task.task = "url-create:QAR control";
  by_task.task_ms = 30000;
  verdict = policy.judge(by_task);
  EXPECT_TRUE(verdict.call_stalled);
  EXPECT_EQ(verdict.stalled_in, "task url-create:QAR control");

  WatchInput both = joined_input();
  both.call = "qar_query_gui_panels";
  both.call_ms = 12000;
  both.task = "url-create:QAR control";
  both.task_ms = 31000;
  EXPECT_EQ(policy.judge(both).stalled_in, "qar_query_gui_panels") << "the call is named first";
}

TEST(OutagePolicy, LostIsDueAfterTheSilenceLasted)
{
  const OutagePolicy policy(timings());
  WatchInput in = joined_input();
  in.silence = qar_qar_adapter::kSilenceHubPeersGone;
  in.silent_ms = 19999;
  EXPECT_FALSE(policy.judge(in).lost_due);
  in.silent_ms = 20000;
  EXPECT_TRUE(policy.judge(in).lost_due);

  WatchInput stalled = joined_input();
  stalled.silence = qar_qar_adapter::kSilenceCallStalled;
  stalled.silent_ms = 25000;
  EXPECT_TRUE(policy.judge(stalled).lost_due);

  WatchInput hints_only = joined_input();
  hints_only.silence =
    qar_qar_adapter::kSilenceEndpointGone | qar_qar_adapter::kSilenceHubProcessGone;
  hints_only.silent_ms = 60000;
  EXPECT_FALSE(policy.judge(hints_only).lost_due) << "a hint alone never makes the Hub silent";

  WatchInput not_joined = in;
  not_joined.joined = false;
  not_joined.silent_ms = 60000;
  EXPECT_FALSE(policy.judge(not_joined).lost_due);
}

TEST(OutagePolicy, AHintShortensTheWait)
{
  const OutagePolicy policy(timings());
  WatchInput in = joined_input();
  in.silence = qar_qar_adapter::kSilenceHubPeersGone;
  in.silent_ms = 5000;
  EXPECT_FALSE(policy.judge(in).lost_due);
  in.endpoint_gone_long = true;
  EXPECT_TRUE(policy.judge(in).lost_due);
  in.endpoint_gone_long = false;
  in.hub_process_gone_long = true;
  EXPECT_TRUE(policy.judge(in).lost_due);
  in.silent_ms = 4999;
  EXPECT_FALSE(policy.judge(in).lost_due);
}

TEST(OutagePolicy, AHintThatTheHubIsBackAsksForAnAttemptOnlyWhileLost)
{
  const OutagePolicy policy(timings());
  WatchInput in;
  in.hub_came_back = true;
  EXPECT_FALSE(policy.judge(in).rejoin_now);
  in.lost = true;
  EXPECT_TRUE(policy.judge(in).rejoin_now);
}

TEST(OutagePolicy, AbandonIsDueOnlyForACallNotForATask)
{
  const OutagePolicy policy(timings());
  WatchInput task_only = joined_input();
  task_only.task = "url-create:QAR control";
  task_only.task_ms = 600000;
  EXPECT_FALSE(policy.judge(task_only).abandon_due);

  WatchInput call = joined_input();
  call.call = "qar_session_handle_destroy";
  call.call_ms = 44999;
  EXPECT_FALSE(policy.judge(call).abandon_due);
  call.call_ms = 45000;
  EXPECT_TRUE(policy.judge(call).abandon_due);
  call.joined = false;
  EXPECT_TRUE(policy.judge(call).abandon_due) << "a call that never returns counts in any state";
}

HubHints present_hints(std::int64_t mtime, const std::string & written)
{
  HubHints hints;
  hints.endpoint_known = true;
  hints.endpoint_present = true;
  hints.endpoint_mtime = mtime;
  hints.endpoint_written = written;
  return hints;
}

TEST(HubHints, AMissingEndpointFileIsReportedOnce)
{
  HubHints before = present_hints(5, "10:00:00");
  HubHints missing;
  missing.endpoint_known = true;
  const std::vector<std::string> first = qar_qar_adapter::describe_hint_changes(before, missing);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0], "hint: hub endpoint file missing");
  EXPECT_TRUE(qar_qar_adapter::describe_hint_changes(missing, missing).empty())
    << "an unchanged hint is silent";
}

TEST(HubHints, AChangedMtimeIsReported)
{
  const HubHints before = present_hints(5, "10:00:00");
  const HubHints later = present_hints(9, "10:00:07");
  const std::vector<std::string> lines = qar_qar_adapter::describe_hint_changes(before, later);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "hint: hub endpoint file present (written 10:00:07)");
  EXPECT_TRUE(qar_qar_adapter::describe_hint_changes(later, later).empty());

  HubHints missing;
  missing.endpoint_known = true;
  const std::vector<std::string> back = qar_qar_adapter::describe_hint_changes(missing, later);
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0], "hint: hub endpoint file present (written 10:00:07)");
}

TEST(HubHints, ProcessChangesAreDescribedByStemAndPid)
{
  HubHints before;
  before.processes_known = true;
  before.hub_pid = 4242;
  before.viz_pid = 77;
  HubHints now = before;
  now.hub_pid = 0;
  now.gui_pid = 900;
  const std::vector<std::string> lines = qar_qar_adapter::describe_hint_changes(before, now);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "process qaros-hub-service gone (pid 4242)");
  EXPECT_EQ(lines[1], "process qar-stereokit-gui running (pid 900)");

  const std::vector<std::string> back = qar_qar_adapter::describe_hint_changes(now, before);
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[0], "process qaros-hub-service running (pid 4242)");
  EXPECT_EQ(back[1], "process qar-stereokit-gui gone (pid 900)");

  const std::vector<std::string> first =
    qar_qar_adapter::describe_hint_changes(HubHints(), before);
  ASSERT_EQ(first.size(), 2u) << "the first reading names what runs, and names nothing as gone";
  EXPECT_EQ(first[0], "process qaros-hub-service running (pid 4242)");
  EXPECT_EQ(first[1], "process qar-streaming-viz running (pid 77)");
}

TEST(OnboardingStatus, TheOwnersOwnCausesReadInPlainWords)
{
  using qar_qar_adapter::OnboardingContext;
  const qar_qar_adapter::StatusText lost = qar_qar_adapter::describe_status(
    qar_qar_adapter::kStatusSessionLost, OnboardingContext::kLocal, nullptr);
  EXPECT_EQ(lost.symbol, "LOST");
  EXPECT_NE(lost.message.find("joins the Hub again by itself"), std::string::npos) << lost.message;
  EXPECT_TRUE(lost.retry_offered);

  const qar_qar_adapter::StatusText left = qar_qar_adapter::describe_status(
    qar_qar_adapter::kStatusOwnerAbandoned, OnboardingContext::kLocal, nullptr);
  EXPECT_EQ(left.symbol, "OWNER_ABANDONED");
  EXPECT_NE(left.message.find("restart RViz to stream again"), std::string::npos) << left.message;
}

TEST(OwnerCallClock, NamesTheCallThatTheMarkedThreadIsInside)
{
  qar_qar_adapter::OwnerCallClock clock;
  std::string seen_inside;
  std::string seen_nested;
  std::thread worker([&clock, &seen_inside, &seen_nested] {
      qar_qar_adapter::mark_owner_thread({}, &clock);
      {
        QAR_GUI_TIMED("qar_outer_call");
        const char * outer = clock.call.load();
        seen_inside = outer != nullptr ? outer : "";
        {
          QAR_GUI_TIMED("qar_inner_call");
          const char * inner = clock.call.load();
          seen_nested = inner != nullptr ? inner : "";
        }
        const char * after = clock.call.load();
        seen_nested += std::string("|") + (after != nullptr ? after : "");
      }
      qar_qar_adapter::mark_owner_thread({}, nullptr);
    });
  worker.join();
  EXPECT_EQ(seen_inside, "qar_outer_call");
  EXPECT_EQ(seen_nested, "qar_inner_call|qar_outer_call")
    << "a call inside a timed scope hides the outer one until it returns";
  EXPECT_EQ(clock.call.load(), nullptr) << "no call is left behind";
}

TEST(OwnerCallClock, AnUnmarkedThreadWritesNothing)
{
  qar_qar_adapter::OwnerCallClock clock;
  QAR_GUI_TIMED("qar_unmarked_call");
  EXPECT_EQ(clock.call.load(), nullptr);
}

}  // namespace
