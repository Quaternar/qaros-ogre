// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The panel adapters on the session owner's thread: every call the GUI thread makes returns at
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
#include "qar_qar_adapter/qar_capi_panel_input_source.hpp"
#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"
#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"
#include "session_owner_harness.hpp"

namespace
{

using namespace std::chrono_literals;
using qar_ar_core::PanelOpResult;
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

qar_ar_core::PanelDesc stream_desc(const std::string & key)
{
  qar_ar_core::PanelDesc desc;
  desc.key = key;
  desc.display_name = key;
  desc.common_name = qar_ar_core::make_panel_common_name(
    key, qar_ar_core::make_name_scope("test-program", "", "lab-pc"));
  desc.buffer_width = 64;
  desc.buffer_height = 48;
  desc.placement.size = QarGuiPanelSize{0.4f, 0.3f, 1.0f};
  return desc;
}

qar_ar_core::UrlPanelDesc url_desc(const std::string & name)
{
  qar_ar_core::UrlPanelDesc desc;
  desc.url = "http://127.0.0.1:9/first";
  desc.display_name = name;
  desc.common_name = qar_ar_core::make_panel_common_name(
    name, qar_ar_core::make_name_scope("test-program", "", "lab-pc"));
  return desc;
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

// ---------------------------------------------------------------------------------------------
// T3: streamed panels
// ---------------------------------------------------------------------------------------------

TEST(PanelSender, EveryConsumerCallReturnsWithinMilliseconds)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  slow_down(h);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  const auto test_thread = std::this_thread::get_id();

  std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> adapter;
  EXPECT_LT(milliseconds_of([&] { adapter = factory.create_panel_sender(stream_desc("dock")); }),
    50.0);
  ASSERT_NE(adapter, nullptr) << factory.last_error();
  EXPECT_TRUE(adapter->pending());

  bool ready = true;
  bool begun = false;
  EXPECT_LT(milliseconds_of([&] { begun = adapter->try_begin_frame(ready); }), 50.0);
  EXPECT_TRUE(begun);
  EXPECT_FALSE(ready);
  EXPECT_EQ(adapter->not_ready_cause(), qar_ar_core::PanelNotReadyCause::Creating);

  qar_ar_core::PanelPlacement placed;
  placed.size = QarGuiPanelSize{0.8f, 0.6f, 1.0f};
  bool accepted = false;
  EXPECT_LT(milliseconds_of([&] { accepted = adapter->set_placement(placed); }), 50.0);
  EXPECT_TRUE(accepted);
  EXPECT_LT(milliseconds_of([&] {
        accepted = adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN);
      }), 50.0);
  EXPECT_TRUE(accepted);

  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create) &&
        has_result(r, PanelOpResult::Op::Placement) && has_result(r, PanelOpResult::Op::State);
      })) << "the outcomes arrive within 30 s";
  EXPECT_FALSE(adapter->pending());
  EXPECT_FALSE(adapter->stream_id_text().empty());
  const auto state_result = std::find_if(results.begin(), results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::State; });
  ASSERT_NE(state_result, results.end());
  EXPECT_EQ(state_result->state, QAR_GUI_PANEL_STATE_HIDDEN);

  qar_ar_core::PanelCloseReport report;
  EXPECT_LT(milliseconds_of([&] { report = adapter->close(false); }), 50.0);
  EXPECT_EQ(report.panel, qar_ar_core::PanelCloseOutcome::Queued);
  EXPECT_FALSE(report.sender_destroyed) << "the close is only queued";
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));

  const auto create = indices_of(h, "gui_panels_get_or_create");
  const auto state = indices_of(h, "gui_panels_set_state");
  const auto close = indices_of(h, "gui_panels_close_panel");
  const auto destroy = indices_of(h, "video_sender_handle_destroy");
  ASSERT_EQ(create.size(), 1u);
  ASSERT_FALSE(state.empty());
  ASSERT_EQ(close.size(), 1u);
  ASSERT_EQ(destroy.size(), 1u);
  EXPECT_LT(create[0], state[0]);
  EXPECT_LT(state.back(), close[0]);
  EXPECT_LT(close[0], destroy[0]);

  const auto calls = h.fake().calls();
  for (const int index : {create[0], state[0], close[0], destroy[0]}) {
    EXPECT_EQ(calls[static_cast<std::size_t>(index)].thread, h.owner_thread())
      << calls[static_cast<std::size_t>(index)].name;
    EXPECT_NE(calls[static_cast<std::size_t>(index)].thread, test_thread);
  }
  const auto panels = h.fake().panels();
  ASSERT_EQ(panels.size(), 1u);
  EXPECT_EQ(panels[0].state, QAR_GUI_PANEL_STATE_CLOSED);
  EXPECT_FLOAT_EQ(panels[0].size.width_meters, 0.8f) << "the placement reached the panel";
}

TEST(PanelSender, ASessionThatIsNotRunningRefusesThePanel)
{
  Harness h;
  h.start();
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  EXPECT_EQ(factory.create_panel_sender(stream_desc("dock")), nullptr);
  EXPECT_FALSE(factory.last_error().empty());
  EXPECT_EQ(h.fake().count("gui_panels_get_or_create"), 0);
}

TEST(PanelSender, AFailedCreateComesBackAsAResultAndThePanelStaysUnusable)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_failure("gui_panels_get_or_create", QAR_STATUS_LOGIC_ERROR);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);

  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create, false);
      }));
  EXPECT_FALSE(adapter->pending());
  bool ready = true;
  EXPECT_FALSE(adapter->try_begin_frame(ready));
  EXPECT_FALSE(ready);
  EXPECT_NE(adapter->last_error().find("gui_panels_get_or_create"), std::string::npos);
  EXPECT_FALSE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  // The sender made before the panel is released with the adapter.
  adapter.reset();
  EXPECT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
}

TEST(PanelSender, AFrameIsShownOnceThePanelIsReadyAndNeverOnTheOwnerThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_video_sender_ready(true);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));

  std::vector<std::uint8_t> pixels(64u * 48u * 4u, 0x7F);
  qar_ar_core::PanelPixels frame;
  frame.data = pixels.data();
  frame.width = 64;
  frame.height = 48;
  frame.stride_bytes = 64u * 4u;
  bool ready = false;
  ASSERT_TRUE(adapter->try_begin_frame(ready));
  ASSERT_TRUE(ready);
  ASSERT_TRUE(adapter->submit_frame(frame)) << adapter->last_error();
  EXPECT_EQ(h.fake().video_frames_shown(), 1);

  const auto calls = h.fake().calls();
  for (const auto & call : calls) {
    if (call.name == "video_sender_try_begin_frame" || call.name == "video_sender_frame_cpu" ||
      call.name == "video_sender_show_frame")
    {
      EXPECT_EQ(call.thread, std::this_thread::get_id()) << call.name << " stays on the GUI thread";
    }
  }
}

TEST(PanelSender, ThePanelInputIsSubscribedOnTheOwnerThreadAndEndedBeforeTheClose)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto input = std::make_shared<qar_qar_adapter::QarCApiPanelInputSource>(h.owner().session());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner(), input);
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_TRUE(input->has_panel("dock"));

  adapter->close(false);
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
  EXPECT_FALSE(input->has_panel("dock"));

  const auto subscribed = h.fake().first_index("gui_panels_subscribe_all_gesture_events");
  ASSERT_GE(subscribed, 0);
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(subscribed)].thread, h.owner_thread());
  EXPECT_LT(h.fake().last_index("gui_panels_subscribe_panel_updates"),
    h.fake().last_index("gui_panels_close_panel"));
  EXPECT_LT(h.fake().last_index("cancel_token_cancel"),
    h.fake().last_index("gui_panels_close_panel")) << "input first, then the panel";
  adapter.reset();
}

// ---------------------------------------------------------------------------------------------
// T9: a resize swaps the stream of the same panel
// ---------------------------------------------------------------------------------------------

TEST(PanelSender, ResizeSwapDetachRunsBeforeTheNewCreate)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto old_adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(old_adapter, nullptr);
  std::vector<PanelOpResult> old_results;
  ASSERT_TRUE(drain_until(*old_adapter, old_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));

  // The swap: the old stream lets go, a new one is made for the same panel, back to back.
  old_adapter->detach();
  auto new_adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(new_adapter, nullptr);
  std::vector<PanelOpResult> new_results;
  ASSERT_TRUE(drain_until(*new_adapter, new_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));

  const auto destroy = indices_of(h, "video_sender_handle_destroy");
  const auto create = indices_of(h, "gui_panels_get_or_create");
  ASSERT_EQ(destroy.size(), 1u) << "the old sender is gone, the new one lives";
  ASSERT_EQ(create.size(), 2u);
  EXPECT_LT(destroy[0], create[1]) << "the detach runs before the new panel is found";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0) << "the panel is kept";
  ASSERT_EQ(h.fake().panels().size(), 1u);
  const auto created = std::find_if(new_results.begin(), new_results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::Create; });
  ASSERT_NE(created, new_results.end());
  EXPECT_TRUE(created->reattached) << "the second adapter found the panel of the first";
  EXPECT_EQ(h.fake().live_video_senders(), 1);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE);
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

TEST(PanelSender, HideThenDetachRightAwayHidesThePanelAndKeepsIt)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "gui_panels_get_or_create") {
        std::this_thread::sleep_for(200ms);
      }
    });
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  // The create is under way (it makes the panel): the hide and the detach queue behind it, and
  // both run on the panel it made.
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 1; }));
  EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  adapter->detach();
  EXPECT_FALSE(adapter->set_state(QAR_GUI_PANEL_STATE_VISIBLE))
    << "nothing is queued after a detach";
  ASSERT_TRUE(wait_until([&h] {
        return h.fake().count("video_sender_handle_destroy") == 1 &&
        h.fake().live_video_senders() == 0;
      }));
  const auto panels = h.fake().panels();
  ASSERT_EQ(panels.size(), 1u);
  EXPECT_EQ(panels[0].state, QAR_GUI_PANEL_STATE_HIDDEN) << "the hide ran before the detach";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0) << "the panel is kept";
  EXPECT_LT(h.fake().first_index("gui_panels_set_state"),
    h.fake().first_index("video_sender_handle_destroy"));
}

TEST(PanelSender, TheEndOfASessionHidesAndDetachesBeforeTheSessionIsReleased)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto input = std::make_shared<qar_qar_adapter::QarCApiPanelInputSource>(h.owner().session());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner(), input);
  auto first = factory.create_panel_sender(stream_desc("dock"));
  auto second = factory.create_panel_sender(stream_desc("tools"));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  ASSERT_TRUE(drain_until(*second, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create) && r.size() >= 2;
      }));

  // What the panel does at the end: hide, detach, then the owner lets go of the session.
  EXPECT_TRUE(first->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  first->detach();
  EXPECT_TRUE(second->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  second->detach();
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 15s));

  for (const auto & panel : h.fake().panels()) {
    EXPECT_EQ(panel.state, QAR_GUI_PANEL_STATE_HIDDEN) << panel.common_name;
  }
  EXPECT_EQ(h.fake().panels().size(), 2u);
  EXPECT_EQ(h.fake().live_video_senders(), 0);
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
  const int released = h.fake().first_index("session_handle_destroy");
  ASSERT_GE(released, 0);
  EXPECT_LT(h.fake().last_index("video_sender_handle_destroy"), released);
  EXPECT_LT(h.fake().last_index("gui_panels_set_state"), released);
  EXPECT_LT(h.fake().last_index("cancel_token_cancel"), released + 1);
}

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

TEST(PanelSender, AFailedStateChangeComesBackAsAResult)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  h.fake().set_call_failure("gui_panels_set_state", QAR_STATUS_LOGIC_ERROR);
  EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN)) << "accepted";
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::State, false);
      }));
  const auto failed = std::find_if(results.begin(), results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::State && !r.ok; });
  ASSERT_NE(failed, results.end());
  EXPECT_EQ(failed->state, QAR_GUI_PANEL_STATE_HIDDEN);
  EXPECT_NE(failed->error.find("gui_panels_set_state"), std::string::npos);
}

TEST(PanelSender, OfSeveralPlacementsWhileTheThreadIsBusyOnlyTheLastIsApplied)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  std::atomic<bool> release{false};
  std::atomic<bool> held{false};
  ASSERT_TRUE(h.owner().post_session_task("hold", [&](QarSession *) {
        held.store(true);
        while (!release.load()) {
          std::this_thread::sleep_for(1ms);
        }
      }));
  ASSERT_TRUE(wait_until([&held] { return held.load(); }));
  for (int i = 1; i <= 5; ++i) {
    qar_ar_core::PanelPlacement placement;
    placement.size = QarGuiPanelSize{0.1f * static_cast<float>(i), 0.3f, 1.0f};
    EXPECT_TRUE(adapter->set_placement(placement));
  }
  release.store(true);
  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Placement);
      }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(h.fake().count("gui_panels_update_pose"), 1) << "five placements make one task";
  EXPECT_FLOAT_EQ(h.fake().panels()[0].size.width_meters, 0.5f);
}

// ---------------------------------------------------------------------------------------------
// A reattach that fails midway leaves the panel of the earlier session alone
// ---------------------------------------------------------------------------------------------

/// A first adapter makes the panel "dock" and lets go of it (hidden, kept), the way the end of a
/// session leaves it.
void leave_stream_panel_behind(Harness & h, qar_qar_adapter::QarCApiPanelSenderFactory & factory)
{
  auto first = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(first, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_TRUE(first->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  first->detach();
  first.reset();
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
  ASSERT_EQ(h.fake().panels().size(), 1u);
  ASSERT_EQ(h.fake().count("gui_panels_close_panel"), 0);
}

TEST(PanelSender, AReattachThatFailsInTheMiddleNeverClosesThePanelOfTheEarlierSession)
{
  for (const char * failing : {"gui_panels_set_stream", "gui_panels_update_pose"}) {
    SCOPED_TRACE(failing);
    Harness h;
    h.start();
    ASSERT_TRUE(h.connect_and_run());
    qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
    ASSERT_NO_FATAL_FAILURE(leave_stream_panel_behind(h, factory));

    h.fake().set_call_failure(failing, QAR_STATUS_LOGIC_ERROR);
    auto second = factory.create_panel_sender(stream_desc("dock"));
    ASSERT_NE(second, nullptr);
    std::vector<PanelOpResult> results;
    ASSERT_TRUE(drain_until(*second, results, [](const std::vector<PanelOpResult> & r) {
          return has_result(r, PanelOpResult::Op::Create, false);
        })) << "the reattach step fails";
    EXPECT_NE(second->last_error().find(failing), std::string::npos);

    // What the consumer does with a failed adapter: it drops it.
    second.reset();
    ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
    h.owner().disconnect();
    ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 15s));
    EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0)
      << "the panel of the earlier session is left alone";
    ASSERT_EQ(h.fake().panels().size(), 1u);
    EXPECT_NE(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_CLOSED);
  }
}

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
// The frames do not wait for the attach probe
// ---------------------------------------------------------------------------------------------

/// The measured profile of the installed QAROS: the two queries of the attach probe take 2
/// seconds, the calls that make and change a panel are fast.
void slow_panel_queries(Harness & h)
{
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "query_gui_panels_count" || name == "query_gui_panels") {
        std::this_thread::sleep_for(2000ms);
      }
    });
}

/// One frame through the adapter, the way the GUI thread does it. True when it was shown.
bool show_one_frame(
  qar_ar_core::IQarPanelSenderAdapter & adapter, const std::vector<std::uint8_t> & data)
{
  bool ready = false;
  if (!adapter.try_begin_frame(ready) || !ready) {
    return false;
  }
  qar_ar_core::PanelPixels frame;
  frame.data = data.data();
  frame.width = 64;
  frame.height = 48;
  frame.stride_bytes = 64u * 4u;
  return adapter.submit_frame(frame);
}

TEST(PanelSender, FramesFlowWhileTheAttachProbeRuns)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_video_sender_ready(true);
  slow_panel_queries(h);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto adapter = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 1; }, 5s));
  const auto panel_made = Clock::now();

  // The panel exists; its attach probe is still in the first slow query.
  std::vector<std::uint8_t> pixels(64u * 48u * 4u, 0x7F);
  ASSERT_TRUE(wait_until([&] { return !adapter->pending() && show_one_frame(*adapter, pixels); },
      1500ms)) << "a frame is shown while the probe runs";
  const double after_ms =
    std::chrono::duration<double, std::milli>(Clock::now() - panel_made).count();
  EXPECT_LT(after_ms, 300.0);
  EXPECT_EQ(h.fake().video_frames_shown(), 1);

  std::vector<PanelOpResult> results;
  adapter->drain_results(results);
  EXPECT_FALSE(has_result(results, PanelOpResult::Op::Create))
    << "the probe has not finished: no result yet";

  ASSERT_TRUE(drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_TRUE(show_one_frame(*adapter, pixels)) << "frames go on after the probe";
  for (const auto & call : h.fake().calls()) {
    if (call.name == "video_sender_try_begin_frame" || call.name == "video_sender_frame_cpu" ||
      call.name == "video_sender_show_frame")
    {
      EXPECT_EQ(call.thread, std::this_thread::get_id()) << call.name << " stays on the GUI thread";
    }
  }
}

TEST(PanelSender, FramesFlowBeforeTheReattachSwapsTheStreamAndGoOnAfterIt)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_video_sender_ready(true);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  ASSERT_NO_FATAL_FAILURE(leave_stream_panel_behind(h, factory));
  const int frames_before = h.fake().video_frames_shown();
  slow_panel_queries(h);

  auto second = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 2; }, 5s));
  std::vector<std::uint8_t> pixels(64u * 48u * 4u, 0x7F);
  ASSERT_TRUE(wait_until([&] { return !second->pending() && show_one_frame(*second, pixels); },
      1500ms)) << "a frame is shown before the probe has read the earlier panel";
  EXPECT_EQ(h.fake().count("gui_panels_set_stream"), 0) << "the stream is not swapped yet";

  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*second, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  const auto created = std::find_if(results.begin(), results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::Create; });
  ASSERT_NE(created, results.end());
  EXPECT_TRUE(created->reattached);
  const int swapped = h.fake().last_index("gui_panels_set_stream");
  const int shown = h.fake().last_index("gui_panels_set_state");
  ASSERT_GE(swapped, 0);
  EXPECT_GT(shown, swapped) << "the stream is swapped, then the panel is shown";
  const auto calls = h.fake().calls();
  EXPECT_EQ(calls[static_cast<std::size_t>(swapped)].thread, h.owner_thread());
  EXPECT_EQ(calls[static_cast<std::size_t>(shown)].thread, h.owner_thread());
  ASSERT_EQ(h.fake().panels().size(), 1u);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_VISIBLE);
  EXPECT_TRUE(h.fake().panels()[0].has_stream);
  EXPECT_FALSE(second->pending());
  EXPECT_TRUE(show_one_frame(*second, pixels)) << "the same sender goes on after the swap";
  EXPECT_EQ(h.fake().video_frames_shown(), frames_before + 2);
}

TEST(PanelSender, AReattachThatFailsAfterTheFramesOpenedStopsTheFrames)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_video_sender_ready(true);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  ASSERT_NO_FATAL_FAILURE(leave_stream_panel_behind(h, factory));
  h.fake().set_call_failure("gui_panels_set_stream", QAR_STATUS_LOGIC_ERROR);
  slow_panel_queries(h);

  auto second = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 2; }, 5s));
  std::vector<std::uint8_t> pixels(64u * 48u * 4u, 0x7F);
  ASSERT_TRUE(wait_until([&] { return !second->pending() && show_one_frame(*second, pixels); },
      1500ms)) << "frames flow while the probe runs";

  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*second, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create, false);
      })) << "the swap fails";
  EXPECT_FALSE(second->pending());
  bool ready = true;
  EXPECT_FALSE(second->try_begin_frame(ready)) << "a failed panel takes no more frames";
  EXPECT_FALSE(ready);
  EXPECT_NE(second->last_error().find("gui_panels_set_stream"), std::string::npos);

  second.reset();
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
  h.owner().disconnect();
  ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 15s));
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0)
    << "the panel of the earlier session is left alone";
}

TEST(PanelSender, ANameTakenByAClosedPanelKeepsTheFramesOpenAcrossTheRetry)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_video_sender_ready(true);
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto first = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(first, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*first, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  first->close(false);
  first.reset();
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
  slow_panel_queries(h);

  auto second = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("gui_panels_get_or_create") >= 2; }, 5s));
  std::vector<std::uint8_t> pixels(64u * 48u * 4u, 0x7F);
  ASSERT_TRUE(wait_until([&] { return !second->pending() && show_one_frame(*second, pixels); },
      1500ms));

  std::vector<PanelOpResult> second_results;
  ASSERT_TRUE(drain_until(*second, second_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_EQ(h.fake().count("gui_panels_get_or_create"), 3) << "the name with a suffix is made";
  EXPECT_EQ(h.fake().panels().size(), 2u);
  EXPECT_TRUE(show_one_frame(*second, pixels)) << "the same sender serves the new panel";
}

TEST(PanelSender, TheProbeAsksOnceAndFindsOurPanelInALongList)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  // 70 panels of other makers come first in the list: ours is beyond the first 64 handles.
  ASSERT_TRUE(h.owner().post_session_task("fill", [](QarSession * session) {
        for (int i = 0; i < 70; ++i) {
          const std::string name = "filler-" + std::to_string(i);
          QarGuiPanelInit init = qar_gui_panel_init_default();
          init.common_name = name.c_str();
          QarGuiPanelId id{};
          qar_gui_panels_get_or_create(session, &init, &id);
        }
      }));
  ASSERT_TRUE(wait_until([&h] { return h.fake().panels().size() == 70u; }));
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto first = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(first, nullptr);
  std::vector<PanelOpResult> first_results;
  ASSERT_TRUE(drain_until(*first, first_results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  EXPECT_TRUE(first->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  first->detach();
  first.reset();
  ASSERT_TRUE(wait_until([&h] { return h.fake().live_video_senders() == 0; }));
  ASSERT_EQ(h.fake().panels().size(), 71u);
  auto second = factory.create_panel_sender(stream_desc("dock"));
  ASSERT_NE(second, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*second, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  const auto created = std::find_if(results.begin(), results.end(),
      [](const PanelOpResult & r) { return r.op == PanelOpResult::Op::Create; });
  ASSERT_NE(created, results.end());
  EXPECT_TRUE(created->reattached) << "the panel of the earlier session was found in the list";
  EXPECT_EQ(h.fake().count("query_gui_panels_count"), 0) << "no count is asked for";
  EXPECT_GE(h.fake().count("query_gui_panels"), 2) << "the first handle buffer was too short";
  EXPECT_EQ(h.fake().panels()[70].state, QAR_GUI_PANEL_STATE_VISIBLE);
}

// ---------------------------------------------------------------------------------------------
// A panel that was never made costs nothing at the end of the session
// ---------------------------------------------------------------------------------------------

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

TEST(PanelSender, ObsoleteCreatesDoNotKeepTheHideAndDetachOfARealPanelFromRunningInTime)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(&h.owner());
  auto real = factory.create_panel_sender(stream_desc("real"));
  ASSERT_NE(real, nullptr);
  std::vector<PanelOpResult> results;
  ASSERT_TRUE(drain_until(*real, results, [](const std::vector<PanelOpResult> & r) {
        return has_result(r, PanelOpResult::Op::Create);
      }));
  slow_down(h);

  // Four panels are asked for and dropped before their turn comes, behind a busy thread.
  OwnerHold hold;
  ASSERT_TRUE(hold_owner(h, hold));
  std::vector<std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter>> obsolete;
  for (int i = 0; i < 4; ++i) {
    obsolete.push_back(factory.create_panel_sender(stream_desc("gone" + std::to_string(i))));
    ASSERT_NE(obsolete.back(), nullptr);
  }
  for (auto & adapter : obsolete) {
    EXPECT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
    adapter->detach();
  }
  // The end of the session: hide and detach the real panel, then let go of the session.
  EXPECT_TRUE(real->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  real->detach();
  h.owner().disconnect();
  const auto start = Clock::now();
  hold.release.store(true);
  ASSERT_TRUE(h.wait_state(qar_qar_adapter::SessionState::kIdle, 20s));
  const double took_s = std::chrono::duration<double>(Clock::now() - start).count();

  EXPECT_LT(took_s, 8.0) << "the hide and the detach ran within the drain budget";
  EXPECT_EQ(h.fake().count("gui_panels_get_or_create"), 1) << "no panel was made for a dropped one";
  EXPECT_EQ(h.fake().count("video_sender_create"), 1);
  ASSERT_EQ(h.fake().panels().size(), 1u);
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_HIDDEN) << "the real panel was hidden";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), 0);
  EXPECT_EQ(h.fake().live_video_senders(), 0);
}

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

/// The panel kinds the guard is tested on: the adapter type and how to make one.
struct SenderKit
{
  using Adapter = qar_qar_adapter::QarCApiPanelSenderAdapter;
  using Factory = qar_qar_adapter::QarCApiPanelSenderFactory;
  static Factory factory(Harness & h) { return Factory(&h.owner()); }
  static std::shared_ptr<Adapter> make(Factory & factory, const std::string & name)
  {
    return std::static_pointer_cast<Adapter>(factory.create_panel_sender(stream_desc(name)));
  }
};

struct UrlKit
{
  using Adapter = qar_qar_adapter::QarCApiUrlPanelAdapter;
  using Factory = qar_qar_adapter::QarCApiUrlPanelFactory;
  static Factory factory(Harness & h) { return Factory(&h.owner()); }
  static std::shared_ptr<Adapter> make(Factory & factory, const std::string & name)
  {
    return std::static_pointer_cast<Adapter>(factory.create_url_panel(url_desc(name)));
  }
};

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
  first->set_state(QAR_GUI_PANEL_STATE_HIDDEN);
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

TEST(PanelStateGuard, AStreamPanelThatQarOsReportsHiddenAfterTheReattachIsShownAgain)
{
  expect_one_ask_for_a_contrary_report<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelThatQarOsReportsHiddenAfterTheReattachIsShownAgain)
{
  expect_one_ask_for_a_contrary_report<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelIsAskedThreeTimesAtMostAndThenWarnedAbout)
{
  expect_at_most_three_asks_then_a_warning<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelIsAskedThreeTimesAtMostAndThenWarnedAbout)
{
  expect_at_most_three_asks_then_a_warning<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelIsNotAskedAgainForTheEchoOfTheStateBeforeAnAsk)
{
  expect_a_report_right_after_an_ask_not_to_be_asked_again<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelIsNotAskedAgainForTheEchoOfTheStateBeforeAnAsk)
{
  expect_a_report_right_after_an_ask_not_to_be_asked_again<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelThatQarOsHidesAgainInsideTheHoldAndFallsSilentIsAskedAgain)
{
  expect_a_silent_runtime_to_be_asked_again_when_the_hold_ends<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelThatQarOsHidesAgainInsideTheHoldAndFallsSilentIsAskedAgain)
{
  expect_a_silent_runtime_to_be_asked_again_when_the_hold_ends<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelThatQarOsShowedAgainIsNotAskedWhenTheHoldEnds)
{
  expect_no_ask_after_the_hold_when_a_report_confirmed_the_state<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelThatQarOsShowedAgainIsNotAskedWhenTheHoldEnds)
{
  expect_no_ask_after_the_hold_when_a_report_confirmed_the_state<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelAskedByTicksStaysWithinTheBudgetOfThreeAsks)
{
  expect_the_ticks_to_keep_the_budget<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelAskedByTicksStaysWithinTheBudgetOfThreeAsks)
{
  expect_the_ticks_to_keep_the_budget<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelIsNotAskedByATickSixSecondsAfterTheChange)
{
  expect_a_tick_to_leave_the_operators_state_alone<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelIsNotAskedByATickSixSecondsAfterTheChange)
{
  expect_a_tick_to_leave_the_operators_state_alone<UrlKit>();
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
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_VISIBLE));
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

TEST(PanelStateGuard, AStreamPanelCountsFromOneAgainAfterAStateItSetItself)
{
  expect_an_own_state_to_start_the_count_anew<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelCountsFromOneAgainAfterAStateItSetItself)
{
  expect_an_own_state_to_start_the_count_anew<UrlKit>();
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

TEST(PanelStateGuard, AStreamPanelReportedHiddenSixSecondsAfterTheChangeIsLeftAlone)
{
  expect_a_late_report_to_be_left_alone<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelReportedHiddenSixSecondsAfterTheChangeIsLeftAlone)
{
  expect_a_late_report_to_be_left_alone<UrlKit>();
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
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_MINIMIZED));
  ASSERT_TRUE(wait_until([&h, before] { return h.fake().count("gui_panels_set_state") > before; }));
  ASSERT_TRUE(owner_caught_up(h));
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_HIDDEN);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "only the caller's own call";
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_MINIMIZED);
}

TEST(PanelStateGuard, AStreamPanelNeverContestsClosedOrMinimized)
{
  expect_closed_and_minimized_to_be_left_alone<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelNeverContestsClosedOrMinimized)
{
  expect_closed_and_minimized_to_be_left_alone<UrlKit>();
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
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
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

TEST(PanelStateGuard, AStreamPanelThatTheAdapterHidesIsNotShownAgain)
{
  expect_an_own_hide_not_to_be_shown_again<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelThatTheAdapterHidesIsNotShownAgain)
{
  expect_an_own_hide_not_to_be_shown_again<UrlKit>();
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
  ASSERT_TRUE(adapter->set_state(QAR_GUI_PANEL_STATE_HIDDEN));
  h.fake().deliver_panel_state(adapter->common_name(), QAR_GUI_PANEL_STATE_VISIBLE);
  ASSERT_TRUE(owner_caught_up(h));
  EXPECT_EQ(h.fake().count("gui_panels_set_state"), before + 1) << "only the hide";
  EXPECT_EQ(h.fake().panels()[0].state, QAR_GUI_PANEL_STATE_HIDDEN);
}

TEST(PanelStateGuard, AStreamPanelAsksForNothingWhileItsOwnStateTaskIsQueued)
{
  expect_nothing_to_be_asked_while_an_own_state_task_is_queued<SenderKit>();
}

TEST(PanelStateGuard, AUrlPanelAsksForNothingWhileItsOwnStateTaskIsQueued)
{
  expect_nothing_to_be_asked_while_an_own_state_task_is_queued<UrlKit>();
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

TEST(PanelStateGuard, TheWatchOfAStreamPanelEndsWithItsClose)
{
  expect_the_watch_to_end_with_the_panel<SenderKit>(false);
}

TEST(PanelStateGuard, TheWatchOfAStreamPanelEndsWithItsDetach)
{
  expect_the_watch_to_end_with_the_panel<SenderKit>(true);
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
