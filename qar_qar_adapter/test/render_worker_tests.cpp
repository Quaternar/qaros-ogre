// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The 3D render sender when its QarOS frame calls run on a worker thread: every frame call is
// made by the worker, the render thread never waits for a call that blocks, a sender released
// while its worker is inside a call returns at once and its stream is destroyed only when the
// call returns, the frame-call breaker pauses and resumes on the worker, and the WGL context of a
// stream is deleted only after that stream was destroyed. The owner runs on the fake QarOS
// library; no test loads the real one and no GL object is made. Every time bound has a lower and
// an upper side.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_gl_fence.hpp"
#include "outage_test_kit.hpp"
#include "qar_ar_core/common_name.hpp"
#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"

namespace qar_qar_adapter
{

// The friend of the factory that lets a test build a factory that chains no GL context, and hand it
// contexts with the flag of their stream.
struct QarCApiSenderFactoryWorkerAccess
{
  static void skip_gl(QarCApiSenderFactory & factory) { factory.chain_gl_context_ = false; }

  static std::shared_ptr<std::atomic<bool>> add_context(
    QarCApiSenderFactory & factory, void * context)
  {
    auto ctx = std::make_unique<QarCApiSenderFactory::GlSenderContext>();
    ctx->context = context;
    ctx->stream_released = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> flag = ctx->stream_released;
    factory.gl_sender_contexts_.push_back(std::move(ctx));
    return flag;
  }

  static void capture(QarCApiSenderFactory & factory) { factory.capture_gl_context(); }

  static void set_deleter(void (* deleter)(void *))
  {
    QarCApiSenderFactory::set_gl_context_deleter(deleter);
  }
};

}  // namespace qar_qar_adapter

namespace
{

using qar_qar_adapter::FrameWorker;
using qar_qar_adapter::FrameWorkerTimings;
using qar_qar_adapter::QarCApiRenderSenderAdapter;
using qar_qar_adapter::QarCApiSenderFactory;
using qar_qar_adapter::QarCApiSenderFactoryWorkerAccess;
using qar_qar_adapter::abandoned_owner_threads;
using qar_qar_adapter::abandoned_owner_threads_running;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::HubGone;
using qar_qar_adapter::test::RenderFrameScript;
using qar_qar_adapter::test::allow_abandons;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

constexpr const char * kShowFrame = "render_sender_show_frame";
constexpr const char * kTryBegin = "render_sender_try_begin_frame";
constexpr const char * kFrameOpengl = "render_sender_frame_opengl";

QarPeerId peer_with(std::uint8_t tag)
{
  QarPeerId id{};
  for (std::size_t i = 0; i < sizeof(id.data); ++i) {
    id.data[i] = tag;
  }
  return id;
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

// The fake answers every try-begin "ready", so each opens a frame cycle that only a show-frame
// ends. True when no try-begin comes while a cycle is open and no show-frame comes without a cycle.
bool cycles_are_paired(const std::vector<qar_qar_adapter::test::Call> & calls)
{
  bool open = false;
  for (const qar_qar_adapter::test::Call & call : calls) {
    if (call.name == kTryBegin) {
      if (open) {
        return false;
      }
      open = true;
    } else if (call.name == kShowFrame) {
      if (!open) {
        return false;
      }
      open = false;
    }
  }
  return true;
}

double milliseconds_since(std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

// Captures standard error until finish() or the end of the scope, so that an assertion that
// returns early does not leave a capture open.
class StderrCapture
{
public:
  StderrCapture() { testing::internal::CaptureStderr(); }
  ~StderrCapture() { finish(); }
  StderrCapture(const StderrCapture &) = delete;
  StderrCapture & operator=(const StderrCapture &) = delete;

  std::string finish()
  {
    if (active_) {
      active_ = false;
      text_ = testing::internal::GetCapturedStderr();
    }
    return text_;
  }

private:
  bool active_ = true;
  std::string text_;
};

// Runs @p fn on a thread of its own and tells whether it returned within @p limit. When it did not,
// @p unblock runs, so that a test whose subject waits fails instead of hanging.
bool returns_within_or_unblock(
  const std::function<void()> & fn, std::chrono::milliseconds limit,
  const std::function<void()> & unblock)
{
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> result = done->get_future();
  std::thread runner([fn, done] {
      fn();
      done->set_value();
    });
  const bool in_time = result.wait_for(limit) == std::future_status::ready;
  if (!in_time) {
    unblock();
  }
  runner.join();
  return in_time;
}

QarCApiSenderFactory::Config worker_config(bool on_worker = true)
{
  QarCApiSenderFactory::Config config;
  config.name_scope = qar_ar_core::make_name_scope("test-program", "", "lab-pc");
  config.frame_calls_on_worker = on_worker;
  config.frame_worker_timings.slow_call = 50ms;
  config.frame_worker_timings.wait_notice = 150ms;
  return config;
}

// An owner that is connected and a factory on it that is attached and chains no GL context. The
// destructor lets every frame worker and owner thread end before the fake library is reset.
class Rig
{
public:
  explicit Rig(const QarCApiSenderFactory::Config & config = worker_config())
  {
    use_scaled_timings(h);
    h.config().timings.frame_worker_settle = 200ms;
    h.start();
    ok = h.connect_and_run();
    factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), config);
    QarCApiSenderFactoryWorkerAccess::skip_gl(*factory);
    ok = ok && factory->attach();
  }

  ~Rig()
  {
    h.fake().set_hub_gone(HubGone::kNo);
    h.fake().set_frame_block(false);
    h.fake().set_frame_delay_ms(0);
    h.fake().release_pending();
    wait_until([] { return FrameWorker::running() == 0; }, 10s);
    wait_until([] { return abandoned_owner_threads_running() == 0; }, 10s);
    FrameWorker::reap();
  }

  Rig(const Rig &) = delete;
  Rig & operator=(const Rig &) = delete;

  // The sender of @p peer, once the factory hands it out.
  std::shared_ptr<QarCApiRenderSenderAdapter> sender(const QarPeerId & peer)
  {
    std::shared_ptr<qar_ogre_streaming::IQarRenderSenderAdapter> made;
    wait_until([&] {
        made = factory->create_sender(peer);
        return made != nullptr;
      });
    return std::dynamic_pointer_cast<QarCApiRenderSenderAdapter>(made);
  }

  Harness h;
  std::shared_ptr<QarCApiSenderFactory> factory;
  bool ok = false;
};

// One tick of the producer: begin_frame until a frame is ready, render nothing, submit it.
// @return 1 the frame was submitted, 0 the submit refused it, -1 no frame came within @p limit.
int run_frame(
  QarCApiRenderSenderAdapter & adapter, std::chrono::milliseconds limit = 3s,
  std::uint64_t * index = nullptr)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    qar_ogre_streaming::QarFrameInfo info;
    adapter.begin_frame(info);
    if (info.should_render) {
      if (index != nullptr) {
        *index = info.frame_index;
      }
      return adapter.submit_opengl_stereo_frame(frame) ? 1 : 0;
    }
    std::this_thread::sleep_for(1ms);
  }
  return -1;
}

// Frames until one is submitted, then waits until the worker has shown it.
bool show_one(Rig & rig, QarCApiRenderSenderAdapter & adapter)
{
  const int before = rig.h.fake().count(kShowFrame);
  for (int tries = 0; tries < 5; ++tries) {
    const int result = run_frame(adapter);
    if (result < 0) {
      return false;
    }
    if (result == 1) {
      break;
    }
  }
  return wait_until([&] { return rig.h.fake().count(kShowFrame) > before; });
}

void frames_are_ready(Rig & rig)
{
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
}

// Blocks the frame calls of the fake and ticks the render side until the worker is inside a call.
bool block_the_worker(Rig & rig, QarCApiRenderSenderAdapter & adapter)
{
  rig.h.fake().set_frame_block(true);
  bool blocked = false;
  // The render side must not wait for the worker, so the ticks end in time; a frame call on the
  // caller would wait for the fake and the unblock lets the test fail instead of hang.
  const bool in_time = returns_within_or_unblock([&] {
      qar_ogre_streaming::QarOpenGlStereoFrame frame;
      const auto end = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < end && !blocked) {
        qar_ogre_streaming::QarFrameInfo info;
        adapter.begin_frame(info);
        if (info.should_render) {
          adapter.submit_opengl_stereo_frame(frame);
        }
        blocked = rig.h.fake().frame_calls_blocked() == 1;
        std::this_thread::sleep_for(1ms);
      }
    }, 5s, [&] { rig.h.fake().set_frame_block(false); });
  return in_time && blocked;
}

std::shared_ptr<QarCApiRenderSenderAdapter> ready_sender(Rig & rig, std::uint8_t tag)
{
  frames_are_ready(rig);
  return rig.sender(peer_with(tag));
}

// Breaker times shortened by a factor of about 40; a call of 100 ms or more is slow.
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

TEST(RenderWorker, TheDefaultIsTheWorkerAndTheFactoryNamesIt)
{
  EXPECT_TRUE(qar_qar_adapter::default_frame_calls_on_worker());
  EXPECT_TRUE(QarCApiSenderFactory::Config().frame_calls_on_worker);

  testing::internal::CaptureStderr();
  {
    Rig rig;
    ASSERT_TRUE(rig.ok);
  }
  std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(
      err, "[qar-adapter] frame calls run on one worker thread per sender "
      "(QAR_FRAME_CALLS=worker)\n")) << err;

  testing::internal::CaptureStderr();
  {
    Rig rig(worker_config(/*on_worker=*/false));
    ASSERT_TRUE(rig.ok);
  }
  err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(
      err, "[qar-adapter] frame calls run on the GUI thread (QAR_FRAME_CALLS=gui)\n")) << err;
}

// Test 1 of the frame design: after one shown frame every frame call was made by a thread that is
// neither the caller nor the owner's, and all by the same one.
TEST(RenderWorker, EveryFrameCallRunsOnTheWorkersThread)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  StderrCapture capture;
  const auto adapter = ready_sender(rig, 21);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(adapter->frame_calls_on_worker());
  ASSERT_TRUE(show_one(rig, *adapter));
  // The second show begins after the line about the first one was written.
  ASSERT_TRUE(show_one(rig, *adapter));
  const std::string err = capture.finish();

  const std::thread::id caller = std::this_thread::get_id();
  const std::thread::id begin = rig.h.fake().thread_of_last(kTryBegin);
  EXPECT_NE(begin, std::thread::id());
  EXPECT_NE(begin, caller);
  EXPECT_NE(begin, rig.h.owner_thread());
  EXPECT_EQ(rig.h.fake().thread_of_last("render_sender_last_hands"), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last("render_frame_info_get_view_pose"), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last("render_frame_info_get_view_fov"), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last(kFrameOpengl), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last(kShowFrame), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last("render_frame_info_handle_destroy"), begin);
  EXPECT_TRUE(contains(err, " peer 15151515 frame worker started tid=")) << err;
  EXPECT_TRUE(contains(err, " peer 15151515 first show_frame tid=")) << err;
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 0) << "the worker destroys the frame infos";
}

// The layout of the frames is read from the sender before the first frame, so the first cycle of a
// stream is rendered and shown like every other one.
TEST(RenderWorker, TheFirstCycleIsShownWithTheLayoutOfTheSender)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 22);
  ASSERT_NE(adapter, nullptr);
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  EXPECT_EQ(run_frame(*adapter, 3s, &first), 1) << "the first cycle is not dropped";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 1; }));
  EXPECT_EQ(run_frame(*adapter, 3s, &second), 1);
  EXPECT_GT(second, first);
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 2; }));
  EXPECT_EQ(rig.h.fake().count("render_sender_layout"), 1)
    << "the layout is read once, before the first frame";
  EXPECT_GE(rig.h.fake().count(kFrameOpengl), 2);
  const std::vector<qar_qar_adapter::test::Call> calls = rig.h.fake().calls();
  EXPECT_TRUE(cycles_are_paired(calls));
}

// A sender whose layout cannot be read: the first cycle has no texture ids. It is not abandoned
// either: it is shown as the runtime holds it, and the layout of that frame serves the next ones.
TEST(RenderWorker, AFirstCycleWithoutTextureIdsIsShownAsTheRuntimeHoldsIt)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.h.fake().set_call_failure("render_sender_layout", 1802);
  const auto adapter = ready_sender(rig, 23);
  ASSERT_NE(adapter, nullptr);
  EXPECT_EQ(run_frame(*adapter), 0) << "the frame could not be written into unknown textures";
  EXPECT_TRUE(contains(adapter->last_skip_detail(), "shown as they are"))
    << adapter->last_skip_detail();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 1; }))
    << "the cycle that was begun ends in a show-frame";
  EXPECT_EQ(run_frame(*adapter), 1) << "the layout of that frame describes the next cycle";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 2; }));
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
}

// Test 2: the render thread never waits for the worker.
TEST(RenderWorker, TheRenderThreadNeverWaitsForABlockedCall)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(23);
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));

  StderrCapture capture;
  rig.h.fake().set_frame_block(true);
  std::vector<double> took_ms;
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  const bool in_time = returns_within_or_unblock([&] {
      for (int i = 0; i < 200; ++i) {
        const auto start = std::chrono::steady_clock::now();
        qar_ogre_streaming::QarFrameInfo info;
        adapter->begin_frame(info);
        if (info.should_render) {
          adapter->submit_opengl_stereo_frame(frame);
        }
        took_ms.push_back(milliseconds_since(start));
        if (i == 10) {
          std::this_thread::sleep_for(1ms);
        }
      }
    }, 3s, [&] { rig.h.fake().set_frame_block(false); });
  const bool blocked = wait_until([&] { return rig.h.fake().frame_calls_blocked() == 1; });
  ASSERT_TRUE(in_time) << "the render thread waited for the blocked worker";
  ASSERT_EQ(took_ms.size(), 200u);
  std::vector<double> sorted = took_ms;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_LT(sorted[189], 5.0) << "the 95th percentile of 200 ticks";
  EXPECT_LT(sorted.back(), 100.0) << "the slowest tick";
  ASSERT_TRUE(blocked);

  // Before wait_notice the stream looks as it does while it waits for the viewer.
  EXPECT_FALSE(adapter->frame_calls_stopped());
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer));
  // After it the skip detail names the call, and the frame calls count as stopped.
  std::this_thread::sleep_for(260ms);
  qar_ogre_streaming::QarFrameInfo info;
  EXPECT_TRUE(adapter->begin_frame(info));
  EXPECT_FALSE(info.should_render);
  EXPECT_TRUE(contains(adapter->last_skip_detail(), "waiting for QarOS: qar_render_sender_"))
    << adapter->last_skip_detail();
  EXPECT_TRUE(contains(adapter->last_skip_detail(), " for 0.")) << adapter->last_skip_detail();
  EXPECT_TRUE(adapter->frame_calls_stopped());
  EXPECT_TRUE(rig.factory->frame_calls_stopped(peer));
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer_with(99))) << "a peer without a stream";

  // The call returns: the frames go on, and the line says how long the worker was away.
  const int shown = rig.h.fake().count(kShowFrame);
  rig.h.fake().set_frame_block(false);
  const bool shown_again = wait_until([&] {
      run_frame(*adapter, 100ms);
      return rig.h.fake().count(kShowFrame) > shown;
    });
  const std::string err = capture.finish();
  EXPECT_TRUE(shown_again);
  EXPECT_FALSE(adapter->frame_calls_stopped());
  EXPECT_TRUE(contains(err, " frame worker waiting in qar_render_sender_")) << err;
  EXPECT_TRUE(contains(err, " frame worker free again after ")) << err;
}

// Test 3: a sender released while its worker is inside a call returns at once; its stream is
// destroyed on the owner's thread, exactly once, when the call returns.
TEST(RenderWorker, ReleasingASenderWhileItsWorkerBlocksReturnsAtOnce)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = ready_sender(rig, 24);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(block_the_worker(rig, *adapter));
  std::this_thread::sleep_for(200ms);
  ASSERT_EQ(rig.h.fake().render_streams_destroyed(), 0);

  testing::internal::CaptureStderr();
  const bool in_time = returns_within_or_unblock(
    [&] { adapter.reset(); }, 2s, [&] { rig.h.fake().set_frame_block(false); });
  const std::string err = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(in_time) << "the destructor waited for the blocked worker";
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0) << "nothing is released while it blocks";
  EXPECT_TRUE(contains(err, " frame worker left running inside qar_render_sender_")) << err;
  EXPECT_TRUE(contains(err, "; its stream handle stays unreleased")) << err;
  EXPECT_EQ(rig.h.owner().release_lane()->workers_alive(), 1);

  rig.h.fake().set_frame_block(false);
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  const std::thread::id destroyed_on = rig.h.fake().thread_of_last("render_stream_handle_destroy");
  EXPECT_EQ(destroyed_on, rig.h.owner_thread()) << "the owner's thread destroys the stream";
  EXPECT_NE(destroyed_on, std::this_thread::get_id());
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  FrameWorker::reap();
  EXPECT_EQ(FrameWorker::parked(), 0) << "the reaper joined the ended thread";
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 1) << "destroyed exactly once";
  EXPECT_EQ(rig.h.owner().release_lane()->workers_alive(), 0);
}

TEST(RenderWorker, AHealthySenderIsReleasedOnTheOwnersThreadAndNeverOnTheCaller)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = ready_sender(rig, 25);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  const auto start = std::chrono::steady_clock::now();
  adapter.reset();
  EXPECT_LT(milliseconds_since(start), 20.0);
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(rig.h.fake().live_render_senders(), 0);
  const std::thread::id destroyed_on = rig.h.fake().thread_of_last("render_stream_handle_destroy");
  EXPECT_EQ(destroyed_on, rig.h.owner_thread());
  EXPECT_NE(destroyed_on, std::this_thread::get_id());
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
}

// Test 4: the owner ends while a worker never returns: the destructor keeps its budget, the
// session, the runtime and the library stay, and the worker that was left behind makes no call.
TEST(RenderWorker, TheOwnerEndsWithinItsBudgetWhileAWorkerNeverReturns)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = ready_sender(rig, 26);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(block_the_worker(rig, *adapter));
  adapter.reset();
  rig.factory->detach();
  rig.factory.reset();
  const int owners_before = abandoned_owner_threads();

  const auto start = std::chrono::steady_clock::now();
  const bool in_time = rig.h.destroy_owner_within(rig.h.config().timings.exit_join + 200ms);
  const double took_ms = milliseconds_since(start);
  if (!in_time) {
    rig.h.fake().set_frame_block(false);
  }
  ASSERT_TRUE(in_time) << "the destructor took " << took_ms << " ms";
  EXPECT_GE(took_ms, 150.0) << "it waited for the worker first (frame_worker_settle 200 ms)";
  EXPECT_LE(took_ms, 800.0) << "exit_join 600 ms plus 200 ms";
  EXPECT_EQ(abandoned_owner_threads(), owners_before) << "the owner thread itself ended";
  EXPECT_TRUE(rig.h.logged(
      "QAROS: exit: 1 frame worker(s) still inside a QarOS call; the session, the runtime and "
      "the library stay loaded"));
  EXPECT_EQ(rig.h.fake().count("session_handle_destroy"), 0);
  EXPECT_EQ(rig.h.fake().count("runtime_handle_destroy"), 0);
  EXPECT_EQ(rig.h.fake().count("library_destroy"), 0);
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);

  const std::size_t calls_at_exit = rig.h.fake().calls().size();
  rig.h.fake().set_frame_block(false);
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  EXPECT_EQ(rig.h.fake().calls().size(), calls_at_exit)
    << "the worker that was left behind makes no further QarOS call";
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);
}

// The worker is a registered user of the owner: a sender that is still alive when the owner ends
// keeps the stream (E-1), and its release afterwards returns at once and calls nothing.
TEST(RenderWorker, ASenderThatOutlivesTheOwnerMakesNoCallAndReturnsAtOnce)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = ready_sender(rig, 27);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(rig.h.destroy_owner_within());
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);

  const std::size_t calls_before = rig.h.fake().calls().size();
  EXPECT_LT(
    [&] {
      const auto start = std::chrono::steady_clock::now();
      adapter.reset();
      return milliseconds_since(start);
    }(), 20.0);
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0) << "the stream was left alone";
  EXPECT_EQ(rig.h.fake().calls().size(), calls_before);
}

// A submit without an open cycle (the test seam of the render-thread path) is refused.
TEST(RenderWorker, ASubmitWithoutAnOpenCycleIsRefused)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = rig.sender(peer_with(29));
  ASSERT_NE(adapter, nullptr);
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  EXPECT_FALSE(adapter->submit_opengl_stereo_frame(frame));
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 0);
}

// Two slow show_frame calls: the frame calls are paused when the second returns.
void trip(Rig & rig, QarCApiRenderSenderAdapter & adapter)
{
  rig.h.fake().set_frame_delay_ms(130, kShowFrame);
  ASSERT_TRUE(show_one(rig, adapter));
  ASSERT_TRUE(show_one(rig, adapter));
  ASSERT_TRUE(wait_until([&] { return adapter.frame_calls_stopped(); }));
  rig.h.fake().set_frame_delay_ms(0, kShowFrame);
}

// Ticks the render side every 2 ms until the fake has seen one more call of @p call (the probe).
// Returns the milliseconds from @p since to that call, or -1 when none came in 3 s.
double tick_until_called(
  Rig & rig, QarCApiRenderSenderAdapter & adapter, const char * call,
  std::chrono::steady_clock::time_point since)
{
  const int before = rig.h.fake().count(call);
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < end) {
    qar_ogre_streaming::QarFrameInfo info;
    adapter.begin_frame(info);
    if (rig.h.fake().count(call) > before) {
      const double waited = milliseconds_since(since);
      // The call reads its delay a moment after it is counted: let it, before the caller sets
      // the next one.
      std::this_thread::sleep_for(30ms);
      return waited;
    }
    std::this_thread::sleep_for(2ms);
  }
  return -1.0;
}

// Test 7: the breaker runs on the worker with its times unchanged.
TEST(RenderWorker, AFastProbeFrameAfterThePauseResumesTheFrameCalls)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(31);
  const auto adapter = ready_sender(rig, 31);
  ASSERT_NE(adapter, nullptr);
  adapter->set_frame_breaker_timings(quick_breaker());
  testing::internal::CaptureStderr();
  trip(rig, *adapter);
  std::string err = testing::internal::GetCapturedStderr();
  ASSERT_TRUE(adapter->frame_calls_stopped());
  EXPECT_TRUE(rig.factory->frame_calls_stopped(peer));
  EXPECT_TRUE(contains(err, "[qar-adapter] ")) << err;
  EXPECT_TRUE(contains(err, " peer 1f1f1f1f frame calls stopped: show_frame took ")) << err;
  EXPECT_TRUE(contains(err, " ms twice\n")) << err;

  // During the pause no frame call is made, however often the render side ticks.
  const int shown = rig.h.fake().count(kShowFrame);
  const int begun = rig.h.fake().count(kTryBegin);
  const int opened = rig.h.fake().count(kFrameOpengl);
  const auto paused_from = std::chrono::steady_clock::now();
  while (milliseconds_since(paused_from) < 90.0) {
    qar_ogre_streaming::QarFrameInfo info;
    EXPECT_TRUE(adapter->begin_frame(info));
    EXPECT_FALSE(info.should_render);
    EXPECT_TRUE(contains(adapter->last_skip_detail(), "frame calls stopped"));
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown);
  EXPECT_EQ(rig.h.fake().count(kTryBegin), begun);
  EXPECT_EQ(rig.h.fake().count(kFrameOpengl), opened);
  EXPECT_TRUE(adapter->frame_calls_stopped());

  // After it the next poll is the probe; it is fast, so the frame calls resume and one line says
  // how long they were paused.
  std::this_thread::sleep_for(150ms);
  testing::internal::CaptureStderr();
  const bool resumed = wait_until([&] {
      qar_ogre_streaming::QarFrameInfo info;
      adapter->begin_frame(info);
      return !adapter->frame_calls_stopped();
    });
  err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(resumed);
  EXPECT_FALSE(rig.factory->frame_calls_stopped(peer));
  EXPECT_TRUE(contains(err, " peer 1f1f1f1f frame calls resumed after 0.")) << err;
  EXPECT_TRUE(show_one(rig, *adapter)) << "normal frames go on";
}

TEST(RenderWorker, ASlowProbeDoublesThePauseUpToTheCap)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 32);
  ASSERT_NE(adapter, nullptr);
  adapter->set_frame_breaker_timings(quick_breaker());
  trip(rig, *adapter);
  // The probe is the first call of the next poll: try-begin.
  rig.h.fake().set_frame_delay_ms(130, kTryBegin);

  // The pause from a trip or a slow probe is 150, 300, 600, 600 ms (the cap): the next probe is
  // not made earlier. The wait is measured from the end of the call that started the pause.
  const double pauses_ms[] = {150.0, 300.0, 600.0, 600.0};
  auto since = std::chrono::steady_clock::now();
  for (const double pause : pauses_ms) {
    const double waited = tick_until_called(rig, *adapter, kTryBegin, since);
    ASSERT_GE(waited, 0.0) << "no probe within 3 s after a pause of " << pause << " ms";
    EXPECT_GE(waited, pause - 40.0) << "the pause is not cut short";
    EXPECT_LE(waited, pause + 500.0) << "the pause is not longer than the cap allows";
    // The slow probe ends 130 ms after it began; the next pause starts there.
    std::this_thread::sleep_for(120ms);
    EXPECT_TRUE(adapter->frame_calls_stopped()) << "a slow probe keeps the pause";
    since = std::chrono::steady_clock::now();
  }
}

TEST(RenderWorker, ASlowCallRightAfterAResumePausesAgainForTheLongerPause)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 33);
  ASSERT_NE(adapter, nullptr);
  adapter->set_frame_breaker_timings(quick_breaker());
  trip(rig, *adapter);

  // One slow probe (the pause doubles to 300 ms), then a fast one: the frame calls resume.
  rig.h.fake().set_frame_delay_ms(130, kTryBegin);
  ASSERT_GE(tick_until_called(rig, *adapter, kTryBegin, std::chrono::steady_clock::now()), 0.0);
  rig.h.fake().set_frame_delay_ms(0, kTryBegin);
  ASSERT_TRUE(wait_until([&] {
      qar_ogre_streaming::QarFrameInfo info;
      adapter->begin_frame(info);
      return !adapter->frame_calls_stopped();
    }, 3s));

  // The slow probe is less than a window ago, so one slow call pauses again, for 300 ms.
  rig.h.fake().set_frame_delay_ms(130, kShowFrame);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(wait_until([&] { return adapter->frame_calls_stopped(); }));
  rig.h.fake().set_frame_delay_ms(0, kShowFrame);
  const auto paused_from = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(200ms);
  const int begun = rig.h.fake().count(kTryBegin);
  qar_ogre_streaming::QarFrameInfo info;
  adapter->begin_frame(info);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(rig.h.fake().count(kTryBegin), begun) << "150 ms are over, 300 are not";
  const double waited = tick_until_called(rig, *adapter, kTryBegin, paused_from);
  EXPECT_GE(waited, 260.0);
}

TEST(RenderWorker, ThePauseIsTheFirstOneAgainAfterAHealthyWindow)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 34);
  ASSERT_NE(adapter, nullptr);
  adapter->set_frame_breaker_timings(quick_breaker());
  trip(rig, *adapter);
  rig.h.fake().set_frame_delay_ms(130, kTryBegin);
  ASSERT_GE(tick_until_called(rig, *adapter, kTryBegin, std::chrono::steady_clock::now()), 0.0);
  rig.h.fake().set_frame_delay_ms(0, kTryBegin);
  ASSERT_TRUE(wait_until([&] {
      qar_ogre_streaming::QarFrameInfo info;
      adapter->begin_frame(info);
      return !adapter->frame_calls_stopped();
    }, 3s));

  // No slow call for longer than the healthy window: the next pause is 150 ms, not 300.
  std::this_thread::sleep_for(1700ms);
  trip(rig, *adapter);
  const auto paused_from = std::chrono::steady_clock::now();
  const double waited = tick_until_called(rig, *adapter, kTryBegin, paused_from);
  EXPECT_GE(waited, 110.0);
  EXPECT_LE(waited, 300.0) << "a pause of 150 ms, not 300 ms";
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

// Test 8: the WGL context of a stream is deleted only after that stream was destroyed, whenever
// the frame worker posts the destroy; the context of another stream waits for its own.
TEST(RenderWorker, AContextIsFreedOnlyAfterItsOwnStreamWasDestroyedByTheWorker)
{
  g_deleted = 0;
  g_deleted_contexts.clear();
  QarCApiSenderFactoryWorkerAccess::set_deleter(&count_deleted_context);
  {
    Rig rig;
    ASSERT_TRUE(rig.ok);
    void * const first = reinterpret_cast<void *>(0x4001);
    void * const second = reinterpret_cast<void *>(0x4002);
    const auto first_flag = QarCApiSenderFactoryWorkerAccess::add_context(*rig.factory, first);
    const auto second_flag = QarCApiSenderFactoryWorkerAccess::add_context(*rig.factory, second);

    // The sender of the first context: a stream the fake does not know, a worker on the lane.
    rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
    auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
      reinterpret_cast<QarRenderSender *>(0x5001), QarNearFar{0.1f, 500.0f},
      qar_qar_adapter::kDefaultGlSubmitMode, nullptr, 1.0f, nullptr, peer_with(41), std::string(),
      rig.h.owner().release_lane(), first_flag, /*frame_calls_on_worker=*/true,
      worker_config().frame_worker_timings);
    ASSERT_TRUE(adapter->frame_calls_on_worker());
    ASSERT_TRUE(show_one(rig, *adapter));

    // The worker is inside a call when the sender and the factory are released.
    ASSERT_TRUE(block_the_worker(rig, *adapter));
    adapter.reset();
    rig.factory->detach();
    rig.factory.reset();
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 0) << "no stream was destroyed yet";

    // The call returns: the worker posts the release; the first context goes after its stream.
    rig.h.fake().set_frame_block(false);
    ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
    ASSERT_TRUE(wait_until([&] {
          QarCApiSenderFactory::delete_released_gl_contexts();
          return g_deleted.load() == 1;
        }));
    EXPECT_TRUE(first_flag->load());
    EXPECT_FALSE(second_flag->load());
    {
      std::lock_guard<std::mutex> lock(g_deleted_mutex);
      ASSERT_EQ(g_deleted_contexts.size(), 1u);
      EXPECT_EQ(g_deleted_contexts[0], first);
    }
    std::this_thread::sleep_for(50ms);
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 1) << "the second context waits for its own stream";

    second_flag->store(true);
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 2);
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 2) << "each context is deleted once";
  }
  QarCApiSenderFactoryWorkerAccess::set_deleter(nullptr);
}

// With the frame calls on the render thread the sender sets the same flag when it destroys the
// stream (the lane runs the destroy).
TEST(RenderWorker, TheFlagOfTheContextIsSetByTheReleaseOnTheRenderThreadPathToo)
{
  Rig rig(worker_config(/*on_worker=*/false));
  ASSERT_TRUE(rig.ok);
  const auto flag = std::make_shared<std::atomic<bool>>(false);
  auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
    reinterpret_cast<QarRenderSender *>(0x5002), QarNearFar{0.1f, 500.0f},
    qar_qar_adapter::kDefaultGlSubmitMode, nullptr, 1.0f, nullptr, peer_with(42), std::string(),
    rig.h.owner().release_lane(), flag, /*frame_calls_on_worker=*/false);
  EXPECT_FALSE(adapter->frame_calls_on_worker());
  adapter.reset();
  ASSERT_TRUE(wait_until([&] { return flag->load(); }));
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 1);
}

// A sender without a lane keeps its frame calls on the render thread (nothing could release its
// stream), and so does the supply-own mode.
TEST(RenderWorker, ASenderWithoutALaneOrInTheSupplyOwnModeKeepsItsFrameCallsOnTheCaller)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  QarCApiRenderSenderAdapter without_lane(
    reinterpret_cast<QarRenderSender *>(0x5003), QarNearFar{0.1f, 500.0f},
    qar_qar_adapter::kDefaultGlSubmitMode, nullptr, 1.0f, nullptr, peer_with(43), std::string(),
    nullptr, nullptr, /*frame_calls_on_worker=*/true);
  EXPECT_FALSE(without_lane.frame_calls_on_worker());
  QarCApiRenderSenderAdapter supply_own(
    reinterpret_cast<QarRenderSender *>(0x5004), QarNearFar{0.1f, 500.0f},
    qar_qar_adapter::GlSubmitMode::kSupplyOwnTextureIds, nullptr, 1.0f, nullptr, peer_with(44),
    std::string(), rig.h.owner().release_lane(), nullptr, /*frame_calls_on_worker=*/true);
  EXPECT_FALSE(supply_own.frame_calls_on_worker());
}

// A cycle the render thread took and never submitted stays open: the next begin_frame offers the
// same frame again, and the worker makes no second try-begin before that frame is shown.
TEST(RenderWorker, ACycleThatIsTakenAndNeverSubmittedIsOfferedAgain)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 28);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  const int shown = rig.h.fake().count(kShowFrame);

  // A frame is taken and not submitted.
  qar_ogre_streaming::QarFrameInfo info;
  ASSERT_TRUE(wait_until([&] {
      adapter->begin_frame(info);
      return info.should_render;
    }));
  const std::uint64_t index = info.frame_index;
  const int begun = rig.h.fake().count(kTryBegin);
  for (int tick = 0; tick < 5; ++tick) {
    qar_ogre_streaming::QarFrameInfo again;
    EXPECT_TRUE(adapter->begin_frame(again));
    EXPECT_TRUE(again.should_render) << "the frame that was taken is offered again";
    EXPECT_EQ(again.frame_index, index);
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(rig.h.fake().count(kTryBegin), begun) << "no second try-begin while the cycle is open";
  EXPECT_EQ(rig.h.fake().count(kShowFrame), shown);

  // It is rendered and shown at last.
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  EXPECT_TRUE(adapter->submit_opengl_stereo_frame(frame));
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == shown + 1; }));
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 0);
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
}

// The frame calls are paused by a slow call that answers "ready": the cycle that call began is
// closed with one show-frame, so that no cycle is left open and no begin follows an open one.
TEST(RenderWorker, ABreakerTripAfterAReadyBeginClosesTheCycleWithOneShow)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 33);
  ASSERT_NE(adapter, nullptr);
  QarCApiRenderSenderAdapter::FrameBreakerTimings timings = quick_breaker();
  timings.pause_first = 400ms;
  timings.pause_cap = 800ms;
  adapter->set_frame_breaker_timings(timings);
  ASSERT_TRUE(show_one(rig, *adapter));
  rig.h.fake().set_frame_delay_ms(130, kTryBegin);
  ASSERT_TRUE(wait_until([&] {
      qar_ogre_streaming::QarFrameInfo info;
      adapter->begin_frame(info);
      if (info.should_render) {
        qar_ogre_streaming::QarOpenGlStereoFrame frame;
        adapter->submit_opengl_stereo_frame(frame);
      }
      return adapter->frame_calls_stopped();
    }));
  ASSERT_TRUE(wait_until([&] {
      return rig.h.fake().count(kShowFrame) == rig.h.fake().count(kTryBegin);
    })) << "a show-frame for every try-begin";
  rig.h.fake().set_frame_delay_ms(0, kTryBegin);

  // The pause holds: no further call, however often the render side ticks.
  const std::size_t calls_at_trip = rig.h.fake().calls().size();
  const auto paused_from = std::chrono::steady_clock::now();
  while (milliseconds_since(paused_from) < 100.0) {
    qar_ogre_streaming::QarFrameInfo info;
    adapter->begin_frame(info);
    EXPECT_FALSE(info.should_render);
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(rig.h.fake().calls().size(), calls_at_trip);
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 0);

  // After the pause the stream goes on.
  std::this_thread::sleep_for(500ms);
  EXPECT_TRUE(show_one(rig, *adapter)) << "normal frames go on";
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
}

// A view getter that fails: the cycle that was begun is closed with a show-frame, the error is the
// skip detail until a frame is published, and the log line about it is rate limited.
TEST(RenderWorker, AFailedViewGetterClosesTheCycleAndItsErrorStaysVisible)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  const auto adapter = ready_sender(rig, 34);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  StderrCapture capture;
  rig.h.fake().set_call_failure("render_frame_info_get_view_pose", 1802);
  const int begun_before = rig.h.fake().count(kTryBegin);
  qar_ogre_streaming::QarFrameInfo info;
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  bool reported_as_error = false;
  ASSERT_TRUE(wait_until([&] {
      if (!adapter->begin_frame(info)) {
        reported_as_error = true;
      } else if (info.should_render) {
        adapter->submit_opengl_stereo_frame(frame);
      }
      return rig.h.fake().count(kTryBegin) >= begun_before + 12;
    }));
  ASSERT_TRUE(wait_until([&] {
      return rig.h.fake().count(kShowFrame) == rig.h.fake().count(kTryBegin);
    })) << "every cycle that was begun ends in a show-frame";
  EXPECT_TRUE(reported_as_error);
  EXPECT_TRUE(contains(adapter->last_skip_detail(), "qar_render_frame_info_get_view_pose(0)"))
    << adapter->last_skip_detail();
  const std::string err = capture.finish();
  int lines = 0;
  for (std::size_t at = err.find("frame call failed ("); at != std::string::npos;
    at = err.find("frame call failed (", at + 1))
  {
    ++lines;
  }
  EXPECT_EQ(lines, 5) << "the first five failures are logged, then one in a hundred: " << err;
  EXPECT_TRUE(contains(err, " frame call failed (1): qar_render_frame_info_get_view_pose(0)"))
    << err;
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 0);

  // The error stays the skip detail while the poll merely finds nothing, and until a frame is
  // published.
  rig.h.fake().set_call_failure("render_frame_info_get_view_pose", 0);
  EXPECT_TRUE(show_one(rig, *adapter)) << "the frames go on once the getter works again";
  qar_ogre_streaming::QarFrameInfo clean;
  adapter->begin_frame(clean);
  EXPECT_FALSE(contains(adapter->last_skip_detail(), "qar_render_frame_info_get_view_pose"));
}

namespace
{

// A copy hook that fails.
class FailingCopyHook : public qar_qar_adapter::IQarGlTextureCopyHook
{
public:
  bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame &, const QarVideoFrameOpenGl &) override
  {
    return false;
  }
};

}  // namespace

// A frame that cannot be copied into the textures of the runtime still ends its cycle: it is
// handed over and shown as the textures are, and the submit reports the failure.
TEST(RenderWorker, AFailedCopyStillEndsTheCycleWithAShow)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
    reinterpret_cast<QarRenderSender *>(0x5101), QarNearFar{0.1f, 500.0f},
    qar_qar_adapter::kDefaultGlSubmitMode, std::make_shared<FailingCopyHook>(), 1.0f, nullptr,
    peer_with(46), std::string(), rig.h.owner().release_lane(), nullptr,
    /*frame_calls_on_worker=*/true, worker_config().frame_worker_timings);
  ASSERT_TRUE(adapter->frame_calls_on_worker());
  EXPECT_EQ(run_frame(*adapter), 0);
  EXPECT_TRUE(contains(adapter->last_skip_detail(), "could not be copied"))
    << adapter->last_skip_detail();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 1; }));
  EXPECT_EQ(rig.h.fake().count(kTryBegin) >= 1, true);
  EXPECT_TRUE(cycles_are_paired(rig.h.fake().calls()));
  adapter.reset();
}

// A sender whose lane refuses the worker keeps its frame calls on the render thread and says so.
TEST(RenderWorker, ASenderWhoseWorkerCouldNotStartSaysSo)
{
  class Refusing : public qar_qar_adapter::IReleaseLane
  {
public:
    bool post_release(const char *, std::function<void()>) override { return false; }
    bool enter_worker(const std::shared_ptr<WorkerSlot> &) override { return false; }
  };
  Rig rig;
  ASSERT_TRUE(rig.ok);
  StderrCapture capture;
  {
    QarCApiRenderSenderAdapter adapter(
      reinterpret_cast<QarRenderSender *>(0x5102), QarNearFar{0.1f, 500.0f},
      qar_qar_adapter::kDefaultGlSubmitMode, nullptr, 1.0f, nullptr, peer_with(47),
      std::string(), std::make_shared<Refusing>(), nullptr, /*frame_calls_on_worker=*/true);
    EXPECT_FALSE(adapter.frame_calls_on_worker());
  }
  const std::string err = capture.finish();
  EXPECT_TRUE(contains(err, " peer 2f2f2f2f frame calls run on the GUI thread "
      "(no frame worker could start)\n")) << err;
}

// A sender context is deleted on the render thread as soon as the stream that used it was
// destroyed, not only when the factory ends.
TEST(RenderWorker, AContextOfAReleasedStreamIsDeletedAtTheNextCapture)
{
  g_deleted = 0;
  g_deleted_contexts.clear();
  QarCApiSenderFactoryWorkerAccess::set_deleter(&count_deleted_context);
  {
    Rig rig;
    ASSERT_TRUE(rig.ok);
    void * const first = reinterpret_cast<void *>(0x6001);
    void * const second = reinterpret_cast<void *>(0x6002);
    const auto first_flag = QarCApiSenderFactoryWorkerAccess::add_context(*rig.factory, first);
    const auto second_flag = QarCApiSenderFactoryWorkerAccess::add_context(*rig.factory, second);
    QarCApiSenderFactoryWorkerAccess::capture(*rig.factory);
    EXPECT_EQ(g_deleted.load(), 0) << "no stream was destroyed";

    first_flag->store(true);
    QarCApiSenderFactoryWorkerAccess::capture(*rig.factory);
    EXPECT_EQ(g_deleted.load(), 1);
    {
      std::lock_guard<std::mutex> lock(g_deleted_mutex);
      ASSERT_EQ(g_deleted_contexts.size(), 1u);
      EXPECT_EQ(g_deleted_contexts[0], first);
    }
    QarCApiSenderFactoryWorkerAccess::capture(*rig.factory);
    EXPECT_EQ(g_deleted.load(), 1) << "once";
    EXPECT_FALSE(second_flag->load());

    rig.factory->detach();
    rig.factory.reset();
    second_flag->store(true);
    QarCApiSenderFactory::delete_released_gl_contexts();
    EXPECT_EQ(g_deleted.load(), 2) << "the other one goes with its stream; the first is not "
      "deleted again";
  }
  QarCApiSenderFactoryWorkerAccess::set_deleter(nullptr);
}

// MINOR-1: with the frame trace on, a worker that the owner left behind inside a call keeps the
// handle of its open frame; nothing destroys it afterwards, on any thread. Needs
// QAR_ADAPTER_FRAME_TRACE in the environment of the process (it is read once).
TEST(RenderWorker, TheTraceHandleOfAWorkerThatWasLeftBehindIsNeverDestroyedByAnotherThread)
{
  if (qar_qar_adapter::read_environment("QAR_ADAPTER_FRAME_TRACE").empty()) {
    GTEST_SKIP() << "run with QAR_ADAPTER_FRAME_TRACE=1 in the environment";
  }
  Rig rig;
  ASSERT_TRUE(rig.ok);
  qar_qar_adapter::test::Gate gate;
  qar_qar_adapter::test::LeftBehindGuard guard(gate);
  qar_qar_adapter::test::hold_first_call(rig.h.fake(), kFrameOpengl, gate);
  rig.h.fake().set_render_frame(RenderFrameScript{true, 0});
  auto adapter = std::make_shared<QarCApiRenderSenderAdapter>(
    reinterpret_cast<QarRenderSender *>(0x5103), QarNearFar{0.1f, 500.0f},
    qar_qar_adapter::kDefaultGlSubmitMode, nullptr, 1.0f, nullptr, peer_with(45),
    rig.h.dir() + "/trace", rig.h.owner().release_lane(), nullptr,
    /*frame_calls_on_worker=*/true, worker_config().frame_worker_timings);
  ASSERT_TRUE(adapter->frame_calls_on_worker());
  ASSERT_TRUE(wait_until([&] {
      qar_ogre_streaming::QarFrameInfo info;
      adapter->begin_frame(info);
      return gate.entered();
    })) << "the worker is inside frame_opengl with its trace row open";
  ASSERT_EQ(rig.h.fake().live_render_frame_infos(), 1);
  adapter.reset();
  rig.factory->detach();
  rig.factory.reset();
  ASSERT_TRUE(rig.h.destroy_owner_within(rig.h.config().timings.exit_join + 200ms));
  const int destroys = rig.h.fake().count("render_frame_info_handle_destroy");
  gate.open();
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  FrameWorker::reap();
  EXPECT_EQ(rig.h.fake().count("render_frame_info_handle_destroy"), destroys)
    << "no thread destroys the handle of a left-behind worker";
  EXPECT_EQ(rig.h.fake().live_render_frame_infos(), 1)
    << "the handle is left to the runtime";
}


// ---------------------------------------------------------------------------------------------
//  The GL fence: the render thread ends its copy with a fence and a flush, the worker waits for it
// ---------------------------------------------------------------------------------------------

using qar_qar_adapter::IQarGlTextureCopyHook;
using qar_qar_adapter::test::FakeGl;

// A copy hook that copies nothing and tells which of its two copies was called.
class CountingCopyHook : public IQarGlTextureCopyHook
{
public:
  bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame &, const QarVideoFrameOpenGl &) override
  {
    ++loud;
    return true;
  }

  bool copy_frame_into_quiet(
    const qar_ogre_streaming::QarOpenGlStereoFrame &, const QarVideoFrameOpenGl &) override
  {
    ++quiet;
    return true;
  }

  bool mean_left_colour(
    const qar_ogre_streaming::QarOpenGlStereoFrame &, float (& rgb)[3]) override
  {
    if (readback_delay_ms.load() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(readback_delay_ms.load()));
    }
    rgb[0] = 10.0f;
    rgb[1] = 20.0f;
    rgb[2] = 30.0f;
    {
      std::lock_guard<std::mutex> lock(mutex);
      readback_thread = std::this_thread::get_id();
    }
    ++readbacks;
    return true;
  }

  void forget_texture_targets() override { ++forgets; }

  std::thread::id readback_thread_id()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return readback_thread;
  }

  std::atomic<int> forgets{0};
  std::atomic<int> loud{0};
  std::atomic<int> quiet{0};
  std::atomic<int> readbacks{0};
  std::atomic<int> readback_delay_ms{0};
  std::mutex mutex;
  std::thread::id readback_thread;
};

// Resets the fake GL before a test and lets every fence wait end when the test does, so that a
// worker never outlives its test inside a wait. Declared after the Rig: it is destroyed first.
class FakeGlScope
{
public:
  FakeGlScope() { FakeGl::get().reset(); }
  ~FakeGlScope()
  {
    FakeGl::get().unblock();
    FakeGl::get().set_auto_signal(true);
  }
  FakeGlScope(const FakeGlScope &) = delete;
  FakeGlScope & operator=(const FakeGlScope &) = delete;
};

QarCApiSenderFactory::Config fence_config(const std::shared_ptr<IQarGlTextureCopyHook> & hook)
{
  QarCApiSenderFactory::Config config = worker_config();
  config.copy_hook = hook;
  config.gl_fence_ops = FakeGl::get().ops();
  config.frame_fence_timings.slice = 10ms;
  config.frame_fence_timings.notice = 120ms;
  config.gpu_gate = false;
  return config;
}

// One tick that submits: waits for a frame, then times the submit call.
// @return the submit time in milliseconds, or -1 when no frame came.
double timed_submit(QarCApiRenderSenderAdapter & adapter, std::uint64_t * index = nullptr)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < end) {
    qar_ogre_streaming::QarFrameInfo info;
    adapter.begin_frame(info);
    if (info.should_render) {
      if (index != nullptr) {
        *index = info.frame_index;
      }
      const auto start = std::chrono::steady_clock::now();
      adapter.submit_opengl_stereo_frame(frame);
      return milliseconds_since(start);
    }
    std::this_thread::sleep_for(1ms);
  }
  return -1.0;
}

TEST(RenderFence, TheDefaultIsTheWorkerAndTheFactoryNamesIt)
{
  EXPECT_TRUE(qar_qar_adapter::default_frame_fence_on_worker());
  EXPECT_TRUE(QarCApiSenderFactory::Config().frame_fence_on_worker);
  EXPECT_TRUE(QarCApiSenderFactory::Config().gpu_gate);

  StderrCapture capture;
  {
    Rig rig(worker_config());
    ASSERT_TRUE(rig.ok);
    ASSERT_NE(rig.factory->gpu_watch(), nullptr);
    EXPECT_FALSE(rig.factory->gpu_watch()->gate_closed()) << "a watch that is not started is open";
  }
  std::string err = capture.finish();
  EXPECT_TRUE(contains(
      err, "[qar-adapter] GL fence waits run on the frame worker of each sender "
      "(QAR_FRAME_FENCE=worker)\n")) << err;
  EXPECT_TRUE(contains(
      err, "[qar-gpu] GPU progress gate on (QAR_GPU_GATE=on): RViz frames and the 3D render batch "
      "pause while a frame stays unfinished for more than max(50 ms, 3 x the median frame "
      "time)\n")) << err;

  testing::internal::CaptureStderr();
  {
    QarCApiSenderFactory::Config config = worker_config();
    config.frame_fence_on_worker = false;
    config.gpu_gate = false;
    Rig rig(config);
    ASSERT_TRUE(rig.ok);
  }
  err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(
      err, "[qar-adapter] GL fence waits run on the GUI thread (QAR_FRAME_FENCE=gui)\n")) << err;
  EXPECT_TRUE(contains(
      err, "[qar-gpu] GPU progress gate off (QAR_GPU_GATE=off): RViz frames and the 3D render "
      "batch are never paused\n")) << err;
}

TEST(RenderFence, TheProbesOfTheCopyAreDueOnlyWhereTheyDoNotWaitForTheGpu)
{
  using qar_qar_adapter::copy_probe_due;
  // The render thread's own path: frame 1 and every 600th, up to 6.
  EXPECT_TRUE(copy_probe_due(false, 1, 600, false, 0, 6));
  EXPECT_FALSE(copy_probe_due(false, 2, 600, false, 1, 6));
  EXPECT_TRUE(copy_probe_due(false, 600, 600, false, 1, 6));
  EXPECT_FALSE(copy_probe_due(false, 1200, 600, false, 6, 6)) << "at most 6";
  // The worker path: none, whatever the frame, unless QAR_ADAPTER_PROBE_EVERY was set.
  EXPECT_FALSE(copy_probe_due(true, 1, 600, false, 0, 6));
  EXPECT_FALSE(copy_probe_due(true, 600, 600, false, 0, 6));
  EXPECT_TRUE(copy_probe_due(true, 1, 600, true, 0, 6));
  EXPECT_TRUE(copy_probe_due(true, 10, 10, true, 1, 6));
  EXPECT_FALSE(copy_probe_due(true, 11, 10, true, 2, 6));
  EXPECT_FALSE(copy_probe_due(true, 20, 0, true, 2, 6)) << "an interval of 0 probes the first only";
}

// The copy of a worker sender is the quiet one, and with healthy fences the render thread inserts
// and the worker waits.
TEST(RenderFence, TheRenderThreadInsertsFencesAndTheWorkerWaitsForThemWithoutProbes)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  FakeGl::get().set_auto_signal(true);
  StderrCapture capture;
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer_with(31));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(adapter->frame_calls_on_worker());
  EXPECT_EQ(FakeGl::get().creates(), 1) << "the context is made at the creation of the sender";

  std::vector<double> took_ms;
  for (int i = 0; i < 50; ++i) {
    const int before = rig.h.fake().count(kShowFrame);
    const double ms = timed_submit(*adapter);
    ASSERT_GE(ms, 0.0) << "frame " << i;
    took_ms.push_back(ms);
    ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) > before; })) << i;
  }
  const std::string err = capture.finish();
  std::sort(took_ms.begin(), took_ms.end());
  EXPECT_LT(took_ms[47], 5.0) << "the 95th percentile of 50 submits";
  EXPECT_TRUE(adapter->frame_fence_active());
  EXPECT_EQ(hook->loud.load(), 0) << "no probing copy on the worker path";
  EXPECT_EQ(hook->quiet.load(), 50);
  EXPECT_EQ(FakeGl::get().inserts(), 50);
  EXPECT_EQ(FakeGl::get().removes(), 50) << "the worker deletes each fence once it is signalled";
  EXPECT_EQ(FakeGl::get().binds(), 1) << "the worker binds its context once";
  const auto inserters = FakeGl::get().insert_threads();
  ASSERT_EQ(inserters.size(), 1u);
  EXPECT_EQ(*inserters.begin(), std::this_thread::get_id())
    << "the fence is inserted by the caller";
  const auto waiters = FakeGl::get().wait_threads();
  ASSERT_EQ(waiters.size(), 1u);
  EXPECT_NE(*waiters.begin(), std::this_thread::get_id()) << "the caller never waits";
  const auto binders = FakeGl::get().bind_threads();
  ASSERT_EQ(binders.size(), 1u);
  EXPECT_EQ(*binders.begin(), *waiters.begin()) << "the thread that waits is the one that bound";
  EXPECT_FALSE(contains(err, "waiting for the GPU")) << err;
  EXPECT_FALSE(adapter->frame_calls_stopped());
}

// A fence that never signals. The submit returns at once, the frame is not shown, the line at the
// notice time and the skip detail say what the worker waits for, and a signalled fence shows
// exactly one frame.
TEST(RenderFence, AFenceThatNeverSignalsHoldsTheFrameAndNeverTheCaller)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(32);
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer);
  ASSERT_NE(adapter, nullptr);
  StderrCapture capture;

  std::uint64_t index = 99;
  const double submit_ms = timed_submit(*adapter, &index);
  ASSERT_GE(submit_ms, 0.0);
  EXPECT_LT(submit_ms, 5.0) << "the submit only inserts a fence";
  EXPECT_TRUE(adapter->frame_fence_active());

  // 200 ticks while the fence is unsignalled: none waits, none gets a frame, none shows one.
  std::vector<double> took_ms;
  for (int i = 0; i < 200; ++i) {
    const auto start = std::chrono::steady_clock::now();
    qar_ogre_streaming::QarFrameInfo info;
    adapter->begin_frame(info);
    took_ms.push_back(milliseconds_since(start));
    EXPECT_FALSE(info.should_render) << "the cycle is not finished";
    if (i == 20) {
      std::this_thread::sleep_for(1ms);
    }
  }
  std::sort(took_ms.begin(), took_ms.end());
  EXPECT_LT(took_ms[189], 5.0) << "the 95th percentile of 200 ticks";
  EXPECT_LT(took_ms.back(), 100.0);
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 0) << "nothing is shown while the GPU has not finished";
  EXPECT_FALSE(adapter->frame_calls_stopped()) << "before the notice time";

  std::this_thread::sleep_for(200ms);
  qar_ogre_streaming::QarFrameInfo info;
  EXPECT_TRUE(adapter->begin_frame(info));
  EXPECT_FALSE(info.should_render);
  EXPECT_TRUE(contains(
      adapter->last_skip_detail(), "waiting for the GPU: frame " + std::to_string(index) + " for "))
    << adapter->last_skip_detail();
  EXPECT_TRUE(adapter->frame_calls_stopped());
  EXPECT_TRUE(rig.factory->frame_calls_stopped(peer));
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 0);
  EXPECT_EQ(FakeGl::get().inserts(), 1);
  EXPECT_EQ(FakeGl::get().removes(), 0) << "the fence stays until it is signalled";

  FakeGl::get().signal_all();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) == 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 1) << "exactly one frame is shown";
  ASSERT_TRUE(wait_until([&] { return !adapter->frame_calls_stopped(); }));
  EXPECT_EQ(FakeGl::get().removes(), 1);
  const std::string err = capture.finish();
  const std::string g3 =
    " peer 20202020 frame worker waiting for the GPU (frame " + std::to_string(index) + ") for ";
  const std::size_t at = err.find(g3);
  ASSERT_NE(at, std::string::npos) << err;
  EXPECT_EQ(err.find(g3, at + 1), std::string::npos) << "the line is written once";
  EXPECT_TRUE(contains(
      err, " peer 20202020 the GPU finished frame " + std::to_string(index) + " after ")) << err;
}

// A sender released while its worker waits for a fence returns at once and shows no frame; the
// worker leaves at its next look at the stop flag, deletes the fence of the frame it dropped,
// releases and deletes its context, and the stream is destroyed.
TEST(RenderFence, ASenderReleasedWhileItsFrameWaitsForTheGpuShowsNothing)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  auto adapter = rig.sender(peer_with(33));
  ASSERT_NE(adapter, nullptr);
  ASSERT_GE(timed_submit(*adapter), 0.0);
  ASSERT_TRUE(wait_until([&] { return FakeGl::get().waits() >= 2; })) << "the worker is waiting";

  const auto start = std::chrono::steady_clock::now();
  adapter.reset();
  EXPECT_LT(milliseconds_since(start), 5.0) << "the release does not wait for the GPU";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 0) << "a released sender shows no frame";
  EXPECT_EQ(FakeGl::get().removes(), 1) << "the worker deletes the fence of the frame it dropped";
  EXPECT_EQ(FakeGl::get().unbinds(), 1);
  EXPECT_EQ(FakeGl::get().deletes(), 1) << "the worker deletes its own context, once";
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  FrameWorker::reap();
  EXPECT_EQ(FakeGl::get().deletes(), 1);
}

// A driver call that does not return: the worker cannot look at the stop flag. Its context is
// neither released nor deleted and the stream stays; when the call returns the worker leaves in
// order.
TEST(RenderFence, AContextIsNeverDeletedWhileItsWorkerIsStuckInADriverCall)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  auto adapter = rig.sender(peer_with(34));
  ASSERT_NE(adapter, nullptr);
  FakeGl::get().block_hard();
  ASSERT_GE(timed_submit(*adapter), 0.0);
  ASSERT_TRUE(wait_until([&] { return FakeGl::get().waits() >= 1; })) << "the worker is inside it";

  const auto start = std::chrono::steady_clock::now();
  adapter.reset();
  EXPECT_LT(milliseconds_since(start), 5.0);
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0) << "the stream is not released yet";
  EXPECT_EQ(FakeGl::get().unbinds(), 0);
  EXPECT_EQ(FakeGl::get().deletes(), 0) << "the context may be current on the stuck worker";
  EXPECT_EQ(FrameWorker::running(), 1);

  FakeGl::get().unblock();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(FakeGl::get().unbinds(), 1);
  EXPECT_EQ(FakeGl::get().deletes(), 1);
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 0);
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  FrameWorker::reap();
}

// A worker that cannot bind its context says so, and its sender goes on as before: it waits for
// the GPU on the caller's thread and shows its frames.
TEST(RenderFence, AWorkerThatCannotBindItsContextFallsBackToTheGuiThreadWait)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  FakeGl::get().set_bind_result(5);
  frames_are_ready(rig);
  StderrCapture capture;
  const auto adapter = rig.sender(peer_with(35));
  ASSERT_NE(adapter, nullptr);
  for (int i = 0; i < 3; ++i) {
    const int before = rig.h.fake().count(kShowFrame);
    ASSERT_GE(timed_submit(*adapter), 0.0);
    ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShowFrame) > before; })) << i;
  }
  const std::string err = capture.finish();
  EXPECT_FALSE(adapter->frame_fence_active());
  EXPECT_EQ(FakeGl::get().binds(), 1) << "the bind is tried once";
  EXPECT_EQ(FakeGl::get().inserts(), 0) << "no fence: the sender does not use the context";
  EXPECT_EQ(FakeGl::get().waits(), 0);
  const std::string g5 =
    " peer 23232323 the frame worker could not bind its GL context (err=5); this sender waits for "
    "the GPU on the GUI thread\n";
  const std::size_t at = err.find(g5);
  ASSERT_NE(at, std::string::npos) << err;
  EXPECT_EQ(err.find(g5, at + 1), std::string::npos) << "the line is written once";
  EXPECT_EQ(hook->quiet.load(), 3);
}

// A sender without a fence setup (the switch is `gui`) keeps the wait on the caller's thread.
TEST(RenderFence, WithTheSwitchOnGuiTheSenderUsesNoFenceAndNoContext)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  QarCApiSenderFactory::Config config = fence_config(hook);
  config.frame_fence_on_worker = false;
  Rig rig(config);
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer_with(36));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  EXPECT_FALSE(adapter->frame_fence_active());
  EXPECT_EQ(FakeGl::get().creates(), 0) << "no context is made";
  EXPECT_EQ(FakeGl::get().inserts(), 0);
  EXPECT_EQ(FakeGl::get().binds(), 0);
}

// The first frame after a stall of two seconds is read back once. With a fence the worker does it,
// on its own context; the render thread leaves the read-back alone.
TEST(RenderFence, TheReadBackAfterAStallRunsOnTheWorkerWhenTheSenderHasAFence)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  hook->readback_delay_ms.store(300);
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  FakeGl::get().set_auto_signal(true);
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer_with(37));
  ASSERT_NE(adapter, nullptr);
  adapter->set_prime_enabled(false);  // the next frame is reserved by the next tick, after the gap
  ASSERT_TRUE(show_one(rig, *adapter));
  EXPECT_EQ(hook->readbacks.load(), 0) << "an ordinary frame is not read back";

  std::this_thread::sleep_for(2100ms);
  StderrCapture capture;
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(wait_until([&] { return hook->readbacks.load() == 1; }));
  std::this_thread::sleep_for(100ms);  // the worker writes its lines after the read-back returns
  const std::string err = capture.finish();
  EXPECT_NE(hook->readback_thread_id(), std::this_thread::get_id());
  const auto waiters = FakeGl::get().wait_threads();
  ASSERT_EQ(waiters.size(), 1u);
  EXPECT_EQ(hook->readback_thread_id(), *waiters.begin()) << "the worker, which bound the context";
  EXPECT_TRUE(contains(err, "first 3D frame after a 2.")) << err;
  EXPECT_TRUE(contains(err, "mean colour (10.0, 20.0, 30.0)")) << err;
  // The read-back took 300 ms on the worker: the warning names the worker, not the GUI thread.
  EXPECT_TRUE(contains(err, " WARNING readback-probe took ")) << err;
  EXPECT_TRUE(contains(err, " ms on the frame worker (limit 250 ms)\n")) << err;
  EXPECT_FALSE(contains(err, "readback-probe took 3") && contains(err, " ms on the GUI thread"))
    << "the slow step is not reported as one of the GUI thread";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(hook->readbacks.load(), 1) << "once";
}

// A fence wait that fails (GL_WAIT_FAILED, for example a fence of another share group) is said
// once; the frame is shown, and the sender finishes its next frames on the render thread.
TEST(RenderFence, AFenceWaitThatFailsIsSaidOnceAndTheSenderFallsBackToTheGuiThread)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  FakeGl::get().fail_fence(1);
  frames_are_ready(rig);
  StderrCapture capture;
  const auto adapter = rig.sender(peer_with(39));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(wait_until([&] { return !adapter->frame_fence_active(); }));
  const int inserts = FakeGl::get().inserts();
  EXPECT_EQ(inserts, 1) << "the first frame had a fence";
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(show_one(rig, *adapter)) << i;
  }
  const std::string err = capture.finish();
  EXPECT_EQ(FakeGl::get().inserts(), inserts) << "the later frames use no fence";
  EXPECT_EQ(rig.h.fake().count(kShowFrame), 4);
  EXPECT_EQ(FakeGl::get().removes(), 1) << "the failed fence is deleted";
  const std::string line =
    " peer 27272727 the frame worker could not wait for its fence; this sender waits for the GPU "
    "on the GUI thread\n";
  const std::size_t at = err.find(line);
  ASSERT_NE(at, std::string::npos) << err;
  EXPECT_EQ(err.find(line, at + 1), std::string::npos) << "the line is written once";
}

// While the GPU progress gate is closed the render thread makes no GL context and deletes none:
// the contexts of released streams (of this factory and of an earlier one) and the creation of a
// sender wait for the next tick that finds the gate open.
TEST(RenderFence, NoContextIsDeletedOrMadeWhileTheGateIsClosed)
{
  g_deleted = 0;
  g_deleted_contexts.clear();
  QarCApiSenderFactoryWorkerAccess::set_deleter(&count_deleted_context);
  const auto hook = std::make_shared<CountingCopyHook>();
  FakeGlScope scope;

  // A factory that ended earlier leaves a context behind whose stream is gone.
  std::shared_ptr<std::atomic<bool>> earlier;
  {
    Rig gone(worker_config());
    ASSERT_TRUE(gone.ok);
    earlier = QarCApiSenderFactoryWorkerAccess::add_context(
      *gone.factory, reinterpret_cast<void *>(0x4101));
  }
  earlier->store(true);

  QarCApiSenderFactory::Config config = fence_config(hook);
  config.gpu_gate = true;
  config.gpu_gate_timings.min_age = 40ms;
  config.gpu_gate_timings.slice = 10ms;
  {
    Rig rig(config);
    ASSERT_TRUE(rig.ok);
    const auto flag = QarCApiSenderFactoryWorkerAccess::add_context(
      *rig.factory, reinterpret_cast<void *>(0x4102));
    flag->store(true);
    const std::shared_ptr<qar_qar_adapter::GpuProgressWatch> watch = rig.factory->gpu_watch();
    ASSERT_TRUE(watch->start(reinterpret_cast<void *>(0x1), reinterpret_cast<void *>(0x2)));
    ASSERT_TRUE(wait_until([&] { return watch->active(); }));
    StderrCapture capture;
    watch->note_frame_issued(qar_qar_adapter::GpuProgressWatch::Kind::kBatch);  // never signalled
    ASSERT_TRUE(wait_until([&] { return watch->any_gate_closed(); }));

    QarCApiSenderFactoryWorkerAccess::capture(*rig.factory);
    EXPECT_EQ(g_deleted.load(), 0) << "no context is deleted while the gate is closed";
    frames_are_ready(rig);
    const int creates = FakeGl::get().creates();
    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(rig.factory->create_sender(peer_with(61)), nullptr);
    }
    EXPECT_EQ(FakeGl::get().creates(), creates) << "no context is made while the gate is closed";
    const std::string err = capture.finish();
    EXPECT_EQ(
      err.find("create_sender: the GPU progress gate is closed; the stream of this peer is "
        "created when it opens\n") != std::string::npos, true) << err;

    FakeGl::get().signal_all();
    ASSERT_TRUE(wait_until([&] { return !watch->any_gate_closed(); }));
    QarCApiSenderFactoryWorkerAccess::capture(*rig.factory);
    {
      std::lock_guard<std::mutex> lock(g_deleted_mutex);
      const auto gone_at = [](void * context) {
          return std::find(g_deleted_contexts.begin(), g_deleted_contexts.end(), context) !=
                 g_deleted_contexts.end();
        };
      EXPECT_TRUE(gone_at(reinterpret_cast<void *>(0x4101))) << "the earlier factory's context";
      EXPECT_TRUE(gone_at(reinterpret_cast<void *>(0x4102))) << "the context of this factory";
    }
    const auto adapter = rig.sender(peer_with(61));
    ASSERT_NE(adapter, nullptr) << "the creation goes on";
    EXPECT_EQ(FakeGl::get().creates(), creates + 1);
  }
  QarCApiSenderFactoryWorkerAccess::set_deleter(nullptr);
}

TEST(RenderFence, TheReadBackAfterAStallStaysOnTheCallerWithoutAFence)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  QarCApiSenderFactory::Config config = fence_config(hook);
  config.frame_fence_on_worker = false;
  Rig rig(config);
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  const auto adapter = rig.sender(peer_with(38));
  ASSERT_NE(adapter, nullptr);
  adapter->set_prime_enabled(false);
  ASSERT_TRUE(show_one(rig, *adapter));
  std::this_thread::sleep_for(2100ms);
  ASSERT_TRUE(show_one(rig, *adapter));
  ASSERT_TRUE(wait_until([&] { return hook->readbacks.load() == 1; }));
  EXPECT_EQ(hook->readback_thread_id(), std::this_thread::get_id());
}

// ---------------------------------------------------------------------------------------------
//  The copy hook: the names of the textures of the runtime are reused by GL
// ---------------------------------------------------------------------------------------------

TEST(RenderWorker, ANewSenderMakesTheCopyHookForgetTextureTargets)
{
  const auto hook = std::make_shared<CountingCopyHook>();
  Rig rig(fence_config(hook));
  FakeGlScope scope;
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  const int before = hook->forgets.load();
  auto adapter = rig.sender(peer_with(71));
  ASSERT_NE(adapter, nullptr);
  EXPECT_GE(hook->forgets.load(), before + 1) << "a sender that starts";

  const int made = hook->forgets.load();
  adapter.reset();
  EXPECT_TRUE(wait_until([&] { return hook->forgets.load() > made; }))
    << "a sender that is released";
}

#ifdef _WIN32

namespace
{

// A GL that keeps the kind of every texture name and raises GL_INVALID_OPERATION for an attachment
// or a binding of the wrong kind, as the real one does. Its entry points are free functions, so
// there is one instance.
class FakeBlitGl
{
public:
  static constexpr unsigned int kPlain = 0x0DE1;
  static constexpr unsigned int kArray = 0x8C1A;
  static constexpr unsigned int kInvalidOperation = 0x0502;

  static FakeBlitGl & get()
  {
    static FakeBlitGl instance;
    return instance;
  }

  void reset()
  {
    kinds_.clear();
    attached_.clear();
    errors_.clear();
    read_ = draw_ = 0u;
    next_fbo_ = 100u;
    blits_ = 0;
    attach_errors_ = 0;
    blit_log_.clear();
  }

  /// The textures of the runtime are arrays; ours are plain.
  void define(unsigned int id, unsigned int kind) { kinds_[id] = kind; }
  int blits() const { return blits_; }
  int attach_errors() const { return attach_errors_; }
  /// The (read, draw) attachment of every blit that went through, in order.
  const std::vector<std::pair<unsigned int, unsigned int>> & blit_log() const { return blit_log_; }

  qar_qar_adapter::GlBlitApi api()
  {
    qar_qar_adapter::GlBlitApi a;
    a.gen_framebuffers = &FakeBlitGl::gen_framebuffers;
    a.bind_framebuffer = &FakeBlitGl::bind_framebuffer;
    a.framebuffer_texture_2d = &FakeBlitGl::framebuffer_texture_2d;
    a.framebuffer_texture_layer = &FakeBlitGl::framebuffer_texture_layer;
    a.blit_framebuffer = &FakeBlitGl::blit_framebuffer;
    a.check_framebuffer_status = &FakeBlitGl::check_framebuffer_status;
    a.get_error = &FakeBlitGl::get_error;
    a.get_integerv = &FakeBlitGl::get_integerv;
    a.bind_texture = &FakeBlitGl::bind_texture;
    return a;
  }

private:
  static void __stdcall gen_framebuffers(int n, unsigned int * out)
  {
    FakeBlitGl & g = get();
    for (int i = 0; i < n; ++i) { out[i] = g.next_fbo_++; }
  }

  static void __stdcall bind_framebuffer(unsigned int target, unsigned int id)
  {
    FakeBlitGl & g = get();
    (target == 0x8CA8 ? g.read_ : g.draw_) = id;
  }

  unsigned int & fbo_of(unsigned int target) { return attached_[target == 0x8CA8 ? read_ : draw_]; }

  void raise_attach_error()
  {
    errors_.push_back(kInvalidOperation);
    ++attach_errors_;
  }

  static void __stdcall framebuffer_texture_2d(
    unsigned int fb_target, unsigned int, unsigned int texture_target, unsigned int texture, int)
  {
    FakeBlitGl & g = get();
    if (texture == 0u) {
      g.fbo_of(fb_target) = 0u;
    } else if (g.kinds_[texture] != texture_target) {
      g.raise_attach_error();
    } else {
      g.fbo_of(fb_target) = texture;
    }
  }

  static void __stdcall framebuffer_texture_layer(
    unsigned int fb_target, unsigned int, unsigned int texture, int, int)
  {
    FakeBlitGl & g = get();
    if (texture == 0u) {
      g.fbo_of(fb_target) = 0u;
    } else if (g.kinds_[texture] != kArray) {
      g.raise_attach_error();
    } else {
      g.fbo_of(fb_target) = texture;
    }
  }

  static unsigned int __stdcall check_framebuffer_status(unsigned int fb_target)
  {
    return get().fbo_of(fb_target) != 0u ? 0x8CD5u : 0x8CD7u;
  }

  static void __stdcall blit_framebuffer(
    int, int, int, int, int, int, int, int, unsigned int, unsigned int)
  {
    FakeBlitGl & g = get();
    if (g.fbo_of(0x8CA8) == 0u || g.fbo_of(0x8CA9) == 0u) {
      g.errors_.push_back(kInvalidOperation);
    } else {
      ++g.blits_;
      g.blit_log_.emplace_back(g.fbo_of(0x8CA8), g.fbo_of(0x8CA9));
    }
  }

  static unsigned int __stdcall get_error()
  {
    FakeBlitGl & g = get();
    if (g.errors_.empty()) { return 0u; }
    const unsigned int first = g.errors_.front();
    g.errors_.erase(g.errors_.begin());
    return first;
  }

  static void __stdcall get_integerv(unsigned int name, int * out)
  {
    FakeBlitGl & g = get();
    *out = static_cast<int>(name == 0x8CAA ? g.read_ : g.draw_);
  }

  static void __stdcall bind_texture(unsigned int target, unsigned int id)
  {
    FakeBlitGl & g = get();
    if (id != 0u && g.kinds_[id] != target) { g.errors_.push_back(kInvalidOperation); }
  }

  std::map<unsigned int, unsigned int> kinds_;
  std::map<unsigned int, unsigned int> attached_;
  std::vector<unsigned int> errors_;
  unsigned int read_ = 0u;
  unsigned int draw_ = 0u;
  unsigned int next_fbo_ = 100u;
  int blits_ = 0;
  int attach_errors_ = 0;
  std::vector<std::pair<unsigned int, unsigned int>> blit_log_;
};

// Our frame: the colour of the left eye is the plain texture @p id, 1280x720.
qar_ogre_streaming::QarOpenGlStereoFrame blit_source(unsigned int id)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  frame.left_color.texture_id = id;
  frame.left_color.size.width = 1280;
  frame.left_color.size.height = 720;
  frame.left_color.size.array_layers = 1;
  return frame;
}

// The frame of the runtime: one texture, 1920x1080.
QarVideoFrameOpenGl blit_target(unsigned int id)
{
  QarVideoFrameOpenGl frame{};
  frame.textures_count = 1;
  frame.textures[0].texture = id;
  frame.textures[0].size.width = 1920;
  frame.textures[0].size.height = 1080;
  frame.textures[0].size.array_layers = 1;
  return frame;
}

// Our frame with two views: the colour of the left eye is @p left, of the right eye @p right.
qar_ogre_streaming::QarOpenGlStereoFrame blit_source_pair(unsigned int left, unsigned int right)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame = blit_source(left);
  frame.right_color.texture_id = right;
  frame.right_color.size.width = 1280;
  frame.right_color.size.height = 720;
  frame.right_color.size.array_layers = 1;
  return frame;
}

// The frame of the runtime with two textures, 1920x1080 each.
QarVideoFrameOpenGl blit_target_pair(unsigned int first, unsigned int second)
{
  QarVideoFrameOpenGl frame = blit_target(first);
  frame.textures_count = 2;
  frame.textures[1].texture = second;
  frame.textures[1].size.width = 1920;
  frame.textures[1].size.height = 1080;
  frame.textures[1].size.array_layers = 1;
  return frame;
}

}  // namespace

// The stream of a sender ends, GL gives the name of one of its textures to a texture of the
// runtime that is of another kind, and the new sender copies into it.
TEST(RenderCopyHook, ATextureNameReusedWithAnotherTargetIsCopied)
{
  FakeBlitGl & gl = FakeBlitGl::get();
  gl.reset();
  const auto hook = qar_qar_adapter::make_gl_copy_image_hook_with_api(gl.api());
  ASSERT_NE(hook, nullptr);

  // The first sender: our eye texture 8 is plain, the texture 4 of the runtime is an array.
  gl.define(8, FakeBlitGl::kPlain);
  gl.define(4, FakeBlitGl::kArray);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(8), blit_target(4)));
  EXPECT_EQ(gl.blits(), 1);

  // It ends. The name 8 now belongs to a texture of the runtime that is an array; ours is 12.
  gl.define(8, FakeBlitGl::kArray);
  gl.define(12, FakeBlitGl::kPlain);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(12), blit_target(8)))
    << "the kind remembered for the name 8 is wrong";
  EXPECT_EQ(gl.blits(), 2);

  // The kind that completed the framebuffer is remembered: the next copy attaches at once.
  const int errors = gl.attach_errors();
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(12), blit_target(8)));
  EXPECT_EQ(gl.blits(), 3);
  EXPECT_EQ(gl.attach_errors(), errors) << "no failed attach after the kind was corrected";
}

TEST(RenderCopyHook, AnArrayNameReusedAsAPlainTextureIsCopied)
{
  FakeBlitGl & gl = FakeBlitGl::get();
  gl.reset();
  const auto hook = qar_qar_adapter::make_gl_copy_image_hook_with_api(gl.api());
  ASSERT_NE(hook, nullptr);
  gl.define(8, FakeBlitGl::kPlain);
  gl.define(4, FakeBlitGl::kArray);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(8), blit_target(4)));

  // The name 4 comes back as a plain texture.
  gl.define(4, FakeBlitGl::kPlain);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(8), blit_target(4)));
  EXPECT_EQ(gl.blits(), 2);
}

// A form of attachment that GL rejects leaves the attachment of the view before in place. The copy
// must not take that for an attachment of its own texture: every view goes into its own texture.
TEST(RenderCopyHook, EveryViewGoesIntoItsOwnTextureWhenTheKindsOfSeveralNamesAreWrong)
{
  FakeBlitGl & gl = FakeBlitGl::get();
  gl.reset();
  const auto hook = qar_qar_adapter::make_gl_copy_image_hook_with_api(gl.api());
  ASSERT_NE(hook, nullptr);
  gl.define(12, FakeBlitGl::kPlain);
  gl.define(13, FakeBlitGl::kPlain);
  gl.define(8, FakeBlitGl::kPlain);
  gl.define(9, FakeBlitGl::kPlain);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source_pair(12, 13), blit_target_pair(8, 9)));
  using Blits = std::vector<std::pair<unsigned int, unsigned int>>;
  EXPECT_EQ(gl.blit_log(), (Blits{{12u, 8u}, {13u, 9u}}));

  // The names come back as arrays, on both sides, and nothing made the hook forget what it knew.
  gl.define(8, FakeBlitGl::kArray);
  gl.define(9, FakeBlitGl::kArray);
  gl.define(12, FakeBlitGl::kArray);
  gl.define(13, FakeBlitGl::kArray);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source_pair(12, 13), blit_target_pair(8, 9)));
  EXPECT_EQ(gl.blit_log(), (Blits{{12u, 8u}, {13u, 9u}, {12u, 8u}, {13u, 9u}}))
    << "the second view of the copy went into the texture of the first";

  // The kinds that completed the framebuffers are remembered for both views.
  const int errors = gl.attach_errors();
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source_pair(12, 13), blit_target_pair(8, 9)));
  EXPECT_EQ(gl.attach_errors(), errors);
  EXPECT_EQ(gl.blit_log().size(), 6u);
  EXPECT_EQ(gl.blit_log().back(), (std::pair<unsigned int, unsigned int>{13u, 9u}));
}

TEST(RenderCopyHook, AForgottenKindIsLookedUpAgainWithoutAFailedAttach)
{
  FakeBlitGl & gl = FakeBlitGl::get();
  gl.reset();
  const auto hook = qar_qar_adapter::make_gl_copy_image_hook_with_api(gl.api());
  ASSERT_NE(hook, nullptr);
  gl.define(8, FakeBlitGl::kPlain);
  gl.define(4, FakeBlitGl::kArray);
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(8), blit_target(4)));
  EXPECT_EQ(gl.attach_errors(), 0);

  gl.define(8, FakeBlitGl::kArray);
  gl.define(12, FakeBlitGl::kPlain);
  hook->forget_texture_targets();
  EXPECT_TRUE(hook->copy_frame_into_quiet(blit_source(12), blit_target(8)));
  EXPECT_EQ(gl.attach_errors(), 0) << "the hook asked GL what the name 8 is";
  EXPECT_EQ(gl.blits(), 2);
}

#endif  // _WIN32

// ---------------------------------------------------------------------------------------------
//  A worker sender that the runtime asks for no frames is made again
// ---------------------------------------------------------------------------------------------

QarCApiSenderFactory::Config recycle_config(std::chrono::milliseconds reconnecting_limit = 1500ms)
{
  QarCApiSenderFactory::Config config = worker_config();
  config.render_recycle.after = 200ms;
  config.render_recycle.cap = 1600ms;
  config.render_recycle.reconnecting_limit = reconnecting_limit;
  config.render_recycle.error_window = 500ms;
  config.render_recycle.error_persist = 100ms;
  return config;
}

// Ticks every 2 ms for @p duration, submitting every frame that comes; returns at once when the
// sender closed. Tells whether it did.
bool worker_closes_within(QarCApiRenderSenderAdapter & adapter, std::chrono::milliseconds duration)
{
  qar_ogre_streaming::QarOpenGlStereoFrame frame;
  const auto end = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < end) {
    qar_ogre_streaming::QarFrameInfo info;
    adapter.begin_frame(info);
    if (info.should_render) {
      adapter.submit_opengl_stereo_frame(frame);
    }
    if (adapter.is_closed()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return adapter.is_closed();
}

TEST(RenderWorker, AWorkerSenderWithoutRenderRequestsAfterAHubRestartIsMadeAgain)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  const QarPeerId peer = peer_with(72);
  frames_are_ready(rig);
  auto adapter = rig.sender(peer);
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(adapter->frame_calls_on_worker());
  ASSERT_TRUE(show_one(rig, *adapter)) << "frames before the Hub restarts";

  rig.h.fake().set_render_frame(RenderFrameScript{false, 803});
  StderrCapture capture;
  const bool closed = worker_closes_within(*adapter, 5s);
  const std::string err = capture.finish();
  ASSERT_TRUE(closed) << err;
  EXPECT_TRUE(adapter->recycled());
  EXPECT_TRUE(contains(
      err, " s after the stream reconnected (the runtime answers 803); the 3D sender is made "
      "again\n")) << err;

  std::vector<QarPeerId> closed_peers;
  rig.factory->poll_closed_peers(closed_peers);
  ASSERT_EQ(closed_peers.size(), 1u);
  adapter.reset();
  rig.factory->forget(peer);
  frames_are_ready(rig);
  auto again = rig.sender(peer);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(rig.h.fake().render_creates(), 2);
  EXPECT_TRUE(show_one(rig, *again)) << "the 3D stream shows frames again";
  EXPECT_FALSE(again->is_closed());
}

TEST(RenderWorker, AWorkerSenderIsNotMadeAgainForReconnectingOrForNoAnswerAtAll)
{
  Rig rig(recycle_config());
  ASSERT_TRUE(rig.ok);
  frames_are_ready(rig);
  auto adapter = rig.sender(peer_with(73));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(show_one(rig, *adapter));
  rig.h.fake().set_render_frame(RenderFrameScript{false, 603});
  EXPECT_FALSE(worker_closes_within(*adapter, 500ms)) << "reconnecting for 500 ms";
  rig.h.fake().set_render_frame(RenderFrameScript{false, 0});
  EXPECT_FALSE(worker_closes_within(*adapter, 1s)) << "not ready, no error";
  EXPECT_FALSE(adapter->recycled());
}

// ---------------------------------------------------------------------------------------------
//  The exit: the stream of a sender is destroyed before the session, and nothing is called after
//  the runtime was destroyed
// ---------------------------------------------------------------------------------------------

// The worker of the sender is inside a slow try-begin when the sender, the factory and the owner
// are released one after the other, as RViz does at its exit. The release of the stream that the
// worker posts when its call returns must run before the session is destroyed.
TEST(RenderWorker, TheExitDestroysTheStreamBeforeTheSessionWhileTheWorkerIsInASlowCall)
{
  Rig rig;
  ASSERT_TRUE(rig.ok);
  auto adapter = rig.sender(peer_with(75));
  ASSERT_NE(adapter, nullptr);
  ASSERT_TRUE(adapter->frame_calls_on_worker());

  // Ticks until the worker has entered a try-begin that takes 100 ms (the settle time of the owner
  // is 200 ms).
  rig.h.fake().set_frame_delay_ms(100, kTryBegin);
  const int begun = rig.h.fake().count(kTryBegin);
  ASSERT_TRUE(wait_until([&] {
        qar_ogre_streaming::QarFrameInfo info;
        adapter->begin_frame(info);
        return rig.h.fake().count(kTryBegin) > begun;
      })) << "the worker is inside a slow try-begin";

  EXPECT_TRUE(returns_within_or_unblock([&] {
        adapter.reset();
        rig.factory->detach();
        rig.factory.reset();
      }, 500ms, [&] { rig.h.fake().set_frame_delay_ms(0); }));
  ASSERT_TRUE(rig.h.destroy_owner_within(10s));

  const std::vector<qar_qar_adapter::test::Call> calls = rig.h.fake().calls();
  const int stream = rig.h.fake().first_index("render_stream_handle_destroy");
  const int session = rig.h.fake().first_index("session_handle_destroy");
  const int runtime = rig.h.fake().first_index("runtime_handle_destroy");
  ASSERT_GE(stream, 0) << "the stream is destroyed";
  ASSERT_GE(session, 0);
  ASSERT_GE(runtime, 0);
  EXPECT_LT(stream, session) << "the stream goes before the session";
  EXPECT_LT(session, runtime);
  EXPECT_EQ(rig.h.fake().count("render_stream_handle_destroy"), 1);
  EXPECT_EQ(rig.h.fake().live_render_senders(), 0);
  for (std::size_t i = static_cast<std::size_t>(runtime) + 1; i < calls.size(); ++i) {
    EXPECT_TRUE(calls[i].name == "library_destroy" || calls[i].name == "library_unload")
      << "no call on a handle after the runtime was destroyed: " << calls[i].name;
  }
}

}  // namespace
