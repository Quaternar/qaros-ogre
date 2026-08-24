// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__RENDER_BUDGET_HPP_
#define QAR_OGRE_STREAMING__RENDER_BUDGET_HPP_

#include <cstdint>
#include <map>
#include <vector>

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Global render-budget policy applied per render tick.
struct RenderBudgetConfig
{
  /// @brief Soft wall-clock budget for one batch (ALL peers), in milliseconds.
  /// Optional per-peer frames beyond this are shed this tick. 0 = unlimited.
  /// Default ~16 ms (a 60 FPS batch budget); per-eye render measured ~1 ms so
  /// 3-4 peers fit comfortably and up to 8 peers stay near the budget.
  double max_batch_milliseconds = 16.0;

  /// @brief Hard cap on peers rendered per tick. 0 = unlimited.
  uint32_t max_streams_per_tick = 0;

  /// @brief Per-peer HARD FPS floor (C8): a peer that has not rendered within
  /// 1/floor_fps seconds is ALWAYS admitted (never shed by the time budget), so
  /// an active peer never drops below this rate while headroom exists. 0 disables
  /// floor protection.
  uint32_t floor_fps = 30;

  /// @brief Estimated per-peer batch cost (ms) used for time-budget admission
  /// before that peer has ever been measured. Replaced by the real measured cost
  /// after the first render (~2 ms = two ~1 ms eye renders).
  double default_stream_cost_ms = 2.0;
};

/// @brief A stream offered to the budget for this tick (peer id + its FPS target).
struct BudgetCandidate
{
  QarPeerId peer_id{};
  uint32_t max_fps = 0;  ///< Per-peer target FPS (0 = uncapped / render every tick).
};

/// @brief The budget's decision for one tick.
struct BudgetSelection
{
  /// @brief Indices into the candidate list to render this tick, in render order
  /// (fair: most-overdue first, then registration order).
  std::vector<std::size_t> to_render;
  /// @brief Indices of candidates that WERE due but were shed to stay within the
  /// per-tick time/stream budget (accounted as skipped frames).
  std::vector<std::size_t> skipped_due;
};

/// @brief Decides which streams may render this tick under FPS + time budgets
///.
///
/// Stateful per peer (next-due timestamp, last-render time, measured cost,
/// registration order) but with NO wall-clock of its own: the caller passes the
/// current time in seconds so the whole policy is deterministically unit-testable
/// with a fake clock.
///
/// ## FPS pacing (next-due accumulator)
/// Each peer carries a @c next_due time. A peer is "due" when `now >= next_due`.
/// On render, `next_due = max(next_due + 1/max_fps, now)` — the accumulator form
/// achieves the target FPS on average even when the tick rate is not an exact
/// multiple of the frame interval (no beat/aliasing), while the `max(…, now)`
/// clamp prevents a burst of catch-up frames after a stall.
///
/// ## Fair shedding + 30 FPS floor (C8)
/// Due peers are ordered by (next_due ascending, registration order) so the most
/// overdue peer renders first. A due peer that has not rendered within
/// 1/floor_fps ("must render") is admitted unconditionally (floor protection);
/// the remaining "optional" due peers are admitted greedily in fair order while
/// the accumulated estimated cost stays within @c max_batch_milliseconds and
/// @c max_streams_per_tick. Optional peers that do not fit are shed this tick and
/// counted as skipped frames.
class RenderBudget
{
public:
  RenderBudget() = default;
  explicit RenderBudget(const RenderBudgetConfig & config);

  const RenderBudgetConfig & config() const { return config_; }
  void set_config(const RenderBudgetConfig & config) { config_ = config; }

  /// @brief Would a stream at @p max_fps be due at @p now_seconds given it last
  /// rendered at @p last_render_seconds? Pure arithmetic; @p max_fps == 0 => always
  /// due. Retained for callers that want the raw gate.
  bool is_frame_due(
    uint32_t max_fps,
    double now_seconds,
    double last_render_seconds) const;

  /// @brief Choose which of @p candidates render this tick (see class docs).
  /// Creates per-peer state on first sight and increments the skipped-frame
  /// counter for due-but-shed candidates. Does NOT advance any peer's schedule —
  /// call @ref note_rendered after a candidate is actually rendered.
  BudgetSelection select(
    const std::vector<BudgetCandidate> & candidates, double now_seconds);

  /// @brief Commit a peer's render: advance its next-due accumulator (by
  /// 1/@p max_fps, clamped to @p now_seconds to avoid catch-up bursts), record its
  /// render time and measured batch cost (ms). Called once per rendered stream per
  /// tick, after it actually rendered.
  void note_rendered(
    const QarPeerId & peer_id, double now_seconds, double cost_ms, uint32_t max_fps);

  /// @brief Drop per-peer state for a removed stream (keeps the map bounded).
  void forget(const QarPeerId & peer_id);

  /// @brief Global count of frames skipped to stay within budget.
  uint64_t skipped_frames() const { return skipped_frames_; }
  void note_skipped_frame() { ++skipped_frames_; }

private:
  struct StreamState
  {
    double next_due_seconds = 0.0;         ///< Due when now >= this.
    double last_render_seconds = -1.0e18;  ///< Sentinel: never rendered.
    double last_cost_ms = 0.0;             ///< Last measured batch cost.
    uint64_t rotation_seq = 0;             ///< Registration order (fairness tiebreak).
    bool seen = false;                     ///< next_due initialised on first sight.
  };

  StreamState & state_for(const QarPeerId & peer_id, double now_seconds);
  double frame_interval(uint32_t max_fps) const;

  RenderBudgetConfig config_{};
  std::map<QarPeerId, StreamState, PeerIdLess> states_;
  uint64_t next_rotation_seq_ = 0;
  uint64_t skipped_frames_ = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__RENDER_BUDGET_HPP_
