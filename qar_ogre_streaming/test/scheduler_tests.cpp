// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// OgreQarRenderScheduler tests. These exercise the batch structure WITHOUT a
// GL context: the streams' senders decline a frame (should_render=false), so the
// batch runs collect -> lock -> begin-all -> (no render/submit) -> unlock and
// never touches Ogre. The real render/submit path is exercised in RViz and in the
// standalone sample. What is asserted here:
//   * render-thread affinity is enforced before any lock/Ogre work,
//   * the WHOLE batch is wrapped in exactly ONE lockRender/unlockRender,
//   * every due stream is collected and begun in one batch (begin-all),
//   * an empty tick takes no lock (early-out),
//   * the achieved-FPS rolling math.

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/ogre_qar_render_scheduler.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming_test_support/immediate_render_thread_executor.hpp"
#include "qar_ogre_streaming_test_support/manual_clock.hpp"
#include "qar_ogre_streaming_test_support/recording_render_lock.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;
namespace qts = qar_ogre_streaming::test_support;

namespace
{

// Records begin/submit calls; always DECLINES a frame so the batch stays headless.
class DecliningSpySender : public qos::IQarRenderSenderAdapter
{
public:
  bool begin_frame(qos::QarFrameInfo & out) override
  {
    ++begin_calls;
    out = qos::QarFrameInfo{};
    out.should_render = false;  // decline => no Ogre work this tick
    return true;
  }
  bool submit_opengl_stereo_frame(const qos::QarOpenGlStereoFrame &) override
  {
    ++submit_calls;
    return true;
  }
  int begin_calls = 0;
  int submit_calls = 0;
};

qos::StereoStreamConfig stream_cfg(uint8_t seed, uint32_t fps = 60)
{
  qos::StereoStreamConfig c;
  c.peer_id = qos::test::make_peer_id(seed);
  c.width = 320;
  c.height = 240;
  c.max_fps = fps;
  return c;
}

struct SchedulerFixture
{
  std::shared_ptr<qts::ImmediateRenderThreadExecutor> executor =
    std::make_shared<qts::ImmediateRenderThreadExecutor>();
  std::shared_ptr<qts::RecordingRenderLock> lock =
    std::make_shared<qts::RecordingRenderLock>();
  std::shared_ptr<qts::ManualClock> clock = std::make_shared<qts::ManualClock>();

  std::vector<std::shared_ptr<DecliningSpySender>> senders;
  std::vector<std::unique_ptr<qos::OgreQarStereoStream>> owned;
  std::vector<qos::OgreQarStereoStream *> streams;

  std::unique_ptr<qos::OgreQarRenderScheduler> make_scheduler()
  {
    return std::make_unique<qos::OgreQarRenderScheduler>(
      executor, qos::RenderBudgetConfig{}, nullptr, lock, clock, nullptr);
  }

  void add_stream(uint8_t seed, bool active = true)
  {
    auto sender = std::make_shared<DecliningSpySender>();
    auto s = std::make_unique<qos::OgreQarStereoStream>(
      qos::test::fake_scene_manager(), stream_cfg(seed), sender);
    s->set_active(active);
    senders.push_back(sender);
    streams.push_back(s.get());
    owned.push_back(std::move(s));
  }
};

}  // namespace

TEST(Scheduler, RefusesOffRenderThread)
{
  SchedulerFixture f;
  f.add_stream(1);
  auto sched = f.make_scheduler();

  f.executor->set_is_render_thread(false);
  const qos::Result r = sched->render_tick(f.streams);
  EXPECT_EQ(r.status, qos::Status::RenderThreadRequired);
  // Nothing was locked or begun off the render thread.
  EXPECT_EQ(f.lock->lock_calls(), 0);
  EXPECT_EQ(f.senders[0]->begin_calls, 0);
}

TEST(Scheduler, OneLockAroundWholeBatchAndCollectsAll)
{
  SchedulerFixture f;
  f.add_stream(1);
  f.add_stream(2);
  f.add_stream(3);
  auto sched = f.make_scheduler();

  f.executor->set_is_render_thread(true);
  const qos::Result r = sched->render_tick(f.streams);
  EXPECT_TRUE(r.ok());

  // Exactly ONE lock/unlock pair wrapped the whole batch (never nested).
  EXPECT_EQ(f.lock->lock_calls(), 1);
  EXPECT_EQ(f.lock->unlock_calls(), 1);
  EXPECT_EQ(f.lock->max_depth(), 1);
  EXPECT_TRUE(f.lock->balanced());

  // Every due stream was collected and begun in this single batch.
  EXPECT_EQ(f.senders[0]->begin_calls, 1);
  EXPECT_EQ(f.senders[1]->begin_calls, 1);
  EXPECT_EQ(f.senders[2]->begin_calls, 1);

  // All declined => none served/submitted this tick.
  EXPECT_EQ(sched->last_batch_streams_served(), 0u);
  EXPECT_EQ(f.senders[0]->submit_calls, 0);
  EXPECT_EQ(sched->batches_rendered(), 1u);
}

TEST(Scheduler, EmptyTickTakesNoLock)
{
  SchedulerFixture f;
  f.add_stream(1, /*active=*/false);  // inactive => not collected
  auto sched = f.make_scheduler();

  f.executor->set_is_render_thread(true);
  const qos::Result r = sched->render_tick(f.streams);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(f.lock->lock_calls(), 0);       // early-out before any lock
  EXPECT_EQ(f.senders[0]->begin_calls, 0);  // and before any begin
  EXPECT_EQ(sched->last_batch_streams_served(), 0u);
}

TEST(Scheduler, RollingFpsMath)
{
  EXPECT_DOUBLE_EQ(qos::OgreQarRenderScheduler::rolling_fps({}), 0.0);
  EXPECT_DOUBLE_EQ(qos::OgreQarRenderScheduler::rolling_fps({1.0}), 0.0);
  EXPECT_DOUBLE_EQ(qos::OgreQarRenderScheduler::rolling_fps({0.0, 0.0}), 0.0);  // zero span
  EXPECT_DOUBLE_EQ(qos::OgreQarRenderScheduler::rolling_fps({0.0, 1.0}), 1.0);
  // 5 samples spanning 1.0 s => 4 intervals => 4 FPS.
  EXPECT_DOUBLE_EQ(
    qos::OgreQarRenderScheduler::rolling_fps({0.0, 0.25, 0.5, 0.75, 1.0}), 4.0);
  // 61 samples spanning 1.0 s => 60 FPS.
  std::vector<double> minute;
  for (int i = 0; i <= 60; ++i) {
    minute.push_back(static_cast<double>(i) / 60.0);
  }
  EXPECT_NEAR(qos::OgreQarRenderScheduler::rolling_fps(minute), 60.0, 1e-9);
}
