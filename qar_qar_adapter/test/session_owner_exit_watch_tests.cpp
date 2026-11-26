// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The exit of the session owner never waits without a bound for the supervisor thread: it is held
// inside the hint source at a gate, and the destructor must still return within exit_join plus a
// margin, on the path that leaves the owner thread behind and on the normal path.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "outage_test_kit.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using qar_qar_adapter::HubHints;
using qar_qar_adapter::test::Gate;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::LeftBehindGuard;
using qar_qar_adapter::test::hold_first_call;
using qar_qar_adapter::test::use_scaled_timings;
using namespace std::chrono_literals;

void query_once(QarSession * session)
{
  std::size_t count = 0;
  (void)QAR_CALL(qar_query_app_volumes_count, session, &count);
}

struct Result
{
  bool in_time = false;
  long long ms = -1;
};

// Destroys the owner on a thread of its own; when that takes longer than @p limit the gates open,
// so that the test fails instead of hanging.
Result destroy_within(Harness & h, Gate & a, Gate & b, std::chrono::milliseconds limit)
{
  auto done = std::make_shared<std::promise<long long>>();
  std::future<long long> result = done->get_future();
  std::thread worker([&h, done] {
      const auto start = std::chrono::steady_clock::now();
      h.destroy_owner_within(60s);
      done->set_value(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
    });
  Result out;
  if (result.wait_for(limit) == std::future_status::ready) {
    out.in_time = true;
    out.ms = result.get();
  } else {
    a.open();
    b.open();
    out.ms = result.get();
  }
  worker.join();
  return out;
}

// The hint source answers at once until `stuck` is set; then it waits at the gate. `inside`
// counts the threads that are in the call, so that a test can wait for the detached one.
struct StuckHints
{
  Gate gate;
  std::atomic<bool> stuck{false};
  std::atomic<int> inside{0};
};

void install_stuck_hints(Harness & h, const std::shared_ptr<StuckHints> & hints)
{
  h.config().watch_hub_hints = true;
  h.config().hint_source = [hints] {
      hints->inside.fetch_add(1);
      if (hints->stuck.load()) {
        hints->gate.wait();
      }
      hints->inside.fetch_sub(1);
      return HubHints{};
    };
}

// Opens the gates and waits until the supervisor thread that was left behind has left the hint
// source, so that nothing touches the test's state after it ends.
void release_stuck(Gate & call_gate, const std::shared_ptr<StuckHints> & hints)
{
  call_gate.open();
  hints->gate.open();
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (hints->inside.load() > 0 && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(2ms);
  }
  std::this_thread::sleep_for(50ms);
}

TEST(SessionOwnerExitWatch, AbandonWithAStuckWatchThreadStaysWithinTheBudget)
{
  Gate call_gate;
  auto hints = std::make_shared<StuckHints>();
  Harness h;
  LeftBehindGuard guard(call_gate, hints->gate);
  use_scaled_timings(h);
  install_stuck_hints(h, hints);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hold_first_call(h.fake(), "query_app_volumes_count", call_gate);
  ASSERT_TRUE(h.owner().post_session_task("never-returns", &query_once));
  ASSERT_TRUE(call_gate.wait_entered());
  hints->stuck.store(true);
  ASSERT_TRUE(hints->gate.wait_entered());
  const auto limit =
    std::chrono::duration_cast<std::chrono::milliseconds>(h.config().timings.exit_join) + 400ms;
  const Result r = destroy_within(h, call_gate, hints->gate, limit);
  EXPECT_TRUE(r.in_time) << "the destructor took " << r.ms << " ms (limit " << limit.count()
                         << ")";
  EXPECT_TRUE(h.logged("QAROS: exit: owner thread left running after "));
  release_stuck(call_gate, hints);
}

TEST(SessionOwnerExitWatch, ANormalExitWithAStuckWatchThreadStaysWithinTheBudget)
{
  Gate call_gate;
  auto hints = std::make_shared<StuckHints>();
  Harness h;
  LeftBehindGuard guard(call_gate, hints->gate);
  use_scaled_timings(h);
  install_stuck_hints(h, hints);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  hints->stuck.store(true);
  ASSERT_TRUE(hints->gate.wait_entered());
  const auto limit =
    std::chrono::duration_cast<std::chrono::milliseconds>(h.config().timings.exit_join) + 400ms;
  const Result r = destroy_within(h, call_gate, hints->gate, limit);
  EXPECT_TRUE(r.in_time) << "the destructor took " << r.ms << " ms (limit " << limit.count()
                         << ")";
  EXPECT_TRUE(h.logged("QAROS: exit: a thread of the owner did not leave the process"));
  EXPECT_TRUE(h.logged("supervisor still there"));
  EXPECT_FALSE(h.logged("QAROS: exit: owner finished in "));
  release_stuck(call_gate, hints);
}

TEST(SessionOwnerExitWatch, AHealthyExitJoinsBothThreadsAndLogsTheOldLine)
{
  Gate call_gate;
  Gate unused;
  Harness h;
  LeftBehindGuard guard(call_gate, unused);
  use_scaled_timings(h);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  const Result r = destroy_within(h, call_gate, unused, 2000ms);
  EXPECT_TRUE(r.in_time);
  EXPECT_LT(r.ms, 500) << "nothing waits for the budget when both threads end at once";
  EXPECT_TRUE(h.logged("QAROS: exit: owner finished in "));
  EXPECT_FALSE(h.logged("did not leave the process"));
}

}  // namespace
