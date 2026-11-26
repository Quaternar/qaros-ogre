// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_qar_render_scheduler.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/qar_scene_lighting.hpp"

namespace qar_ogre_streaming
{

OgreQarRenderScheduler::OgreQarRenderScheduler(
  std::shared_ptr<IRenderThreadExecutor> executor,
  const RenderBudgetConfig & budget,
  OgreQarDiagnosticsSink * diagnostics,
  std::shared_ptr<IRenderLock> render_lock,
  std::shared_ptr<IClock> clock,
  std::shared_ptr<IGlFenceHook> fence_hook,
  std::shared_ptr<QarSceneLighting> lighting)
: executor_(std::move(executor)),
  budget_(budget),
  diagnostics_(diagnostics),
  render_lock_(std::move(render_lock)),
  clock_(std::move(clock)),
  fence_hook_(std::move(fence_hook)),
  lighting_(std::move(lighting))
{
  if (executor_ == nullptr) {
    throw std::invalid_argument("OgreQarRenderScheduler: executor must not be null");
  }
  if (clock_ == nullptr) {
    clock_ = std::make_shared<SteadyClock>();
  }
}

Result OgreQarRenderScheduler::render_tick(
  const std::vector<OgreQarStereoStream *> & streams)
{
  // Thread-affinity guard: rendering must happen on the render thread. A Qar
  // callback thread that reaches here is a programming error; refuse rather than
  // touch Ogre / the render lock from the wrong thread.
  if (!executor_->is_render_thread()) {
    if (diagnostics_ != nullptr) {
      diagnostics_->on_message(
        DiagnosticSeverity::Error, "scheduler",
        "render_tick called off the render thread; refusing to render", "");
    }
    return Result::error(
      Status::RenderThreadRequired, "render_tick must be called on the render thread");
  }

  const double now = clock_->now_seconds();
  last_skip_reason_ = "";

  // Step 1 (collect): budget FPS gate + time-budget shedding. Pure; no Ogre.
  // `active` is the filtered, ordered set of collectable streams; candidate index
  // i maps back to active[i].
  std::vector<OgreQarStereoStream *> active;
  active.reserve(streams.size());
  std::vector<BudgetCandidate> candidates;
  candidates.reserve(streams.size());
  for (OgreQarStereoStream * s : streams) {
    if (s != nullptr && s->active()) {
      active.push_back(s);
      candidates.push_back(BudgetCandidate{s->peer_id(), s->config().max_fps});
    }
  }
  const BudgetSelection selection = budget_.select(candidates, now);

  // Account budget-shed (due but dropped) frames against their streams.
  for (std::size_t idx : selection.skipped_due) {
    if (idx < active.size()) {
      active[idx]->note_budget_skip();
    }
  }
  last_batch_streams_skipped_ = static_cast<uint32_t>(selection.skipped_due.size());

  // Step 1b (early-out): nothing due => a valid, complete, lock-free tick.
  if (selection.to_render.empty()) {
    last_batch_streams_served_ = 0;
    return Result::success();
  }

  // Resolve the selected candidate indices back to their streams, in fair order.
  std::vector<OgreQarStereoStream *> due;
  due.reserve(selection.to_render.size());
  for (std::size_t idx : selection.to_render) {
    if (idx < active.size()) {
      due.push_back(active[idx]);
    }
  }

  // Step 2 (GPU gate): while the GPU makes no progress, the batch would issue GL work that the
  // render thread can end up waiting for. The streams stay due; the next tick asks again.
  if (gpu_gate_ != nullptr && gpu_gate_->gate_closed()) {
    last_skip_reason_ = "GPU not progressing";
    last_batch_streams_served_ = 0;
    ++gate_skipped_batches_;
    gpu_gate_->note_batch_skipped();
    return Result::success();
  }

  // --- ONE render lock around the WHOLE batch --------------------------------
  const double batch_start = clock_->now_seconds();
  if (render_lock_ != nullptr) {
    render_lock_->lock_render();
  }

  // Step 3 (begin all Qar frames): everyone samples the SAME scene snapshot.
  const double begin_start = clock_->now_seconds();
  std::vector<OgreQarStereoStream *> rendering;
  rendering.reserve(due.size());
  for (OgreQarStereoStream * s : due) {
    if (s->begin_frame_phase()) {
      rendering.push_back(s);
    }
  }

  Result first_error = Result::success();
  const double cameras_start = clock_->now_seconds();

  // Step 5 (update all cameras).
  for (OgreQarStereoStream * s : rendering) {
    const Result r = s->update_cameras_phase();
    if (!r.ok() && first_error.ok()) {
      first_error = r;
    }
  }
  // Step 5b (place per-peer lights): AFTER every camera is placed and BEFORE any eye
  // renders, so no peer's frame is lit by another peer's stale light position. A rig
  // without per-peer lights (or none at all) makes this free.
  if (lighting_ != nullptr) {
    std::vector<PeerHeadLight> heads;
    heads.reserve(rendering.size());
    for (OgreQarStereoStream * s : rendering) {
      PeerHeadLight head;
      head.peer_id = s->peer_id();
      if (s->current_head_position(head.position)) {
        heads.push_back(head);
      }
    }
    lighting_->update_peer_heads(heads);
  }

  // Step 6 (render all offscreen targets back-to-back).
  const double render_start = clock_->now_seconds();
  for (OgreQarStereoStream * s : rendering) {
    const Result r = s->render_targets_phase();
    if (!r.ok() && first_error.ok()) {
      first_error = r;
    }
  }
  // The first frame of a stream that leaves screen-space objects of the scene out of
  // the 3D stream is announced once; the host sees why such an object is not in AR.
  if (diagnostics_ != nullptr) {
    for (OgreQarStereoStream * s : rendering) {
      const uint64_t left_out = s->take_left_out_report();
      if (left_out > 0u) {
        diagnostics_->on_message(
          DiagnosticSeverity::Info, "render",
          "AR render: " + std::to_string(left_out) +
          " screen-space object(s) of the RViz scene (such as the selection box) are left "
          "out of the 3D stream",
          peer_id_to_hex(s->peer_id()));
      }
    }
  }
  // Step 7 (fence all textures): a no-op unless a fence hook is installed.
  for (OgreQarStereoStream * s : rendering) {
    s->fence_phase(fence_hook_.get());
  }
  // Step 8 (submit all frames).
  const double submit_start = clock_->now_seconds();
  uint32_t served = 0;
  for (OgreQarStereoStream * s : rendering) {
    if (!s->wants_frame_this_tick()) {
      continue;  // a phase cancelled this stream's frame (error above)
    }
    const Result r = s->submit_phase();
    if (!r.ok()) {
      if (first_error.ok()) {
        first_error = r;
      }
      continue;
    }
    ++served;
    budget_.note_rendered(s->peer_id(), now, s->last_render_ms(), s->config().max_fps);
    record_submit(s->peer_id(), now);
  }

  const double submit_end = clock_->now_seconds();
  if (render_lock_ != nullptr) {
    render_lock_->unlock_render();
  }
  // The batch issued GL work: the gate follows how far the GPU gets with it.
  if (gpu_gate_ != nullptr && !rendering.empty()) {
    gpu_gate_->note_batch_issued();
  }

  // --- Timing + accounting (outside the lock) -------------------------------
  const double batch_ms = (clock_->now_seconds() - batch_start) * 1000.0;
  if (batch_ms >= slow_batch_ms_) {
    report_slow_batch(
      batch_ms, (cameras_start - begin_start) * 1000.0, (render_start - cameras_start) * 1000.0,
      (submit_start - render_start) * 1000.0, (submit_end - submit_start) * 1000.0, due.size());
  }
  last_batch_ms_ = batch_ms;
  last_batch_streams_served_ = served;
  ++batches_rendered_;

  batch_ms_window_.push_back(batch_ms);
  while (batch_ms_window_.size() > kBatchWindow) {
    batch_ms_window_.pop_front();
  }
  double sum = 0.0;
  for (double v : batch_ms_window_) {
    sum += v;
  }
  avg_batch_ms_ = batch_ms_window_.empty() ? 0.0 : sum / static_cast<double>(batch_ms_window_.size());

  return first_error;
}

void OgreQarRenderScheduler::report_slow_batch(
  double batch_ms, double begin_ms, double cameras_ms, double render_ms, double submit_ms,
  std::size_t streams)
{
  // The first few are reported, then one in a hundred, so a scene that is slow on every batch
  // does not fill the log.
  const uint64_t n = ++slow_batches_;
  if (diagnostics_ == nullptr || (n > kSlowBatchReportFirst && (n % kSlowBatchReportEvery) != 0)) {
    return;
  }
  // The cameras phase includes the placing of the per-peer lights, the render phase the fence step.
  char text[192];
  std::snprintf(
    text, sizeof(text),
    "render batch took %.0f ms: begin %.0f ms, cameras %.0f ms, render %.0f ms, submit %.0f ms "
    "(%zu streams)",
    batch_ms, begin_ms, cameras_ms, render_ms, submit_ms, streams);
  diagnostics_->on_message(DiagnosticSeverity::Warning, "scheduler", text, "");
}

void OgreQarRenderScheduler::record_submit(const QarPeerId & peer_id, double now_seconds)
{
  PeerTiming & t = timing_[peer_id];
  t.submit_times.push_back(now_seconds);
  while (t.submit_times.size() > kFpsWindow) {
    t.submit_times.pop_front();
  }
}

double OgreQarRenderScheduler::rolling_fps(const std::vector<double> & submit_times)
{
  if (submit_times.size() < 2) {
    return 0.0;
  }
  const double span = submit_times.back() - submit_times.front();
  if (span <= 0.0) {
    return 0.0;
  }
  // (n-1) intervals over the window span.
  return static_cast<double>(submit_times.size() - 1) / span;
}

double OgreQarRenderScheduler::achieved_fps(const QarPeerId & peer_id) const
{
  auto it = timing_.find(peer_id);
  if (it == timing_.end()) {
    return 0.0;
  }
  const std::deque<double> & ts = it->second.submit_times;
  return rolling_fps(std::vector<double>(ts.begin(), ts.end()));
}

void OgreQarRenderScheduler::forget_stream(const QarPeerId & peer_id)
{
  budget_.forget(peer_id);
  timing_.erase(peer_id);
}

}  // namespace qar_ogre_streaming
