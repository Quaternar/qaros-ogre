// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The spatial gesture source when the Hub stops answering: unsubscribe() only disowns the
// callbacks on the calling thread, and the tokens of the subscription are cancelled and destroyed
// on the release lane of the session's owner. The owner runs on the fake QarOS library.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/qar_capi_spatial_gesture_source.hpp"

namespace
{

using qar_qar_adapter::QarCApiSpatialGestureSource;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::returns_within;
using namespace std::chrono_literals;

bool wait_until(const std::function<bool()> & predicate, std::chrono::milliseconds limit = 5s)
{
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

TEST(SpatialGestureOutage, UnsubscribeOnTheCallerOnlyRetiresSlots)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const int tokens_before = h.fake().live_tokens();
  QarCApiSpatialGestureSource source(h.owner().session(), h.owner().release_lane());
  ASSERT_TRUE(source.subscribe(QarAppVolumeId{})) << source.last_error();
  ASSERT_EQ(h.fake().gesture_subscriptions(), 1);

  // The cancel of the token waits at a gate on the owner's thread.
  Gate gate;
  hold_first_call(h.fake(), "cancel_token_cancel", gate);
  EXPECT_TRUE(returns_within([&] { source.unsubscribe(); }, 20ms));
  EXPECT_FALSE(source.subscribed());
  ASSERT_TRUE(gate.wait_entered());
  EXPECT_EQ(h.fake().gesture_subscriptions(), 1) << "the cancel has not finished";
  EXPECT_TRUE(returns_within([&] { source.unsubscribe(); }, 20ms)) << "a second call";

  gate.open();
  EXPECT_TRUE(wait_until([&] { return h.fake().gesture_subscriptions() == 0; }));
  EXPECT_TRUE(wait_until([&] { return h.fake().live_tokens() == tokens_before; }));
}

TEST(SpatialGestureOutage, TheTokensAreCancelledOnTheOwnerThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSpatialGestureSource source(h.owner().session(), h.owner().release_lane());
  ASSERT_TRUE(source.subscribe(QarAppVolumeId{})) << source.last_error();

  source.unsubscribe();
  ASSERT_TRUE(wait_until([&] { return h.fake().gesture_subscriptions() == 0; }));
  EXPECT_EQ(h.fake().thread_of_last("cancel_token_cancel"), h.owner_thread());
  EXPECT_NE(h.fake().thread_of_last("cancel_token_cancel"), std::this_thread::get_id());
  ASSERT_TRUE(wait_until([&] {
        return h.fake().thread_of_last("cancel_token_handle_destroy") == h.owner_thread();
      }));
}

TEST(SpatialGestureOutage, WithoutALaneTheCallerReleasesTheTokensItself)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSpatialGestureSource source(h.owner().session());
  ASSERT_TRUE(source.subscribe(QarAppVolumeId{})) << source.last_error();

  source.unsubscribe();
  EXPECT_EQ(h.fake().gesture_subscriptions(), 0) << "cancelled before unsubscribe() returned";
  EXPECT_EQ(h.fake().thread_of_last("cancel_token_cancel"), std::this_thread::get_id());
}

TEST(SpatialGestureOutage, AGestureCallbackAfterTheRetireSeesNoOwner)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSpatialGestureSource source(h.owner().session(), h.owner().release_lane());
  ASSERT_TRUE(source.subscribe(QarAppVolumeId{})) << source.last_error();
  h.fake().deliver_volume_gesture();
  ASSERT_EQ(source.events_received(), 1u) << "a delivery reaches the source while it is subscribed";

  // The token is still not cancelled when the next gesture arrives.
  Gate gate;
  hold_first_call(h.fake(), "cancel_token_cancel", gate);
  source.unsubscribe();
  ASSERT_TRUE(gate.wait_entered());
  ASSERT_EQ(h.fake().gesture_subscriptions(), 1);
  h.fake().deliver_volume_gesture();
  EXPECT_EQ(source.events_received(), 1u) << "the retired slot has no owner to hand it to";
  std::vector<qar_ar_core::PanelPointerSample> samples;
  source.drain(samples);
  EXPECT_LE(samples.size(), 1u);

  gate.open();
  EXPECT_TRUE(wait_until([&] { return h.fake().gesture_subscriptions() == 0; }));
}

TEST(SpatialGestureOutage, TheTokensAreLeftAloneWhenTheLaneRefuses)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSpatialGestureSource source(h.owner().session(), h.owner().release_lane());
  ASSERT_TRUE(source.subscribe(QarAppVolumeId{})) << source.last_error();
  ASSERT_TRUE(h.destroy_owner_within());
  const int cancels = h.fake().count("cancel_token_cancel");

  testing::internal::CaptureStderr();
  EXPECT_TRUE(returns_within([&] { source.unsubscribe(); }, 20ms));
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("gesture subscriptions left unreleased"), std::string::npos) << err;
  EXPECT_EQ(h.fake().count("cancel_token_cancel"), cancels) << "no call into the library";
}

}  // namespace
