// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_QAR_STREAMING_MANAGER_HPP_
#define QAR_OGRE_STREAMING__OGRE_QAR_STREAMING_MANAGER_HPP_

#include <map>
#include <memory>
#include <vector>

#include "qar_ogre_streaming/clock.hpp"
#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/gl_fence_hook.hpp"
#include "qar_ogre_streaming/ogre_qar_render_scheduler.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/render_budget.hpp"
#include "qar_ogre_streaming/render_lock.hpp"
#include "qar_ogre_streaming/render_thread_executor.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

class OgreQarStereoStream;
class QarSceneLighting;

/// @brief Generic initialization inputs for the reusable library
///.
///
/// Deliberately free of any RViz/Qt/ament type. @c scene_manager may originate
/// from RViz2, a standalone Ogre app, or a test harness — the library does not
/// know or care. @c diagnostics may be null.
struct OgreQarStreamingConfig
{
  Ogre::SceneManager * scene_manager = nullptr;
  std::shared_ptr<IRenderThreadExecutor> render_thread_executor;
  std::shared_ptr<IQarRenderSenderFactory> qar_sender_factory;
  OgreQarDiagnosticsSink * diagnostics = nullptr;

  /// @brief Optional render-budget policy applied by the scheduler.
  RenderBudgetConfig render_budget{};

  /// @brief Optional host render lock wrapping the whole batch (§12.2). In RViz
  /// this forwards to DisplayContext::lockRender()/unlockRender(). Null => the
  /// scheduler renders without locking (single-threaded sample / headless tests).
  std::shared_ptr<IRenderLock> render_lock;

  /// @brief Optional monotonic clock for FPS pacing + timing. Null => the
  /// scheduler uses a default SteadyClock. Tests inject a fake clock.
  std::shared_ptr<IClock> clock;

  /// @brief Optional GL-fence seam for the batch's fence step (M7). Null => the
  /// fence step is a no-op.
  std::shared_ptr<IGlFenceHook> fence_hook;

  /// @brief Optional QAR lighting rig for the shared scene (M10-live). Applied when
  /// streaming is activated and shut down (restoring the host's lighting) when it is
  /// deactivated or the manager is destroyed. Null => the host's own lighting is
  /// used unchanged — which in RViz means a headlight glued to the LOCAL operator's
  /// camera, so every remote peer's shading swings around as the operator orbits.
  /// See qar_scene_lighting.hpp.
  std::shared_ptr<QarSceneLighting> scene_lighting;
};

/// @brief Top-level coordinator of the reusable Ogre<->Qar library.
///
/// Owns the render scheduler and all active per-peer stereo streams, and turns
/// high-level commands (start/stop streaming, add/remove peer stream) into
/// library operations. RViz-specific logic stays out of here.
///
/// M1 status: full construction-time validation and stream bookkeeping
/// (create/destroy/lookup), diagnostics snapshot assembly, and a render_tick()
/// that forwards to the scheduler. The pieces that actually render are stubs
/// returning Status::NotImplemented in the scheduler/stream classes.
class OgreQarStreamingManager
{
public:
  /// @brief Construct and validate.
  /// @throws std::invalid_argument if scene_manager, render_thread_executor or
  ///         qar_sender_factory is null (clear message identifies which).
  explicit OgreQarStreamingManager(const OgreQarStreamingConfig & config);
  ~OgreQarStreamingManager();

  OgreQarStreamingManager(const OgreQarStreamingManager &) = delete;
  OgreQarStreamingManager & operator=(const OgreQarStreamingManager &) = delete;

  /// @brief Mark streaming globally active/inactive (streams still created/
  /// destroyed individually). Affects whether render_tick() collects streams.
  ///
  /// Also applies (true) / restores (false) the optional scene-lighting rig, so the
  /// host's scene is only relit while we are actually streaming. Touches Ogre when a
  /// rig is configured — call it on the render thread.
  void set_streaming_active(bool active);
  bool streaming_active() const { return streaming_active_; }

  /// @brief The configured lighting rig (null when none was supplied).
  QarSceneLighting * scene_lighting() { return config_.scene_lighting.get(); }

  /// @brief Swap the sender factory used to create future per-peer senders.
  /// Intended for deferred/late binding of the live QAR C-API factory (M10:
  /// the RViz panel connects to the real QarOS runtime only on a user button,
  /// not at construction) — call it BEFORE any live stream is created. Existing
  /// streams keep the adapter they were built with. Must not be null.
  void set_sender_factory(std::shared_ptr<IQarRenderSenderFactory> factory);

  /// @brief Create a stereo stream for a peer.
  /// @return AlreadyExists if a stream for that peer exists; InvalidArgument if
  ///         @p config is invalid; InvalidState if the sender factory returns
  ///         null; otherwise Ok.
  Result create_stream(const StereoStreamConfig & config);

  /// @brief Destroy the stream for @p peer_id.
  /// @return NotFound if no such stream; otherwise Ok.
  Result destroy_stream(const QarPeerId & peer_id);

  bool has_stream(const QarPeerId & peer_id) const;
  std::size_t stream_count() const { return streams_.size(); }

  /// @brief Non-owning access to a stream (nullptr if absent).
  OgreQarStereoStream * get_stream(const QarPeerId & peer_id);

  /// @brief Run one batched render tick across all active streams. Forwards to
  /// the scheduler (which enforces render-thread affinity).
  Result render_tick();

  /// @brief Coherent copy of current diagnostics for all streams.
  DiagnosticsSnapshot diagnostics_snapshot() const;

  OgreQarRenderScheduler & scheduler() { return *scheduler_; }

private:
  void emit(
    DiagnosticSeverity severity,
    const std::string & category,
    const std::string & message,
    const std::string & peer_id_hex = {});

  OgreQarStreamingConfig config_;
  std::unique_ptr<OgreQarRenderScheduler> scheduler_;

  std::map<QarPeerId, std::unique_ptr<OgreQarStereoStream>, PeerIdLess> streams_;

  bool streaming_active_ = false;
  std::string last_error_;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_QAR_STREAMING_MANAGER_HPP_
