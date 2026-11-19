// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The GPU progress watch on a fake of the GL fence calls: a series (RViz's frames, the 3D render
// batch) closes when its oldest unfinished frame is older than its age limit and opens when that
// frame is done, a healthy GPU never closes a series, a GPU that cannot keep up holds only the
// frames issued within one age limit, the render thread only inserts fences (it never waits for
// one), nothing is issued while a series is closed, a probe fence reopens a gate that a lost fence
// holds, and a watch that cannot bind its context or wait for a fence leaves the gate open. No test
// makes a GL context; every time bound has a lower and an upper side.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "fake_gl_fence.hpp"
#include "qar_qar_adapter/gpu_progress_watch.hpp"

namespace
{

using qar_qar_adapter::GpuGateTimings;
using qar_qar_adapter::GpuProgressWatch;
using qar_qar_adapter::test::FakeGl;
using namespace std::chrono_literals;
using Kind = GpuProgressWatch::Kind;

bool wait_until(const std::function<bool()> & predicate, std::chrono::milliseconds limit = 5s)
{
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

double ms_since(std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

bool contains(const std::string & text, const std::string & part)
{
  return text.find(part) != std::string::npos;
}

int count_of(const std::string & text, const std::string & part)
{
  int n = 0;
  for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
    ++n;
  }
  return n;
}

// Limits scaled down from the shipped ones (50 ms, 50 ms, 30 s, 1 s).
GpuGateTimings quick_timings()
{
  GpuGateTimings t;
  t.min_age = 40ms;
  t.slice = 10ms;
  t.test_start_after = 60ms;
  t.probe_interval = 100ms;
  return t;
}

class GpuWatchTest : public ::testing::Test
{
protected:
  void SetUp() override { FakeGl::get().reset(); }

  void TearDown() override
  {
    FakeGl::get().unblock();
    FakeGl::get().set_auto_signal(true);
    watch.reset();
    wait_until([] { return GpuProgressWatch::running_threads() == 0; });
    GpuProgressWatch::reap();
    FakeGl::get().reset();
  }

  // Starts a watch on the fake; the context handles are arbitrary non-null values.
  bool start(
    const GpuGateTimings & timings = quick_timings(), bool enabled = true, double test_s = 0.0)
  {
    watch = std::make_shared<GpuProgressWatch>(FakeGl::get().ops(), enabled, timings, test_s);
    return watch->start(reinterpret_cast<void *>(0x1), reinterpret_cast<void *>(0x2));
  }

  bool start_active(const GpuGateTimings & timings = quick_timings(), double test_s = 0.0)
  {
    if (!start(timings, true, test_s)) { return false; }
    return wait_until([&] { return watch->active(); });
  }

  // One frame of RViz as the frame listener does it: asks the gate, and either skips the frame or
  // lets it run and reports it. @return true when the frame ran.
  bool rviz_frame()
  {
    if (watch->rviz_gate_closed()) {
      watch->note_frame_skipped();
      return false;
    }
    watch->note_frame_issued(Kind::kRvizFrame);
    return true;
  }

  // Frames at a steady rate on a GPU whose fences take @p latency; @return the frames skipped.
  int run_steady(std::chrono::microseconds interval, std::chrono::milliseconds latency, int frames)
  {
    FakeGl::get().set_latency(latency);
    int skipped = 0;
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      if (!rviz_frame()) { ++skipped; }
      if (i % 2 == 0) {
        if (watch->gate_closed()) {
          watch->note_batch_skipped();
          ++skipped;
        } else {
          watch->note_batch_issued();
        }
      }
      next += interval;
      std::this_thread::sleep_until(next);
    }
    return skipped;
  }

  std::shared_ptr<GpuProgressWatch> watch;
};

TEST_F(GpuWatchTest, TheBannerNamesTheSwitchAndTheLimits)
{
  EXPECT_EQ(
    qar_qar_adapter::gpu_gate_banner(true),
    "[qar-gpu] GPU progress gate on (QAR_GPU_GATE=on): RViz frames and the 3D render batch pause "
    "while a frame stays unfinished for more than max(50 ms, 3 x the median frame time)");
  EXPECT_EQ(
    qar_qar_adapter::gpu_gate_banner(false),
    "[qar-gpu] GPU progress gate off (QAR_GPU_GATE=off): RViz frames and the 3D render batch are "
    "never paused");
  EXPECT_TRUE(qar_qar_adapter::default_gpu_gate_enabled()) << "the gate is on by default";
  EXPECT_EQ(qar_qar_adapter::default_gpu_gate_test_seconds(), 0.0) << "the test close is off";
}

TEST_F(GpuWatchTest, AWatchThatIsOffNeverStartsAndNeverCloses)
{
  EXPECT_FALSE(start(quick_timings(), /*enabled=*/false));
  EXPECT_FALSE(watch->active());
  EXPECT_EQ(FakeGl::get().creates(), 0) << "no context is made";
  for (int i = 0; i < 5; ++i) {
    watch->note_frame_issued(Kind::kBatch);
  }
  EXPECT_EQ(FakeGl::get().inserts(), 0);
  std::this_thread::sleep_for(80ms);
  EXPECT_FALSE(watch->gate_closed());
  EXPECT_FALSE(watch->rviz_gate_closed());
  EXPECT_FALSE(watch->any_gate_closed());
}

TEST_F(GpuWatchTest, TheContextIsMadeOnTheCallersThreadAndBoundOnceOnTheWatchThread)
{
  ASSERT_TRUE(start());
  ASSERT_TRUE(wait_until([&] { return watch->active(); }));
  EXPECT_EQ(FakeGl::get().creates(), 1);
  EXPECT_EQ(FakeGl::get().binds(), 1);
  const auto binders = FakeGl::get().bind_threads();
  ASSERT_EQ(binders.size(), 1u);
  EXPECT_EQ(binders.count(std::this_thread::get_id()), 0u) << "the caller binds nothing";
  EXPECT_FALSE(watch->start(reinterpret_cast<void *>(0x1), reinterpret_cast<void *>(0x2)))
    << "a started watch is not started twice";
  EXPECT_EQ(FakeGl::get().creates(), 1);
}

// The render thread inserts fences and reads atomics: every wait is made by the watch thread.
TEST_F(GpuWatchTest, TheCallerOnlyInsertsFencesAndNeverWaitsForOne)
{
  ASSERT_TRUE(start());
  ASSERT_TRUE(wait_until([&] { return watch->active(); }));
  FakeGl::get().set_auto_signal(true);
  std::vector<double> took_ms;
  for (int i = 0; i < 100; ++i) {
    const auto start_time = std::chrono::steady_clock::now();
    if (i % 2 == 0) {
      EXPECT_TRUE(rviz_frame());
    } else {
      watch->note_batch_issued();
    }
    EXPECT_FALSE(watch->gate_closed());
    took_ms.push_back(ms_since(start_time));
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(wait_until([&] { return watch->frames_completed() == 100u; }));
  EXPECT_EQ(FakeGl::get().inserts(), 100);
  EXPECT_EQ(FakeGl::get().removes(), 100) << "every fence is deleted once";
  const auto inserters = FakeGl::get().insert_threads();
  ASSERT_EQ(inserters.size(), 1u);
  EXPECT_EQ(*inserters.begin(), std::this_thread::get_id());
  const auto waiters = FakeGl::get().wait_threads();
  ASSERT_EQ(waiters.size(), 1u);
  EXPECT_NE(*waiters.begin(), std::this_thread::get_id()) << "the caller never waits";
  std::sort(took_ms.begin(), took_ms.end());
  EXPECT_LT(took_ms[94], 5.0) << "the 95th percentile of the 100 calls";
  EXPECT_EQ(watch->closes(), 0u) << "a GPU that keeps up never closes the gate";
}

TEST_F(GpuWatchTest, AFrameUnfinishedLongerThanTheLimitClosesItsSeriesAndItsDoneOpensIt)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  EXPECT_TRUE(rviz_frame());
  EXPECT_EQ(watch->frames_pending(), 1u);
  EXPECT_FALSE(watch->rviz_gate_closed()) << "one unfinished frame, but not for the limit yet";

  // Lower side: not closed before 40 ms; upper side: closed soon after.
  std::this_thread::sleep_for(15ms);
  EXPECT_FALSE(watch->rviz_gate_closed());
  ASSERT_TRUE(wait_until([&] { return watch->rviz_gate_closed(); }, 1s));
  ASSERT_TRUE(wait_until([&] { return watch->closes() == 1u; })) << "the watch thread noted it";
  EXPECT_TRUE(watch->any_gate_closed());
  EXPECT_FALSE(watch->gate_closed()) << "the 3D batch is a series of its own, and it is open";

  // A frame of the closed series gets no fence; a batch frame does.
  const int inserts = FakeGl::get().inserts();
  watch->note_frame_issued(Kind::kRvizFrame);
  EXPECT_EQ(FakeGl::get().inserts(), inserts) << "no fence for a frame of a closed series";
  watch->note_frame_skipped();
  watch->note_frame_skipped();
  watch->note_frame_skipped();
  watch->note_batch_skipped();
  watch->note_batch_skipped();

  FakeGl::get().signal_all();
  ASSERT_TRUE(wait_until([&] { return !watch->rviz_gate_closed(); }, 1s));
  ASSERT_TRUE(wait_until([&] { return watch->frames_completed() == 1u; }));
  // Let the watch thread see the open gate before the next test step.
  std::this_thread::sleep_for(40ms);
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(watch->closes(), 1u);
  EXPECT_EQ(watch->skipped_frames(), 3u);
  EXPECT_EQ(watch->skipped_batches(), 2u);
  EXPECT_FALSE(watch->any_gate_closed());

  // Each line once, with its counts.
  const std::string first = "the GPU has not finished a frame for ";
  const std::string rest = " ms: RViz frames and the 3D render batch pause\n";
  const std::size_t at = err.find(first);
  ASSERT_NE(at, std::string::npos) << err;
  EXPECT_EQ(err.find(first, at + 1), std::string::npos)
    << "the line about the close is written once";
  EXPECT_TRUE(contains(err, rest)) << err;
  EXPECT_TRUE(contains(err, "[qar-gpu] ")) << err;
  EXPECT_TRUE(contains(err, "the GPU finishes frames again after ")) << err;
  EXPECT_TRUE(contains(err, "; 3 RViz frame(s) and 2 render batch(es) were skipped")) << err;
  const std::string open_line = "the GPU finishes frames again";
  EXPECT_EQ(err.find(open_line, err.find(open_line) + 1), std::string::npos)
    << "the line about the open is written once";
}

TEST_F(GpuWatchTest, AFrameYoungerThanTheLimitNeverClosesAnything)
{
  ASSERT_TRUE(start_active());
  EXPECT_TRUE(rviz_frame());
  watch->note_batch_issued();
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(watch->frames_pending(), 2u);
  EXPECT_FALSE(watch->rviz_gate_closed());
  EXPECT_FALSE(watch->gate_closed());
  EXPECT_EQ(watch->closes(), 0u);
  FakeGl::get().signal_all();
  ASSERT_TRUE(wait_until([&] { return watch->frames_completed() == 2u; }));
  EXPECT_EQ(watch->closes(), 0u);
}

TEST_F(GpuWatchTest, TheBatchSeriesAndTheRvizSeriesCloseOnTheirOwn)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  watch->note_batch_issued();
  std::this_thread::sleep_for(60ms);
  EXPECT_TRUE(watch->gate_closed()) << "the unfinished batch is older than 40 ms";
  EXPECT_FALSE(watch->rviz_gate_closed()) << "RViz has no unfinished frame";
  EXPECT_TRUE(rviz_frame()) << "a frame of RViz is issued while the batch series is closed";
  const int inserts = FakeGl::get().inserts();
  watch->note_batch_issued();
  EXPECT_EQ(FakeGl::get().inserts(), inserts) << "no fence for a batch of a closed series";
  FakeGl::get().signal_all();
  ASSERT_TRUE(wait_until([&] { return !watch->gate_closed(); }, 1s));
  testing::internal::GetCapturedStderr();
}

// A healthy GPU: the frames complete within a few frame times, so the age limit (3 x the median)
// is never reached and no frame is skipped, whatever the rate.
TEST_F(GpuWatchTest, HealthyFramesAtSixtyFpsNeverSkip)
{
  ASSERT_TRUE(start_active());
  run_steady(16667us, 33ms, 8);
  std::this_thread::sleep_for(60ms);  // the first frames build the median; their fences end
  const std::uint64_t closes_before = watch->closes();
  EXPECT_EQ(run_steady(16667us, 33ms, 40), 0);
  EXPECT_EQ(watch->closes(), closes_before);
  EXPECT_GE(watch->age_limit_ms(Kind::kRvizFrame), 95) << "3 x the median of about 33 ms";
  EXPECT_LE(watch->age_limit_ms(Kind::kRvizFrame), 250);
}

TEST_F(GpuWatchTest, HealthyFramesAtTwentyOneFpsNeverSkip)
{
  ASSERT_TRUE(start_active());
  run_steady(47619us, 33ms, 4);
  std::this_thread::sleep_for(60ms);  // the first frames build the median; their fences end
  const std::uint64_t closes_before = watch->closes();
  EXPECT_EQ(run_steady(47619us, 33ms, 24), 0);
  EXPECT_EQ(watch->closes(), closes_before);
  EXPECT_GE(watch->age_limit_ms(Kind::kRvizFrame), 95);
  EXPECT_LE(watch->age_limit_ms(Kind::kRvizFrame), 250);
}

TEST_F(GpuWatchTest, HealthyFramesAtTenFpsNeverSkip)
{
  ASSERT_TRUE(start_active());
  run_steady(100ms, 20ms, 3);
  std::this_thread::sleep_for(60ms);  // the first frames build the median; their fences end
  const std::uint64_t closes_before = watch->closes();
  EXPECT_EQ(run_steady(100ms, 20ms, 12), 0);
  EXPECT_EQ(watch->closes(), closes_before);
  EXPECT_GE(watch->age_limit_ms(Kind::kRvizFrame), 40) << "never below the floor";
  EXPECT_LE(watch->age_limit_ms(Kind::kRvizFrame), 150);
}

// A GPU that takes far longer than the limit: while a frame is unfinished beyond the limit, every
// later frame is skipped. Frames that come less often than the limit leave one frame in flight.
// One stalled frame does not raise the limit.
TEST_F(GpuWatchTest, ASlowGpuKeepsOneFrameInFlight)
{
  ASSERT_TRUE(start_active());
  EXPECT_EQ(run_steady(20ms, 10ms, 10), 0) << "ten healthy frames first";
  const std::int64_t healthy_limit = watch->age_limit_ms(Kind::kRvizFrame);
  EXPECT_GE(healthy_limit, 40);
  EXPECT_LE(healthy_limit, 60);

  ASSERT_TRUE(wait_until([&] { return watch->frames_pending() == 0u; })) << "the warm-up ends";
  FakeGl::get().set_latency(1000ms);
  int skipped = 0;
  std::uint64_t most_in_flight = 0;
  for (int i = 0; i < 6; ++i) {
    if (!rviz_frame()) { ++skipped; }
    most_in_flight = std::max(most_in_flight, watch->frames_pending());
    std::this_thread::sleep_for(70ms);  // every gap is longer than the limit (40 to 60 ms)
  }
  EXPECT_EQ(most_in_flight, 1u) << "a frame is unfinished beyond the limit when the next one asks";
  EXPECT_EQ(skipped, 5) << "all but the first frame";
  EXPECT_EQ(watch->closes(), 1u);

  // The stalled frame ends: the gate opens, and the limit stays near the healthy median.
  ASSERT_TRUE(wait_until([&] { return !watch->any_gate_closed(); }, 3s));
  ASSERT_TRUE(wait_until([&] { return watch->frames_pending() == 0u; }, 3s));
  EXPECT_LE(watch->age_limit_ms(Kind::kRvizFrame), 120) << "one slow frame among eleven";
}

// Frames that come more often than the limit: those issued within one limit are in flight, no more.
TEST_F(GpuWatchTest, AFastRateOnASlowGpuHoldsOnlyTheFramesOfOneLimit)
{
  ASSERT_TRUE(start_active());
  FakeGl::get().set_latency(500ms);
  std::uint64_t most_in_flight = 0;
  int issued = 0;
  const auto begin = std::chrono::steady_clock::now();
  while (ms_since(begin) < 300.0) {
    if (rviz_frame()) { ++issued; }
    most_in_flight = std::max(most_in_flight, watch->frames_pending());
    std::this_thread::sleep_for(5ms);
  }
  // Frames are issued only while the first is younger than 40 ms: a few at one per 5 ms.
  EXPECT_GE(issued, 1);
  EXPECT_LE(issued, 12) << "the frames of the first 40 ms";
  EXPECT_EQ(most_in_flight, static_cast<std::uint64_t>(issued)) << "none finished yet";
  EXPECT_GE(watch->skipped_frames(), 10u) << "every frame after them is skipped";
}

TEST_F(GpuWatchTest, AWatchThatCannotBindItsContextLeavesTheGateOpen)
{
  FakeGl::get().set_bind_result(5);
  testing::internal::CaptureStderr();
  ASSERT_TRUE(start());
  ASSERT_TRUE(wait_until([&] { return FakeGl::get().binds() == 1; }));
  ASSERT_TRUE(wait_until([&] { return FakeGl::get().deletes() == 1; })) << "the context is deleted";
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(
      err, " the GPU watch could not bind its GL context (err=5); the gate is off\n")) << err;
  EXPECT_TRUE(contains(err, "[qar-gpu] ")) << err;
  EXPECT_FALSE(watch->active());
  for (int i = 0; i < 4; ++i) {
    watch->note_frame_issued(Kind::kBatch);
  }
  EXPECT_EQ(FakeGl::get().inserts(), 0) << "nothing is tracked";
  std::this_thread::sleep_for(80ms);
  EXPECT_FALSE(watch->gate_closed());
  EXPECT_FALSE(watch->rviz_gate_closed());
}

TEST_F(GpuWatchTest, AContextThatCannotBeMadeLeavesTheGateOpenAndSaysSo)
{
  FakeGl::get().set_create_result(nullptr);
  testing::internal::CaptureStderr();
  EXPECT_FALSE(start());
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(contains(
      err, " the GPU watch could not create its GL context (err=87); the gate is off\n")) << err;
  EXPECT_FALSE(contains(err, "could not bind")) << "creating and binding are told apart";
  EXPECT_FALSE(watch->active());
  EXPECT_EQ(FakeGl::get().binds(), 0);
  EXPECT_FALSE(watch->gate_closed());
}

TEST_F(GpuWatchTest, StoppingTheWatchOpensTheGateAtOnceAndEndsTheThread)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  watch->note_batch_issued();
  watch->note_frame_issued(Kind::kRvizFrame);
  ASSERT_TRUE(wait_until([&] { return watch->gate_closed() && watch->rviz_gate_closed(); }, 1s));

  const auto stopped_at = std::chrono::steady_clock::now();
  watch->stop();
  EXPECT_FALSE(watch->gate_closed()) << "a stopped watch holds nothing back";
  EXPECT_FALSE(watch->rviz_gate_closed());
  EXPECT_FALSE(watch->any_gate_closed());
  EXPECT_LT(ms_since(stopped_at), 5.0) << "stop() does not wait for the thread";
  ASSERT_TRUE(wait_until([&] { return GpuProgressWatch::running_threads() == 0; }, 2s));
  GpuProgressWatch::reap();
  testing::internal::GetCapturedStderr();
  EXPECT_EQ(FakeGl::get().unbinds(), 1);
  EXPECT_EQ(FakeGl::get().deletes(), 1) << "the context is deleted by the thread that bound it";
  EXPECT_EQ(FakeGl::get().removes(), 3)
    << "the fences that were never waited for (two frames and the probe) are deleted";
  const int inserts = FakeGl::get().inserts();
  watch->note_frame_issued(Kind::kBatch);
  EXPECT_EQ(FakeGl::get().inserts(), inserts) << "a stopped watch issues no fence";
}

TEST_F(GpuWatchTest, ADriverCallThatDoesNotReturnNeverBlocksTheCaller)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  FakeGl::get().block_hard();
  watch->note_frame_issued(Kind::kBatch);
  watch->note_frame_issued(Kind::kRvizFrame);
  ASSERT_TRUE(wait_until([] { return FakeGl::get().waits() >= 1; })) << "the thread is inside it";
  const auto begin = std::chrono::steady_clock::now();
  watch->stop();
  EXPECT_FALSE(watch->gate_closed());
  watch.reset();
  EXPECT_LT(ms_since(begin), 20.0) << "the caller returns while the watch thread is inside a call";
  EXPECT_EQ(GpuProgressWatch::running_threads(), 1);
  FakeGl::get().unblock();
  ASSERT_TRUE(wait_until([&] { return GpuProgressWatch::running_threads() == 0; }, 2s));
  GpuProgressWatch::reap();
  testing::internal::GetCapturedStderr();
  EXPECT_EQ(FakeGl::get().deletes(), 1);
}

// QAR_GPU_GATE_TEST: both series close once, on purpose, a while after the first batch, and open
// after the given time; no probe fence is inserted for it.
TEST_F(GpuWatchTest, TheTestCloseShutsTheGateOnceForTheGivenTime)
{
  FakeGl::get().set_auto_signal(true);
  ASSERT_TRUE(start_active(quick_timings(), /*test_s=*/0.15));
  testing::internal::CaptureStderr();
  EXPECT_FALSE(watch->gate_closed()) << "open before the first batch";
  watch->note_frame_issued(Kind::kRvizFrame);
  EXPECT_FALSE(watch->gate_closed()) << "a frame of RViz does not start the timer";
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(watch->gate_closed());

  watch->note_batch_issued();
  EXPECT_FALSE(watch->gate_closed()) << "open until the start time has passed (60 ms)";
  ASSERT_TRUE(wait_until([&] { return watch->gate_closed(); }, 1s));
  const auto closed_at = std::chrono::steady_clock::now();
  EXPECT_TRUE(watch->rviz_gate_closed()) << "RViz's frames pause too";
  ASSERT_TRUE(wait_until([&] { return !watch->gate_closed(); }, 2s));
  const double closed_ms = ms_since(closed_at);
  EXPECT_GT(closed_ms, 90.0) << "closed for about 150 ms";
  EXPECT_LT(closed_ms, 400.0);
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(watch->gate_closed()) << "closed only once";
  ASSERT_TRUE(wait_until([&] { return watch->closes() >= 1u; }));
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(watch->closes(), 1u);
  EXPECT_EQ(watch->probes(), 0u) << "a test close gets no probe";
  EXPECT_TRUE(contains(err, "QAR_GPU_GATE_TEST=0.15: the gate is closed on purpose for 0.15 s"))
    << err;
  EXPECT_TRUE(contains(err, "the GPU finishes frames again after ")) << err;
}

// ---------------------------------------------------------------------------------------------
//  The probe fence: a fence that is never signalled cannot hold the gate closed for good
// ---------------------------------------------------------------------------------------------

TEST_F(GpuWatchTest, AProbeFenceThatFinishesDropsTheOlderFenceAndOpensTheGate)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  EXPECT_TRUE(rviz_frame());  // fence 1: never signalled
  ASSERT_TRUE(wait_until([&] { return watch->rviz_gate_closed(); }, 1s));
  EXPECT_EQ(watch->probes(), 1u) << "the first closed look inserts the probe (fence 2)";

  // One probe at a time, and at most one per interval: more looks add none.
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(watch->rviz_gate_closed());
  }
  std::this_thread::sleep_for(150ms);
  EXPECT_TRUE(watch->rviz_gate_closed());
  EXPECT_EQ(watch->probes(), 1u) << "the previous probe is not done, so there is no next one";
  EXPECT_EQ(FakeGl::get().inserts(), 2);

  // A frame of the batch series after the probe stays pending when the probe is done.
  watch->note_batch_issued();
  EXPECT_EQ(watch->frames_pending(), 2u);

  FakeGl::get().signal_fence(2);  // only the probe
  ASSERT_TRUE(wait_until([&] { return !watch->rviz_gate_closed(); }, 2s));
  ASSERT_TRUE(wait_until([&] { return watch->dropped_fences() == 1u; }));
  EXPECT_EQ(watch->frames_pending(), 1u) << "only the fence issued after the probe is left";
  EXPECT_TRUE(FakeGl::get().was_removed(1)) << "the stale fence is deleted";
  EXPECT_TRUE(FakeGl::get().was_removed(2)) << "the probe is deleted";
  EXPECT_FALSE(FakeGl::get().was_removed(3)) << "the later fence is not touched";
  std::this_thread::sleep_for(40ms);
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(count_of(
      err, " a probe fence finished while 1 older fence(s) stayed unfinished; they were dropped\n"),
    1) << err;
  EXPECT_EQ(count_of(err, "the GPU finishes frames again after "), 1) << err;

  // The frames of RViz are tracked again.
  EXPECT_TRUE(rviz_frame());
  EXPECT_EQ(FakeGl::get().inserts(), 4);
}

TEST_F(GpuWatchTest, AProbeThatFailsIsForgottenAndTheNextOneComesAfterTheInterval)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  EXPECT_TRUE(rviz_frame());  // fence 1: never signalled
  FakeGl::get().fail_fence(2);  // the first probe
  ASSERT_TRUE(wait_until([&] { return watch->rviz_gate_closed(); }, 1s));
  EXPECT_EQ(watch->probes(), 1u);
  ASSERT_TRUE(wait_until([&] { return FakeGl::get().was_removed(2); })) << "the failed probe goes";
  EXPECT_EQ(watch->dropped_fences(), 0u) << "a failed probe proves nothing";
  EXPECT_TRUE(watch->rviz_gate_closed());
  EXPECT_EQ(watch->probes(), 1u) << "the interval has not passed";
  std::this_thread::sleep_for(110ms);
  EXPECT_TRUE(watch->rviz_gate_closed());
  EXPECT_EQ(watch->probes(), 2u) << "the next probe, after the interval";
  EXPECT_EQ(watch->frames_pending(), 1u);
  testing::internal::GetCapturedStderr();
}

// A wait that fails on the watch thread: said once, the gate opens for good, nothing is followed.
TEST_F(GpuWatchTest, AFailedFenceWaitStopsTheWatchAndLeavesTheGateOpen)
{
  ASSERT_TRUE(start_active());
  testing::internal::CaptureStderr();
  FakeGl::get().fail_fence(1);
  EXPECT_TRUE(rviz_frame());
  ASSERT_TRUE(wait_until([&] { return watch->wait_failed(); }, 1s));
  EXPECT_FALSE(watch->active());
  EXPECT_FALSE(watch->rviz_gate_closed());
  EXPECT_FALSE(watch->gate_closed());
  ASSERT_TRUE(wait_until([&] { return GpuProgressWatch::running_threads() == 0; }, 2s));
  GpuProgressWatch::reap();
  const int inserts = FakeGl::get().inserts();
  watch->note_frame_issued(Kind::kRvizFrame);
  watch->note_frame_issued(Kind::kBatch);
  EXPECT_EQ(FakeGl::get().inserts(), inserts) << "the watch issues no more fences";
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(count_of(err, " the GPU watch could not wait for a fence; the gate is off\n"), 1)
    << err;
  EXPECT_EQ(FakeGl::get().unbinds(), 1) << "the thread ends in order";
  EXPECT_EQ(FakeGl::get().deletes(), 1);
}

// The lines of a gate that closes and opens again and again: the first five closes, then one in a
// hundred, with the number of the close. Each cycle is one stalled frame followed by three quick
// ones, so that the median frame time (and with it the age limit) stays small.
TEST_F(GpuWatchTest, TheLinesOfTheGateAreRateLimited)
{
  GpuGateTimings fast = quick_timings();
  fast.min_age = 4ms;
  fast.slice = 2ms;
  ASSERT_TRUE(start_active(fast));
  testing::internal::CaptureStderr();
  const int cycles = 102;
  for (int i = 1; i <= cycles; ++i) {
    ASSERT_TRUE(rviz_frame()) << i;
    ASSERT_TRUE(wait_until([&] { return watch->closes() == static_cast<std::uint64_t>(i); }, 1s))
      << "close " << i;
    FakeGl::get().signal_all();
    ASSERT_TRUE(wait_until([&] { return !watch->any_gate_closed(); }, 1s)) << i;
    for (int k = 0; k < 3; ++k) {
      ASSERT_TRUE(rviz_frame()) << i;
      FakeGl::get().signal_all();
      ASSERT_TRUE(wait_until([&] { return watch->frames_pending() == 0u; }, 1s)) << i;
    }
    std::this_thread::sleep_for(8ms);  // the watch thread looks at least every 2 ms
  }
  const std::string err = testing::internal::GetCapturedStderr();
  const std::string close_line = "the GPU has not finished a frame for ";
  EXPECT_EQ(watch->closes(), static_cast<std::uint64_t>(cycles));
  EXPECT_EQ(count_of(err, close_line), 6) << "closes 1 to 5 and the 100th";
  EXPECT_EQ(count_of(err, "the GPU finishes frames again after "), 6)
    << "an open line goes with every close line";
  EXPECT_EQ(count_of(err, " (close 100)\n"), 1) << err;
  EXPECT_EQ(count_of(err, " (close "), 1) << "the first five lines have no number";
}

}  // namespace
