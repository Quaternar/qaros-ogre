// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "qar_ogre_streaming/gl_interop.hpp"

// Ogre's headers are not /W4-clean; isolate them behind a warning push/pop.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <OgreCamera.h>
#include <OgreMatrix4.h>
#include <OgreQuaternion.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace qar_ogre_streaming
{
namespace
{

/// Apply one eye's M2-converted pose + projection to its offscreen camera.
/// Degenerate pose/FOV inputs leave the corresponding camera state unchanged
/// (compute_eye_render_params reports validity), so a bad frame never blanks the
/// view. No-op if the target has no camera.
void apply_eye(OgreStereoRenderTarget * target, const EyeView & eye)
{
  if (target == nullptr) {
    return;
  }
  Ogre::Camera * cam = target->camera();
  if (cam == nullptr) {
    return;
  }
  const EyeRenderParams p = compute_eye_render_params(eye);
  if (p.pose_valid) {
    cam->setPosition(p.position.x, p.position.y, p.position.z);
    cam->setOrientation(
      Ogre::Quaternion(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z));
  }
  if (p.projection_valid) {
    const float * m = p.projection.m;  // row-major, matches Ogre::Matrix4 layout
    const Ogre::Matrix4 proj(
      m[0], m[1], m[2], m[3],
      m[4], m[5], m[6], m[7],
      m[8], m[9], m[10], m[11],
      m[12], m[13], m[14], m[15]);
    cam->setCustomProjectionMatrix(true, proj);
  }
}

StereoFrameTextures collect_textures(
  const OgreStereoRenderTarget & left, const OgreStereoRenderTarget & right)
{
  StereoFrameTextures t;
  t.color_size = left.colorTextureSize();
  t.left_color_id = left.colorGlTextureId();
  t.right_color_id = right.colorGlTextureId();
  t.has_depth = left.has_depth() && right.has_depth();
  if (t.has_depth) {
    t.depth_size = left.depthTextureSize();
    t.left_depth_id = left.depthGlTextureId();
    t.right_depth_id = right.depthGlTextureId();
  }
  t.gl_target = kGlTexture2D;
  return t;
}

}  // namespace

OgreQarStereoStream::OgreQarStereoStream(
  Ogre::SceneManager * scene_manager,
  const StereoStreamConfig & config,
  std::shared_ptr<IQarRenderSenderAdapter> sender)
: scene_manager_(scene_manager), config_(config), sender_(std::move(sender))
{
  if (scene_manager_ == nullptr) {
    throw std::invalid_argument("OgreQarStereoStream: scene_manager must not be null");
  }
  if (sender_ == nullptr) {
    throw std::invalid_argument("OgreQarStereoStream: sender must not be null");
  }
  const Result valid = config_.validate();
  if (!valid) {
    throw std::invalid_argument("OgreQarStereoStream: invalid config: " + valid.message);
  }

  const std::string base = "qar_stream_" + peer_id_to_hex(config_.peer_id);
  left_target_ = std::make_unique<OgreStereoRenderTarget>(scene_manager_, base + "_left");
  right_target_ = std::make_unique<OgreStereoRenderTarget>(scene_manager_, base + "_right");
}

OgreQarStereoStream::~OgreQarStereoStream() = default;

Result OgreQarStereoStream::initialize_targets()
{
  const bool with_depth = true;  // color + depth.
  if (left_target_->is_created() && right_target_->is_created()) {
    return Result::success();  // idempotent
  }

  if (!left_target_->is_created()) {
    const Result l = left_target_->create(
      config_.width, config_.height, config_.color_format, config_.depth_format, with_depth);
    if (!l.ok()) {
      return l;
    }
  }
  if (!right_target_->is_created()) {
    const Result r = right_target_->create(
      config_.width, config_.height, config_.color_format, config_.depth_format, with_depth);
    if (!r.ok()) {
      left_target_->destroy();  // keep both-or-neither invariant
      return r;
    }
  }
  return Result::success();
}

bool OgreQarStereoStream::targets_ready() const
{
  return left_target_ && right_target_ &&
    left_target_->is_created() && right_target_->is_created();
}

Result OgreQarStereoStream::render_and_submit()
{
  if (!active_) {
    return Result::error(Status::InvalidState, "render_and_submit: stream is not active");
  }
  if (!targets_ready()) {
    return Result::error(
      Status::InvalidState, "render_and_submit: call initialize_targets() first");
  }

  // Compose the M6 batch phases for a single stream. A declined frame is a valid,
  // complete (skipped) tick; a phase failure is surfaced verbatim.
  if (!begin_frame_phase()) {
    return Result::success();
  }
  Result r = update_cameras_phase();
  if (!r.ok()) {
    return r;
  }
  r = render_targets_phase();
  if (!r.ok()) {
    return r;
  }
  fence_phase(nullptr);  // no GL fence until M7
  return submit_phase();
}

// --- Batched render phases ---------------------------------

bool OgreQarStereoStream::begin_frame_phase()
{
  pending_wants_render_ = false;
  pending_info_ = QarFrameInfo{};
  if (!active_) {
    return false;
  }
  QarFrameInfo info{};
  if (!sender_->begin_frame(info) || !info.should_render) {
    ++frames_skipped_;  // Sender declined a frame this tick.
    return false;
  }
  pending_info_ = info;
  pending_wants_render_ = true;
  return true;
}

Result OgreQarStereoStream::update_cameras_phase()
{
  if (!pending_wants_render_) {
    return Result::success();
  }
  // Lazily create the eye targets on first use (needs the GL context, so it must
  // run here, inside the batch render lock — not from a Qar callback thread).
  if (!targets_ready()) {
    const Result init = initialize_targets();
    if (!init.ok()) {
      pending_wants_render_ = false;
      ++frames_skipped_;
      return init;
    }
  }
  apply_eye(left_target_.get(), pending_info_.left_eye);
  apply_eye(right_target_.get(), pending_info_.right_eye);
  return Result::success();
}

Result OgreQarStereoStream::render_targets_phase()
{
  if (!pending_wants_render_) {
    return Result::success();
  }
  const auto t0 = std::chrono::steady_clock::now();
  const Result lr = left_target_->render();
  const auto t1 = std::chrono::steady_clock::now();
  const Result rr = right_target_->render();
  const auto t2 = std::chrono::steady_clock::now();

  last_left_render_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
  last_right_render_ms_ = std::chrono::duration<double, std::milli>(t2 - t1).count();
  last_render_ms_ = last_left_render_ms_ + last_right_render_ms_;

  if (!lr.ok()) {
    pending_wants_render_ = false;
    return lr;
  }
  if (!rr.ok()) {
    pending_wants_render_ = false;
    return rr;
  }
  return Result::success();
}

void OgreQarStereoStream::fence_phase(IGlFenceHook * fence_hook)
{
  if (!pending_wants_render_ || fence_hook == nullptr) {
    return;  // No-op seam when no fence hook is installed (M6 default).
  }
  fence_hook->insert_fence(*this);  // -> set_pending_fence(glFenceSync)
}

void OgreQarStereoStream::set_pending_fence(void * gl_sync)
{
  // Delete the previous fence (if any) now that a newer one supersedes it. This
  // runs on the render thread inside the batch lock (fence_phase), so it is the
  // safe place to release GL objects. At most one fence per stream is outstanding.
  if (current_fence_ != nullptr && current_fence_ != gl_sync) {
    gl_interop::fence_delete(current_fence_);
  }
  current_fence_ = gl_sync;
}

Result OgreQarStereoStream::submit_phase()
{
  if (!pending_wants_render_) {
    return Result::success();
  }
  const StereoFrameTextures textures = collect_textures(*left_target_, *right_target_);
  QarOpenGlStereoFrame frame =
    assemble_separated_stereo_frame(config_.peer_id, pending_info_.frame_index, textures);

  // Attach the GL fence created in fence_phase (M7). Null => no fence this frame
  // (e.g. no fence hook installed) and qar_waits_on_fence stays false.
  frame.synchronization.fence_sync = current_fence_;
  frame.synchronization.qar_waits_on_fence = (current_fence_ != nullptr);

  if (!sender_->submit_opengl_stereo_frame(frame)) {
    pending_wants_render_ = false;
    return Result::error(Status::InvalidState, "submit_phase: sender rejected the frame");
  }

  ++frames_submitted_;
  avg_render_ms_ += (last_render_ms_ - avg_render_ms_) /
    static_cast<double>(frames_submitted_);
  pending_wants_render_ = false;
  return Result::success();
}

StreamDiagnostics OgreQarStereoStream::diagnostics() const
{
  StreamDiagnostics d;
  d.peer_id_hex = peer_id_to_hex(config_.peer_id);
  d.width = config_.width;
  d.height = config_.height;
  d.max_fps = config_.max_fps;
  d.active = active_;
  d.frames_submitted = frames_submitted_;
  d.frames_skipped = frames_skipped_;
  d.last_render_ms = last_render_ms_;
  d.avg_render_ms = avg_render_ms_;
  d.last_left_render_ms = last_left_render_ms_;
  d.last_right_render_ms = last_right_render_ms_;
  // achieved_fps is filled by the scheduler (it owns the tick clock); left 0 here.
  return d;
}

}  // namespace qar_ogre_streaming
