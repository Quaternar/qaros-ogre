// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The destructor of the session owner has a fixed budget: it waits for the owner thread at most
// exit_join and leaves a thread that is still inside a QarOS call running. A call that never
// returns is a call of the fake library held at a gate; the gate opens at the end of each test,
// and the test waits for the thread to end before the fake library is reset.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using qar_qar_adapter::SessionState;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::LeftBehindGuard;
using qar_qar_adapter::test::Outcome;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

void query_once(QarSession * session)
{
  std::size_t count = 0;
  (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
}

struct Destroyed
{
  bool in_time = false;
  long long ms = -1;
};

// Destroys the owner of @p h on a thread of its own. When that takes longer than @p limit the gate
// opens, so that the test fails instead of hanging.
Destroyed destroy_within(Harness & h, Gate & gate, std::chrono::milliseconds limit)
{
  auto done = std::make_shared<std::promise<long long>>();
  std::future<long long> result = done->get_future();
  std::thread worker([&h, done] {
      const auto start = std::chrono::steady_clock::now();
      h.destroy_owner_within(60s);
      done->set_value(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
    });
  Destroyed out;
  if (result.wait_for(limit) == std::future_status::ready) {
    out.in_time = true;
    out.ms = result.get();
  } else {
    gate.open();
    out.ms = result.get();
  }
  worker.join();
  return out;
}

std::chrono::milliseconds exit_limit(Harness & h)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(h.config().timings.exit_join) +
         200ms;
}

// The state every exit that leaves a thread behind must show.
void expect_left_behind(Harness & h, const Destroyed & destroyed, int abandoned_before)
{
  EXPECT_TRUE(destroyed.in_time) << "the destructor took " << destroyed.ms << " ms";
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), abandoned_before + 1);
  EXPECT_TRUE(h.fake().library_loaded()) << "the library stays loaded under a thread inside it";
  EXPECT_EQ(h.fake().count("library_destroy"), 0);
  EXPECT_EQ(h.fake().count("library_unload"), 0);
  EXPECT_TRUE(h.logged("QAROS: exit: owner thread left running after "));
  EXPECT_TRUE(h.logged("QAROS stays loaded, the plugin module is pinned"));
}

TEST(SessionOwnerExit, TheDestructorReturnsWithinTheBudgetWhileAQueryNeverReturns)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(gate.wait_entered());
  const int before = qar_qar_adapter::abandoned_owner_threads();

  const Destroyed destroyed = destroy_within(h, gate, exit_limit(h));
  expect_left_behind(h, destroyed, before);
  EXPECT_LE(destroyed.ms, 800) << "exit_join 600 ms plus 200 ms";
  EXPECT_GE(destroyed.ms, 500) << "it waited for the thread first";

  const std::size_t calls_at_exit = h.fake().calls().size();
  gate.open();
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (qar_qar_adapter::abandoned_owner_threads_running() > 0 &&
    std::chrono::steady_clock::now() < end)
  {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads_running(), 0);
  EXPECT_EQ(h.fake().calls().size(), calls_at_exit)
    << "the thread that was left behind makes no further QarOS call";
}

TEST(SessionOwnerExit, TheDestructorReturnsWithinTheBudgetWhileTheSessionDestroyBlocks)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "session_handle_destroy", gate);
  const int before = qar_qar_adapter::abandoned_owner_threads();
  const Destroyed destroyed = destroy_within(h, gate, exit_limit(h));
  ASSERT_TRUE(gate.entered());
  expect_left_behind(h, destroyed, before);
  EXPECT_EQ(h.fake().count("runtime_handle_destroy"), 0) << "nothing after the stuck call";
}

TEST(SessionOwnerExit, TheDestructorReturnsWithinTheBudgetWhileTheLibraryDestroyBlocks)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "library_destroy", gate);
  const int before = qar_qar_adapter::abandoned_owner_threads();
  const Destroyed destroyed = destroy_within(h, gate, exit_limit(h));
  ASSERT_TRUE(gate.entered());
  EXPECT_TRUE(destroyed.in_time) << destroyed.ms;
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 1);
  EXPECT_TRUE(h.fake().library_loaded()) << "never unloaded under the thread";
  EXPECT_EQ(h.fake().count("library_unload"), 0);
}

TEST(SessionOwnerExit, TheDestructorReturnsWithinTheBudgetWhileAnApprovalWaitIsOpen)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  Outcome open_prompt;
  open_prompt.wait_for_release = true;
  open_prompt.progress = true;
  h.fake().script_onboard(open_prompt);
  hold_first_call(h.fake(), "cancel_token_cancel", gate);
  h.start();
  h.owner().connect();
  ASSERT_TRUE(h.wait_state(SessionState::kWaitingForApproval, 5s));
  const int before = qar_qar_adapter::abandoned_owner_threads();
  const Destroyed destroyed = destroy_within(h, gate, exit_limit(h));
  ASSERT_TRUE(gate.entered());
  EXPECT_TRUE(destroyed.in_time) << destroyed.ms;
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 1);
  EXPECT_TRUE(h.logged("QAROS: exit: owner thread left running after "));
}

TEST(SessionOwnerExit, AHealthyExitReleasesInTheDesignOrderAndUnloads)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const int before = qar_qar_adapter::abandoned_owner_threads();
  ASSERT_TRUE(h.destroy_owner_within(10s));
  EXPECT_EQ(qar_qar_adapter::abandoned_owner_threads(), before);
  const int peer = h.fake().first_index("cancel_token_cancel");
  const int session = h.fake().first_index("session_handle_destroy");
  const int runtime = h.fake().first_index("runtime_handle_destroy");
  const int library = h.fake().first_index("library_destroy");
  const int unload = h.fake().first_index("library_unload");
  EXPECT_GE(peer, 0);
  EXPECT_LT(peer, session);
  EXPECT_LT(session, runtime);
  EXPECT_LT(runtime, library);
  EXPECT_LT(library, unload);
  EXPECT_FALSE(h.fake().library_loaded());
  EXPECT_TRUE(h.logged("QAROS: exit: owner finished in "));
  EXPECT_FALSE(h.logged("left running"));
}

TEST(SessionOwnerExit, TheExitDrainDropsTasksAfterItsBudget)
{
  Harness h;
  use_scaled_timings(h);
  h.config().timings.exit_join = 5s;   // the budget is for the drain here, not for the join
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::atomic<int> ran{0};
  std::atomic<int> dropped{0};
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(h.owner().post_session_task("slow", [&ran](QarSession * session) {
          query_once(session);
          std::this_thread::sleep_for(150ms);
          ran.fetch_add(1);
        }, [&dropped] { dropped.fetch_add(1); }));
  }
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(h.destroy_owner_within(10s));
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();
  EXPECT_EQ(ran.load() + dropped.load(), 8);
  EXPECT_GT(ran.load(), 0);
  EXPECT_GT(dropped.load(), 0) << "tasks whose turn came after 300 ms were dropped";
  EXPECT_LT(ms, 1200);
  EXPECT_TRUE(h.logged("drain budget 0.3 s spent"));
}

TEST(SessionOwnerExit, AnOwnerAfterAnAbandonedOneNeverUnloadsTheLibrary)
{
  Gate gate;
  Harness h;
  LeftBehindGuard guard(gate);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", gate);
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(gate.wait_entered());
  const int before = qar_qar_adapter::abandoned_owner_threads();
  const Destroyed first = destroy_within(h, gate, exit_limit(h));
  ASSERT_TRUE(first.in_time) << first.ms;
  ASSERT_EQ(qar_qar_adapter::abandoned_owner_threads(), before + 1);
  ASSERT_TRUE(h.fake().library_loaded());

  // The thread inside the library is still there: an owner that starts and ends now leaves the
  // library alone.
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  ASSERT_TRUE(h.destroy_owner_within(10s));
  EXPECT_EQ(h.fake().count("library_destroy"), 0);
  EXPECT_EQ(h.fake().count("library_unload"), 0);
  EXPECT_TRUE(h.fake().library_loaded());
}

TEST(SessionOwnerExit, TheLibraryIsDestroyedOnlyByTheLastOwner)
{
  Harness h;
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto second = std::make_unique<qar_qar_adapter::QarSessionOwner>(
    h.config(), h.fake().make_loader());
  second->connect();
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (second->state() != SessionState::kRunning && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_EQ(second->state(), SessionState::kRunning);

  ASSERT_TRUE(h.destroy_owner_within(10s));
  EXPECT_EQ(h.fake().count("library_destroy"), 0) << "the second owner still uses the library";
  EXPECT_TRUE(h.fake().library_loaded());
  second.reset();
  EXPECT_EQ(h.fake().count("library_destroy"), 1);
  EXPECT_EQ(h.fake().count("library_unload"), 1);
  EXPECT_FALSE(h.fake().library_loaded());
}

}  // namespace
