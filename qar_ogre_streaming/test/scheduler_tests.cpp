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
#include <string>
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

// Declines a frame like the spy above, but takes `seconds` of the manual clock to do so: a phase
// of the batch that is slow.
class SlowBeginSender : public DecliningSpySender
{
public:
  SlowBeginSender(std::shared_ptr<qts::ManualClock> clock, double seconds)
  : clock_(std::move(clock)), seconds_(seconds) {}

  bool begin_frame(qos::QarFrameInfo & out) override
  {
    clock_->advance(seconds_);
    return DecliningSpySender::begin_frame(out);
  }

private:
  std::shared_ptr<qts::ManualClock> clock_;
  double seconds_;
};

struct RecordingSink : qos::OgreQarDiagnosticsSink
{
  struct Line
  {
    qos::DiagnosticSeverity severity;
    std::string category;
    std::string message;
  };
  void on_message(
    qos::DiagnosticSeverity severity, const std::string & category, const std::string & message,
    const std::string &) override
  {
    lines.push_back(Line{severity, category, message});
  }
  std::vector<Line> lines;
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

// A batch of 250 ms or more is reported once, with the time of each phase (test 11 of the frame
// design). The begin phase is the slow one here: the sender takes 400 ms to answer.
TEST(Scheduler, ASlowBatchIsReportedWithItsPhaseSplit)
{
  SchedulerFixture f;
  RecordingSink sink;
  auto sender = std::make_shared<SlowBeginSender>(f.clock, 0.4);
  auto stream = std::make_unique<qos::OgreQarStereoStream>(
    qos::test::fake_scene_manager(), stream_cfg(1), sender);
  stream->set_active(true);
  std::vector<qos::OgreQarStereoStream *> streams{stream.get()};
  qos::OgreQarRenderScheduler sched(
    f.executor, qos::RenderBudgetConfig{}, &sink, f.lock, f.clock, nullptr);
  f.executor->set_is_render_thread(true);

  EXPECT_DOUBLE_EQ(sched.slow_batch_ms(), 250.0);
  EXPECT_TRUE(sched.render_tick(streams).ok());
  ASSERT_EQ(sink.lines.size(), 1u);
  EXPECT_EQ(sink.lines[0].severity, qos::DiagnosticSeverity::Warning);
  EXPECT_EQ(sink.lines[0].category, "scheduler");
  EXPECT_EQ(
    sink.lines[0].message,
    "render batch took 400 ms: begin 400 ms, cameras 0 ms, render 0 ms, submit 0 ms (1 streams)");
}

TEST(Scheduler, AFastBatchAndAHigherLimitReportNothing)
{
  SchedulerFixture f;
  RecordingSink sink;
  auto sender = std::make_shared<SlowBeginSender>(f.clock, 0.2);
  auto stream = std::make_unique<qos::OgreQarStereoStream>(
    qos::test::fake_scene_manager(), stream_cfg(1), sender);
  stream->set_active(true);
  std::vector<qos::OgreQarStereoStream *> streams{stream.get()};
  qos::OgreQarRenderScheduler sched(
    f.executor, qos::RenderBudgetConfig{}, &sink, f.lock, f.clock, nullptr);
  f.executor->set_is_render_thread(true);

  // 200 ms is under the default limit of 250 ms.
  EXPECT_TRUE(sched.render_tick(streams).ok());
  EXPECT_TRUE(sink.lines.empty());
  // A lower limit reports the same batch; a limit above it does not.
  sched.set_slow_batch_ms(150.0);
  EXPECT_TRUE(sched.render_tick(streams).ok());
  EXPECT_EQ(sink.lines.size(), 1u);
  sched.set_slow_batch_ms(1000.0);
  EXPECT_TRUE(sched.render_tick(streams).ok());
  EXPECT_EQ(sink.lines.size(), 1u);
}

TEST(Scheduler, OnlyTheFirstSlowBatchesAreReported)
{
  SchedulerFixture f;
  RecordingSink sink;
  auto sender = std::make_shared<SlowBeginSender>(f.clock, 0.4);
  auto stream = std::make_unique<qos::OgreQarStereoStream>(
    qos::test::fake_scene_manager(), stream_cfg(1), sender);
  stream->set_active(true);
  std::vector<qos::OgreQarStereoStream *> streams{stream.get()};
  qos::OgreQarRenderScheduler sched(
    f.executor, qos::RenderBudgetConfig{}, &sink, f.lock, f.clock, nullptr);
  f.executor->set_is_render_thread(true);

  for (int i = 0; i < 12; ++i) {
    EXPECT_TRUE(sched.render_tick(streams).ok());
  }
  EXPECT_EQ(sink.lines.size(), 5u);
}

namespace
{

// A gate the test closes and opens, counting what the scheduler tells it.
class FakeGate : public qos::IGpuProgressGate
{
public:
  bool gate_closed() override
  {
    ++closed_asked;
    return closed;
  }
  void note_batch_issued() override { ++issued; }
  void note_batch_skipped() override { ++skipped; }

  bool closed = false;
  int closed_asked = 0;
  int issued = 0;
  int skipped = 0;
};

}  // namespace

TEST(Scheduler, AClosedGateSkipsTheBatchBeforeAnyLockOrBegin)
{
  SchedulerFixture f;
  f.add_stream(1);
  f.add_stream(2);
  auto sched = f.make_scheduler();
  auto gate = std::make_shared<FakeGate>();
  gate->closed = true;
  sched->set_gpu_gate(gate);
  f.executor->set_is_render_thread(true);

  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(sched->render_tick(f.streams).ok());
  }
  EXPECT_EQ(f.lock->lock_calls(), 0) << "no render lock is taken while the gate is closed";
  EXPECT_EQ(f.senders[0]->begin_calls, 0) << "no frame is begun";
  EXPECT_EQ(f.senders[1]->begin_calls, 0);
  EXPECT_EQ(gate->skipped, 3);
  EXPECT_EQ(gate->issued, 0) << "a skipped batch issues nothing and is not followed by a fence";
  EXPECT_STREQ(sched->last_skip_reason(), "GPU not progressing");
  EXPECT_EQ(sched->gate_skipped_batches(), 3u);
  EXPECT_EQ(sched->batches_rendered(), 0u);
  EXPECT_EQ(sched->last_batch_streams_served(), 0u);
}

TEST(Scheduler, TheBatchRunsAgainWhenTheGateOpens)
{
  SchedulerFixture f;
  f.add_stream(1);
  auto sched = f.make_scheduler();
  auto gate = std::make_shared<FakeGate>();
  gate->closed = true;
  sched->set_gpu_gate(gate);
  f.executor->set_is_render_thread(true);

  EXPECT_TRUE(sched->render_tick(f.streams).ok());
  EXPECT_EQ(f.lock->lock_calls(), 0);
  EXPECT_STREQ(sched->last_skip_reason(), "GPU not progressing");

  gate->closed = false;
  EXPECT_TRUE(sched->render_tick(f.streams).ok());
  EXPECT_EQ(f.lock->lock_calls(), 1);
  EXPECT_EQ(f.senders[0]->begin_calls, 1);
  EXPECT_STREQ(sched->last_skip_reason(), "") << "the reason is cleared by a tick that rendered";
  EXPECT_EQ(gate->skipped, 1);
  EXPECT_EQ(gate->issued, 0) << "no frame was rendered (the sender declined), so none was issued";
}

TEST(Scheduler, AnIdleTickDoesNotAskTheGateOrCountASkip)
{
  SchedulerFixture f;
  f.add_stream(1, /*active=*/false);
  auto sched = f.make_scheduler();
  auto gate = std::make_shared<FakeGate>();
  gate->closed = true;
  sched->set_gpu_gate(gate);
  f.executor->set_is_render_thread(true);

  EXPECT_TRUE(sched->render_tick(f.streams).ok());
  EXPECT_EQ(gate->skipped, 0) << "nothing was due, so no batch was skipped";
  EXPECT_STREQ(sched->last_skip_reason(), "");
}

TEST(Scheduler, WithoutAGateNothingChanges)
{
  SchedulerFixture f;
  f.add_stream(1);
  auto sched = f.make_scheduler();
  f.executor->set_is_render_thread(true);
  EXPECT_TRUE(sched->render_tick(f.streams).ok());
  EXPECT_EQ(f.lock->lock_calls(), 1);
  EXPECT_STREQ(sched->last_skip_reason(), "");
  EXPECT_EQ(sched->gate_skipped_batches(), 0u);
}
