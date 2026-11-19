// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The render sender factory when the Hub stops answering: no call of the thread that drives the
// factory waits, a creation that never answers is cancelled and tried again after a back-off, the
// stream handles are destroyed on the thread that owns the session, and frame calls that block
// are stopped. The owner runs on the fake QarOS library; no test loads the real one and no GL
// object is made (the deleter of WGL contexts is replaced by a counter).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_ar_core/common_name.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"

namespace qar_qar_adapter
{

// The friend of the factory that lets a test do what a host with a GL context would: build a
// factory that chains no context, give it the lane, and hand it contexts to delete.
struct QarCApiSenderFactoryOutageAccess
{
  static void skip_gl(QarCApiSenderFactory & factory) { factory.chain_gl_context_ = false; }

  static void fetch_lane(QarCApiSenderFactory & factory)
  {
    factory.lane_ = factory.release_lane();
    factory.lane_fetched_ = true;
  }

  static void add_gl_contexts(QarCApiSenderFactory & factory, std::vector<void *> contexts)
  {
    for (void * context : contexts) {
      auto ctx = std::make_unique<QarCApiSenderFactory::GlSenderContext>();
      ctx->context = context;
      factory.gl_sender_contexts_.push_back(std::move(ctx));
    }
  }

  static void set_deleter(void (* deleter)(void *))
  {
    QarCApiSenderFactory::set_gl_context_deleter(deleter);
  }
};

}  // namespace qar_qar_adapter

namespace
{

using qar_qar_adapter::QarCApiRenderSenderAdapter;
using qar_qar_adapter::QarCApiSenderFactory;
using qar_qar_adapter::QarCApiSenderFactoryOutageAccess;
using qar_qar_adapter::RenderRecycleTimings;
using qar_qar_adapter::RenderRecycleWatch;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::HubGone;
using qar_qar_adapter::test::Outcome;
using qar_qar_adapter::test::RenderFrameScript;
using qar_qar_adapter::test::returns_within;
using namespace std::chrono_literals;

constexpr double kCallerLimitMs = 20.0;

QarPeerId peer_with(std::uint8_t tag)
{
  QarPeerId id{};
  for (std::size_t i = 0; i < sizeof(id.data); ++i) {
    id.data[i] = tag;
  }
  return id;
}

QarCApiSenderFactory::Config outage_config()
{
  QarCApiSenderFactory::Config config;
  config.name_scope = qar_ar_core::make_name_scope("test-program", "", "lab-pc");
  config.create_timeout = 150ms;
  config.create_backoff_first = 60ms;
  config.create_backoff_cap = 200ms;
  config.create_give_up_after = 4;
  // These cases drive the frame calls from the test's own thread and look at them at once, so
  // they run the render-thread way of making them; render_worker_tests.cpp has the same cases for
  // the worker thread.
  config.frame_calls_on_worker = false;
  return config;
}

Outcome never_answers(bool fire_on_cancel)
{
  Outcome outcome;
  outcome.wait_for_release = true;
  outcome.fire_on_cancel = fire_on_cancel;
  return outcome;
}

// An owner that is connected, and a factory on it that is attached and chains no GL context. The
// destructor lets go of everything a test may have left blocked, so the fake library can end.
class Rig
{
public:
  explicit Rig(const QarCApiSenderFactory::Config & config = outage_config())
  {
    h.start();
    ok = h.connect_and_run();
    factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), config);
    QarCApiSenderFactoryOutageAccess::skip_gl(*factory);
    ok = ok && factory->attach();
    tokens_before = h.fake().live_tokens();
  }

  ~Rig()
  {
    h.fake().set_hub_gone(HubGone::kNo);
    h.fake().set_frame_block(false);
    h.fake().set_frame_delay_ms(0);
    h.fake().release_pending();
  }

  Rig(const Rig &) = delete;
  Rig & operator=(const Rig &) = delete;

  Harness h;
  std::shared_ptr<QarCApiSenderFactory> factory;
  bool ok = false;
  int tokens_before = 0;
};

double milliseconds_since(std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

struct Driven
{
  std::shared_ptr<qar_ogre_streaming::IQarRenderSenderAdapter> adapter;
  double worst_ms = 0.0;
  int calls = 0;
};

// Calls create_sender() every 2 ms, as the producer's tick does, until it hands out an adapter,
// @p stop holds or @p limit passes. The slowest call is kept.
Driven drive(
  QarCApiSenderFactory & factory, const QarPeerId & peer, std::chrono::milliseconds limit,
  const std::function<bool()> & stop = {})
{
  Driven out;
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    const auto start = std::chrono::steady_clock::now();
    out.adapter = factory.create_sender(peer);
    out.worst_ms = std::max(out.worst_ms, milliseconds_since(start));
    ++out.calls;
    if (out.adapter != nullptr || (stop && stop())) {
      break;
    }
    std::this_thread::sleep_for(2ms);
  }
  return out;
}

std::shared_ptr<QarCApiRenderSenderAdapter> adapter_of(Rig & rig, const QarPeerId & peer)
{
  const Driven driven = drive(*rig.factory, peer, 5s);
  return std::dynamic_pointer_cast<QarCApiRenderSenderAdapter>(driven.adapter);
}

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

bool contains(const std::string & text, const std::string & part)
{
  return text.find(part) != std::string::npos;
}

void a_creation_without_answer_is_abandoned_and_tried_again(bool fire_on_cancel)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.h.fake().script_render_create(never_answers(fire_on_cancel));
  const QarPeerId peer = peer_with(5);

  testing::internal::CaptureStderr();
  const Driven driven = drive(*rig.factory, peer, 5s);
  const std::string err = testing::internal::GetCapturedStderr();

  ASSERT_NE(driven.adapter, nullptr) << "the second creation answers at once";
  EXPECT_LT(driven.worst_ms, kCallerLimitMs) << "the slowest create_sender() call";
  EXPECT_EQ(rig.h.fake().render_creates(), 2);
  EXPECT_EQ(rig.h.fake().render_creates_cancelled(), 1) << "the first creation was cancelled";
  EXPECT_TRUE(contains(err, "create_async gave no answer in "));
  EXPECT_TRUE(contains(err, "abandoned (attempt 1), next try in 60 ms")) << err;
  EXPECT_EQ(rig.factory->create_attempt(peer), 0) << "the tries end with the sender";

  // The token of each creation is destroyed on the owner's thread.
  EXPECT_TRUE(wait_until([&] { return rig.h.fake().live_tokens() == rig.tokens_before; }));
  EXPECT_EQ(rig.h.fake().thread_of_last("cancel_token_handle_destroy"), rig.h.owner_thread());

  if (!fire_on_cancel) {
    // The cancel did not make the first creation answer. When it does, the sender it made is
    // released on the owner's thread.
    rig.h.fake().release_pending();
    EXPECT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
    EXPECT_EQ(rig.h.fake().live_render_senders(), 1) << "only the adapter's sender is left";
  }
}

TEST(RenderFactoryOutage, ACreateWithoutAnswerIsAbandonedAfterTheTimeoutAndTriedAgain)
{
  a_creation_without_answer_is_abandoned_and_tried_again(/*fire_on_cancel=*/true);
}

TEST(
  RenderFactoryOutage,
  ACreateWithoutAnswerIsAbandonedAndTriedAgainWhenTheCancelDoesNotAnswer)
{
  a_creation_without_answer_is_abandoned_and_tried_again(/*fire_on_cancel=*/false);
}

TEST(RenderFactoryOutage, RetriesBackOffAndGiveUpUntilTheTargetReturns)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  for (int i = 0; i < 5; ++i) {
    rig.h.fake().script_render_create(never_answers(true));
  }
  const QarPeerId peer = peer_with(6);
  const auto limit = std::chrono::steady_clock::now() + 15s;

  // The moments the creations started, seen from the tick that drives the factory.
  std::vector<std::chrono::steady_clock::time_point> starts;
  testing::internal::CaptureStderr();
  while (!rig.factory->create_given_up(peer) && std::chrono::steady_clock::now() < limit) {
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(rig.factory->create_sender(peer), nullptr);
    EXPECT_LT(milliseconds_since(start), kCallerLimitMs);
    if (static_cast<int>(starts.size()) < rig.h.fake().render_creates()) {
      starts.push_back(std::chrono::steady_clock::now());
    }
    std::this_thread::sleep_for(2ms);
  }
  const std::string err = testing::internal::GetCapturedStderr();

  EXPECT_TRUE(rig.factory->create_given_up(peer));
  ASSERT_EQ(starts.size(), 5u) << "the fifth cancelled creation ends the tries";
  EXPECT_EQ(rig.h.fake().render_creates(), 5);
  EXPECT_EQ(rig.h.fake().render_creates_cancelled(), 5);
  EXPECT_EQ(rig.factory->create_attempt(peer), 5);
  // The wait between two creations is the timeout and the back-off: 60, 120, 200, 200 ms.
  const long long expected_backoff_ms[] = {60, 120, 200, 200};
  for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
    const long long gap =
      std::chrono::duration_cast<std::chrono::milliseconds>(starts[i + 1] - starts[i]).count();
    EXPECT_GE(gap, 150 + expected_backoff_ms[i] - 25) << "gap " << i;
    EXPECT_LT(gap, 150 + expected_backoff_ms[i] + 400) << "gap " << i;
  }
  EXPECT_TRUE(contains(err, "abandoned (attempt 4), next try in 200 ms")) << err;
  EXPECT_TRUE(contains(err, "abandoned (attempt 5), giving up until the target returns")) << err;

  // Nothing is tried while the target stays.
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(rig.factory->create_sender(peer), nullptr);
    std::this_thread::sleep_for(3ms);
  }
  EXPECT_EQ(rig.h.fake().render_creates(), 5) << "no new creation after the tries were given up";

  // The target goes down and comes up: the tries start afresh, and the next creation answers.
  rig.factory->on_target_down(peer, QAR_PEER_PRESENCE_OFFLINE);
  EXPECT_FALSE(rig.factory->create_given_up(peer));
  EXPECT_EQ(rig.factory->create_attempt(peer), 0);
  const Driven again = drive(*rig.factory, peer, 5s);
  EXPECT_NE(again.adapter, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 6);
}

TEST(RenderFactoryOutage, ALateSenderOfAnAbandonedCreateIsReleasedOnTheOwnerThread)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.h.fake().script_render_create(never_answers(/*fire_on_cancel=*/false));
  const QarPeerId peer = peer_with(7);
  const Driven first = drive(*rig.factory, peer, 5s, [&] {
      return rig.h.fake().render_creates_cancelled() == 1;
    });
  ASSERT_EQ(first.adapter, nullptr);
  ASSERT_EQ(rig.h.fake().render_creates_cancelled(), 1);
  EXPECT_EQ(rig.h.fake().live_render_senders(), 0) << "the creation has not answered";

  // The runtime answers after all: the sender it made arrives at a creation nobody waits for.
  rig.h.fake().release_pending();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(rig.h.fake().live_render_senders(), 0);
  const std::thread::id destroyed_on = rig.h.fake().thread_of_last("render_stream_handle_destroy");
  EXPECT_EQ(destroyed_on, rig.h.owner_thread()) << "the owner's thread destroys the late sender";
  EXPECT_NE(destroyed_on, std::this_thread::get_id());
}

TEST(RenderFactoryOutage, TheStreamHandleIsReleasedOnTheOwnerThreadNeverOnTheCaller)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(8);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  ASSERT_EQ(rig.h.fake().live_render_senders(), 1);

  // A frame was primed: the adapter still holds a frame info besides the stream.
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  ASSERT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  ASSERT_EQ(rig.h.fake().live_render_frame_infos(), 1) << "the primed frame info";

  EXPECT_TRUE(returns_within([&] { adapter.reset(); }, 20ms));
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(rig.h.fake().live_render_senders(), 0);
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 0) << "the frame info goes with the stream";
  const std::thread::id destroyed_on = rig.h.fake().thread_of_last("render_stream_handle_destroy");
  EXPECT_EQ(destroyed_on, rig.h.owner_thread());
  EXPECT_NE(destroyed_on, std::this_thread::get_id());
  EXPECT_EQ(
    rig.h.fake().thread_of_last("render_frame_info_handle_destroy"), rig.h.owner_thread());
}

TEST(RenderFactoryOutage, TheGuiSwitchKeepsTheDestroyOnTheCallingThread)
{
  QarCApiSenderFactory::Config config = outage_config();
  config.stream_release_on_owner = false;
  Rig rig(config);
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(9));
  ASSERT_NE(adapter, nullptr);

  adapter.reset();
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 1) << "destroyed before reset() returned";
  EXPECT_EQ(
    rig.h.fake().thread_of_last("render_stream_handle_destroy"), std::this_thread::get_id());
  EXPECT_EQ(rig.h.fake().render_creates_cancelled(), 0) << "no token, no lane";
}

TEST(RenderFactoryOutage, InARefusedLaneNoHandleIsDestroyed)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(10));
  ASSERT_NE(adapter, nullptr);
  ASSERT_EQ(rig.h.fake().live_render_senders(), 1);

  // The owner's thread ends: its lane refuses from now on, and the library is not in the process.
  ASSERT_TRUE(rig.h.destroy_owner_within());
  testing::internal::CaptureStderr();
  EXPECT_TRUE(returns_within([&] { adapter.reset(); }, 20ms));
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(err, "stream handle left unreleased (the owner thread is gone)")) << err;
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0) << "the stream was left alone";
  EXPECT_EQ(rig.h.fake().live_render_senders(), 1);
}

TEST(
  RenderFactoryOutage,
  DetachAndTheFactoryDestructorReturnWithinTwentyMillisecondsWhileTheOwnerIsBlocked)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(11));
  ASSERT_NE(adapter, nullptr);
  // A second target whose creation is in flight when the Hub stops answering.
  rig.h.fake().script_render_create(never_answers(true));
  const QarPeerId waiting = peer_with(12);
  EXPECT_EQ(rig.factory->create_sender(waiting), nullptr);
  ASSERT_EQ(rig.h.fake().render_creates_pending(), 1);

  rig.h.fake().set_hub_gone(HubGone::kBlockAll);
  EXPECT_TRUE(returns_within([&] { adapter.reset(); }, 20ms)) << "the sender's destructor";
  EXPECT_TRUE(returns_within([&] { rig.factory->detach(); }, 20ms)) << "detach()";
  EXPECT_TRUE(returns_within([&] { rig.factory.reset(); }, 20ms)) << "the factory's destructor";
  EXPECT_EQ(rig.h.fake().live_render_senders(), 1) << "the destroy waits on the owner thread";

  rig.h.fake().set_hub_gone(HubGone::kNo);
  EXPECT_TRUE(wait_until([&] { return rig.h.fake().live_render_senders() == 0; }));
  EXPECT_TRUE(wait_until([&] { return rig.h.fake().render_creates_pending() == 0; }));
  EXPECT_EQ(rig.h.fake().render_creates_cancelled(), 1) << "detach() cancelled the creation";
  // Both creation tokens are gone, and so are the tokens of the placement subscription and of the
  // render request subscription that detach() ended.
  EXPECT_TRUE(wait_until([&] { return rig.h.fake().live_tokens() == rig.tokens_before - 2; }))
    << "tokens: " << rig.h.fake().live_tokens() << " before: " << rig.tokens_before;
}

TEST(RenderFactoryOutage, TwoSlowFrameCallsStopTheFrameCallsOfThatStream)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(13);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  rig.h.fake().set_frame_delay_ms(300, "render_sender_show_frame");
  qar_ogre_streaming::QarOpenGlStereoFrame frame;

  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame)) << "the first slow call";
  EXPECT_FALSE(adapter->frame_calls_stopped()) << "one slow call is not enough";
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer));

  testing::internal::CaptureStderr();
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame)) << "the second slow call";
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(adapter->frame_calls_stopped());
  EXPECT_TRUE(rig.factory->frame_calls_stopped(peer));
  EXPECT_TRUE(contains(err, "frame calls stopped: show_frame took ")) << err;
  EXPECT_TRUE(contains(err, " ms twice")) << err;
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer_with(99))) << "a peer without a stream";

  // From now on the stream makes no frame call at all.
  const int shown = rig.h.fake().count("render_sender_show_frame");
  const int begun = rig.h.fake().count("render_sender_try_begin_frame");
  const int opened = rig.h.fake().count("render_sender_frame_opengl");
  EXPECT_EQ(shown, 2);
  EXPECT_FALSE(adapter->submit_opengl_stereo_frame(frame));
  qar_ogre_streaming::QarFrameInfo info;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(adapter->begin_frame(info)) << "not an error: the stream has no frame to render";
  EXPECT_LT(milliseconds_since(start), kCallerLimitMs);
  EXPECT_FALSE(info.should_render);
  EXPECT_TRUE(contains(adapter->last_skip_detail(), "frame calls stopped"));
  EXPECT_EQ(rig.h.fake().count("render_sender_show_frame"), shown);
  EXPECT_EQ(rig.h.fake().count("render_sender_try_begin_frame"), begun);
  EXPECT_EQ(rig.h.fake().count("render_sender_frame_opengl"), opened);
}

TEST(RenderFactoryOutage, SlowCallsOfDifferentKindsCountTogether)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(14);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  // Slow calls of different kinds count together; fast calls in between do not matter.
  rig.h.fake().set_frame_delay_ms(280, "render_sender_frame_opengl");
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  rig.h.fake().set_frame_delay_ms(0, "render_sender_frame_opengl");
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  EXPECT_FALSE(adapter->frame_calls_stopped());
  rig.h.fake().set_frame_delay_ms(280, "render_sender_show_frame");
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  EXPECT_TRUE(adapter->frame_calls_stopped());
}

TEST(RenderFactoryOutage, ACreationThatTakesLongOnAHealthyHubIsNotCancelledByTheDefaults)
{
  // The longest creation on a healthy Hub in the recorded runs took 22.74 s.
  const QarCApiSenderFactory::Config defaults;
  EXPECT_GE(defaults.create_timeout, 45s);
  EXPECT_LE(defaults.create_timeout, 120s);
}

TEST(RenderFactoryOutage, ACreationThatAnswersLateButBeforeTheTimeoutIsNotCancelled)
{
  // The 22.7 s of a healthy Hub against a 60 s limit, scaled down: it answers after 38 % of it.
  QarCApiSenderFactory::Config config = outage_config();
  config.create_timeout = 400ms;
  Rig rig(config);
  ASSERT_TRUE(rig.ok);
  Outcome late;
  late.delay_ms = 150;
  rig.h.fake().script_render_create(late);
  const QarPeerId peer = peer_with(15);

  testing::internal::CaptureStderr();
  const Driven driven = drive(*rig.factory, peer, 5s);
  const std::string err = testing::internal::GetCapturedStderr();

  ASSERT_NE(driven.adapter, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 1) << "one creation, no second try";
  EXPECT_EQ(rig.h.fake().render_creates_cancelled(), 0);
  EXPECT_FALSE(contains(err, "gave no answer")) << err;
  EXPECT_EQ(rig.factory->create_attempt(peer), 0);
}

// Breaker times shortened by a factor of about 40: a call of 100 ms or more is slow, the delay of
// a slow call is 130 ms.
QarCApiRenderSenderAdapter::FrameBreakerTimings quick_breaker()
{
  QarCApiRenderSenderAdapter::FrameBreakerTimings timings;
  timings.slow_call = 100ms;
  timings.window = 1000ms;
  timings.pause_first = 150ms;
  timings.pause_cap = 600ms;
  timings.calm_reset = 1500ms;
  return timings;
}

constexpr const char * kShowFrame = "render_sender_show_frame";
constexpr const char * kFrameOpengl = "render_sender_frame_opengl";

// An adapter with the quick breaker whose frames the fake answers; no call is slow yet.
std::shared_ptr<QarCApiRenderSenderAdapter> breaker_adapter(Rig & rig, const QarPeerId & peer)
{
  auto adapter = adapter_of(rig, peer);
  if (adapter != nullptr) {
    adapter->set_frame_breaker_timings(quick_breaker());
    rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  }
  return adapter;
}

// Two slow show_frame calls: the frame calls are paused when it returns.
void trip(Rig & rig, QarCApiRenderSenderAdapter & adapter)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  rig.h.fake().set_frame_delay_ms(130, kShowFrame);
  adapter.submit_opengl_stereo_frame(frame);
  adapter.submit_opengl_stereo_frame(frame);
  rig.h.fake().set_frame_delay_ms(0, kShowFrame);
}

// Submits a frame every 2 ms until one of them reaches @p call of the fake (the probe). Returns the
// milliseconds from @p since to the moment that submit was entered, or -1 when none did in 3 s.
double submit_until_called(
  Rig & rig, QarCApiRenderSenderAdapter & adapter, const char * call,
  std::chrono::steady_clock::time_point since)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < end) {
    const int before = rig.h.fake().count(call);
    const double entered_ms = milliseconds_since(since);
    adapter.submit_opengl_stereo_frame(frame);
    if (rig.h.fake().count(call) > before) {
      return entered_ms;
    }
    std::this_thread::sleep_for(2ms);
  }
  return -1.0;
}

// One slow probe (the pause doubles to 300 ms), then one fast probe (the frame calls resume).
void slow_then_fast_probe(Rig & rig, QarCApiRenderSenderAdapter & adapter)
{
  rig.h.fake().set_frame_delay_ms(130, kFrameOpengl);
  ASSERT_GE(submit_until_called(rig, adapter, kFrameOpengl, std::chrono::steady_clock::now()), 0.0);
  rig.h.fake().set_frame_delay_ms(0, kFrameOpengl);
  ASSERT_GE(submit_until_called(rig, adapter, kFrameOpengl, std::chrono::steady_clock::now()), 0.0);
  ASSERT_FALSE(adapter.frame_calls_stopped());
}

TEST(RenderFactoryOutage, AFastProbeFrameAfterThePauseResumesTheFrameCalls)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(16);
  auto adapter = breaker_adapter(rig, peer);
  ASSERT_NE(adapter, nullptr);
  trip(rig, *adapter);
  ASSERT_TRUE(adapter->frame_calls_stopped());
  qar_ogre_streaming::QarOpenGlStereoFrame frame;

  // During the pause no frame call is made.
  const int shown = rig.h.fake().count(kShowFrame);
  EXPECT_FALSE(adapter->submit_opengl_stereo_frame(frame));
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown);
  EXPECT_TRUE(adapter->frame_calls_stopped());

  // After it the next frame is the probe; it is fast, so the frame calls resume and one line says
  // how long they were paused.
  std::this_thread::sleep_for(250ms);
  testing::internal::CaptureStderr();
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(adapter->frame_calls_stopped());
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer));
  EXPECT_TRUE(contains(err, "[qar-adapter] ")) << err;
  EXPECT_TRUE(contains(err, " frame calls resumed after 0.")) << err;
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown + 1);
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame)) << "normal frames go on";
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown + 2);
}

TEST(RenderFactoryOutage, ASlowProbeFrameDoublesThePauseUpToTheCap)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(17);
  auto adapter = breaker_adapter(rig, peer);
  ASSERT_NE(adapter, nullptr);
  trip(rig, *adapter);
  ASSERT_TRUE(adapter->frame_calls_stopped());
  rig.h.fake().set_frame_delay_ms(130, kFrameOpengl);

  // The pause from a trip or a slow probe is 150, 300, 600, 600 ms (the cap): the next probe is
  // not made earlier. The wait is measured from the end of the call that started the pause.
  const double pauses_ms[] = {150.0, 300.0, 600.0, 600.0};
  auto since = std::chrono::steady_clock::now();
  for (const double pause : pauses_ms) {
    const double waited = submit_until_called(rig, *adapter, kFrameOpengl, since);
    ASSERT_GE(waited, 0.0) << "no probe within 3 s after a pause of " << pause << " ms";
    EXPECT_GE(waited, pause - 20.0) << "the pause is not cut short";
    EXPECT_LE(waited, pause + 400.0) << "the pause is not longer than the cap allows";
    EXPECT_TRUE(adapter->frame_calls_stopped()) << "a slow probe keeps the pause";
    since = std::chrono::steady_clock::now();
  }
}

TEST(RenderFactoryOutage, ASlowCallRightAfterAResumePausesAgainForTheLongerPause)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(18);
  auto adapter = breaker_adapter(rig, peer);
  ASSERT_NE(adapter, nullptr);
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  trip(rig, *adapter);
  slow_then_fast_probe(rig, *adapter);

  // The slow probe is less than a window ago, so one slow call pauses again, for 300 ms.
  rig.h.fake().set_frame_delay_ms(130, kShowFrame);
  adapter->submit_opengl_stereo_frame(frame);
  rig.h.fake().set_frame_delay_ms(0, kShowFrame);
  ASSERT_TRUE(adapter->frame_calls_stopped());
  const auto paused_from = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(200ms);
  const int shown = rig.h.fake().count(kShowFrame);
  EXPECT_FALSE(adapter->submit_opengl_stereo_frame(frame)) << "150 ms are over, 300 are not";
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown);
  const double waited = submit_until_called(rig, *adapter, kShowFrame, paused_from);
  EXPECT_GE(waited, 280.0);
}

TEST(RenderFactoryOutage, ThePauseIsTheFirstOneAgainAfterAHealthyWindow)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(19);
  auto adapter = breaker_adapter(rig, peer);
  ASSERT_NE(adapter, nullptr);
  trip(rig, *adapter);
  slow_then_fast_probe(rig, *adapter);

  // No slow call for longer than the healthy window: the next pause is 150 ms, not 300.
  std::this_thread::sleep_for(1600ms);
  trip(rig, *adapter);
  ASSERT_TRUE(adapter->frame_calls_stopped());
  const auto paused_from = std::chrono::steady_clock::now();
  const double waited = submit_until_called(rig, *adapter, kShowFrame, paused_from);
  EXPECT_GE(waited, 130.0);
  EXPECT_LE(waited, 260.0) << "a pause of 150 ms, not 300 ms";
}

std::atomic<int> g_deleted{0};
std::mutex g_deleted_mutex;
std::vector<void *> g_deleted_contexts;

void count_deleted_context(void * context)
{
  std::lock_guard<std::mutex> lock(g_deleted_mutex);
  g_deleted_contexts.push_back(context);
  g_deleted.fetch_add(1);
}

TEST(RenderFactoryOutage, GlContextsAreFreedOnlyAfterTheirStreamsWereReleased)
{
  g_deleted = 0;
  g_deleted_contexts.clear();
  QarCApiSenderFactoryOutageAccess::set_deleter(&count_deleted_context);
  {
    Rig rig;
    ASSERT_TRUE(rig.ok);
    void * const first = reinterpret_cast<void *>(0x1001);
    void * const second = reinterpret_cast<void *>(0x1002);
    QarCApiSenderFactoryOutageAccess::fetch_lane(*rig.factory);
    QarCApiSenderFactoryOutageAccess::add_gl_contexts(*rig.factory, {first, second});

    // The thread that owns the session is busy with a release that has not finished: the stream
    // releases queued behind it are not done, so the contexts must stay.
    Gate gate;
    auto lane = rig.factory->release_lane();
    ASSERT_NE(lane, nullptr);
    ASSERT_TRUE(lane->post_release("busy", [&gate] { gate.wait(); }));
    ASSERT_TRUE(gate.wait_entered());
    EXPECT_TRUE(returns_within([&] { rig.factory.reset(); }, 20ms));
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 0) << "the lane has not run the item behind the streams yet";

    gate.open();
    EXPECT_TRUE(wait_until([&] {
          QarCApiSenderFactory::delete_released_gl_contexts();
          return g_deleted.load() == 2;
        }));
    {
      std::lock_guard<std::mutex> lock(g_deleted_mutex);
      ASSERT_EQ(g_deleted_contexts.size(), 2u);
      EXPECT_EQ(g_deleted_contexts[0], first);
      EXPECT_EQ(g_deleted_contexts[1], second);
    }
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 2) << "each context is deleted once";
  }
  QarCApiSenderFactoryOutageAccess::set_deleter(nullptr);
}

TEST(RenderFactoryOutage, GlContextsOfAFactoryWithoutALaneAreDeletedAtOnce)
{
  g_deleted = 0;
  QarCApiSenderFactoryOutageAccess::set_deleter(&count_deleted_context);
  {
    QarCApiSenderFactory::Config config = outage_config();
    config.stream_release_on_owner = false;
    Rig rig(config);
    ASSERT_TRUE(rig.ok);
    QarCApiSenderFactoryOutageAccess::add_gl_contexts(
      *rig.factory, {reinterpret_cast<void *>(0x2001)});
    rig.factory.reset();
    EXPECT_EQ(g_deleted.load(), 1) << "the old path: deleted in the destructor";
  }
  QarCApiSenderFactoryOutageAccess::set_deleter(nullptr);
}

TEST(RenderFactoryOutage, GlContextsAreNeverDeletedWhenTheLaneRefuses)
{
  g_deleted = 0;
  QarCApiSenderFactoryOutageAccess::set_deleter(&count_deleted_context);
  {
    Rig rig;
    ASSERT_TRUE(rig.ok);
    QarCApiSenderFactoryOutageAccess::fetch_lane(*rig.factory);
    QarCApiSenderFactoryOutageAccess::add_gl_contexts(
      *rig.factory, {reinterpret_cast<void *>(0x3001)});
    ASSERT_TRUE(rig.h.destroy_owner_within());
    rig.factory.reset();
    for (int i = 0; i < 10; ++i) {
      QarCApiSenderFactory::delete_released_gl_contexts();
      std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(g_deleted.load(), 0) << "the streams may still use the context";
  }
  QarCApiSenderFactoryOutageAccess::set_deleter(nullptr);
}

// ---------------------------------------------------------------------------------------------
//  A sender that the runtime asks for no frames is made again
// ---------------------------------------------------------------------------------------------

// The times of the recycle rule shortened by a factor of 225: 200 ms instead of 45 s.
QarCApiSenderFactory::Config recycle_config(std::chrono::milliseconds reconnecting_limit = 1500ms)
{
  QarCApiSenderFactory::Config config = outage_config();
  config.render_recycle.after = 200ms;
  config.render_recycle.cap = 1600ms;
  config.render_recycle.reconnecting_limit = reconnecting_limit;
  config.render_recycle.error_window = 500ms;
  config.render_recycle.error_persist = 100ms;
  return config;
}

// One tick of the producer: begin_frame and, when a frame is ready, the submit.
bool tick_frame(QarCApiRenderSenderAdapter & adapter)
{
  qar_ogre_streaming::QarFrameInfo info;
  adapter.begin_frame(info);
  if (!info.should_render) {
    return false;
  }
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  adapter.submit_opengl_stereo_frame(frame);
  return true;
}

// Ticks every 2 ms for @p duration; returns at once when the sender closed. Tells whether it did.
bool closes_within(QarCApiRenderSenderAdapter & adapter, std::chrono::milliseconds duration)
{
  const auto end = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < end) {
    tick_frame(adapter);
    if (adapter.is_closed()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return adapter.is_closed();
}

bool shows_a_frame(QarCApiRenderSenderAdapter & adapter)
{
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < end) {
    if (tick_frame(adapter)) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

TEST(RenderFactoryOutage, ASenderWithoutRenderRequestsAfterAHubRestartIsMadeAgain)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(30);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(shows_a_frame(*adapter)) << "the stream shows frames before the Hub restarts";
  EXPECT_FALSE(adapter->is_closed());

  // The Hub is gone and back: the runtime replaced the transfer and sends no render request, so
  // every try-begin answers an error that is no plain "not yet".
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  testing::internal::CaptureStderr();
  const auto started = std::chrono::steady_clock::now();
  const bool closed = closes_within(*adapter, 5s);
  const double after_ms = milliseconds_since(started);
  const std::string err = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(closed) << err;
  EXPECT_GE(after_ms, 150.0) << "not before the limit of 200 ms";
  EXPECT_TRUE(adapter->recycled());
  EXPECT_TRUE(contains(err, " no render request for ")) << err;
  EXPECT_TRUE(contains(
      err, " s after the stream reconnected (the runtime answers 803); the 3D sender is made "
      "again\n")) << err;

  // The producer sees it as a closed stream, destroys it and the factory makes a new one.
  std::vector<QarPeerId> closed_peers;
  rig.factory->poll_closed_peers(closed_peers);
  ASSERT_EQ(closed_peers.size(), 1u);
  adapter.reset();
  rig.factory->forget(peer);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto again = adapter_of(rig, peer);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 2);
  EXPECT_FALSE(again->recycled());
  EXPECT_TRUE(shows_a_frame(*again)) << "the Hub is back and the 3D stream shows frames again";
  EXPECT_FALSE(again->is_closed());
}

TEST(RenderFactoryOutage, AReconnectingAnswerBelowTheLimitDoesNotMakeTheSenderAgain)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(31));
  ASSERT_NE(adapter, nullptr);
  // "Reconnecting" for 500 ms: longer than the 200 ms of the no-request rule, shorter than the
  // 1500 ms of its own limit.
  rig.h.fake().set_render_frame(RenderFrameScript{false, 603});
  EXPECT_FALSE(closes_within(*adapter, 500ms));
  EXPECT_FALSE(adapter->recycled());
  // The error that follows is current, and the sender has begun no frame for long: now it closes.
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  EXPECT_TRUE(closes_within(*adapter, 3s));
}

TEST(RenderFactoryOutage, AnAnswerOfNoFrameWithoutAnErrorNeverMakesTheSenderAgain)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(32));
  ASSERT_NE(adapter, nullptr);
  rig.h.fake().set_render_frame(RenderFrameScript{false, 0});
  EXPECT_FALSE(closes_within(*adapter, 1s)) << "an idle viewer is not a wedged sender";
  EXPECT_FALSE(adapter->recycled());
  // Nor is a stream that the receiver paused, or whose volume is not resolved yet.
  rig.h.fake().set_render_frame(RenderFrameScript{false, 602});
  EXPECT_FALSE(closes_within(*adapter, 600ms));
  rig.h.fake().set_render_frame(RenderFrameScript{false, 805});
  EXPECT_FALSE(closes_within(*adapter, 600ms));
}

TEST(RenderFactoryOutage, AFrameThatBeginsStartsTheClockAgain)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(33));
  ASSERT_NE(adapter, nullptr);
  // Errors for 150 ms, then a frame, six times: more than 200 ms of errors in all, never that
  // long after a frame.
  for (int round = 0; round < 6; ++round) {
    rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
    EXPECT_FALSE(closes_within(*adapter, 150ms)) << "round " << round;
    rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
    ASSERT_TRUE(shows_a_frame(*adapter)) << "round " << round;
  }
  EXPECT_FALSE(adapter->is_closed());
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  EXPECT_TRUE(closes_within(*adapter, 3s)) << "errors for longer than the limit close it";
}

TEST(RenderFactoryOutage, ASenderToldItReconnectsForTooLongIsMadeAgain)
{
  Rig rig(recycle_config(/*reconnecting_limit=*/400ms));
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(34));
  ASSERT_NE(adapter, nullptr);
  rig.h.fake().set_render_frame(RenderFrameScript{false, 603});
  testing::internal::CaptureStderr();
  const auto started = std::chrono::steady_clock::now();
  const bool closed = closes_within(*adapter, 3s);
  const double after_ms = milliseconds_since(started);
  const std::string err = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(closed) << err;
  EXPECT_GE(after_ms, 380.0);
  EXPECT_TRUE(contains(err, "(the runtime answers 603); the 3D sender is made again")) << err;
}

TEST(RenderFactoryOutage, TheLimitDoublesForEveryFurtherSenderOfThePeerAndAFrameResetsIt)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(35);
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});

  const auto recycle_once = [&](double * after_ms) -> bool {
      auto adapter = adapter_of(rig, peer);
      if (adapter == nullptr) {
        return false;
      }
      const auto started = std::chrono::steady_clock::now();
      const bool closed = closes_within(*adapter, 5s);
      *after_ms = milliseconds_since(started);
      adapter.reset();
      rig.factory->forget(peer);
      return closed;
    };
  double first = 0.0, second = 0.0, third = 0.0;
  ASSERT_TRUE(recycle_once(&first));
  ASSERT_TRUE(recycle_once(&second));
  ASSERT_TRUE(recycle_once(&third));
  EXPECT_GE(first, 190.0);
  EXPECT_GE(second, 390.0) << "the second sender of the peer waits twice as long";
  EXPECT_GE(third, 790.0) << "and the third four times";

  // A frame that begins resets the count: the next sender is given 200 ms again.
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(shows_a_frame(*adapter));
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(closes_within(*adapter, 5s));
  EXPECT_LT(milliseconds_since(started), 700.0) << "back to the first limit";
}

TEST(RenderFactoryOutage, TheRecycleRuleIsOffWhenItsTimeIsZero)
{
  QarCApiSenderFactory::Config config = recycle_config();
  config.render_recycle.after = 0ms;
  Rig rig(config);
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(36));
  ASSERT_NE(adapter, nullptr);
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  EXPECT_FALSE(closes_within(*adapter, 700ms));
}

TEST(RenderFactoryOutage, AStreamingGuiSenderWithPrimedFramesIsNotMadeAgainByLaterErrors)
{
  QarCApiSenderFactory::Config config = recycle_config();
  config.render_recycle.after = 400ms;
  config.render_recycle.cap = 3200ms;
  Rig rig(config);
  ASSERT_TRUE(rig.ok);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto adapter = adapter_of(rig, peer_with(37));
  ASSERT_NE(adapter, nullptr);
  adapter->set_prime_enabled(true);
  // Frames for longer than the limit; all but the first are the ones that were primed.
  ASSERT_TRUE(shows_a_frame(*adapter));
  EXPECT_FALSE(closes_within(*adapter, 700ms)) << "the stream is healthy";
  // Errors that last longer than error_persist but not as long as the limit counts from the last
  // frame: the stream is not made again.
  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  EXPECT_FALSE(closes_within(*adapter, 250ms));
  EXPECT_FALSE(adapter->recycled());
  // Errors that go on are what the rule is for.
  EXPECT_TRUE(closes_within(*adapter, 3s));
}

// ---------------------------------------------------------------------------------------------
//  The judgement of the recycle rule on its own, with the default times and a clock of the test
// ---------------------------------------------------------------------------------------------

constexpr int kStatusError = 802;
constexpr int kStatusStreamReconnecting = 803;

class RecycleWatchClock
{
public:
  std::chrono::steady_clock::time_point now() const { return now_; }
  void advance(std::chrono::milliseconds step) { now_ += step; }

private:
  std::chrono::steady_clock::time_point now_ = std::chrono::steady_clock::time_point{} + 1000s;
};

TEST(RecycleWatch, OneStrayErrorAfterALongTimeWithoutAFrameDoesNotMakeTheSenderAgain)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  clock.advance(60s);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kNotReady, 0, clock.now()));
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()))
    << "the first error of a run";
  clock.advance(100ms);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kNotReady, 0, clock.now()));
  // The next error comes after a gap longer than the window: it starts a run of its own.
  clock.advance(10s);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()));
  clock.advance(100ms);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()));
  EXPECT_FALSE(watch.recycled());
}

TEST(RecycleWatch, ErrorsThatLastForThreeSecondsMakeTheSenderAgain)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  clock.advance(50s);
  bool recycled = false;
  long long at_ms = 0;
  for (long long ms = 0; ms <= 4000 && !recycled; ms += 250) {
    // The runtime mixes pending answers in, as it does in the wedge.
    recycled = watch.note(RenderRecycleWatch::Answer::kNotReady, 0, clock.now());
    recycled = watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()) ||
      recycled;
    at_ms = ms;
    clock.advance(250ms);
  }
  EXPECT_TRUE(recycled);
  EXPECT_EQ(at_ms, 3000) << "not before the errors lasted 3 s";
  EXPECT_TRUE(watch.recycled());
  EXPECT_EQ(watch.last_code(), kStatusError);
}

TEST(RecycleWatch, ReconnectingMixedWithPendingAnswersIsMadeAgainAfterTheLimit)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  bool recycled = false;
  long long at_s = 0;
  for (long long ms = 0; ms <= 100000 && !recycled; ms += 500) {
    recycled = watch.note(RenderRecycleWatch::Answer::kNotReady, 0, clock.now());
    clock.advance(100ms);
    recycled = watch.note(
      RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()) ||
      recycled;
    at_s = ms / 1000;
    clock.advance(400ms);
  }
  EXPECT_TRUE(recycled);
  EXPECT_GE(at_s, 90) << "not before the limit of 90 s";
  EXPECT_LE(at_s, 91);
  // The runtime answers "reconnecting" in the line, whatever the outer code was.
  EXPECT_EQ(watch.last_code(), 603);
}

TEST(RecycleWatch, AReconnectingRunEndsWhenTheRuntimeStopsSayingSo)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  for (int i = 0; i < 100; ++i) {  // 80 s of "reconnecting"
    EXPECT_FALSE(watch.note(
        RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()));
    clock.advance(800ms);
  }
  // Pending answers for longer than the window: the run is over, and a new one starts its clock.
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kNotYet, 0, clock.now()));
    clock.advance(500ms);
  }
  for (int i = 0; i < 100; ++i) {  // 80 s of "reconnecting" again, less than the limit
    EXPECT_FALSE(watch.note(
        RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()));
    clock.advance(800ms);
  }
  EXPECT_FALSE(watch.recycled());
}

TEST(RecycleWatch, AFrameThatBeginsEndsTheRunsAndStartsTheClockAgain)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  clock.advance(60s);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()));
  clock.advance(1s);
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kBegan, 0, clock.now()));
  // The errors of before do not count; the limit of 45 s counts from the frame.
  clock.advance(40s);
  for (int i = 0; i < 10; ++i) {  // errors every 500 ms, from 40 s to 44.5 s after the frame
    EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()))
      << "step " << i << " of the errors before the limit";
    clock.advance(500ms);
  }
  EXPECT_TRUE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()))
    << "45 s after the frame";
  EXPECT_EQ(watch.seconds_without_frame(clock.now()), 45);
}

TEST(RecycleWatch, TheDefaultTimingsAre45SecondsAndACapOf240)
{
  const RenderRecycleTimings t;
  EXPECT_EQ(t.after, 45000ms);
  EXPECT_EQ(t.cap, 240000ms);
  EXPECT_EQ(t.reconnecting_limit, 90000ms);
}

// The shape of the Hub restart: the runtime answers "reconnecting" for a long time, then errors
// for less than the limit, then it asks for frames. The time without a request counts from the
// end of the reconnecting run, so the sender is not made again just before it is served.
TEST(RecycleWatch, ErrorsAfterALongReconnectingRunDoNotCountTheRunAsTimeWithoutARequest)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  for (int i = 0; i < 160; ++i) {  // 60 s of "reconnecting", less than its limit of 90 s
    EXPECT_FALSE(watch.note(
        RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()));
    clock.advance(375ms);
  }
  for (int i = 0; i < 120; ++i) {  // 30 s of errors, less than the limit of 45 s
    EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kNotReady, 0, clock.now()));
    EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()))
      << "the error at " << i * 250 << " ms after the run";
    clock.advance(250ms);
  }
  EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kBegan, 0, clock.now()));
  EXPECT_FALSE(watch.recycled());
}

TEST(RecycleWatch, ErrorsForTheWholeLimitAfterAReconnectingRunMakeTheSenderAgainOnce)
{
  RecycleWatchClock clock;
  auto count = std::make_shared<qar_qar_adapter::RenderRecycleCount>(0u);
  RenderRecycleWatch watch(RenderRecycleTimings{}, count, clock.now());
  clock.advance(5s);
  for (int i = 0; i < 20; ++i) {  // 10 s of "reconnecting"
    EXPECT_FALSE(watch.note(
        RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()));
    clock.advance(500ms);
  }
  // The last "reconnecting" answer was 500 ms ago; the clock of the rule started there.
  clock.advance(-500ms);
  long long recycles = 0;
  long long at_ms = -1;
  for (long long ms = 250; ms <= 60000; ms += 250) {
    clock.advance(250ms);
    if (watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now())) {
      ++recycles;
      if (at_ms < 0) { at_ms = ms; }
    }
  }
  EXPECT_EQ(recycles, 1);
  EXPECT_EQ(at_ms, 45000) << "not before 45 s after the last answer \"reconnecting\"";
  EXPECT_EQ(count->load(), 1u);
  EXPECT_EQ(watch.last_code(), kStatusError);
}

TEST(RecycleWatch, ASenderThatNeverReconnectedIsMadeAgainAfterTheLimitFromItsCreation)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  bool recycled = false;
  long long at_ms = 0;
  for (long long ms = 0; ms <= 60000 && !recycled; ms += 250) {
    recycled = watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now());
    at_ms = ms;
    if (!recycled) { clock.advance(250ms); }
  }
  EXPECT_TRUE(recycled);
  EXPECT_EQ(at_ms, 45000);
  EXPECT_EQ(watch.seconds_without_frame(clock.now()), 45);
}

TEST(RecycleWatch, AReconnectingRunThatReturnsStartsTheClockAgain)
{
  RecycleWatchClock clock;
  RenderRecycleWatch watch(RenderRecycleTimings{}, nullptr, clock.now());
  for (int round = 0; round < 3; ++round) {
    EXPECT_FALSE(watch.note(
        RenderRecycleWatch::Answer::kReconnecting, kStatusStreamReconnecting, clock.now()));
    for (int i = 0; i < 120; ++i) {  // 30 s of errors after each run
      clock.advance(250ms);
      EXPECT_FALSE(watch.note(RenderRecycleWatch::Answer::kError, kStatusError, clock.now()))
        << "round " << round << " step " << i;
    }
  }
  EXPECT_FALSE(watch.recycled());
}


// ---------------------------------------------------------------------------------------------
// A Hub restart: the 3D senders are released when the Hub process ends and made again after
// ---------------------------------------------------------------------------------------------

qar_qar_adapter::HubProcessInfo hub_info(
  std::uint32_t pid, std::uint32_t gone_count, std::uint32_t last_gone_pid)
{
  qar_qar_adapter::HubProcessInfo info;
  info.known = true;
  info.pid = pid;
  info.gone_count = gone_count;
  info.last_gone_pid = last_gone_pid;
  return info;
}

std::vector<QarPeerId> listed_as_closed(QarCApiSenderFactory & factory)
{
  std::vector<QarPeerId> out;
  factory.poll_closed_peers(out);
  return out;
}

std::vector<QarPeerId> requested(QarCApiSenderFactory & factory)
{
  std::vector<QarPeerId> out;
  factory.poll_new_peer_requests(out);
  return out;
}

TEST(HubRestart, TheSenderIsReleasedWhenTheHubProcessEndsAndMadeAgainWhenTheNewOneRuns)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(7);
  qar_qar_adapter::TargetInfo target;
  target.id = peer;
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 1);
  EXPECT_TRUE(rig.factory->note_hub_process(hub_info(4242, 0, 0)).empty())
    << "the first call seeds";
  EXPECT_TRUE(rig.factory->note_hub_process(hub_info(4242, 0, 0)).empty());
  EXPECT_TRUE(listed_as_closed(*rig.factory).empty());

  // The Hub process ends: the sender is listed as closed, and no request is handed out.
  const std::vector<std::string> gone = rig.factory->note_hub_process(hub_info(0, 1, 4242));
  ASSERT_EQ(gone.size(), 1u);
  EXPECT_TRUE(contains(gone[0], "pid 4242")) << gone[0];
  EXPECT_TRUE(contains(gone[0], "released now and made again")) << gone[0];
  EXPECT_TRUE(rig.factory->senders_held_for_hub());
  EXPECT_EQ(listed_as_closed(*rig.factory).size(), 1u);

  // What the producer does with a closed peer: destroy the stream, forget the peer. The target is
  // still served, so it is requested again - but not handed out while the Hub is away.
  adapter.reset();
  ASSERT_TRUE(wait_until([&rig] { return rig.h.fake().render_streams_destroyed() == 1; }));
  rig.factory->forget(peer);
  rig.factory->on_target_up(target);
  EXPECT_TRUE(requested(*rig.factory).empty()) << "held until the new Hub process runs";
  EXPECT_EQ(rig.h.fake().render_creates(), 1);

  // The new Hub process: the peer is handed out once and its sender is made.
  const std::vector<std::string> back = rig.factory->note_hub_process(hub_info(5151, 1, 4242));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_TRUE(contains(back[0], "4242 -> 5151")) << back[0];
  EXPECT_FALSE(rig.factory->senders_held_for_hub());
  EXPECT_EQ(requested(*rig.factory).size(), 1u);
  auto again = adapter_of(rig, peer);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 2);
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 1) << "exactly one destroy across the restart";
  EXPECT_EQ(rig.h.fake().count("gui_panels_get_or_create"), 0) << "no panel is touched";
  EXPECT_EQ(rig.h.fake().count("gui_panels_close_panel"), 0);
  EXPECT_EQ(rig.h.fake().count("gui_panels_set_state"), 0);
}

TEST(HubRestart, ARestartOfTheVisualizerAloneChangesNothing)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(7);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  rig.factory->note_hub_process(hub_info(4242, 0, 0));
  // The Hub process stays: the count of ended Hub processes does not move.
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(rig.factory->note_hub_process(hub_info(4242, 0, 0)).empty());
  }
  EXPECT_FALSE(rig.factory->senders_held_for_hub());
  EXPECT_TRUE(listed_as_closed(*rig.factory).empty());
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);
  EXPECT_EQ(rig.h.fake().render_creates(), 1);
}

TEST(HubRestart, AHubThatEndsWhileNoSenderExistsHoldsNothing)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.factory->note_hub_process(hub_info(4242, 0, 0));
  EXPECT_TRUE(rig.factory->note_hub_process(hub_info(0, 1, 4242)).empty());
  EXPECT_FALSE(rig.factory->senders_held_for_hub());
}

TEST(HubRestart, ACounterThatStartsAgainIsNotAHubThatEnded)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = adapter_of(rig, peer_with(7));
  ASSERT_NE(adapter, nullptr);
  rig.factory->note_hub_process(hub_info(4242, 1, 3131));
  // The owner made a new implementation after an abandoned call: the count of ended Hub processes
  // is zero again, and no Hub process ended.
  EXPECT_TRUE(rig.factory->note_hub_process(hub_info(4242, 0, 0)).empty());
  EXPECT_FALSE(rig.factory->senders_held_for_hub());
  EXPECT_TRUE(listed_as_closed(*rig.factory).empty());
  // A Hub process that ends afterwards is noticed.
  const std::vector<std::string> gone = rig.factory->note_hub_process(hub_info(0, 1, 4242));
  ASSERT_EQ(gone.size(), 1u);
  EXPECT_TRUE(contains(gone[0], "pid 4242")) << gone[0];
  EXPECT_TRUE(rig.factory->senders_held_for_hub());
}

TEST(HubRestart, ACreationInFlightWhenTheHubEndsIsSaidToBeNotHeld)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.factory->note_hub_process(hub_info(4242, 0, 0));
  rig.h.fake().script_render_create(never_answers(true));
  EXPECT_EQ(rig.factory->create_sender(peer_with(12)), nullptr);
  ASSERT_EQ(rig.h.fake().render_creates_pending(), 1);
  const std::vector<std::string> gone = rig.factory->note_hub_process(hub_info(0, 1, 4242));
  ASSERT_EQ(gone.size(), 1u) << "no sender exists, so there is nothing to release";
  EXPECT_TRUE(contains(gone[0], "pid 4242")) << gone[0];
  EXPECT_TRUE(contains(gone[0], "was being created")) << gone[0];
  EXPECT_TRUE(contains(gone[0], "not held")) << gone[0];
  EXPECT_FALSE(rig.factory->senders_held_for_hub());
}

TEST(RenderRequests, EachPairOfATargetAndAStreamIsLoggedOnceWithTheIdOfOurSender)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(9);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  EXPECT_EQ(rig.h.fake().count("render_sender_subscribe_requests"), 1);
  const QarStreamId ours = qar_qar_adapter::test::FakeQarApi::render_stream_id(1);
  const QarStreamId other = qar_qar_adapter::test::FakeQarApi::render_stream_id(7);

  testing::internal::CaptureStderr();
  // The own id is read on the owner's thread: ask until it is known (the request waits for it).
  int printed = 0;
  rig.h.fake().deliver_render_request(peer, ours);
  ASSERT_TRUE(wait_until([&] {
        printed += rig.factory->log_render_requests();
        return printed >= 1;
      }));
  rig.h.fake().deliver_render_request(peer, ours);   // the same pair again: no second line
  rig.h.fake().deliver_render_request(peer, other);  // another stream of the same target
  printed += rig.factory->log_render_requests();
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(printed, 2) << err;
  EXPECT_TRUE(contains(err, "on stream d1d1d1d1 (ours d1d1d1d1: match)")) << err;
  EXPECT_TRUE(contains(err, "on stream d7d7d7d7 (ours d1d1d1d1: MISMATCH)")) << err;
  EXPECT_EQ(rig.h.fake().render_requests_released(), 3) << "every request handle is released";
  EXPECT_EQ(rig.factory->log_render_requests(), 0);
}

TEST(RenderRequests, ARequestAfterTheSenderIsForgottenIsNotAMatchForItsOldId)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(9);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  const QarStreamId ours = qar_qar_adapter::test::FakeQarApi::render_stream_id(1);
  testing::internal::CaptureStderr();
  rig.h.fake().deliver_render_request(peer, ours);
  int printed = 0;
  ASSERT_TRUE(wait_until([&] {
        printed += rig.factory->log_render_requests();
        return printed >= 1;
      }));
  // The sender is destroyed and the peer forgotten: the old id is no sender of ours.
  adapter.reset();
  ASSERT_TRUE(wait_until([&rig] { return rig.h.fake().render_streams_destroyed() == 1; }));
  rig.factory->forget(peer);
  const QarStreamId asked = qar_qar_adapter::test::FakeQarApi::render_stream_id(2);
  rig.h.fake().deliver_render_request(peer, asked);
  EXPECT_EQ(rig.factory->log_render_requests(), 0) << "waits for the id of the next sender";
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(err.find("d2d2d2d2"), std::string::npos) << err;
}

TEST(RenderRequests, APairPrintedAsUnknownIsPrintedAgainWhenOurIdIsRead)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(9);
  auto adapter = adapter_of(rig, peer);
  ASSERT_NE(adapter, nullptr);
  adapter.reset();
  ASSERT_TRUE(wait_until([&rig] { return rig.h.fake().render_streams_destroyed() == 1; }));
  rig.factory->forget(peer);  // our id is unknown until the next sender exists

  testing::internal::CaptureStderr();
  const QarStreamId asked = qar_qar_adapter::test::FakeQarApi::render_stream_id(2);
  rig.h.fake().deliver_render_request(peer, asked);
  int printed = 0;
  ASSERT_TRUE(wait_until([&] {
        printed += rig.factory->log_render_requests();
        return printed >= 1;
      }, 6s));
  EXPECT_EQ(printed, 1);
  EXPECT_EQ(rig.factory->log_render_requests(), 0);

  // The next sender of the target: its id is read, and the same pair is printed with the verdict.
  auto again = adapter_of(rig, peer);
  ASSERT_NE(again, nullptr);
  ASSERT_TRUE(wait_until([&] {
        printed += rig.factory->log_render_requests();
        return printed >= 2;
      }));
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(err, "(ours unknown: unknown)")) << err;
  EXPECT_TRUE(contains(err, "d2d2d2d2 (ours ")) << err;
  EXPECT_EQ(printed, 2) << err;
}

}  // namespace
