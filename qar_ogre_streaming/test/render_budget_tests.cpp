// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// RenderBudget policy tests. Pure arithmetic + per-peer state: the budget
// takes "now" as a parameter, so these are fully deterministic with a hand-driven
// clock and touch no Ogre.

#include <vector>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/render_budget.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;

namespace
{

qos::BudgetCandidate cand(uint8_t seed, uint32_t max_fps)
{
  return qos::BudgetCandidate{qos::test::make_peer_id(seed), max_fps};
}

bool contains(const std::vector<std::size_t> & v, std::size_t idx)
{
  for (std::size_t i : v) {
    if (i == idx) {
      return true;
    }
  }
  return false;
}

}  // namespace

// --- Raw FPS gate ----------------------------------------------------------

TEST(RenderBudget, IsFrameDueArithmetic)
{
  qos::RenderBudget b;
  // 60 FPS => 16.667 ms interval.
  EXPECT_FALSE(b.is_frame_due(60, 0.010, 0.0));  // 10 ms < 16.667
  EXPECT_TRUE(b.is_frame_due(60, 0.017, 0.0));   // 17 ms >= 16.667
  EXPECT_TRUE(b.is_frame_due(0, 0.0, 0.0));      // uncapped => always due
}

// --- Next-due accumulator paces to the target FPS without aliasing ---------

TEST(RenderBudget, NextDueAccumulatorAchievesTargetFps)
{
  qos::RenderBudgetConfig cfg;
  cfg.max_batch_milliseconds = 0.0;  // unlimited: isolate the FPS gate
  cfg.floor_fps = 0;                 // disable floor: everything is "optional"
  qos::RenderBudget b(cfg);

  const std::vector<qos::BudgetCandidate> cands{cand(1, 60)};
  const double tick_dt = 1.0 / 100.0;  // 100 Hz tick vs a 60 FPS target
  int renders = 0;
  for (int i = 0; i < 100; ++i) {  // ~1 second
    const double now = i * tick_dt;
    const qos::BudgetSelection sel = b.select(cands, now);
    if (contains(sel.to_render, 0)) {
      ++renders;
      b.note_rendered(qos::test::make_peer_id(1), now, 1.0, 60);
    }
  }
  // Accumulator form achieves ~60 FPS despite the 100 Hz tick not being an exact
  // multiple of the frame interval (a naive now+interval gate would alias to 50).
  EXPECT_GE(renders, 57);
  EXPECT_LE(renders, 61);
}

TEST(RenderBudget, UncappedRendersEveryTick)
{
  qos::RenderBudgetConfig cfg;
  cfg.max_batch_milliseconds = 0.0;
  qos::RenderBudget b(cfg);
  const std::vector<qos::BudgetCandidate> cands{cand(1, 0)};  // uncapped
  for (int i = 0; i < 10; ++i) {
    const double now = i * 0.01;
    const qos::BudgetSelection sel = b.select(cands, now);
    ASSERT_TRUE(contains(sel.to_render, 0));
    b.note_rendered(qos::test::make_peer_id(1), now, 1.0, 0);
  }
}

// --- Time-budget shedding, fairness rotation, and the 30 FPS floor ---------

TEST(RenderBudget, TimeBudgetShedsFairlyAndProtectsFloor)
{
  qos::RenderBudgetConfig cfg;
  cfg.max_batch_milliseconds = 12.0;  // only two 5 ms peers fit
  cfg.floor_fps = 30;                 // hard floor
  cfg.default_stream_cost_ms = 5.0;
  qos::RenderBudget b(cfg);

  const std::vector<qos::BudgetCandidate> cands{
    cand(1, 60), cand(2, 60), cand(3, 60), cand(4, 60)};

  // Tick 0: all first-sight => floor-protected "must render" => all 4 admitted
  // despite the budget (never let a fresh peer starve).
  {
    const qos::BudgetSelection sel = b.select(cands, 0.0);
    EXPECT_EQ(sel.to_render.size(), 4u);
    EXPECT_EQ(sel.skipped_due.size(), 0u);
    for (std::size_t i = 0; i < 4; ++i) {
      b.note_rendered(qos::test::make_peer_id(static_cast<uint8_t>(i + 1)), 0.0, 5.0, 60);
    }
  }

  // Tick @ 0.02 s: all due (next_due 0.0167), none starved (0.02 < 1/30),
  // so all "optional". Budget 12 ms admits only two 5 ms peers; the other two are
  // shed. Fair order (equal next_due) => registration order picks peers 1,2.
  {
    const qos::BudgetSelection sel = b.select(cands, 0.02);
    EXPECT_EQ(sel.to_render.size(), 2u);
    EXPECT_EQ(sel.skipped_due.size(), 2u);
    EXPECT_TRUE(contains(sel.to_render, 0));  // peer 1
    EXPECT_TRUE(contains(sel.to_render, 1));  // peer 2
    EXPECT_TRUE(contains(sel.skipped_due, 2));
    EXPECT_TRUE(contains(sel.skipped_due, 3));
    b.note_rendered(qos::test::make_peer_id(1), 0.02, 5.0, 60);
    b.note_rendered(qos::test::make_peer_id(2), 0.02, 5.0, 60);
  }

  // Tick @ 0.04 s: peers 3,4 have not rendered since 0.0 => 0.04 >= 1/30 =>
  // FLOOR-CRITICAL "must render" (admitted unconditionally). Peers 1,2 rendered at
  // 0.02 (0.02 < 1/30) => optional and now shed. => fair rotation: the shed peers
  // get their turn and never drop below the 30 FPS floor.
  {
    const qos::BudgetSelection sel = b.select(cands, 0.04);
    EXPECT_TRUE(contains(sel.to_render, 2));  // peer 3
    EXPECT_TRUE(contains(sel.to_render, 3));  // peer 4
    EXPECT_TRUE(contains(sel.skipped_due, 0));  // peer 1 shed
    EXPECT_TRUE(contains(sel.skipped_due, 1));  // peer 2 shed
  }

  EXPECT_EQ(b.skipped_frames(), 4u);  // 2 (tick .02) + 2 (tick .04)
}

TEST(RenderBudget, MaxStreamsPerTickCap)
{
  qos::RenderBudgetConfig cfg;
  cfg.max_batch_milliseconds = 0.0;  // unlimited time
  cfg.max_streams_per_tick = 2;      // but cap the count
  cfg.floor_fps = 0;                 // no floor override
  qos::RenderBudget b(cfg);

  const std::vector<qos::BudgetCandidate> cands{
    cand(1, 60), cand(2, 60), cand(3, 60)};
  const qos::BudgetSelection sel = b.select(cands, 1.0);
  EXPECT_EQ(sel.to_render.size(), 2u);
  EXPECT_EQ(sel.skipped_due.size(), 1u);
}

TEST(RenderBudget, ForgetDropsState)
{
  qos::RenderBudget b;
  const std::vector<qos::BudgetCandidate> cands{cand(1, 60)};
  b.select(cands, 0.0);
  b.note_rendered(qos::test::make_peer_id(1), 0.0, 1.0, 60);
  // After forget, the peer is first-sight again (due immediately at any now).
  b.forget(qos::test::make_peer_id(1));
  const qos::BudgetSelection sel = b.select(cands, 0.0001);
  EXPECT_TRUE(contains(sel.to_render, 0));
}
