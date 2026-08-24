// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/render_budget.hpp"

#include <algorithm>

namespace qar_ogre_streaming
{

RenderBudget::RenderBudget(const RenderBudgetConfig & config)
: config_(config)
{
}

double RenderBudget::frame_interval(uint32_t max_fps) const
{
  return max_fps == 0 ? 0.0 : 1.0 / static_cast<double>(max_fps);
}

bool RenderBudget::is_frame_due(
  uint32_t max_fps,
  double now_seconds,
  double last_render_seconds) const
{
  if (max_fps == 0) {
    return true;  // No FPS cap: always due.
  }
  return (now_seconds - last_render_seconds) >= frame_interval(max_fps);
}

RenderBudget::StreamState & RenderBudget::state_for(
  const QarPeerId & peer_id, double now_seconds)
{
  auto it = states_.find(peer_id);
  if (it == states_.end()) {
    StreamState st;
    st.rotation_seq = next_rotation_seq_++;
    st.next_due_seconds = now_seconds;   // due immediately on first sight
    st.last_render_seconds = -1.0e18;    // never rendered => floor-protected
    st.seen = true;
    it = states_.emplace(peer_id, st).first;
  }
  return it->second;
}

BudgetSelection RenderBudget::select(
  const std::vector<BudgetCandidate> & candidates, double now_seconds)
{
  BudgetSelection selection;

  const double floor_interval =
    config_.floor_fps == 0 ? 0.0 : 1.0 / static_cast<double>(config_.floor_fps);

  // Partition candidates into "must render" (floor-protected) and "optional",
  // dropping any not currently due. Keep the candidate index alongside so the
  // caller can map back to its stream.
  struct Due
  {
    std::size_t index;
    double next_due;
    uint64_t rotation_seq;
    double est_cost_ms;
    bool must;
  };
  std::vector<Due> due;
  due.reserve(candidates.size());

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const BudgetCandidate & c = candidates[i];
    StreamState & st = state_for(c.peer_id, now_seconds);

    const bool is_due =
      c.max_fps == 0 || now_seconds >= st.next_due_seconds;
    if (!is_due) {
      continue;  // Paced out this tick — NOT a skip, just not its turn.
    }

    const bool must =
      config_.floor_fps != 0 &&
      (now_seconds - st.last_render_seconds) >= floor_interval;
    const double est =
      st.last_cost_ms > 0.0 ? st.last_cost_ms : config_.default_stream_cost_ms;
    due.push_back(Due{i, st.next_due_seconds, st.rotation_seq, est, must});
  }

  // Fair order: most overdue first (smallest next_due), then registration order.
  std::sort(
    due.begin(), due.end(), [](const Due & a, const Due & b) {
      if (a.next_due != b.next_due) {
        return a.next_due < b.next_due;
      }
      return a.rotation_seq < b.rotation_seq;
    });

  double accum_ms = 0.0;
  std::size_t admitted = 0;
  const bool cap_streams = config_.max_streams_per_tick != 0;
  const bool cap_time = config_.max_batch_milliseconds > 0.0;

  // Pass 1: admit all floor-protected "must render" peers unconditionally (they
  // would otherwise drop below the hard FPS floor). They still count toward the
  // accumulated cost so optional admission accounts for them.
  for (const Due & d : due) {
    if (!d.must) {
      continue;
    }
    selection.to_render.push_back(d.index);
    accum_ms += d.est_cost_ms;
    ++admitted;
  }

  // Pass 2: admit optional (target-FPS) peers greedily in fair order while within
  // the per-tick stream + time budget; shed the rest (counted as skipped frames).
  for (const Due & d : due) {
    if (d.must) {
      continue;
    }
    const bool stream_cap_ok = !cap_streams || admitted < config_.max_streams_per_tick;
    const bool time_ok = !cap_time || (accum_ms + d.est_cost_ms) <= config_.max_batch_milliseconds;
    if (stream_cap_ok && time_ok) {
      selection.to_render.push_back(d.index);
      accum_ms += d.est_cost_ms;
      ++admitted;
    } else {
      selection.skipped_due.push_back(d.index);
      ++skipped_frames_;
    }
  }

  // to_render is now: [must-render, in fair order] ++ [admitted optional, in fair
  // order]. Floor-critical peers therefore render ahead of target-FPS ones, which
  // is the intended priority under load.
  return selection;
}

void RenderBudget::note_rendered(
  const QarPeerId & peer_id, double now_seconds, double cost_ms, uint32_t max_fps)
{
  StreamState & st = state_for(peer_id, now_seconds);
  st.last_render_seconds = now_seconds;
  st.last_cost_ms = cost_ms;
  // Advance the next-due accumulator by one frame interval. Using the accumulator
  // form (next_due + interval) rather than (now + interval) achieves the target
  // FPS on average despite discrete tick granularity; the max(…, now) clamp stops
  // a peer that has fallen more than one interval behind from bursting to catch
  // up. For an uncapped peer (max_fps == 0) next_due collapses to now => due every
  // tick.
  const double interval = frame_interval(max_fps);
  st.next_due_seconds = std::max(st.next_due_seconds + interval, now_seconds);
}

void RenderBudget::forget(const QarPeerId & peer_id)
{
  states_.erase(peer_id);
}

}  // namespace qar_ogre_streaming
