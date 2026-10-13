// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_QAR_STEREO_STREAM_HPP_
#define QAR_OGRE_STREAMING__OGRE_QAR_STEREO_STREAM_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/gl_fence_hook.hpp"
#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"
#include "qar_ogre_streaming/qar_pose_conversion.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief GL texture target enum value for a 2D texture (GL_TEXTURE_2D). Defined
/// here (not via a GL header) because the reusable library must not depend on a
/// GL loader; the value is stable across GL versions.
constexpr uint32_t kGlTexture2D = 0x0DE1;

/// @brief The converted camera inputs for one eye, plus validity flags.
///
/// A pure (Ogre-free) value produced by @ref compute_eye_render_params so the
/// per-eye pose/FOV conversion can be unit-tested headlessly; @ref
/// OgreQarStereoStream applies it to a real Ogre::Camera on the render thread.
struct EyeRenderParams
{
  Vec3 position{};             ///< Ogre-space camera position.
  Quat orientation{};          ///< Ogre-space orientation (wxyz).
  bool pose_valid = false;     ///< false => degenerate pose; keep previous camera pose.
  Mat4 projection{};           ///< Row-major asymmetric projection.
  bool projection_valid = false;  ///< false => degenerate FOV; keep previous projection.
};

/// @brief Convert one eye's Qar pose + FOV into Ogre camera inputs using the pose
/// helpers (qar_pose_to_ogre + make_projection_from_qar_fov). Pure; no Ogre.
EyeRenderParams compute_eye_render_params(const EyeView & eye);

/// @brief Backing GL textures + sizes for one stereo frame (separated layout).
struct StereoFrameTextures
{
  uint32_t left_color_id = 0;
  uint32_t right_color_id = 0;
  QarTextureSize color_size{};

  bool has_depth = false;
  uint32_t left_depth_id = 0;
  uint32_t right_depth_id = 0;
  QarTextureSize depth_size{};

  uint32_t gl_target = kGlTexture2D;
};

/// @brief Assemble a separated-texture stereo frame from backing textures. Fills
/// layout, the four positional color/depth textures, and the explicit per-view
/// descriptors (COLOR/DEPTH x LEFT/RIGHT with texture indices 0=left color,
/// 1=right color, 2=left depth, 3=right depth). Pure; no Ogre. @p synchronization
/// is left at its default; the stream attaches its pending GL fence when it submits.
QarOpenGlStereoFrame assemble_separated_stereo_frame(
  const QarPeerId & peer_id,
  uint64_t frame_index,
  const StereoFrameTextures & textures);

/// @brief One remote peer's stereo stream.
///
/// Owns the left/right eye render targets (each an OgreStereoRenderTarget) and
/// references the peer's Qar render-sender adapter. Renders both eyes from the
/// shared scene and submits them as one separated-texture stereo frame.
///
/// ## Threading
/// initialize_targets() and render_and_submit() touch Ogre/OpenGL and MUST be
/// called on the render thread, guarded by the host's render lock (in RViz,
/// DisplayContext::lockRender()/unlockRender()). This class does no locking
/// itself — the caller (plugin render tick / scheduler / standalone sample) owns it.
///
/// initialize_targets() creates the real left/right color+depth render targets.
/// render_and_submit() queries the sender for per-eye pose/FOV, applies them to the
/// eye cameras via the pose conversions (qar_pose_to_ogre +
/// make_projection_from_qar_fov -> setCustomProjectionMatrix), renders both eyes,
/// assembles a separated-texture QarOpenGlStereoFrame and submits it. Batched
/// multi-peer scheduling is OgreQarRenderScheduler's job.
class OgreQarStereoStream
{
public:
  /// @param scene_manager  Non-null shared Ogre scene.
  /// @param config         Validated per-peer stream configuration.
  /// @param sender         Non-null Qar render-sender adapter for this peer.
  /// @throws std::invalid_argument if scene_manager or sender is null, or
  ///         config.validate() fails.
  OgreQarStereoStream(
    Ogre::SceneManager * scene_manager,
    const StereoStreamConfig & config,
    std::shared_ptr<IQarRenderSenderAdapter> sender);
  ~OgreQarStereoStream();

  OgreQarStereoStream(const OgreQarStereoStream &) = delete;
  OgreQarStereoStream & operator=(const OgreQarStereoStream &) = delete;

  const QarPeerId & peer_id() const { return config_.peer_id; }
  const StereoStreamConfig & config() const { return config_; }

  bool active() const { return active_; }
  void set_active(bool active) { active_ = active; }

  /// @brief Create the underlying left/right color+depth render targets at the
  /// configured size/format. Idempotent-safe: returns Ok if already created.
  /// Must run on the render thread under the host render lock.
  /// @return Ok on success; a descriptive error if an Ogre resource fails.
  Result initialize_targets();

  /// @brief True once both eye render targets have been created.
  bool targets_ready() const;

  /// @brief One tick: begin frame on the sender; if it wants a frame, place the
  /// left/right cameras from the returned per-eye pose/FOV (pose conversions),
  /// render both eyes (color + metric depth), assemble a separated-texture stereo
  /// frame and submit it via the sender. Must run on the render thread under the
  /// host render lock.
  ///
  /// Composed from the batch phases below (begin -> update -> render -> fence ->
  /// submit) so a single-stream caller and the batched scheduler share one code
  /// path. Prefer this for a single stream; the scheduler calls the phases across
  /// ALL streams to keep the batch coherent.
  /// @return Ok if a frame was rendered+submitted OR the sender declined a frame
  ///         (a valid skip, accounted in diagnostics); InvalidState if inactive
  ///         or targets are not created; a descriptive error if submit fails.
  Result render_and_submit();

  // --- Batched render phases ------------------------------------------------
  // The scheduler calls these across all active streams in phase order so every
  // peer samples the same scene snapshot. All must run on the render thread under
  // the scheduler's single batch render lock. Each phase is a no-op for a stream
  // that did not want a frame this tick (begin_frame_phase() returned false).

  /// @brief Batch phase 1 (begin): query the sender for this frame's pose/FOV and stash
  /// it. @return true if the sender wants a frame this tick; false marks a skip
  /// (accounted) and makes the remaining phases no-ops. Ogre-free.
  bool begin_frame_phase();

  /// @brief Batch phase 2 (update cameras): lazily create the eye targets (Ogre) and
  /// apply this frame's per-eye pose/projection to the cameras. Touches Ogre only
  /// when a frame is wanted. @return Ok, or the target-init error (which cancels
  /// the frame and counts a skip).
  Result update_cameras_phase();

  /// @brief Batch phase 3 (render): render both eye targets (color + metric depth),
  /// timing each eye. @return Ok, or the render error (cancels the frame).
  Result render_targets_phase();

  /// @brief Batch phase 4 (fence): invoke @p fence_hook for this stream's just-rendered
  /// targets (GL fence seam). No-op when @p fence_hook is null.
  void fence_phase(IGlFenceHook * fence_hook);

  /// @brief Take ownership of the GL fence (opaque `GLsync`) covering this stream's
  /// just-rendered targets, created by the fence hook in fence_phase. The
  /// fence flows into the next submitted frame's GlFrameSync. Ownership /
  /// lifetime contract (in-process, single render thread, shared context):
  /// the stream deletes the PREVIOUS fence when a new one arrives (always on the
  /// render thread inside the batch lock), so at most one fence per stream is ever
  /// outstanding; the Qar consumer only waits on it (never deletes). @p gl_sync may
  /// be null (fence unsupported/failed) -> the frame simply carries no fence.
  void set_pending_fence(void * gl_sync);

  /// @brief Batch phase 5 (submit): assemble the separated-texture stereo frame and
  /// submit it via the sender; update per-stream counters. @return Ok, or an
  /// error if the sender rejected the frame.
  Result submit_phase();

  /// @brief True iff this stream wanted (and has not yet cancelled) a frame this
  /// tick — i.e. begin_frame_phase() returned true and no later phase failed.
  bool wants_frame_this_tick() const { return pending_wants_render_; }

  /// @brief This tick's head position (midpoint of the two eyes) in **Ogre world
  /// space**, converted with the same qar_position_to_ogre mapping the eye cameras
  /// use — so a light placed here lands exactly where this peer's viewpoint is.
  /// Used by QarSceneLighting in per-peer mode.
  /// @return false (leaving @p out untouched) when no frame is pending this tick,
  ///         i.e. there is no fresh pose to place anything from. Pure; no Ogre.
  bool current_head_position(Vec3 & out) const;

  /// @brief Account one frame shed by the render budget (due but not rendered this
  /// tick to stay within the per-tick budget). Diagnostics-only.
  void note_budget_skip() { note_skip(SkipReason::BudgetShed, ""); }

  /// @brief Reason the most recent skip was taken, for diagnostics/heartbeat.
  SkipReason last_skip_reason() const { return last_skip_reason_; }

  OgreStereoRenderTarget * left_target() { return left_target_.get(); }
  OgreStereoRenderTarget * right_target() { return right_target_.get(); }
  IQarRenderSenderAdapter * sender() { return sender_.get(); }

  /// @brief Forward the live "prime next frame" switch (QAR_ADAPTER_PRIME) to this
  /// stream's sender. See IQarRenderSenderAdapter::set_prime_enabled.
  void set_prime_enabled(bool enabled)
  {
    if (sender_) { sender_->set_prime_enabled(enabled); }
  }

  /// @brief Forward the panel's configured render-tick period (QAR_PANEL_TICK_MS,
  /// informational only) to this stream's sender. See
  /// IQarRenderSenderAdapter::set_tick_ms.
  void set_tick_ms(double tick_ms)
  {
    if (sender_) { sender_->set_tick_ms(tick_ms); }
  }

  /// @brief This stream's per-frame trace CSV path, or empty if tracing is off.
  /// See IQarRenderSenderAdapter::frame_trace_path.
  std::string frame_trace_path() const
  {
    return sender_ ? sender_->frame_trace_path() : std::string();
  }

  /// @brief Last both-eyes render time in ms (0 before the first render). Cheap
  /// accessor used by the scheduler to feed the budget's cost estimate.
  double last_render_ms() const { return last_render_ms_; }

  /// @brief Copy of current per-stream diagnostics accounting.
  StreamDiagnostics diagnostics() const;

private:
  /// @brief Record one skipped frame together with WHY it was skipped. Every
  /// bail-out from a render+submit cycle goes through here, so `frames_skipped_`
  /// and the per-reason totals can never drift apart.
  void note_skip(SkipReason reason, const char * detail);

  Ogre::SceneManager * scene_manager_;
  StereoStreamConfig config_;
  std::shared_ptr<IQarRenderSenderAdapter> sender_;

  std::unique_ptr<OgreStereoRenderTarget> left_target_;
  std::unique_ptr<OgreStereoRenderTarget> right_target_;

  bool active_ = false;

  // --- Per-tick batch state (carried across the batch phases) --------------
  QarFrameInfo pending_info_{};
  bool pending_wants_render_ = false;

  // GL fence (opaque GLsync) covering the last rendered frame's draw commands.
  // Owned here; replaced (previous deleted) each fence_phase; see set_pending_fence.
  void * current_fence_ = nullptr;

  uint64_t frames_submitted_ = 0;
  uint64_t frames_skipped_ = 0;
  SkipReason last_skip_reason_ = SkipReason::None;
  std::string last_skip_detail_;
  uint64_t skips_sender_not_ready_ = 0;
  uint64_t skips_sender_error_ = 0;
  uint64_t skips_target_init_ = 0;
  uint64_t skips_budget_ = 0;
  double last_render_ms_ = 0.0;
  double avg_render_ms_ = 0.0;
  double last_left_render_ms_ = 0.0;
  double last_right_render_ms_ = 0.0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_QAR_STEREO_STREAM_HPP_
