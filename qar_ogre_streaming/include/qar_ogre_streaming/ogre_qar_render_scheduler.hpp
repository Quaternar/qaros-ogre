// Copyright 2026 QuaternAR
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
#include "qar_ogre_streaming/gpu_progress_gate.hpp"
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
/// Renders the due streams that the budget admits in ONE coherent batch per render tick,
/// on the render thread, under FPS/time budgets. It never renders from a Qar callback
/// thread — callbacks only enqueue via the IRenderThreadExecutor.
///
/// The render_tick() sequence is:
///   collect -> (early-out if none) -> lockRender -> begin all frames ->
///   update all cameras -> place per-peer lights -> render all targets ->
///   fence all textures (GL fence seam) -> submit all frames -> unlockRender
/// with a SINGLE render lock held around the WHOLE batch so every peer samples the
/// same scene snapshot.
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
  /// @param render_lock Optional host render lock (lockRender/unlockRender). Null
  ///                    => no locking (e.g. a single-threaded sample).
  /// @param clock       Optional monotonic clock; null => a default SteadyClock.
  /// @param fence_hook  Optional GL-fence seam; null => the fence step is a
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

  /// @brief Install the GL-fence hook (replaces the no-op seam).
  void set_fence_hook(std::shared_ptr<IGlFenceHook> fence_hook)
  {
    fence_hook_ = std::move(fence_hook);
  }

  /// @brief Install (or clear) the scene-lighting rig driven each batch.
  void set_scene_lighting(std::shared_ptr<QarSceneLighting> lighting)
  {
    lighting_ = std::move(lighting);
  }

  /// @brief Install (or clear) the GPU progress gate. While it is closed the scheduler renders no
  /// batch (the tick returns success, last_skip_reason() names why); after a batch it tells the
  /// gate that GL work was issued.
  void set_gpu_gate(std::shared_ptr<IGpuProgressGate> gate) { gpu_gate_ = std::move(gate); }

  /// @brief Why the last tick rendered nothing although streams were due: "GPU not progressing"
  /// while the gate was closed, empty otherwise.
  const char * last_skip_reason() const { return last_skip_reason_; }

  /// @brief How many batches were skipped because the gate was closed.
  uint64_t gate_skipped_batches() const { return gate_skipped_batches_; }

  /// @brief A batch that takes this long or longer (milliseconds, default 250) is reported once
  /// to the diagnostics sink with the time of each phase: begin, cameras (with the lights),
  /// render (with the fence step) and submit. The first five are reported, then one in a hundred.
  void set_slow_batch_ms(double milliseconds) { slow_batch_ms_ = milliseconds; }
  double slow_batch_ms() const { return slow_batch_ms_; }

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
  /// Rolling per-peer timing from which the achieved FPS is derived.
  struct PeerTiming
  {
    std::deque<double> submit_times;  ///< Recent submit timestamps (seconds).
  };

  void record_submit(const QarPeerId & peer_id, double now_seconds);
  void report_slow_batch(
    double batch_ms, double begin_ms, double cameras_ms, double render_ms, double submit_ms,
    std::size_t streams);

  std::shared_ptr<IRenderThreadExecutor> executor_;
  RenderBudget budget_;
  OgreQarDiagnosticsSink * diagnostics_;
  std::shared_ptr<IRenderLock> render_lock_;
  std::shared_ptr<IClock> clock_;
  std::shared_ptr<IGlFenceHook> fence_hook_;
  std::shared_ptr<QarSceneLighting> lighting_;
  std::shared_ptr<IGpuProgressGate> gpu_gate_;
  const char * last_skip_reason_ = "";
  uint64_t gate_skipped_batches_ = 0;

  std::map<QarPeerId, PeerTiming, PeerIdLess> timing_;

  uint64_t batches_rendered_ = 0;
  double last_batch_ms_ = 0.0;
  double avg_batch_ms_ = 0.0;
  std::deque<double> batch_ms_window_;  ///< Recent batch times for the rolling avg.
  uint32_t last_batch_streams_served_ = 0;
  uint32_t last_batch_streams_skipped_ = 0;
  double slow_batch_ms_ = 250.0;
  uint64_t slow_batches_ = 0;
  static constexpr uint64_t kSlowBatchReportFirst = 5;
  static constexpr uint64_t kSlowBatchReportEvery = 100;

  /// Rolling-window sizes (ticks). 120 ~= 2 s at 60 FPS.
  static constexpr std::size_t kBatchWindow = 120;
  static constexpr std::size_t kFpsWindow = 120;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_QAR_RENDER_SCHEDULER_HPP_
