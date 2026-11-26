// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The fake QarOS library itself: the scripted render sender creation, the frame calls that are
// delayed or blocked on their own, a Hub that is gone, the gesture subscriptions, and the
// counters the other tests read. The tests of the adapters rely on these behaviours.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

#include "fake_qar_api.hpp"

using namespace std::chrono_literals;
using qar_qar_adapter::test::FakeGesture;
using qar_qar_adapter::test::FakePeer;
using qar_qar_adapter::test::FakeQarApi;
using qar_qar_adapter::test::HubGone;
using qar_qar_adapter::test::Outcome;
using qar_qar_adapter::test::RenderFrameScript;

namespace
{

bool wait_until(const std::function<bool()> & pred, std::chrono::milliseconds limit = 3000ms)
{
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return pred();
}

struct Loaded
{
  explicit Loaded(FakeQarApi & fake) : loader(fake.make_loader())
  {
    std::string error;
    loader->load("", error);
  }
  ~Loaded() { loader->unload(); }
  std::unique_ptr<qar_qar_adapter::ILibraryLoader> loader;
};

struct CreateResult
{
  std::atomic<int> fired{0};
  std::atomic<int> code{-1};
  std::atomic<QarRenderSender *> stream{nullptr};
};

void on_created(QarResult status, QarRenderSender * stream, void * user)
{
  auto * result = static_cast<CreateResult *>(user);
  result->code = static_cast<int>(status.code);
  result->stream = stream;
  result->fired.fetch_add(1);
}

QarSession * session()
{
  static int dummy = 0;
  return reinterpret_cast<QarSession *>(&dummy);
}

QarResult create(CreateResult & out, QarCancelToken * token)
{
  QarRenderSenderInit init = qar_render_sender_init_default();
  init.peer_id.data[0] = 0x42;
  return qar_render_sender_create_async(session(), &init, &on_created, &out, token);
}

}  // namespace

TEST(FakeQarLibrary, ARenderCreateAnswersAtOnceAndTheSenderIsCounted)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  CreateResult out;
  ASSERT_FALSE(qar_result_is_error(create(out, nullptr)));
  ASSERT_TRUE(wait_until([&] {return out.fired.load() == 1;}));
  EXPECT_EQ(out.code.load(), 0);
  ASSERT_NE(out.stream.load(), nullptr);
  EXPECT_EQ(fake.live_render_senders(), 1);
  EXPECT_EQ(fake.render_creates(), 1);
  EXPECT_EQ(fake.render_creates_pending(), 0);
  qar_render_stream_handle_destroy(out.stream.load());
  EXPECT_EQ(fake.live_render_senders(), 0);
  EXPECT_EQ(fake.render_streams_destroyed(), 1);
  EXPECT_NE(fake.thread_of_last("render_stream_handle_destroy"), std::thread::id());
}

TEST(FakeQarLibrary, ACreateThatDoesNotFireOnCancelAnswersLateWithALiveSender)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  Outcome never;
  never.wait_for_release = true;
  never.fire_on_cancel = false;
  fake.script_render_create(never);
  QarCancelToken * token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token)));
  CreateResult out;
  ASSERT_FALSE(qar_result_is_error(create(out, token)));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(out.fired.load(), 0);
  EXPECT_EQ(fake.render_creates_pending(), 1);
  qar_cancel_token_cancel(token);
  qar_cancel_token_handle_destroy(token);
  EXPECT_EQ(fake.render_creates_cancelled(), 1);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(out.fired.load(), 0) << "cancelling did not wake it";
  fake.release_pending();
  ASSERT_TRUE(wait_until([&] {return out.fired.load() == 1;}));
  EXPECT_EQ(out.code.load(), 0);
  ASSERT_NE(out.stream.load(), nullptr) << "the late sender exists";
  EXPECT_EQ(fake.live_render_senders(), 1);
  qar_render_stream_handle_destroy(out.stream.load());
  EXPECT_EQ(fake.live_render_senders(), 0);
}

TEST(FakeQarLibrary, ACreateThatFiresOnCancelFailsWithoutASender)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  Outcome never;
  never.wait_for_release = true;
  fake.script_render_create(never);
  QarCancelToken * token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token)));
  CreateResult out;
  ASSERT_FALSE(qar_result_is_error(create(out, token)));
  qar_cancel_token_cancel(token);
  ASSERT_TRUE(wait_until([&] {return out.fired.load() == 1;}));
  EXPECT_NE(out.code.load(), 0);
  EXPECT_EQ(out.stream.load(), nullptr);
  EXPECT_EQ(fake.live_render_senders(), 0);
  EXPECT_EQ(fake.render_creates_cancelled(), 1);
  qar_cancel_token_handle_destroy(token);
}

TEST(FakeQarLibrary, AFailingCreateAnswersItsCodeAndAnEndingFakeSkipsAnUnansweredOne)
{
  CreateResult never_answered;
  {
    FakeQarApi fake;
    Loaded loaded(fake);
    Outcome fail;
    fail.code = 603;
    fake.script_render_create(fail);
    Outcome never;
    never.wait_for_release = true;
    never.fire_on_cancel = false;
    fake.script_render_create(never);
    CreateResult out;
    ASSERT_FALSE(qar_result_is_error(create(out, nullptr)));
    ASSERT_TRUE(wait_until([&] {return out.fired.load() == 1;}));
    EXPECT_EQ(out.code.load(), 603);
    EXPECT_EQ(out.stream.load(), nullptr);
    ASSERT_FALSE(qar_result_is_error(create(never_answered, nullptr)));
  }
  EXPECT_EQ(never_answered.fired.load(), 0) << "the fake that ended did not call back";
}

TEST(FakeQarLibrary, FrameCallsAreScriptedDelayedAndBlockedOnTheirOwn)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  QarRenderFrameInfo * info = nullptr;
  bool ready = true;
  ASSERT_FALSE(qar_result_is_error(qar_render_sender_try_begin_frame(nullptr, &info, &ready)));
  EXPECT_FALSE(ready);
  RenderFrameScript script;
  script.ready = true;
  fake.set_render_frame(script);
  ASSERT_FALSE(qar_result_is_error(qar_render_sender_try_begin_frame(nullptr, &info, &ready)));
  EXPECT_TRUE(ready);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(fake.live_render_frame_infos(), 1);
  QarPose pose{};
  QarFov fov{};
  EXPECT_FALSE(qar_result_is_error(qar_render_frame_info_get_view_pose(info, 0, &pose)));
  EXPECT_FALSE(qar_result_is_error(qar_render_frame_info_get_view_fov(info, 1, &fov)));
  EXPECT_EQ(pose.orientation.w, 1.0f);
  qar_render_frame_info_handle_destroy(info);
  EXPECT_EQ(fake.live_render_frame_infos(), 0);
  script.error_code = 803;
  fake.set_render_frame(script);
  const QarResult failed = qar_render_sender_try_begin_frame(nullptr, &info, &ready);
  EXPECT_TRUE(qar_result_is_error(failed));
  EXPECT_EQ(static_cast<int>(failed.code), 803);

  QarRenderFrameShow show{};
  fake.set_frame_delay_ms(60, "render_sender_show_frame");
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(qar_result_is_error(qar_render_sender_show_frame(nullptr, &show)));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 55ms);
  EXPECT_EQ(fake.render_frames_shown(), 1);
  fake.set_frame_delay_ms(0, "render_sender_show_frame");

  fake.set_frame_block(true);
  auto blocked = std::async(std::launch::async, [&] {
      return qar_render_sender_show_frame(nullptr, &show);
    });
  ASSERT_TRUE(wait_until([&] {return fake.frame_calls_blocked() == 1;}));
  EXPECT_EQ(blocked.wait_for(50ms), std::future_status::timeout);
  // A hub that is gone does not touch the frame calls.
  fake.set_hub_gone(HubGone::kFailAll);
  EXPECT_EQ(blocked.wait_for(50ms), std::future_status::timeout);
  fake.set_frame_block(false);
  ASSERT_EQ(blocked.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(qar_result_is_error(blocked.get()));
  EXPECT_EQ(fake.render_frames_shown(), 2);
}

TEST(FakeQarLibrary, AGoneHubFailsEverySessionCallAtOnce)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  size_t count = 0;
  EXPECT_FALSE(qar_result_is_error(qar_query_app_volumes_count(session(), &count)));
  fake.set_hub_gone(HubGone::kFailAll, 1802);
  const QarResult failed = qar_query_app_volumes_count(session(), &count);
  EXPECT_EQ(static_cast<int>(failed.code), 1802);
  size_t panels = 0;
  EXPECT_EQ(static_cast<int>(qar_query_gui_panels_count(session(), &panels).code), 1802);
  CreateResult out;
  EXPECT_EQ(static_cast<int>(create(out, nullptr).code), 1802);
  EXPECT_EQ(fake.render_creates(), 0);
  fake.set_hub_gone(HubGone::kNo);
  EXPECT_FALSE(qar_result_is_error(qar_query_app_volumes_count(session(), &count)));
}

TEST(FakeQarLibrary, ABlockedHubHoldsTheCallUntilItIsReleasedThenFails)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  fake.set_hub_gone(HubGone::kBlockAll, 1802);
  size_t count = 0;
  auto call = std::async(std::launch::async, [&] {
      size_t n = 0;
      return qar_query_app_volumes_count(session(), &n);
    });
  ASSERT_TRUE(wait_until([&] {return fake.hub_blocked_calls() == 1;}));
  EXPECT_EQ(call.wait_for(60ms), std::future_status::timeout);
  fake.release_pending();
  ASSERT_EQ(call.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(static_cast<int>(call.get().code), 1802) << "still gone after the release";
  EXPECT_EQ(fake.hub_blocked_calls(), 0);
  auto again = std::async(std::launch::async, [&] {
      size_t n = 0;
      return qar_query_app_volumes_count(session(), &n);
    });
  ASSERT_TRUE(wait_until([&] {return fake.hub_blocked_calls() == 1;}));
  fake.set_hub_gone(HubGone::kNo);
  ASSERT_EQ(again.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(qar_result_is_error(again.get())) << "the hub is back";
  (void)count;
}

TEST(FakeQarLibrary, AnEndingFakeReleasesABlockedCall)
{
  std::future<QarResult> call;
  {
    FakeQarApi fake;
    Loaded loaded(fake);
    fake.set_hub_gone(HubGone::kBlockAll);
    call = std::async(std::launch::async, [] {
        size_t n = 0;
        return qar_query_app_volumes_count(session(), &n);
      });
    ASSERT_TRUE(wait_until([&] {return fake.hub_blocked_calls() == 1;}));
  }
  ASSERT_EQ(call.wait_for(2s), std::future_status::ready);
  EXPECT_NE(static_cast<int>(call.get().code), 0);
}

namespace
{
struct OnboardResult
{
  std::atomic<int> fired{0};
  std::atomic<int> code{-1};
};

void on_onboard(QarResult status, const QarOnboardingId *, QarSession * session_out, void * user)
{
  auto * result = static_cast<OnboardResult *>(user);
  result->code = static_cast<int>(status.code);
  result->fired.fetch_add(1);
  if (session_out != nullptr) {
    qar_session_handle_destroy(session_out);
  }
}
}  // namespace

TEST(FakeQarLibrary, AnOnboardingResultAnswersTheCodeOfAGoneHub)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  fake.set_hub_gone(HubGone::kFailAll, 1802);
  QarOnboardInit init = qar_onboard_init_default();
  OnboardResult out;
  QarCancelToken * token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token)));
  ASSERT_FALSE(qar_result_is_error(
      qar_runtime_onboard_async(nullptr, &init, &on_onboard, nullptr, &out, token)));
  ASSERT_TRUE(wait_until([&] {return out.fired.load() == 1;}));
  EXPECT_EQ(out.code.load(), 1802);
  EXPECT_EQ(fake.live_sessions(), 0);
  // Blocked: the answer waits, a cancel of its token ends the wait, the Hub's code is not given.
  fake.set_hub_gone(HubGone::kBlockAll, 1802);
  OnboardResult blocked;
  QarCancelToken * token2 = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token2)));
  ASSERT_FALSE(qar_result_is_error(
      qar_runtime_onboard_async(nullptr, &init, &on_onboard, nullptr, &blocked, token2)));
  ASSERT_TRUE(wait_until([&] {return fake.hub_blocked_calls() == 1;}));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(blocked.fired.load(), 0);
  qar_cancel_token_cancel(token2);
  ASSERT_TRUE(wait_until([&] {return blocked.fired.load() == 1;}));
  EXPECT_NE(blocked.code.load(), 1802);
  EXPECT_NE(blocked.code.load(), 0);
  qar_cancel_token_handle_destroy(token2);
  qar_cancel_token_handle_destroy(token);
}

namespace
{
struct GestureSink
{
  std::atomic<int> calls{0};
  std::atomic<int> kind{-1};
  std::atomic<int> phase{-1};
  std::atomic<bool> hold{false};
  std::atomic<bool> inside{false};
};

void on_gesture(QarAppVolumeGesture * gesture, void * user)
{
  auto * sink = static_cast<GestureSink *>(user);
  QarGestureKind kind{};
  QarGesturePhase phase{};
  qar_app_volume_gesture_get_classification(gesture, &kind, &phase);
  QarPeerId peer{};
  QarAppVolumeId volume{};
  qar_app_volume_gesture_get_lifecycle(gesture, &peer, &volume);
  QarVector3 a{}, b{};
  bool has_a = false, has_b = false;
  qar_app_volume_gesture_get_points(gesture, &a, &has_a, &b, &has_b);
  QarTimePoint ts{};
  qar_app_volume_gesture_get_timestamp(gesture, &ts);
  sink->kind = static_cast<int>(kind);
  sink->phase = static_cast<int>(phase);
  sink->calls.fetch_add(1);
  sink->inside = true;
  while (sink->hold.load()) {
    std::this_thread::sleep_for(2ms);
  }
  sink->inside = false;
}
}  // namespace

TEST(FakeQarLibrary, GestureSubscriptionsAreFilteredByKindAndCountedByToken)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  QarAppVolumeId volume{};
  QarCancelToken * all_token = nullptr;
  QarCancelToken * hover_token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&all_token)));
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&hover_token)));
  GestureSink all, hover;
  ASSERT_FALSE(qar_result_is_error(qar_app_volumes_subscribe_all_gesture_events(
      session(), &volume, &on_gesture, &all, all_token)));
  ASSERT_FALSE(qar_result_is_error(qar_app_volumes_subscribe_gesture_events(
      session(), &volume, QAR_GESTURE_HOVER, &on_gesture, &hover, hover_token)));
  EXPECT_EQ(fake.gesture_subscriptions(), 2);
  FakeGesture gesture;
  gesture.kind = QAR_GESTURE_CLICK;
  fake.deliver_volume_gesture(gesture);
  EXPECT_EQ(all.calls.load(), 1);
  EXPECT_EQ(hover.calls.load(), 0);
  EXPECT_EQ(all.kind.load(), static_cast<int>(QAR_GESTURE_CLICK));
  gesture.kind = QAR_GESTURE_HOVER;
  fake.deliver_volume_gesture(gesture);
  EXPECT_EQ(all.calls.load(), 2);
  EXPECT_EQ(hover.calls.load(), 1);
  qar_cancel_token_cancel(hover_token);
  EXPECT_EQ(fake.gesture_subscriptions(), 1);
  qar_cancel_token_handle_destroy(hover_token);
  qar_cancel_token_cancel(all_token);
  qar_cancel_token_handle_destroy(all_token);
  EXPECT_EQ(fake.gesture_subscriptions(), 0);
  fake.deliver_volume_gesture(gesture);
  EXPECT_EQ(all.calls.load(), 2) << "no delivery after the cancel";
  fake.set_call_failure("app_volumes_subscribe_all_gesture_events", 1805);
  QarCancelToken * t = nullptr;
  qar_cancel_token_create(&t);
  EXPECT_EQ(static_cast<int>(qar_app_volumes_subscribe_all_gesture_events(
      session(), &volume, &on_gesture, &all, t).code), 1805);
  qar_cancel_token_handle_destroy(t);
}

TEST(FakeQarLibrary, ACancelWaitsForADetachedCallbackThatDoesNotReturnYet)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  QarAppVolumeId volume{};
  QarCancelToken * token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token)));
  GestureSink sink;
  sink.hold = true;
  ASSERT_FALSE(qar_result_is_error(qar_app_volumes_subscribe_all_gesture_events(
      session(), &volume, &on_gesture, &sink, token)));
  fake.deliver_volume_gesture_detached(FakeGesture());
  ASSERT_TRUE(wait_until([&] {return sink.inside.load();}));
  EXPECT_FALSE(fake.wait_detached_deliveries(40));
  auto cancel = std::async(std::launch::async, [&] {
      return qar_cancel_token_cancel(token);
    });
  EXPECT_EQ(cancel.wait_for(80ms), std::future_status::timeout)
    << "the cancel waits for the callback";
  sink.hold = false;
  ASSERT_EQ(cancel.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(fake.wait_detached_deliveries(2000));
  qar_cancel_token_handle_destroy(token);

  // A detached peer delivery with a subscription that holds the callback, released by the end.
  FakePeer peer = qar_qar_adapter::test::make_peer(
    7, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE);
  fake.deliver_peer_detached(peer);  // no subscription: returns at once
  EXPECT_TRUE(fake.wait_detached_deliveries(1000));
}

TEST(FakeQarLibrary, RuntimesPanelGestureSubscriptionsAndVideoSenderFrameCallsAreCounted)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  QarRuntime * runtime = nullptr;
  QarRuntimeInit init = qar_runtime_init_default();
  ASSERT_FALSE(qar_result_is_error(qar_runtime_create(&init, &runtime)));
  EXPECT_EQ(fake.live_runtimes(), 1);
  EXPECT_EQ(fake.runtime_creates(), 1);
  QarRuntime * second = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_runtime_create(&init, &second)));
  EXPECT_EQ(fake.live_runtimes(), 2);
  qar_runtime_handle_destroy(second);
  qar_runtime_handle_destroy(runtime);
  EXPECT_EQ(fake.live_runtimes(), 0);
  EXPECT_EQ(fake.runtime_creates(), 2);

  QarCancelToken * token = nullptr;
  ASSERT_FALSE(qar_result_is_error(qar_cancel_token_create(&token)));
  QarGuiPanelId panel{};
  ASSERT_FALSE(qar_result_is_error(qar_gui_panels_subscribe_all_gesture_events(
      session(), &panel, nullptr, nullptr, token)));
  EXPECT_EQ(fake.panel_gesture_subscriptions(), 1);
  qar_cancel_token_handle_destroy(token);
  EXPECT_EQ(fake.panel_gesture_subscriptions(), 0);

  fake.set_frame_delay_ms(50);
  bool ready = false;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(qar_result_is_error(qar_video_sender_try_begin_frame(nullptr, &ready)));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 45ms);
}
