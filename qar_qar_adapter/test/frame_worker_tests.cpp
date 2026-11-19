// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The frame worker: the thread that makes the QarOS frame calls of one sender. A small test sender
// stands in for the adapters: its poll job makes the same calls as the 3D adapter (begin-frame,
// hands, frame access, frame-info destroy), its show job shows the frame, its leave job posts the
// release of the stream on the owner's lane. The calls go to the fake QarOS library; a frame call
// that never returns is the fake's frame block. Every time bound has a lower and an upper side.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/frame_worker.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using namespace qar_qar_adapter;
using namespace qar_qar_adapter::test;
using namespace std::chrono_literals;

constexpr char kTryBegin[] = "render_sender_try_begin_frame";
constexpr char kShow[] = "render_sender_show_frame";

// The lines a worker writes.
class LineBox
{
public:
  std::function<void(const std::string &)> sink()
  {
    return [this](const std::string & line) {
             std::lock_guard<std::mutex> lock(mutex_);
             lines_.push_back(line);
           };
  }

  int count(const std::string & part) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const std::string & line : lines_) {
      if (line.find(part) != std::string::npos) {
        ++n;
      }
    }
    return n;
  }

  std::string first(const std::string & part) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string & line : lines_) {
      if (line.find(part) != std::string::npos) {
        return line;
      }
    }
    return std::string();
  }

  std::string all() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const std::string & line : lines_) {
      out += line + "\n";
    }
    return out;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

// The fake library loaded for a test that has no owner.
struct Loaded
{
  explicit Loaded(FakeQarApi & fake) : loader(fake.make_loader())
  {
    std::string error;
    loader->load("", error);
  }
  ~Loaded() { loader->unload(); }
  std::unique_ptr<ILibraryLoader> loader;
};

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

// How many lines of the owner's log hold @p part.
int count_logged(const Harness & h, const std::string & part)
{
  int n = 0;
  for (const std::string & line : h.log_lines()) {
    if (line.find(part) != std::string::npos) {
      ++n;
    }
  }
  return n;
}

// What the GUI side and the worker's jobs share. The mutex is never held across a QarOS call.
struct Core
{
  std::mutex m;
  int phase = 0;  // 0 idle, 1 a cycle is published, 2 the frame was handed over
  std::atomic<int> published{0};
  std::atomic<int> shown{0};
  std::atomic<int> leaves{0};
  std::atomic<int> destroyed{0};
  std::shared_ptr<IReleaseLane> lane;
};

FrameWorker::Jobs make_jobs(const std::shared_ptr<Core> & core)
{
  FrameWorker::Jobs jobs;
  jobs.poll = [core] {
      {
        std::lock_guard<std::mutex> lock(core->m);
        if (core->phase != 0) {
          return;
        }
      }
      QarRenderFrameInfo * info = nullptr;
      bool ready = false;
      const QarResult began = QAR_CALL(qar_render_sender_try_begin_frame, nullptr, &info, &ready);
      if (qar_result_is_error(began) || !ready) {
        return;
      }
      QarDeviceHandsWithJoints hands{};
      (void)QAR_CALL(qar_render_sender_last_hands, nullptr, &hands);
      QarVideoFrameOpenGl gl{};
      (void)QAR_CALL(qar_render_sender_frame_opengl, nullptr, &gl);
      QAR_CALL(qar_render_frame_info_handle_destroy, info);
      std::lock_guard<std::mutex> lock(core->m);
      core->phase = 1;
      core->published.fetch_add(1);
    };
  jobs.show = [core] {
      QarRenderFrameShow show{};
      (void)QAR_CALL(qar_render_sender_show_frame, nullptr, &show);
      core->shown.fetch_add(1);
      std::lock_guard<std::mutex> lock(core->m);
      core->phase = 0;
    };
  jobs.leave = [core] {
      core->leaves.fetch_add(1);
      if (core->lane) {
        core->lane->post_release("render-stream-release", [core] {
            QAR_CALL(qar_render_stream_handle_destroy, nullptr);
            core->destroyed.fetch_add(1);
          });
      }
    };
  return jobs;
}

// The GUI thread's side of one tick: take a published cycle and hand it over, else kick.
void tick(Core & core, FrameWorker & worker)
{
  bool take = false;
  {
    std::lock_guard<std::mutex> lock(core.m);
    if (core.phase == 1) {
      core.phase = 2;
      take = true;
    }
  }
  if (take) {
    worker.hand_over();
  } else {
    worker.kick();
  }
}

FrameWorkerTimings scaled()
{
  FrameWorkerTimings t;
  t.slow_call = 50ms;
  t.wait_notice = 150ms;
  return t;
}

// An owner on the fake library, with the limits scaled down, and a guard that lets every frame
// worker and every owner thread end before the fake library is reset.
class Rig
{
public:
  Rig()
  {
    use_scaled_timings(h);
    h.config().timings.frame_worker_settle = 200ms;
  }

  ~Rig()
  {
    h.fake().set_frame_block(false);
    wait_until([] { return FrameWorker::running() == 0; }, 10s);
    wait_until([] { return abandoned_owner_threads_running() == 0; }, 10s);
    FrameWorker::reap();
  }

  bool up()
  {
    h.start();
    if (!h.connect_and_run()) {
      return false;
    }
    core->lane = h.owner().release_lane();
    return core->lane != nullptr;
  }

  std::shared_ptr<FrameWorker> make_worker(const std::string & name = "peer 9733c6bf")
  {
    FrameWorkerLog log;
    log.sink = box.sink();
    return FrameWorker::start(name, core->lane, make_jobs(core), scaled(), log);
  }

  Harness h;
  std::shared_ptr<Core> core = std::make_shared<Core>();
  LineBox box;
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

// One kick of the GUI thread while the frame calls are blocked: it must return at once.
bool kick_blocked(Rig & rig, FrameWorker & worker)
{
  return returns_within_or_unblock(
    [&worker] { worker.kick(); }, 2s, [&rig] { rig.h.fake().set_frame_block(false); });
}

TEST(FrameWorker, EveryFrameCallRunsOnTheWorkersThread)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  EXPECT_EQ(FrameWorker::running(), 1);
  EXPECT_EQ(rig.core->lane->workers_alive(), 1);
  const std::thread::id caller = std::this_thread::get_id();
  ASSERT_TRUE(wait_until([&] {
      tick(*rig.core, *worker);
      return rig.core->shown.load() >= 1;
    }));
  const std::thread::id begin = rig.h.fake().thread_of_last(kTryBegin);
  EXPECT_NE(begin, std::thread::id());
  EXPECT_NE(begin, caller);
  EXPECT_NE(begin, rig.h.owner_thread());
  EXPECT_EQ(rig.h.fake().thread_of_last("render_sender_last_hands"), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last("render_sender_frame_opengl"), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last(kShow), begin);
  EXPECT_EQ(rig.h.fake().thread_of_last("render_frame_info_handle_destroy"), begin);
  EXPECT_EQ(rig.box.count("frame worker started tid="), 1) << rig.box.all();
  EXPECT_NE(rig.box.first("frame worker started").find("[qar-adapter] "), std::string::npos);
  EXPECT_NE(rig.box.first("frame worker started").find(" peer 9733c6bf frame worker started"),
    std::string::npos);
  EXPECT_GT(worker->polls(), 0u);
  EXPECT_EQ(worker->shows(), static_cast<unsigned long long>(rig.core->shown.load()));
  worker->stop();
  ASSERT_TRUE(wait_until([&] { return worker->exited(); }));
  EXPECT_EQ(rig.core->leaves.load(), 1);
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }))
    << "the release runs on the owner's lane";
  EXPECT_EQ(rig.core->destroyed.load(), 1);
  EXPECT_EQ(rig.core->lane->workers_alive(), 0);
}

TEST(FrameWorker, TheCallerNeverWaitsForABlockedCall)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_block(true);
  std::vector<double> took_ms;
  bool in_time = returns_within_or_unblock([&] {
      for (int i = 0; i < 200; ++i) {
        const auto start = std::chrono::steady_clock::now();
        tick(*rig.core, *worker);
        took_ms.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
        if (i == 10) {
          std::this_thread::sleep_for(1ms);
        }
      }
    }, 3s, [&] { rig.h.fake().set_frame_block(false); });
  ASSERT_TRUE(in_time) << "tick() waited for the blocked worker";
  ASSERT_EQ(took_ms.size(), 200u);
  std::vector<double> sorted = took_ms;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_LT(sorted[189], 5.0) << "the 95th percentile of 200 ticks";
  EXPECT_LT(sorted.back(), 100.0) << "the slowest tick";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().frame_calls_blocked() == 1; }));
  EXPECT_EQ(rig.h.fake().count(kTryBegin), 1) << "the blocked worker polls no more";

  // Stuck for longer than wait_notice: the wait is named.
  std::this_thread::sleep_for(300ms);
  FrameWorker::Wait wait;
  EXPECT_TRUE(worker->waiting(&wait));
  EXPECT_EQ(wait.call, "qar_render_sender_try_begin_frame");
  EXPECT_GE(wait.seconds, 0.15);
  EXPECT_LT(wait.seconds, 3.0);
  EXPECT_EQ(rig.box.count("frame worker waiting in qar_render_sender_try_begin_frame for "), 1)
    << "once per wait\n" << rig.box.all();
  for (int i = 0; i < 50; ++i) {
    tick(*rig.core, *worker);
  }
  EXPECT_EQ(rig.box.count("frame worker waiting in"), 1);
  EXPECT_EQ(rig.core->shown.load(), 0);

  rig.h.fake().set_frame_block(false);
  ASSERT_TRUE(wait_until([&] {
      tick(*rig.core, *worker);
      return rig.core->shown.load() >= 2;
    })) << "frames resume";
  EXPECT_FALSE(worker->waiting());
  EXPECT_EQ(rig.box.count("frame worker free again after "), 1) << rig.box.all();
  worker->stop();
}

TEST(FrameWorker, StopReturnsAtOnceAndTheStreamIsReleasedOnlyWhenTheCallReturns)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_block(true);
  ASSERT_TRUE(kick_blocked(rig, *worker)) << "kick() waited for the blocked worker";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().frame_calls_blocked() == 1; }));
  std::this_thread::sleep_for(250ms);

  const auto start = std::chrono::steady_clock::now();
  bool in_time = returns_within_or_unblock([&] {
      worker->stop();
      worker.reset();
    }, 2s, [&] { rig.h.fake().set_frame_block(false); });
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(in_time) << "stop() waited for the blocked worker";
  EXPECT_LT(took_ms, 100.0);
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0) << "nothing is released while it blocks";
  EXPECT_EQ(rig.core->leaves.load(), 0);
  EXPECT_EQ(rig.box.count("frame worker left running inside qar_render_sender_try_begin_frame "),
    1) << rig.box.all();
  EXPECT_NE(rig.box.first("left running").find("; its stream handle stays unreleased"),
    std::string::npos);
  EXPECT_EQ(FrameWorker::running(), 1);
  EXPECT_EQ(FrameWorker::parked(), 1) << "the reaper holds the thread";
  FrameWorker::reap();
  EXPECT_EQ(FrameWorker::parked(), 1) << "a thread that has not ended is never joined";

  rig.h.fake().set_frame_block(false);
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().render_streams_destroyed() == 1; }));
  EXPECT_EQ(rig.core->leaves.load(), 1);
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  FrameWorker::reap();
  EXPECT_EQ(FrameWorker::parked(), 0) << "the reaper joined the ended thread";
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 1) << "released exactly once";
  EXPECT_EQ(rig.core->lane->workers_alive(), 0);
}

TEST(FrameWorker, TheExitBudgetHoldsWhileAWorkerNeverReturns)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_block(true);
  ASSERT_TRUE(kick_blocked(rig, *worker)) << "kick() waited for the blocked worker";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().frame_calls_blocked() == 1; }));
  worker->stop();
  worker.reset();
  const int owners_before = abandoned_owner_threads();

  const auto start = std::chrono::steady_clock::now();
  const bool in_time = rig.h.destroy_owner_within(
    rig.h.config().timings.exit_join + 200ms);
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
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
  EXPECT_EQ(rig.h.fake().count("library_unload"), 0);
  EXPECT_TRUE(rig.h.fake().library_loaded());
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);

  const std::size_t calls_at_exit = rig.h.fake().calls().size();
  rig.h.fake().set_frame_block(false);
  ASSERT_TRUE(wait_until([&] { return FrameWorker::running() == 0; }));
  EXPECT_EQ(rig.h.fake().calls().size(), calls_at_exit)
    << "the worker that was left behind makes no further QarOS call";
  EXPECT_EQ(rig.core->leaves.load(), 0) << "and releases nothing";
  EXPECT_EQ(rig.h.fake().render_streams_destroyed(), 0);
}

// A release with a worker that never returns from its call: the session is left as it is, the
// owner thread stays, nothing is counted as an abandoned owner, and the next join works.
TEST(FrameWorker, AWorkerStuckWhileTheSessionIsReleasedLeavesTheSessionAndKeepsTheOwner)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_block(true);
  ASSERT_TRUE(kick_blocked(rig, *worker)) << "kick() waited for the blocked worker";
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().frame_calls_blocked() == 1; }));
  const int owners_before = abandoned_owner_threads();
  const std::shared_ptr<IReleaseLane> lane = rig.core->lane;
  ASSERT_EQ(lane->workers_alive(), 1);

  const auto start = std::chrono::steady_clock::now();
  rig.h.owner().disconnect();
  ASSERT_TRUE(rig.h.wait_for([&] {
      return rig.h.logged("QAROS: release: 1 frame worker(s) still inside a QarOS call; their "
      "session is left unreleased");
    })) << "the owner gives up waiting after frame_worker_settle";
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
  EXPECT_GE(took_ms, 150.0) << "it waited for the worker first (frame_worker_settle 200 ms)";
  ASSERT_TRUE(rig.h.wait_state(SessionState::kIdle));
  EXPECT_EQ(rig.h.fake().count("session_handle_destroy"), 0)
    << "the session is not destroyed under a worker that is inside a call";
  EXPECT_EQ(abandoned_owner_threads(), owners_before) << "no owner is replaced";
  EXPECT_FALSE(rig.h.logged("watchdog: owner thread abandoned"));
  EXPECT_FALSE(rig.h.logged("the owner is replaced"));
  EXPECT_TRUE(worker->abandoned()) << "the worker is told to make no further call";
  EXPECT_EQ(lane->workers_alive(), 0) << "the worker is not counted any more";

  // The next join works on the same owner.
  ASSERT_TRUE(rig.h.connect_and_run());
  EXPECT_EQ(abandoned_owner_threads(), owners_before);
  const auto second = rig.make_worker("peer 2");
  ASSERT_TRUE(second);
  ASSERT_EQ(lane->workers_alive(), 1);

  // The first worker returns late: it makes no call, releases nothing and takes away no count.
  const std::size_t calls_before = rig.h.fake().calls().size();
  rig.h.fake().set_frame_block(false);
  worker->stop();
  ASSERT_TRUE(wait_until([&] { return worker->exited(); }));
  EXPECT_EQ(rig.h.fake().calls().size(), calls_before)
    << "the abandoned worker makes no further QarOS call";
  EXPECT_EQ(rig.core->leaves.load(), 0);
  EXPECT_EQ(lane->workers_alive(), 1) << "the late leave of a given-up worker changes no count";
  second->stop();
  ASSERT_TRUE(wait_until([&] { return second->exited(); }));
  ASSERT_TRUE(wait_until([&] { return lane->workers_alive() == 0; }));

  // At the end the owner leaves the runtime and the library alone: a stream of that session may
  // still be inside them.
  ASSERT_TRUE(rig.h.destroy_owner_within(5s));
  EXPECT_EQ(rig.h.fake().count("runtime_handle_destroy"), 0);
  EXPECT_EQ(rig.h.fake().count("library_destroy"), 0);
}

// Two such releases in one process: the owner is never replaced, so the count of abandoned owners
// that ends the streaming stays where it was, and after each the owner joins again.
TEST(FrameWorker, TwoReleasesWithAStuckWorkerEachNeverEndTheStreaming)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const int owners_before = abandoned_owner_threads();
  std::vector<std::shared_ptr<FrameWorker>> workers;
  for (int round = 1; round <= 2; ++round) {
    const auto worker = rig.make_worker("peer " + std::to_string(round));
    ASSERT_TRUE(worker);
    workers.push_back(worker);
    rig.h.fake().set_frame_block(true);
    ASSERT_TRUE(kick_blocked(rig, *worker)) << "kick() waited for the blocked worker";
    ASSERT_TRUE(wait_until([&] { return rig.h.fake().frame_calls_blocked() == round; }));
    rig.h.owner().disconnect();
    ASSERT_TRUE(rig.h.wait_for([&] {
        return count_logged(rig.h, "their session is left unreleased") == round;
      })) << "round " << round;
    ASSERT_TRUE(rig.h.wait_state(SessionState::kIdle));
    EXPECT_NE(rig.h.owner().state(), SessionState::kFailed);
    ASSERT_TRUE(rig.h.connect_and_run()) << "the owner joins again after round " << round;
    EXPECT_EQ(abandoned_owner_threads(), owners_before);
  }
  EXPECT_FALSE(rig.h.logged("restart RViz to stream again"));
  EXPECT_EQ(rig.h.fake().count("session_handle_destroy"), 0);
  rig.h.fake().set_frame_block(false);
  for (const auto & worker : workers) {
    worker->stop();
    ASSERT_TRUE(wait_until([&] { return worker->exited(); }));
  }
}

TEST(FrameWorker, KicksAreCoalescedAndNothingPollsWithoutAKick)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  const auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(worker->polls(), 0u) << "an idle worker does not poll";

  const auto start = std::chrono::steady_clock::now();
  const auto end = start + 300ms;
  while (std::chrono::steady_clock::now() < end) {
    worker->kick();
  }
  const double elapsed_us = std::chrono::duration<double, std::micro>(
    std::chrono::steady_clock::now() - start).count();
  std::this_thread::sleep_for(50ms);
  const auto polls = worker->polls();
  EXPECT_LE(static_cast<double>(polls), elapsed_us / 500.0 + 2.0)
    << "at most one poll per kick_min";
  EXPECT_GE(polls, 10u) << "the kicks do reach the worker";
  EXPECT_EQ(static_cast<unsigned long long>(rig.h.fake().count(kTryBegin)), polls)
    << "one begin-frame call per poll";

  const auto settled = worker->polls();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(worker->polls(), settled) << "no polling without a kick";
  worker->stop();
}

TEST(FrameWorker, AShowTakesPriorityOverAKickAndDropsTheKicksOfItsRun)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const auto worker = rig.make_worker();
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_delay_ms(60, kShow);
  worker->hand_over();
  ASSERT_TRUE(wait_until([&] { return rig.h.fake().count(kShow) == 1; }));
  const auto polls_before = worker->polls();
  worker->kick();  // arrives while show runs
  ASSERT_TRUE(wait_until([&] { return rig.core->shown.load() == 1; }));
  std::this_thread::sleep_for(60ms);
  EXPECT_EQ(worker->polls(), polls_before) << "the kick of the show's run is dropped";
  EXPECT_EQ(worker->shows(), 1u);
  worker->stop();
}

TEST(FrameWorker, ASlowCallIsReportedOnTheWorker)
{
  Rig rig;
  ASSERT_TRUE(rig.up());
  RenderFrameScript script;
  script.ready = true;
  rig.h.fake().set_render_frame(script);
  const auto worker = rig.make_worker("peer ab");
  ASSERT_TRUE(worker);
  rig.h.fake().set_frame_delay_ms(120, kShow);
  ASSERT_TRUE(wait_until([&] {
      tick(*rig.core, *worker);
      return rig.core->shown.load() >= 1;
    }));
  const std::string line = rig.box.first("frame call qar_render_sender_show_frame took ");
  ASSERT_FALSE(line.empty()) << rig.box.all();
  EXPECT_NE(line.find(" peer ab frame call qar_render_sender_show_frame took "),
    std::string::npos);
  EXPECT_NE(line.find(" ms on the frame worker"), std::string::npos);
  EXPECT_EQ(rig.box.count("frame call qar_render_sender_try_begin_frame took "), 0)
    << "fast calls are not reported";
  worker->stop();
}

TEST(FrameWorker, AWorkerNeedsALaneThatAcceptsIt)
{
  class Refusing : public IReleaseLane
  {
public:
    bool post_release(const char *, std::function<void()>) override { return false; }
    bool enter_worker(const std::shared_ptr<WorkerSlot> &) override { return false; }
  };
  const auto before = FrameWorker::running();
  EXPECT_EQ(FrameWorker::start("peer 1", std::make_shared<Refusing>(), FrameWorker::Jobs()),
    nullptr);
  EXPECT_EQ(FrameWorker::running(), before);

  // A host without an owner passes no lane; the worker runs its jobs and its leave job.
  FakeQarApi fake;
  Loaded loaded(fake);
  auto core = std::make_shared<Core>();
  auto worker = FrameWorker::start("peer 2", nullptr, make_jobs(core), scaled());
  ASSERT_TRUE(worker);
  worker->hand_over();
  ASSERT_TRUE(wait_until([&] { return core->shown.load() == 1; }));
  worker->stop();
  ASSERT_TRUE(wait_until([&] { return worker->exited(); }));
  EXPECT_EQ(core->leaves.load(), 1);
  worker.reset();
  FrameWorker::reap();
  EXPECT_EQ(FrameWorker::running(), before);
}

TEST(FrameWorker, AJobThatThrowsDoesNotEndTheWorker)
{
  auto count = std::make_shared<std::atomic<int>>(0);
  FrameWorker::Jobs jobs;
  jobs.poll = [count] {
      count->fetch_add(1);
      throw std::runtime_error("job failed");
    };
  auto worker = FrameWorker::start("peer 3", nullptr, jobs, scaled());
  ASSERT_TRUE(worker);
  ASSERT_TRUE(wait_until([&] {
      worker->kick();
      return count->load() >= 2;
    }));
  EXPECT_FALSE(worker->exited());
  worker->stop();
  ASSERT_TRUE(wait_until([&] { return worker->exited(); }));
  FrameWorker::reap();
}

TEST(FrameWorker, TheGuardBlocksAMarkedWorkerThreadOnceItIsAbandoned)
{
  FakeQarApi fake;
  Loaded loaded(fake);
  std::atomic<bool> abandoned{false};
  OwnerCallClock clock;
  std::vector<std::string> reports;
  std::thread worker([&] {
      mark_frame_worker_thread(
        [&](const char * call, double) { reports.push_back(call); }, &clock, &abandoned, 10.0);
      EXPECT_FALSE(frame_worker_calls_blocked());
      fake.set_frame_delay_ms(30, kShow);
      QarRenderFrameShow show{};
      const QarResult first = QAR_CALL(qar_render_sender_show_frame, nullptr, &show);
      EXPECT_FALSE(qar_result_is_error(first));
      EXPECT_EQ(clock.call.load(), nullptr) << "no call in progress afterwards";
      abandoned.store(true);
      EXPECT_TRUE(frame_worker_calls_blocked());
      EXPECT_TRUE(owner_calls_blocked());
      const QarResult blocked = QAR_CALL(qar_render_sender_show_frame, nullptr, &show);
      EXPECT_TRUE(qar_result_is_error(blocked));
      EXPECT_EQ(static_cast<int>(blocked.code), kBlockedCallStatus);
      mark_frame_worker_thread({}, nullptr, nullptr);
      EXPECT_FALSE(frame_worker_calls_blocked());
    });
  worker.join();
  EXPECT_EQ(fake.count(kShow), 1) << "the second call was not made";
  ASSERT_EQ(reports.size(), 1u) << "the 30 ms call is over the 10 ms limit";
  EXPECT_EQ(reports[0], "qar_render_sender_show_frame");
  EXPECT_FALSE(frame_worker_calls_blocked()) << "this thread is not a worker";
}

}  // namespace
