// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/qar_scene_lighting.hpp"

namespace qar_ogre_streaming
{

OgreQarStreamingManager::OgreQarStreamingManager(const OgreQarStreamingConfig & config)
: config_(config)
{
  if (config_.scene_manager == nullptr) {
    throw std::invalid_argument(
      "OgreQarStreamingConfig.scene_manager must not be null");
  }
  if (config_.render_thread_executor == nullptr) {
    throw std::invalid_argument(
      "OgreQarStreamingConfig.render_thread_executor must not be null");
  }
  if (config_.qar_sender_factory == nullptr) {
    throw std::invalid_argument(
      "OgreQarStreamingConfig.qar_sender_factory must not be null");
  }

  scheduler_ = std::make_unique<OgreQarRenderScheduler>(
    config_.render_thread_executor, config_.render_budget, config_.diagnostics,
    config_.render_lock, config_.clock, config_.fence_hook, config_.scene_lighting);

  emit(DiagnosticSeverity::Info, "manager", "OgreQarStreamingManager constructed");
}

OgreQarStreamingManager::~OgreQarStreamingManager()
{
  // Put the host's lighting back before anything else unwinds; the rig holds a raw
  // Ogre::SceneManager pointer that outlives us, so this must not be left to chance.
  if (config_.scene_lighting != nullptr) {
    config_.scene_lighting->shutdown();
  }
}

void OgreQarStreamingManager::set_streaming_active(bool active)
{
  streaming_active_ = active;

  // Relight the shared scene only while we are actually streaming, and hand the host
  // its own lighting back the moment we stop.
  if (config_.scene_lighting != nullptr) {
    if (active) {
      const Result lit = config_.scene_lighting->apply();
      if (lit.ok()) {
        if (config_.scene_lighting->applied()) {
          emit(
            DiagnosticSeverity::Info, "lighting",
            std::string("scene lighting applied (mode=") +
            to_string(config_.scene_lighting->config().mode) + ")");
        }
      } else {
        last_error_ = lit.message;
        emit(DiagnosticSeverity::Warning, "lighting", lit.message);
      }
    } else {
      config_.scene_lighting->shutdown();
    }
  }

  emit(
    DiagnosticSeverity::Info, "manager",
    active ? "streaming activated" : "streaming deactivated");
}

void OgreQarStreamingManager::set_sender_factory(
  std::shared_ptr<IQarRenderSenderFactory> factory)
{
  if (factory == nullptr) {
    throw std::invalid_argument("set_sender_factory: factory must not be null");
  }
  config_.qar_sender_factory = std::move(factory);
  emit(DiagnosticSeverity::Info, "manager", "sender factory swapped");
}

Result OgreQarStreamingManager::create_stream(const StereoStreamConfig & config)
{
  const Result valid = config.validate();
  if (!valid) {
    last_error_ = valid.message;
    emit(DiagnosticSeverity::Error, "manager", valid.message, peer_id_to_hex(config.peer_id));
    return valid;
  }

  if (streams_.find(config.peer_id) != streams_.end()) {
    const std::string msg = "stream already exists for peer";
    last_error_ = msg;
    emit(DiagnosticSeverity::Warning, "manager", msg, peer_id_to_hex(config.peer_id));
    return Result::error(Status::AlreadyExists, msg);
  }

  std::shared_ptr<IQarRenderSenderAdapter> sender =
    config_.qar_sender_factory->create_sender(config.peer_id);
  if (sender == nullptr) {
    const std::string msg = "sender factory returned null for peer";
    last_error_ = msg;
    emit(DiagnosticSeverity::Error, "manager", msg, peer_id_to_hex(config.peer_id));
    return Result::error(Status::InvalidState, msg);
  }

  auto stream = std::make_unique<OgreQarStereoStream>(
    config_.scene_manager, config, std::move(sender));
  // Apply the current live-switch settings to the brand-new stream's sender BEFORE
  // it renders anything, so a peer added after a panel toggle does not briefly run
  // with the pre-toggle behavior.
  stream->set_prime_enabled(prime_enabled_);
  stream->set_tick_ms(tick_ms_);
  OgreQarStereoStream * const stream_ptr = stream.get();
  streams_.emplace(config.peer_id, std::move(stream));

  emit(DiagnosticSeverity::Info, "manager", "stream created", peer_id_to_hex(config.peer_id));
  // If this stream's adapter opened a per-frame trace file (QAR_ADAPTER_FRAME_TRACE),
  // surface its path through the same diagnostics sink the panel already logs —
  // printing it once, right when the stream that will write it comes up.
  const std::string trace_path = stream_ptr->frame_trace_path();
  if (!trace_path.empty()) {
    emit(
      DiagnosticSeverity::Info, "manager", "frame trace: " + trace_path,
      peer_id_to_hex(config.peer_id));
  }
  return Result::success();
}

void OgreQarStreamingManager::set_prime_enabled(bool enabled)
{
  prime_enabled_ = enabled;
  for (auto & entry : streams_) {
    entry.second->set_prime_enabled(enabled);
  }
  emit(
    DiagnosticSeverity::Info, "manager",
    std::string("prime_enabled=") + (enabled ? "1" : "0"));
}

void OgreQarStreamingManager::set_tick_ms(double tick_ms)
{
  tick_ms_ = tick_ms;
  for (auto & entry : streams_) {
    entry.second->set_tick_ms(tick_ms);
  }
}

Result OgreQarStreamingManager::destroy_stream(const QarPeerId & peer_id)
{
  auto it = streams_.find(peer_id);
  if (it == streams_.end()) {
    return Result::error(Status::NotFound, "no stream for peer");
  }
  streams_.erase(it);
  scheduler_->forget_stream(peer_id);  // drop cached budget/timing state
  if (config_.scene_lighting != nullptr) {
    config_.scene_lighting->forget_peer(peer_id);  // remove that peer's head light
  }
  emit(DiagnosticSeverity::Info, "manager", "stream destroyed", peer_id_to_hex(peer_id));
  return Result::success();
}

bool OgreQarStreamingManager::has_stream(const QarPeerId & peer_id) const
{
  return streams_.find(peer_id) != streams_.end();
}

OgreQarStereoStream * OgreQarStreamingManager::get_stream(const QarPeerId & peer_id)
{
  auto it = streams_.find(peer_id);
  return it == streams_.end() ? nullptr : it->second.get();
}

Result OgreQarStreamingManager::render_tick()
{
  if (!streaming_active_) {
    return Result::success();  // Inactive: a valid, empty tick.
  }
  std::vector<OgreQarStereoStream *> active;
  active.reserve(streams_.size());
  for (auto & entry : streams_) {
    if (entry.second->active()) {
      active.push_back(entry.second.get());
    }
  }
  return scheduler_->render_tick(active);
}

DiagnosticsSnapshot OgreQarStreamingManager::diagnostics_snapshot() const
{
  DiagnosticsSnapshot snap;
  snap.streaming_active = streaming_active_;
  snap.stream_count = static_cast<uint32_t>(streams_.size());
  snap.batches_rendered = scheduler_->batches_rendered();
  snap.last_batch_ms = scheduler_->last_batch_ms();
  snap.avg_batch_ms = scheduler_->avg_batch_ms();
  snap.last_batch_streams_served = scheduler_->last_batch_streams_served();
  snap.last_batch_streams_skipped = scheduler_->last_batch_streams_skipped();
  snap.budget_skipped_frames = scheduler_->budget().skipped_frames();
  snap.last_error = last_error_;
  snap.streams.reserve(streams_.size());
  for (const auto & entry : streams_) {
    StreamDiagnostics d = entry.second->diagnostics();
    // The scheduler owns the tick clock, so it derives the achieved FPS.
    d.achieved_fps = scheduler_->achieved_fps(entry.first);
    snap.streams.push_back(d);
  }
  return snap;
}

void OgreQarStreamingManager::emit(
  DiagnosticSeverity severity,
  const std::string & category,
  const std::string & message,
  const std::string & peer_id_hex)
{
  if (config_.diagnostics != nullptr) {
    config_.diagnostics->on_message(severity, category, message, peer_id_hex);
  }
}

}  // namespace qar_ogre_streaming
