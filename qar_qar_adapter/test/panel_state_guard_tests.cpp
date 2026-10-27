// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The decision of PanelStateGuard over scripted times: when a report of the runtime that
// contradicts the state the adapter asked for is contested, and when it is left alone.

#include <gtest/gtest.h>

#include <chrono>

#include "qar_qar_adapter/panel_state_guard.hpp"

namespace
{

using namespace std::chrono_literals;
using qar_qar_adapter::PanelStateGuard;
using Kind = PanelStateGuard::Decision::Kind;

const PanelStateGuard::Clock::time_point kStart{};

// A guard whose panel was made at kStart and wants it VISIBLE.
PanelStateGuard made_guard()
{
  PanelStateGuard guard;
  guard.created(kStart);
  return guard;
}

TEST(PanelStateDecision, ANewPanelWantsToBeVisibleAndAHiddenReportIsContested)
{
  PanelStateGuard guard = made_guard();
  const auto decision = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 40ms);
  EXPECT_EQ(decision.kind, Kind::kAsk);
  EXPECT_EQ(decision.wanted, QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_EQ(decision.seen, QAR_GUI_PANEL_STATE_HIDDEN);
  EXPECT_EQ(decision.number, 1u);
}

TEST(PanelStateDecision, AReportThatAgreesWithTheWantedStateAsksForNothing)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 10ms).kind, Kind::kNone);
}

TEST(PanelStateDecision, NothingIsAskedBeforeThePanelWasMade)
{
  PanelStateGuard guard;
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart).kind, Kind::kNone);
}

TEST(PanelStateDecision, AReportMoreThanFiveSecondsAfterTheOwnChangeIsTheOperatorsAndLeftAlone)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 6s).kind, Kind::kNone);
  // The edge: still contested at five seconds.
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 5s).kind, Kind::kAsk);
}

TEST(PanelStateDecision, MinimizedAndClosedAreNeverContestedInEitherDirection)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_MINIMIZED, kStart + 10ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_CLOSED, kStart + 20ms).kind, Kind::kNone);

  guard.request(QAR_GUI_PANEL_STATE_MINIMIZED);
  guard.request_done(kStart + 100ms);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 110ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 120ms).kind, Kind::kNone);
}

TEST(PanelStateDecision, AHideTheAdapterAskedForIsNotContestedAndAShowReportIsAskedAgain)
{
  PanelStateGuard guard = made_guard();
  guard.request(QAR_GUI_PANEL_STATE_HIDDEN);
  guard.request_done(kStart + 100ms);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 110ms).kind, Kind::kNone);
  const auto decision = guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 120ms);
  EXPECT_EQ(decision.kind, Kind::kAsk);
  EXPECT_EQ(decision.wanted, QAR_GUI_PANEL_STATE_HIDDEN);
}

TEST(PanelStateDecision, NothingIsAskedWhileAStateTaskOfTheAdapterIsQueued)
{
  PanelStateGuard guard = made_guard();
  guard.request(QAR_GUI_PANEL_STATE_HIDDEN);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 10ms).kind, Kind::kNone);
  guard.request_dropped();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 20ms).kind, Kind::kAsk);
}

TEST(PanelStateDecision, ASecondAskWaitsForTheFirstTaskToEnd)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1500ms).kind, Kind::kNone)
    << "the task of the first ask has not ended";
  guard.ask_done();
  const auto second = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1600ms);
  EXPECT_EQ(second.kind, Kind::kAsk);
  EXPECT_EQ(second.number, 2u);
}

TEST(PanelStateDecision, AContraryReportRightAfterAnAskIsTheEchoOfTheOldStateAndNotAskedAgain)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  // Six milliseconds after the ask: the ask is not answered yet.
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 16ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 500ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1009ms).kind, Kind::kNone);
  // The hold is one second: after it the same report is a new reason.
  const auto again = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1010ms);
  EXPECT_EQ(again.kind, Kind::kAsk);
  EXPECT_EQ(again.number, 2u);
}

TEST(PanelStateDecision, AReportThatConfirmsTheWantedStateEndsTheHoldAndStartsTheCountAnew)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 16ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 20ms).kind, Kind::kNone);
  // A late echo of the old state after the confirmation is a first ask, not a second one.
  const auto late = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 25ms);
  EXPECT_EQ(late.kind, Kind::kAsk);
  EXPECT_EQ(late.number, 1u);
}

TEST(PanelStateDecision, AskThatWereConfirmedDoNotCountTowardTheGiveUp)
{
  PanelStateGuard guard = made_guard();
  for (unsigned round = 0; round < 3; ++round) {
    const auto base = kStart + 1500ms * round;
    // Two asks that go unconfirmed, then a confirmation.
    ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, base + 100ms).kind, Kind::kAsk);
    guard.ask_done();
    const auto second = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, base + 1100ms);
    ASSERT_EQ(second.kind, Kind::kAsk) << round;
    EXPECT_EQ(second.number, 2u) << round;
    guard.ask_done();
    EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, base + 1200ms).kind, Kind::kNone);
  }
  // Six asks in all, none of them reported as a failure: the count restarted each time.
  const auto last = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 4800ms);
  EXPECT_EQ(last.kind, Kind::kAsk);
  EXPECT_EQ(last.number, 1u);
}

TEST(PanelStateDecision, AStateTheAdapterSetsItselfStartsTheCountAnew)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1500ms).kind, Kind::kAsk);
  guard.ask_done();
  // Both asks are still inside the window of ten seconds when the adapter shows the panel again;
  // the late report of an earlier hide must be a first ask and no reason to give up.
  guard.request(QAR_GUI_PANEL_STATE_VISIBLE);
  guard.request_done(kStart + 9600ms);
  const auto first = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 9603ms);
  EXPECT_EQ(first.kind, Kind::kAsk);
  EXPECT_EQ(first.number, 1u);
  guard.ask_done();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 9609ms).kind, Kind::kNone);
}

TEST(PanelStateDecision, ANewWantedStateEndsTheHoldOfTheOldOne)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  guard.request(QAR_GUI_PANEL_STATE_HIDDEN);
  guard.request_done(kStart + 100ms);
  // The adapter hid the panel itself; a VISIBLE report right after is contested at once.
  const auto decision = guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 110ms);
  EXPECT_EQ(decision.kind, Kind::kAsk);
  EXPECT_EQ(decision.number, 1u);
}

TEST(PanelStateDecision, AtMostThreeAsksWithinTenSecondsThenOneGiveUp)
{
  PanelStateGuard guard = made_guard();
  for (unsigned i = 1; i <= 3; ++i) {
    const auto decision = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1100ms * i);
    ASSERT_EQ(decision.kind, Kind::kAsk) << i;
    EXPECT_EQ(decision.number, i);
    guard.ask_done();
  }
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 4400ms).kind, Kind::kGaveUp);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 4500ms).kind, Kind::kNone)
    << "the give-up is reported once";
}

TEST(PanelStateDecision, AfterThreeAsksAnOwnChangeLetsTheGuardAskAgain)
{
  PanelStateGuard guard = made_guard();
  for (unsigned i = 1; i <= 3; ++i) {
    ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 1100ms * i).kind, Kind::kAsk);
    guard.ask_done();
  }
  // The adapter changes the panel again at 12 s; a contrary report 100 ms later is contested
  // because the earlier asks do not count any more.
  guard.request(QAR_GUI_PANEL_STATE_VISIBLE);
  guard.request_done(kStart + 12s);
  const auto decision = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 12100ms);
  EXPECT_EQ(decision.kind, Kind::kAsk);
  EXPECT_EQ(decision.number, 1u);
}

TEST(PanelStateDecision, AnOwnChangeDatesTheWindowAgain)
{
  PanelStateGuard guard = made_guard();
  guard.request(QAR_GUI_PANEL_STATE_HIDDEN);
  guard.request_done(kStart + 20s);
  // 3 s after the hide: contested; 6 s after it: not.
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 23s).kind, Kind::kAsk);
  guard.ask_done();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 26s).kind, Kind::kNone);
}

TEST(PanelStateDecision, AStateRequestedBeforeTheCreationEndedIsNotOverwrittenByIt)
{
  PanelStateGuard guard;
  guard.request(QAR_GUI_PANEL_STATE_HIDDEN);
  guard.created(kStart);
  EXPECT_EQ(guard.wanted(), QAR_GUI_PANEL_STATE_HIDDEN);
  guard.request_done(kStart + 10ms);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 20ms).kind, Kind::kNone);
}

TEST(PanelStatePoll, ABusyRuntimeThatHidesThePanelAgainInsideTheHoldAndGoesSilentIsAskedAgain)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 16ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 500ms).kind, Kind::kNone) << "the hold has not ended";
  EXPECT_EQ(guard.poll(kStart + 1009ms).kind, Kind::kNone) << "one millisecond short";
  const auto again = guard.poll(kStart + 1010ms);
  ASSERT_EQ(again.kind, Kind::kAsk);
  EXPECT_EQ(again.number, 2u);
  EXPECT_EQ(again.wanted, QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_EQ(again.seen, QAR_GUI_PANEL_STATE_HIDDEN);
  EXPECT_EQ(guard.poll(kStart + 1011ms).kind, Kind::kNone) << "its task has not ended";
  guard.ask_done();
  EXPECT_EQ(guard.poll(kStart + 2500ms).kind, Kind::kNone)
    << "nothing was reported since the second ask";
}

TEST(PanelStatePoll, AReportThatConfirmsTheWantedStateLeavesNothingToAskAgain)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 16ms).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 20ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 1100ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 3s).kind, Kind::kNone);
}

TEST(PanelStatePoll, NothingIsAskedBeforeAnyReportOrAfterAnAskThatNothingFollowed)
{
  PanelStateGuard guard;
  EXPECT_EQ(guard.poll(kStart).kind, Kind::kNone) << "no panel yet";
  guard.created(kStart);
  EXPECT_EQ(guard.poll(kStart + 2s).kind, Kind::kNone) << "no report yet";
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 2100ms).kind, Kind::kAsk);
  guard.ask_done();
  EXPECT_EQ(guard.poll(kStart + 4s).kind, Kind::kNone) << "the ask replaced the report";
}

TEST(PanelStatePoll, ATaskOfTheAdapterOrOfTheGuardKeepsPollQuiet)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 20ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 2s).kind, Kind::kNone) << "the ask task is still running";
  guard.ask_done();
  guard.request(QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_EQ(guard.poll(kStart + 2100ms).kind, Kind::kNone) << "a state task of the adapter";
  guard.request_done(kStart + 2200ms);
  EXPECT_EQ(guard.poll(kStart + 4s).kind, Kind::kNone)
    << "the report is older than the adapter's own change";
  const auto first = guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 4100ms);
  EXPECT_EQ(first.kind, Kind::kAsk);
  EXPECT_EQ(first.number, 1u);
}

TEST(PanelStatePoll, ThreeAsksAtMostAndPollNeverReportsTheGiveUp)
{
  PanelStateGuard guard = made_guard();
  ASSERT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  guard.ask_done();
  for (unsigned number = 2; number <= 3; ++number) {
    const auto asked = kStart + 10ms + 1000ms * (number - 2);  // the time of the ask before
    EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, asked + 90ms).kind, Kind::kNone);
    const auto again = guard.poll(asked + 1000ms);
    ASSERT_EQ(again.kind, Kind::kAsk) << number;
    EXPECT_EQ(again.number, number);
    guard.ask_done();
  }
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 2100ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 3010ms).kind, Kind::kNone) << "three asks are the most";
  EXPECT_EQ(guard.poll(kStart + 3020ms).kind, Kind::kNone);
  // The give-up belongs to a report that arrives.
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 3500ms).kind, Kind::kGaveUp);
}

TEST(PanelStatePoll, MoreThanFiveSecondsAfterTheOwnChangeIsTheOperatorsAndLeftAlone)
{
  PanelStateGuard late = made_guard();
  ASSERT_EQ(late.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  late.ask_done();
  EXPECT_EQ(late.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 20ms).kind, Kind::kNone);
  EXPECT_EQ(late.poll(kStart + 5001ms).kind, Kind::kNone);

  PanelStateGuard edge = made_guard();
  ASSERT_EQ(edge.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 10ms).kind, Kind::kAsk);
  edge.ask_done();
  EXPECT_EQ(edge.on_record(QAR_GUI_PANEL_STATE_HIDDEN, kStart + 20ms).kind, Kind::kNone);
  EXPECT_EQ(edge.poll(kStart + 5s).kind, Kind::kAsk) << "still contested at five seconds";
}

TEST(PanelStatePoll, MinimizedAndClosedAreNeverAskedAbout)
{
  PanelStateGuard guard = made_guard();
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_MINIMIZED, kStart + 10ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 2s).kind, Kind::kNone);
  EXPECT_EQ(guard.on_record(QAR_GUI_PANEL_STATE_CLOSED, kStart + 2100ms).kind, Kind::kNone);
  EXPECT_EQ(guard.poll(kStart + 4s).kind, Kind::kNone);

  PanelStateGuard minimized = made_guard();
  minimized.request(QAR_GUI_PANEL_STATE_MINIMIZED);
  minimized.request_done(kStart + 100ms);
  EXPECT_EQ(minimized.on_record(QAR_GUI_PANEL_STATE_VISIBLE, kStart + 110ms).kind, Kind::kNone);
  EXPECT_EQ(minimized.poll(kStart + 2s).kind, Kind::kNone);
}

}  // namespace
