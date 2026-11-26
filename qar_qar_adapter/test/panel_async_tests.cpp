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

TEST(UrlPanel, ANewPanelIsMadeWithOnePanelListQueryAndItsOutcomeIsNotAReattach)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiUrlPanelFactory factory(&h.owner());
  const int queries_before = h.fake().count("query_gui_panels");
  auto adapter = factory.create_url_panel(url_desc("tools"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  ASSERT_TRUE(owner_caught_up(h));
  const auto created = std::find_if(results.begin(), results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::Create; });
  ASSERT_NE(created, results.end());
  EXPECT_TRUE(created->ok);
  EXPECT_FALSE(created->reattached) << "the consumer reads the runtime record after a reattach";
  EXPECT_EQ(h.fake().count("query_gui_panels") - queries_before, 1)
    << "the probe's list query is the only one of a new panel";
  EXPECT_TRUE(adapter->runtime_record().empty()) << "the create reads no record by itself";
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

TEST(UrlPanel, AVisiblePanelWithAnotherPageIsHiddenBeforeItsPageChangesAndShownAfter)
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
  // The earlier session ended without the hide: the panel is left on show with its old page.
  first->detach();
  first.reset();
  ASSERT_TRUE(owner_caught_up(h));
  ASSERT_EQ(h.fake().panels().size(), 1u);
  ASSERT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE);
  const int calls_before = static_cast<int>(h.fake().calls().size());
  const int queries_before = h.fake().count("query_gui_panels");

  qar_ar_core::UrlPanelDesc other = url_desc("tools");
  other.url = "http://127.0.0.1:9/second";
  auto second = factory.create_url_panel(other);
  ASSERT_NE(second, nullptr);
  std::vector<PanelOpResult> second_results;
  ASSERT_TRUE(drain_until(*second, second_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));

  // set_state(HIDDEN) < navigate_to_uri < set_state(VISIBLE), among the calls of the second create.
  const auto calls = h.fake().calls();
  int hidden_at = -1;
  int navigate_at = -1;
  int shown_at = -1;
  int sets = 0;
  for (std::size_t i = static_cast<std::size_t>(calls_before); i < calls.size(); ++i) {
    if (calls[i].name == "gui_panels_set_state") {
      ++sets;
      (sets == 1 ? hidden_at : shown_at) = static_cast<int>(i);
    } else if (calls[i].name == "gui_panels_navigate_to_uri") {
      navigate_at = static_cast<int>(i);
    }
  }
  ASSERT_GE(hidden_at, 0);
  ASSERT_GE(navigate_at, 0);
  ASSERT_GE(shown_at, 0);
  EXPECT_LT(hidden_at, navigate_at) << "hidden before the page changes";
  EXPECT_LT(navigate_at, shown_at) << "shown after it";
  EXPECT_EQ(sets, 2);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_EQ(h.fake().panels()[0].uri, other.url);
  EXPECT_EQ(h.fake().count("query_gui_panels") - queries_before, 1)
    << "one panel list query for the whole create";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
}

TEST(UrlPanel, TheRuntimeRecordIsReadOnceOnTheOwnerThreadAndNothingIsChanged)
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
  EXPECT_TRUE(adapter->runtime_record().empty()) << "nothing read yet";
  ASSERT_TRUE(owner_caught_up(h));
  const int calls_before = static_cast<int>(h.fake().calls().size());
  const int queries_before = h.fake().count("query_gui_panels");

  EXPECT_TRUE(adapter->log_runtime_record());
  ASSERT_TRUE(wait_until([&adapter] { return !adapter->runtime_record().empty(); }));
  ASSERT_TRUE(owner_caught_up(h));

  EXPECT_EQ(h.fake().count("query_gui_panels") - queries_before, 1) << "one panel list query";
  const std::string record = adapter->runtime_record();
  EXPECT_NE(record.find("state=VISIBLE"), std::string::npos) << record;
  EXPECT_NE(record.find("pose=("), std::string::npos) << record;
  EXPECT_NE(record.find("size=0.50x0.75"), std::string::npos) << record;
  EXPECT_NE(record.find("url_matches=y"), std::string::npos) << record;
  EXPECT_EQ(record.find("http"), std::string::npos) << "never the URL";
  // Only reads: no call that changes the panel, and the query ran on the owner thread.
  const auto calls = h.fake().calls();
  for (std::size_t i = static_cast<std::size_t>(calls_before); i < calls.size(); ++i) {
    EXPECT_EQ(calls[i].name.find("gui_panels_set_"), std::string::npos) << calls[i].name;
    EXPECT_EQ(calls[i].name.find("gui_panels_update_"), std::string::npos) << calls[i].name;
    EXPECT_EQ(calls[i].name.find("gui_panels_navigate"), std::string::npos) << calls[i].name;
    EXPECT_EQ(calls[i].thread, h.owner_thread()) << calls[i].name;
  }

  adapter->detach();
  EXPECT_FALSE(adapter->log_runtime_record()) << "a detached adapter queues nothing";
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

// ---------------------------------------------------------------------------------------------
// A panel that the operator closed in AR
// ---------------------------------------------------------------------------------------------

TEST(UrlPanelClosedInAr, TheOperatorsCloseIsSeenAndALaterStateClearsIt)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  UrlKit::Factory factory = UrlKit::factory(h);
  auto adapter = reattach_hidden_panel<UrlKit>(h, factory, "tools");
  ASSERT_NE(adapter, nullptr);
  EXPECT_FALSE(adapter->closed_in_ar()) << "a panel that was shown is not closed";
  const int calls_before = h.fake().count("gui_panels_set_state");
  const int closes_before = h.fake().count("gui_panels_close_panel");

  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_TRUE(adapter->closed_in_ar());
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), calls_before) << "nothing is asked again";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), closes_before) << "nothing is closed";

  // Another report of the same close keeps it; a report of another state clears it.
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_TRUE(adapter->closed_in_ar());
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_MINIMIZED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_FALSE(adapter->closed_in_ar());
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_TRUE(adapter->closed_in_ar());
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_FALSE(adapter->closed_in_ar());
}

TEST(UrlPanelClosedInAr, AnOwnHideOrCloseNeverCountsAndAnOwnShowClearsOnlyWithTheReport)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  UrlKit::Factory factory = UrlKit::factory(h);
  auto adapter = reattach_hidden_panel<UrlKit>(h, factory, "tools");
  ASSERT_NE(adapter, nullptr);

  // The adapter's own hide is not a close.
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  ASSERT_TRUE(owner_caught_up(h));
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_FALSE(adapter->closed_in_ar());

  // A close the operator made, then the adapter's own show: the show is asked of QarOS and the
  // flag stays until the runtime reports a state other than CLOSED.
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_TRUE(adapter->closed_in_ar());
  const int sets_before_show = h.fake().count("gui_panels_set_state");
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_VISIBLE));
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), sets_before_show + 1) << "the show was asked";
  EXPECT_TRUE(adapter->closed_in_ar()) << "the show alone does not clear the close";
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_FALSE(adapter->closed_in_ar()) << "the runtime's report of the shown panel clears it";

  // A close the operator made, then the adapter's own close: the row is gone, nothing is left.
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_TRUE(adapter->closed_in_ar());
  const std::string name = adapter->common_name();
  adapter->close();
  EXPECT_FALSE(adapter->closed_in_ar());
  ASSERT_TRUE(wait_until([&h] {
        const auto panels = h.fake().panels();
        return !panels.empty() && panels[0].state == QAR_GUI_PANEL_STATE_CLOSED;
      }));
  h.fake().deliver_panel_state(name, QAR_GUI_PANEL_STATE_CLOSED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_FALSE(adapter->closed_in_ar()) << "the runtime's report of our own close is ignored";
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
