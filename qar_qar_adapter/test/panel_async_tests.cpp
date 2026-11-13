// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The URL panel adapter on the session owner's thread: every call the GUI thread makes returns at
// once, the QarOS calls run on the owner thread in the order they were asked for, and the
// outcomes come back through drain_results(). The owner runs on the fake QarOS library, whose
// calls are slowed with the call hook to the 2 seconds that the installed QAROS needs.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "qar_ar_core/common_name.hpp"
#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"

namespace
{

using qar_ar_core::PanelOpResult;  // read by panel_test_kit.hpp

}  // namespace

#include "panel_test_kit.hpp"

namespace
{

qar_ar_core::UrlPanelDesc url_desc(const std::string & name)
{
  qar_ar_core::UrlPanelDesc desc;
  desc.url = "http://127.0.0.1:9/first";
  desc.display_name = name;
  desc.common_name = qar_ar_core::make_panel_common_name(
    name, qar_ar_core::make_name_scope("test-program", "", "lab-pc"));
  return desc;
}

// ---------------------------------------------------------------------------------------------
// T4: browser panels
// ---------------------------------------------------------------------------------------------

TEST(UrlPanel, EveryConsumerCallReturnsWithinMilliseconds)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  slow_down(h);
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());

  std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter> adapter;
  EXPECT_LT(milliseconds_of([&] { adapter = factory.create_url_panel(url_desc("tools")); }), 50.0);
  ASSERT_NE(adapter, nullptr) << factory.last_error();
  EXPECT_TRUE(adapter->pending());

  bool accepted = false;
  EXPECT_LT(milliseconds_of([&] { accepted = adapter->navigate("http://127.0.0.1:9/second"); }),
    50.0);
  EXPECT_TRUE(accepted);
  qar_ar_core::UrlPanelPlacement placed;
  placed.size = QarGuiPanelSize{0.9f, 0.7f, 1.0f};
  EXPECT_LT(milliseconds_of([&] { accepted = adapter->set_placement(placed); }), 50.0);
  EXPECT_TRUE(accepted);
  EXPECT_LT(milliseconds_of([&] {
        accepted = adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN);
      }), 50.0);
  EXPECT_TRUE(accepted);

  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create) &&
        has_result(r, PanelOpResult::Op::Navigate) &&
        has_result(r, PanelOpResult::Op::Placement) && has_result(r, PanelOpResult::Op::State);
      }));
  EXPECT_FALSE(adapter->pending());

  EXPECT_LT(milliseconds_of([&] { adapter->close(); }), 50.0);
  ASSERT_TRUE(wait_until([&h] {
        const auto panels = h.fake().panels();
        return !panels.empty() && panels[0].state == QAR_GUI_PANEL_STATE_CLOSED;
      }));

  const auto create = indices_of(h, "gui_panels_get_or_create");
  const auto navigate = indices_of(h, "gui_panels_navigate_to_uri");
  const auto state = indices_of(h, "gui_panels_set_state");
  const auto close = indices_of(h, "gui_panels_close_panel");
  ASSERT_EQ(create.size(), 1u);
  ASSERT_EQ(navigate.size(), 1u);
  ASSERT_FALSE(state.empty());
  ASSERT_EQ(close.size(), 1u);
  EXPECT_LT(create[0], navigate[0]);
  EXPECT_LT(navigate[0], state[0]);
  EXPECT_LT(state.back(), close[0]);
  const auto calls = h.fake().calls();
  for (const int index : {create[0], navigate[0], state[0], close[0]}) {
    EXPECT_EQ(calls[static_cast<std::size_t>(index)].thread, h.owner_thread())
      << calls[static_cast<std::size_t>(index)].name;
  }
  const auto panels = h.fake().panels();
  ASSERT_EQ(panels.size(), 1u);
  EXPECT_EQ(panels[0].uri, "http://127.0.0.1:9/second");
  EXPECT_FLOAT_EQ(panels[0].size.width_meters, 0.9f);
}

TEST(UrlPanel, ADetachedPanelIsKeptAndReattachedByTheNextOne)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
  auto first = factory.create_url_panel(url_desc("tools"));
  ASSERT_NE(first, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_TRUE(first->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  first->detach();
  EXPECT_FALSE(first->set_state(QAR_GUI_PANEL_STATE_VISIBLE))
    << "a detached adapter queues nothing";
  first.reset();

  auto second = factory.create_url_panel(url_desc("tools"));
  ASSERT_NE(second, nullptr);
  std::vector<PanelOpResult> second_results;
  ASSERT_TRUE(drain_until(*second, second_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
  ASSERT_EQ(h.fake().panels().size(), 1u);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE) << "shown again";
  const auto created = std::find_if(second_results.begin(), second_results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::Create; });
  ASSERT_NE(created, second_results.end());
  EXPECT_TRUE(created->reattached);
}

// ---------------------------------------------------------------------------------------------
// The end of a session: hide, keep, release
// ---------------------------------------------------------------------------------------------

TEST(UrlPanel, HideThenDetachRightAwayHidesThePanelAndKeepsIt)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "gui_panels_get_or_create") {
        std::this_thread::sleep_for(200ms);
      }
    });
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
  auto adapter = factory.create_url_panel(url_desc("tools"));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 1; }));
  EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  adapter->detach();
  EXPECT_FALSE(adapter->set_state(QAR_GUI_PANEL_STATE_VISIBLE));
  ASSERT_TRUE(wait_until([&h] {
        const auto panels = h.fake().panels();
        return !panels.empty() && panels[0].state == QAR_GUI_PANEL_STATE_HIDDEN;
      }));
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
}

TEST(UrlPanel, AFailedStateChangeComesBackAsAResult)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
  auto adapter = factory.create_url_panel(url_desc("tools"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  h.fake().set_call_failure("gui_panels_set_state", QAR_STATUS_LOGIC_ERROR);
  EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN)) << "accepted";
  EXPECT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::State, false);
      }));
  EXPECT_FALSE(adapter->last_error().empty());
}

// ---------------------------------------------------------------------------------------------
// A reattach that fails midway leaves the panel of the earlier session alone
// ---------------------------------------------------------------------------------------------

TEST(UrlPanel, AReattachThatFailsInTheMiddleNeverClosesThePanelOfTheEarlierSession)
{
  for (const char * failing : {"gui_panels_navigate_to_uri", "gui_panels_update_pose"}) {
    SCOPED_TRACE(failing);
    Harness h;
    h.start();
    ASSERT_TRUE(h.connect_and_run());
    qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
    auto first = factory.create_url_panel(url_desc("tools"));
    ASSERT_NE(first, nullptr);
    std::vector<PanelOpResult> results;
    ASSERT_TRUE(drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
          return has_result(r, PanelOpResult::Op::Create);
        }));
    EXPECT_TRUE(first->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
    first->detach();
    first.reset();
    ASSERT_TRUE(wait_until([&h] {
          const auto panels = h.fake().panels();
          return !panels.empty() && panels[0].state == QAR_GUI_PANEL_STATE_HIDDEN;
        }));

    h.fake().set_call_failure(failing, QAR_STATUS_LOGIC_ERROR);
    qar_ar_core::UrlPanelDesc desc = url_desc("tools");
    desc.url = "http://127.0.0.1:9/other";  // another page: the reattach navigates
    auto second = factory.create_url_panel(desc);
    ASSERT_NE(second, nullptr);
    std::vector<PanelOpResult> second_results;
    ASSERT_TRUE(drain_until(*second, second_results, [](const std::vector<PanelOpResult> & r) {
          return has_result(r, PanelOpResult::Op::Create, false);
        })) << "the reattach step fails";

    second.reset();
    h.owner().disconnect();
    ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 15s));
    EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0)
      << "the panel of the earlier session is left alone";
    ASSERT_EQ(h.fake().panels().size(), 1u);
    EXPECT_NE(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_CLOSED);
  }
}

// ---------------------------------------------------------------------------------------------
// A panel that was never made costs nothing at the end of the session
// ---------------------------------------------------------------------------------------------

TEST(UrlPanel, ObsoleteCreatesDoNotKeepTheHideAndDetachOfARealPanelFromRunningInTime)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
  auto real = factory.create_url_panel(url_desc("real"));
  ASSERT_NE(real, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*real, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  slow_down(h);

  OwnerHold hold;
  ASSERT_TRUE(hold_owner(h, hold));
  std::vector<std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter>> obsolete;
  for (int i = 0; i < 4; ++i) {
    obsolete.push_back(factory.create_url_panel(url_desc("gone" + std::to_string(i))));
    ASSERT_NE(obsolete.back(), nullptr);
  }
  for (auto & adapter : obsolete) {
    EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
    adapter->detach();
  }
  EXPECT_TRUE(real->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  real->detach();
  h.owner().disconnect();
  const auto start = Clock::now();
  hold.release.store(true);
  ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 20s));
  const double took_s = std::chrono::duration<double>(Clock::now() - start).count();

  EXPECT_LT(took_s, 8.0) << "the hide ran within the drain budget";
  EXPECT_EQ(h.fake().count("gui_panels_get_or_create"), 1) << "no panel was made for a dropped one";
  ASSERT_EQ(h.fake().panels().size(), 1u);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_HIDDEN) << "the real panel was hidden";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
}

// ---------------------------------------------------------------------------------------------
// A panel that QarOS reports in the opposite state right after the adapter set it
// ---------------------------------------------------------------------------------------------

struct UrlKit
{
  using Adapter = qar_qar_adapter::QarCApiUrlPanelAdapter;
  using Factory = qar_qar_adapter::QarCApiUrlPanelFactory;
  static Factory factory(Harness & h) { return Factory(&h.owner()); }
  static bool set_state(Adapter & adapter, QarGuiPanelState state)
  {
    return adapter.set_state(state);
  }
  static std::shared_ptr<Adapter> make(Factory & factory, const std::string & name)
  {
    return std::static_pointer_cast<Adapter>(factory.create_url_panel(url_desc(name)));
  }
};

TEST(PanelStateGuard, AUrlPanelThatQarOsReportsHiddenAfterTheReattachIsShownAgain)
{
  expect_one_ask_for_a_contrary_report<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelIsAskedThreeTimesAtMostAndThenWarnedAbout)
{
  expect_at_most_three_asks_then_a_warning<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelIsNotAskedAgainForTheEchoOfTheStateBeforeAnAsk)
{
  expect_a_report_right_after_an_ask_not_to_be_asked_again<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelThatQarOsHidesAgainInsideTheHoldAndFallsSilentIsAskedAgain)
{
  expect_a_silent_runtime_to_be_asked_again_when_the_hold_ends<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelThatQarOsShowedAgainIsNotAskedWhenTheHoldEnds)
{
  expect_no_ask_after_the_hold_when_a_report_confirmed_the_state<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelAskedByTicksStaysWithinTheBudgetOfThreeAsks)
{
  expect_the_ticks_to_keep_the_budget<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelIsNotAskedByATickSixSecondsAfterTheChange)
{
  expect_a_tick_to_leave_the_operators_state_alone<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelCountsFromOneAgainAfterAStateItSetItself)
{
  expect_an_own_state_to_start_the_count_anew<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelReportedHiddenSixSecondsAfterTheChangeIsLeftAlone)
{
  expect_a_late_report_to_be_left_alone<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelNeverContestsClosedOrMinimized)
{
  expect_closed_and_minimized_to_be_left_alone<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelThatTheAdapterHidesIsNotShownAgain)
{
  expect_an_own_hide_not_to_be_shown_again<UrlKit>();
}

TEST(PanelStateGuard, AUrlPanelAsksForNothingWhileItsOwnStateTaskIsQueued)
{
  expect_nothing_to_be_asked_while_an_own_state_task_is_queued<UrlKit>();
}

TEST(PanelStateGuard, TheWatchOfAUrlPanelEndsWithItsClose)
{
  expect_the_watch_to_end_with_the_panel<UrlKit>(false);
}

TEST(PanelStateGuard, TheWatchOfAUrlPanelEndsWithItsDetach)
{
  expect_the_watch_to_end_with_the_panel<UrlKit>(true);
}

}  // namespace
