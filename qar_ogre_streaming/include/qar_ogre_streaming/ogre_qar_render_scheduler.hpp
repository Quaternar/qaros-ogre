// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_QAR_RENDER_SCHEDULER_HPP_
#define QAR_OGRE_STREAMING__OGRE_QAR_RENDER_SCHEDULER_HPP_

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "qar_ogre_streaming/clock.hpp"
#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/gl_fence_hook.hpp"
#include "qar_ogre_streaming/render_budget.hpp"
#include "qar_ogre_streaming/render_lock.hpp"
#include "qar_ogre_streaming/render_thread_executor.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

class OgreQarStereoStream;
class QarSceneLighting;

/// @brief Central batched render scheduler.
///
/// Renders all active streams in ONE coherent batch per render tick, on the
/// render thread, under FPS/time budgets. It never renders from a Qar callback
/// thread — callbacks only enqueue via the IRenderThreadExecutor.
///
/// The canonical render_tick() sequence is implemented exactly:
///   collect -> (early-out if none) -> lockRender -> begin all frames ->
///   update all cameras -> place per-peer lights -> render all targets ->
///   fence all textures (M7 seam) -> submit all frames -> unlockRender
/// with a SINGLE render lock held around the WHOLE batch so every peer samples the
/// same scene snapshot (C2 / §25.2).
///
/// The lighting step sits between "update all cameras" and "render all targets"
/// deliberately: every peer's light must already be placed before ANY eye renders,
/// or peer A's frame would be lit by peer B's stale light position.
class OgreQarRenderScheduler
{
public:
  /// @param executor    Non-null render-thread executor.
  /// @param budget      Render-budget policy (copied).
  /// @param diagnostics Optional sink (may be null).
  /// @param render_lock Optional host render lock (§12.2 lockRender/unlockRender);
  ///                    null => no locking (e.g. a single-threaded sample).
  /// @param clock       Optional monotonic clock; null => a default SteadyClock.
  /// @param fence_hook  Optional GL-fence seam (M7); null => the fence step is a
  ///                    no-op.
  /// @param lighting    Optional QAR scene-lighting rig; null (or a rig in a mode
  ///                    without per-peer lights) => the lighting step is a no-op.
  /// @throws std::invalid_argument if @p executor is null.
  OgreQarRenderScheduler(
    std::shared_ptr<IRenderThreadExecutor> executor,
    const RenderBudgetConfig & budget,
    OgreQarDiagnosticsSink * diagnostics,
    std::shared_ptr<IRenderLock> render_lock = nullptr,
    std::shared_ptr<IClock> clock = nullptr,
    std::shared_ptr<IGlFenceHook> fence_hook = nullptr,
    std::shared_ptr<QarSceneLighting> lighting = nullptr);

  /// @brief Run one batched render tick over @p streams.
  ///
  /// Must be called on the render thread; if executor->is_render_thread() is
  /// false this returns Status::RenderThreadRequired without touching Ogre or the
  /// render lock. Collects the streams due this tick (FPS + time budget), and — if
  /// any — renders them all inside a single lockRender()/unlockRender() pair.
  /// @return Ok (including the empty/all-skipped tick); RenderThreadRequired off
  ///         the render thread; the first phase error otherwise.
  Result render_tick(const std::vector<OgreQarStereoStream *> & streams);

  /// @brief Install the M7 GL-fence hook (replaces the no-op seam).
  void set_fence_hook(std::shared_ptr<IGlFenceHook> fence_hook)
  {
    fence_hook_ = std::move(fence_hook);
  }

  /// @brief Install (or clear) the scene-lighting rig driven each batch.
  void set_scene_lighting(std::shared_ptr<QarSceneLighting> lighting)
  {
    lighting_ = std::move(lighting);
  }

  /// @brief Drop cached per-peer budget/timing state for a removed stream.
  void forget_stream(const QarPeerId & peer_id);

  RenderBudget & budget() { return budget_; }
  const RenderBudget & budget() const { return budget_; }

  uint64_t batches_rendered() const { return batches_rendered_; }
  double last_batch_ms() const { return last_batch_ms_; }
  double avg_batch_ms() const { return avg_batch_ms_; }
  uint32_t last_batch_streams_served() const { return last_batch_streams_served_; }
  uint32_t last_batch_streams_skipped() const { return last_batch_streams_skipped_; }

  /// @brief Derived achieved FPS for @p peer_id over the recent submit window
  /// (0 until at least two frames have been submitted).
  double achieved_fps(const QarPeerId & peer_id) const;

  /// @brief Achieved FPS from a window of submit timestamps (seconds): (n-1) frame
  /// intervals over the window span. Returns 0 for fewer than two samples or a
  /// non-positive span. Pure — exposed so the FPS math is unit-testable.
  static double rolling_fps(const std::vector<double> & submit_times);

private:
  /// Rolling per-peer timing used to derive achieved FPS.
  struct PeerTiming
  {
    std::deque<double> submit_times;  ///< Recent submit timestamps (seconds).
  };

  void record_submit(const QarPeerId & peer_id, double now_seconds);

  std::shared_ptr<IRenderThreadExecutor> executor_;
  RenderBudget budget_;
  OgreQarDiagnosticsSink * diagnostics_;
  std::shared_ptr<IRenderLock> render_lock_;
  std::shared_ptr<IClock> clock_;
  std::shared_ptr<IGlFenceHook> fence_hook_;
  std::shared_ptr<QarSceneLighting> lighting_;

  std::map<QarPeerId, PeerTiming, PeerIdLess> timing_;

  uint64_t batches_rendered_ = 0;
  double last_batch_ms_ = 0.0;
  double avg_batch_ms_ = 0.0;
  std::deque<double> batch_ms_window_;  ///< Recent batch times for the rolling avg.
  uint32_t last_batch_streams_served_ = 0;
  uint32_t last_batch_streams_skipped_ = 0;

  /// Rolling-window sizes (ticks). 120 ~= 2 s at 60 FPS.
  static constexpr std::size_t kBatchWindow = 120;
  static constexpr std::size_t kFpsWindow = 120;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_QAR_RENDER_SCHEDULER_HPP_
