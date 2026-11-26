// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The URL panel adapter when its session is lost: closing or detaching a panel makes no call of
// the Hub on the calling thread, and the watch of the panel's state, which holds a token of the
// session, is ended on the release lane of the owner. The owner runs on the fake QarOS library.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_ar_core/common_name.hpp"
#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"

namespace
{

using qar_ar_core::PanelOpResult;  // read by panel_test_kit.hpp

}  // namespace

#include "panel_test_kit.hpp"

namespace
{

using qar_qar_adapter::SessionState;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::returns_within;

qar_ar_core::UrlPanelDesc url_desc(const std::string & name)
{
  qar_ar_core::UrlPanelDesc desc;
  desc.url = "http://127.0.0.1:9/first";
  desc.display_name = name;
  desc.common_name = qar_ar_core::make_panel_common_name(
    name, qar_ar_core::make_name_scope("test-program", "", "lab-pc"));
  return desc;
}

// A connected owner with a URL panel that exists and is watched, and then a lost session.
struct LostPanel
{
  bool make()
  {
    h.start();
    if (!h.connect_and_run()) {
      return false;
    }
    factory = std::make_unique<qar_qar_adapter::QarCApiUrlPanelFactory>(&h.owner());
    adapter = factory->create_url_panel(url_desc("tools"));
    if (adapter == nullptr) {
      return false;
    }
    std::vector<PanelOpResult> results;
    return drain_until(*adapter, results, [](const std::vector<PanelOpResult> & r) {
          return has_result(r, PanelOpResult::Op::Create);
        }, 10s) &&
           wait_until([this] { return h.fake().panel_subscriptions() == 1; }, 10s);
  }

  Harness h;
  std::unique_ptr<qar_qar_adapter::QarCApiUrlPanelFactory> factory;
  std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter> adapter;
};

TEST(UrlPanelOutage, ACloseInALostSessionMakesNoHubCall)
{
  LostPanel panel;
  ASSERT_TRUE(panel.make());
  Harness & h = panel.h;
  h.owner().declare_lost("test");
  ASSERT_TRUE(h.wait_state(SessionState::kLost));
  const int closes = h.fake().count("gui_panels_close_panel");

  EXPECT_TRUE(returns_within([&] { panel.adapter->close(); }, 20ms));
  EXPECT_FALSE(panel.adapter->pending());
  EXPECT_TRUE(wait_until([&] { return h.fake().panel_subscriptions() == 0; }, 5s))
    << "the watch of the panel was ended";
  EXPECT_EQ(h.fake().count("gui_panels_close_panel"), closes)
    << "the Hub is not asked to close the panel; the next session finds it by its name";
  EXPECT_EQ(h.fake().thread_of_last("cancel_token_cancel"), h.owner_thread())
    << "the token was cancelled on the owner's thread";
}

TEST(UrlPanelOutage, ItsSubscriptionsAreReleasedOnTheLane)
{
  LostPanel panel;
  ASSERT_TRUE(panel.make());
  Harness & h = panel.h;
  h.owner().declare_lost("test");
  ASSERT_TRUE(h.wait_state(SessionState::kLost));

  // The cancel of the token waits at a gate on the owner's thread: the calling thread is free.
  Gate gate;
  hold_first_call(h.fake(), "cancel_token_cancel", gate);
  panel.adapter->detach();
  EXPECT_TRUE(returns_within([&] { panel.adapter->detach(); }, 20ms)) << "a second detach";
  ASSERT_TRUE(gate.wait_entered());
  EXPECT_EQ(h.fake().thread_of_last("cancel_token_cancel"), h.owner_thread());
  EXPECT_NE(h.fake().thread_of_last("cancel_token_cancel"), std::this_thread::get_id());
  EXPECT_EQ(h.fake().panel_subscriptions(), 1) << "the cancel has not finished";

  gate.open();
  EXPECT_TRUE(wait_until([&] { return h.fake().panel_subscriptions() == 0; }, 5s));
  EXPECT_EQ(h.fake().thread_of_last("cancel_token_handle_destroy"), h.owner_thread());
}

TEST(UrlPanelOutage, AClosedPanelOfAnOwnerThatEndedLeavesTheWatchAlone)
{
  LostPanel panel;
  ASSERT_TRUE(panel.make());
  Harness & h = panel.h;
  ASSERT_TRUE(h.destroy_owner_within());
  const int cancels = h.fake().count("cancel_token_cancel");

  EXPECT_TRUE(returns_within([&] { panel.adapter->close(); }, 20ms));
  EXPECT_EQ(h.fake().panel_subscriptions(), 1)
    << "no thread is left to cancel the token, and nothing else may";
  EXPECT_EQ(h.fake().count("cancel_token_cancel"), cancels);
}

}  // namespace
