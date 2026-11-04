// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The helpers the panel adapter tests share: the slowed calls of the fake QarOS library, the waits,
// the owner-thread barrier and the checks of the panel state guard, written once for every kind of
// panel. A kind is a `Kit` (see the callers): `Kit::Adapter`, `Kit::Factory`, `Kit::factory(h)`,
// `Kit::make(factory, name)` and `Kit::set_state(adapter, state)`.
//
// The including file declares `PanelOpResult` in its unnamed namespace before it includes this
// header; the helpers read that name.

#ifndef QAR_QAR_ADAPTER__TEST__PANEL_TEST_KIT_HPP_
#define QAR_QAR_ADAPTER__TEST__PANEL_TEST_KIT_HPP_

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

#include "session_owner_harness.hpp"

namespace
{

using namespace std::chrono_literals;
using qar_qar_adapter::test::Harness;

using Clock = std::chrono::steady_clock;

/// The calls of a panel that took 2 seconds on the installed QAROS (the diagnosis measured the
/// queries; the others are assumed alike).
const std::set<std::string> & slow_calls()
{
  static const std::set<std::string> calls = {
    "gui_panels_get_or_create", "query_gui_panels_count", "query_gui_panels",
    "gui_panels_set_state", "gui_panels_update_pose", "gui_panels_change_size",
    "gui_panels_navigate_to_uri"};
  return calls;
}

void slow_down(Harness & h)
{
  h.fake().set_call_hook([](const std::string & name) {
      if (slow_calls().count(name) != 0) {
        std::this_thread::sleep_for(2000ms);
      }
    });
}

template<typename Fn>
double milliseconds_of(Fn && fn)
{
  const auto start = Clock::now();
  fn();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Calls drain_results() until @p done holds for what arrived or @p timeout passes.
template<typename Adapter>
bool drain_until(
  Adapter & adapter, std::vector<PanelOpResult> & results,
  const std::function<bool(const std::vector<PanelOpResult> &)> & done,
  std::chrono::milliseconds timeout = 30s)
{
  const auto end = Clock::now() + timeout;
  for (;;) {
    adapter.drain_results(results);
    if (done(results)) {
      return true;
    }
    if (Clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
}

bool has_result(
  const std::vector<PanelOpResult> & results, PanelOpResult::Op op, bool ok = true)
{
  return std::any_of(results.begin(), results.end(), [op, ok](const PanelOpResult & r) {
        return r.op == op && r.ok == ok;
      });
}

bool wait_until(const std::function<bool()> & predicate, std::chrono::milliseconds timeout = 30s)
{
  const auto end = Clock::now() + timeout;
  while (!predicate()) {
    if (Clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

std::vector<int> indices_of(const Harness & h, const std::string & name)
{
  std::vector<int> out;
  const auto calls = const_cast<Harness &>(h).fake().calls();
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (calls[i].name == name) {
      out.push_back(static_cast<int>(i));
    }
  }
  return out;
}

/// Holds the owner's thread until the returned flag is set.
struct OwnerHold
{
  std::atomic<bool> release{false};
  std::atomic<bool> held{false};
};

bool hold_owner(Harness & h, OwnerHold & hold)
{
  if (!h.owner().post_session_task("hold", [&hold](QarSession *) {
        hold.held.store(true);
        while (!hold.release.load()) {
          std::this_thread::sleep_for(1ms);
        }
      }))
  {
    return false;
  }
  return wait_until([&hold] { return hold.held.load(); });
}

/// Waits until the owner thread has run everything that was posted before this call.
bool owner_caught_up(Harness & h)
{
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> future = done->get_future();
  if (!h.owner().post_session_task(
      "barrier", [done](QarSession *) { done->set_value(); }, [done] { done->set_value(); }))
  {
    return false;
  }
  return future.wait_for(30s) == std::future_status::ready;
}

/// An adapter that found its panel again, hidden by an earlier session, and showed it.
template<typename Kit>
std::shared_ptr<typename Kit::Adapter> reattach_hidden_panel(
  Harness & h, typename Kit::Factory & factory, const std::string & name)
{
  auto first = Kit::make(factory, name);
  std::vector<PanelOpResult> results;
  if (first == nullptr || !drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }))
  {
    return nullptr;
  }
  Kit::set_state(*first, QAR_GUI_PANEL_STATE_HIDDEN);
  first->detach();
  first.reset();
  if (!wait_until([&h] {
        const auto panels = h.fake().panels();
        return !panels.empty() && panels[0].state == QAR_GUI_PANEL_STATE_HIDDEN;
      }))
  {
    return nullptr;
  }
  auto second = Kit::make(factory, name);
  std::vector<PanelOpResult> second_results;
  if (second == nullptr ||
    !drain_until(*second, second_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }) || !second->reattached())
  {
    return nullptr;
  }
  return second;
}

template<typename Kit>
void expect_one_ask_for_a_contrary_report()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  ASSERT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE) << "shown by the reattach";
  ASSERT_EQ(h.fake().panel_subscriptions(), 1) << "the watch of the reattached panel";
  const int before = h.fake().count("gui_panels_set_state");

  // QarOS reports the panel HIDDEN again, though the adapter called nothing in between.
  testing::internal::CaptureStderr();
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  const bool caught_up = owner_caught_up(h);
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "exactly one ask";
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_NE(log.find("QarOS reported HIDDEN while VISIBLE is wanted - asked again (1/3)"),
    std::string::npos) << log;
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(h.fake().last_index("gui_panels_set_state"))]
    .thread, h.owner_thread()) << "the ask runs on the owner thread";
}

template<typename Kit>
void expect_at_most_three_asks_then_a_warning()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  // Each report arrives 1.1 s after the one before, so none is taken for the echo of an ask.
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");

  testing::internal::CaptureStderr();
  bool caught_up = true;
  for (int i = 0; i < 6; ++i) {
    offset_ms->fetch_add(1100);
    h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
    caught_up = owner_caught_up(h) && caught_up;
  }
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 3) << "three asks, no more";
  for (const char * number : {"(1/3)", "(2/3)", "(3/3)"}) {
    EXPECT_NE(log.find(std::string("asked again ") + number), std::string::npos) << number << log;
  }
  EXPECT_EQ(log.find("(4/3)"), std::string::npos);
  const std::size_t warning = log.find("WARNING: QarOS keeps reporting HIDDEN while VISIBLE");
  ASSERT_NE(warning, std::string::npos) << log;
  EXPECT_EQ(log.find("WARNING", warning + 1), std::string::npos) << "reported once";
}

template<typename Kit>
void expect_a_report_right_after_an_ask_not_to_be_asked_again()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  const int before = h.fake().count("gui_panels_set_state");

  testing::internal::CaptureStderr();
  // The first report is asked about; the second one, a few milliseconds later, is the echo of the
  // state before the ask; VISIBLE confirms; a HIDDEN after the confirmation is a first ask again.
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  bool caught_up = owner_caught_up(h);
  const int after_first = h.fake().count("gui_panels_set_state");
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  caught_up = owner_caught_up(h) && caught_up;
  const int after_echo = h.fake().count("gui_panels_set_state");
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  caught_up = owner_caught_up(h) && caught_up;
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(after_first, before + 1);
  EXPECT_EQ(after_echo, before + 1) << "one ask for two reports a few milliseconds apart";
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 2);
  const std::string first = "asked again (1/3)";
  const std::size_t at = log.find(first);
  ASSERT_NE(at, std::string::npos) << log;
  EXPECT_NE(log.find(first, at + 1), std::string::npos) << "the count started anew: " << log;
  EXPECT_EQ(log.find("(2/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("WARNING"), std::string::npos) << log;
}

template<typename Kit>
void expect_a_silent_runtime_to_be_asked_again_when_the_hold_ends()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");
  std::vector<PanelOpResult> results;

  // QarOS hides the panel, is asked, hides it again inside the hold and then reports nothing more.
  testing::internal::CaptureStderr();
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  bool caught_up = owner_caught_up(h);
  offset_ms->store(100);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  caught_up = owner_caught_up(h) && caught_up;
  const int after_echo = h.fake().count("gui_panels_set_state");

  offset_ms->store(500);
  adapter->drain_results(results);
  caught_up = owner_caught_up(h) && caught_up;
  const int inside_hold = h.fake().count("gui_panels_set_state");

  offset_ms->store(1200);
  adapter->drain_results(results);
  caught_up = owner_caught_up(h) && caught_up;
  const int after_hold = h.fake().count("gui_panels_set_state");

  // Nothing was reported since the second ask: the next ticks ask nothing.
  offset_ms->store(2600);
  adapter->drain_results(results);
  adapter->drain_results(results);
  caught_up = owner_caught_up(h) && caught_up;
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(after_echo, before + 1) << "the report inside the hold is the echo";
  EXPECT_EQ(inside_hold, before + 1) << "a tick inside the hold asks nothing";
  EXPECT_EQ(after_hold, before + 2) << "the first tick after the hold asks again";
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 2) << "once, not on every tick";
  EXPECT_NE(log.find("asked again (1/3)"), std::string::npos) << log;
  EXPECT_NE(log.find("asked again (2/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("(3/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("WARNING"), std::string::npos) << log;
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(h.fake().last_index("gui_panels_set_state"))]
    .thread, h.owner_thread()) << "the ask runs on the owner thread, not on the tick";
}

template<typename Kit>
void expect_no_ask_after_the_hold_when_a_report_confirmed_the_state()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");
  std::vector<PanelOpResult> results;

  testing::internal::CaptureStderr();
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  bool caught_up = owner_caught_up(h);
  offset_ms->store(100);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  caught_up = owner_caught_up(h) && caught_up;
  offset_ms->store(1500);
  adapter->drain_results(results);
  offset_ms->store(3000);
  adapter->drain_results(results);
  caught_up = owner_caught_up(h) && caught_up;
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "the confirmed ask is the only";
  EXPECT_EQ(log.find("(2/3)"), std::string::npos) << log;
}

template<typename Kit>
void expect_the_ticks_to_keep_the_budget()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");
  std::vector<PanelOpResult> results;

  // Each ask is followed by a report inside its hold and then by silence; the ticks ask on.
  testing::internal::CaptureStderr();
  bool caught_up = true;
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  caught_up = owner_caught_up(h) && caught_up;
  for (long long step = 0; step < 3; ++step) {
    offset_ms->store(step * 1100 + 100);
    h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
    caught_up = owner_caught_up(h) && caught_up;
    offset_ms->store(step * 1100 + 1150);
    adapter->drain_results(results);
    caught_up = owner_caught_up(h) && caught_up;
  }
  offset_ms->store(4500);
  adapter->drain_results(results);
  caught_up = owner_caught_up(h) && caught_up;
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 3) << "three asks, no more";
  EXPECT_NE(log.find("asked again (3/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("(4/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("WARNING"), std::string::npos) << "a tick reports no give-up: " << log;
}

template<typename Kit>
void expect_a_tick_to_leave_the_operators_state_alone()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");
  std::vector<PanelOpResult> results;

  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  offset_ms->store(100);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  // The hold has ended, but the adapter's own change is six seconds old: the operator's doing.
  offset_ms->store(6000);
  adapter->drain_results(results);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "no ask 6 s after the change";
}

template<typename Kit>
void expect_an_own_state_to_start_the_count_anew()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");

  testing::internal::CaptureStderr();
  // Two asks, 1.1 s apart, that the runtime does not answer.
  bool caught_up = true;
  for (int i = 0; i < 2; ++i) {
    offset_ms->fetch_add(1100);
    h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
    caught_up = owner_caught_up(h) && caught_up;
  }
  // The adapter shows the panel itself, then the late report of an earlier hide arrives.
  offset_ms->fetch_add(1100);
  const int asked = h.fake().count("gui_panels_set_state");
  EXPECT_EQ(asked, before + 2);
  ASSERT_TRUE(Kit::set_state(*adapter, QAR_GUI_PANEL_STATE_VISIBLE));
  caught_up = wait_until([&h, asked] { return h.fake().count("gui_panels_set_state") > asked; }) &&
    caught_up;
  caught_up = owner_caught_up(h) && caught_up;
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  caught_up = owner_caught_up(h) && caught_up;
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);

  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 4);
  EXPECT_EQ(log.find("asked again (3/3)"), std::string::npos) << log;
  EXPECT_EQ(log.find("WARNING"), std::string::npos) << log;
  const std::size_t last = log.rfind("asked again (");
  ASSERT_NE(last, std::string::npos) << log;
  EXPECT_EQ(log.compare(last, std::string("asked again (1/3)").size(), "asked again (1/3)"), 0)
    << "the ask after the adapter's own state is the first one: " << log;
}

template<typename Kit>
void expect_a_late_report_to_be_left_alone()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  // The guard's clock runs `offset_ms` ahead of the steady clock.
  auto offset_ms = std::make_shared<std::atomic<long long>>(0);
  adapter->set_state_guard_clock([offset_ms] {
      return Clock::now() + std::chrono::milliseconds(offset_ms->load());
    });
  const int before = h.fake().count("gui_panels_set_state");

  // Six seconds after the adapter's last change: the operator's doing.
  offset_ms->store(6000);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before) << "no ask 6 s after the change";

  // Four seconds after it the same report is contested.
  offset_ms->store(4000);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "an ask 4 s after the change";
}

template<typename Kit>
void expect_closed_and_minimized_to_be_left_alone()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  const int before = h.fake().count("gui_panels_set_state");

  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_CLOSED);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_MINIMIZED);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before) << "wanted VISIBLE, reported CLOSED "
    "and MINIMIZED";

  // The caller itself minimises the panel: then a VISIBLE or HIDDEN report is not contested.
  ASSERT_TRUE(Kit::set_state(*adapter, QAR_GUI_PANEL_STATE_MINIMIZED));
  ASSERT_TRUE(wait_until([&h, before] { return h.fake().count("gui_panels_set_state") > before; }));
  ASSERT_TRUE(owner_caught_up(h));
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "only the caller's own call";
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_MINIMIZED);
}

template<typename Kit>
void expect_an_own_hide_not_to_be_shown_again()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);

  // The park of the panel: the adapter hides it itself.
  ASSERT_TRUE(Kit::set_state(*adapter, QAR_GUI_PANEL_STATE_HIDDEN));
  ASSERT_TRUE(wait_until([&h] {
        return h.fake().panels()[0].state == QAR_GUI_PANEL_STATE_HIDDEN;
      }));
  ASSERT_TRUE(owner_caught_up(h));
  const int before = h.fake().count("gui_panels_set_state");

  testing::internal::CaptureStderr();
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  const bool caught_up = owner_caught_up(h);
  const std::string log = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(caught_up);
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before)
    << "HIDDEN is what the adapter asked for";
  EXPECT_EQ(log.find("asked again"), std::string::npos) << log;
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_HIDDEN) << "never shown again";
}

template<typename Kit>
void expect_nothing_to_be_asked_while_an_own_state_task_is_queued()
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  auto adapter = reattach_hidden_panel<Kit>(h, factory, "dock");
  ASSERT_NE(adapter, nullptr);
  const int before = h.fake().count("gui_panels_set_state");
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "gui_panels_set_state") {
        std::this_thread::sleep_for(300ms);
      }
    });

  // The hide is queued and not done; a report of VISIBLE may be older than it.
  ASSERT_TRUE(Kit::set_state(*adapter, QAR_GUI_PANEL_STATE_HIDDEN));
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "only the hide";
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_HIDDEN);
}

template<typename Kit>
void expect_the_watch_to_end_with_the_panel(bool detach)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  typename Kit::Factory factory = Kit::factory(h);
  const int tokens = h.fake().live_tokens();
  auto adapter = Kit::make(factory, "dock");
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  ASSERT_EQ(h.fake().panel_subscriptions(), 1);
  EXPECT_EQ(h.fake().live_tokens(), tokens + 1);
  const std::string name = adapter->common_name();
  const int before = h.fake().count("gui_panels_set_state");

  if (detach) {
    adapter->detach();
  } else {
    adapter->close();
  }
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().panel_subscriptions(), 0) << "the subscription is cancelled";
  EXPECT_EQ(h.fake().live_tokens(), tokens) << "and its token released";

  // A report after the end asks for nothing (and reaches nobody).
  h.fake().deliver_panel_state(name, QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before);
}

}  // namespace

#endif  // QAR_QAR_ADAPTER__TEST__PANEL_TEST_KIT_HPP_
