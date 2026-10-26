// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__GL_FENCE_HOOK_HPP_
#define QAR_OGRE_STREAMING__GL_FENCE_HOOK_HPP_

namespace qar_ogre_streaming
{

class OgreQarStereoStream;

/// @brief Seam for the `fence_all_textures` step of the batch (render, fence,
/// submit).
///
/// The scheduler renders all targets back-to-back inside one render lock and then
/// submits. The zero-copy OpenGL producer path needs a `glFenceSync` inserted
/// after each stream's targets are rendered, exported as the opaque
/// `GlFrameSync::fence_sync` handle so the Qar consumer can wait on it
/// before sampling the textures.
///
/// The scheduler calls @ref insert_fence once per rendered stream, in batch order,
/// AFTER render_all_targets and BEFORE submit_all_frames. When no hook is
/// installed, the fence step is a pure no-op — the seam lets the real GL fence
/// (@ref GlFenceHook) slot in without touching the scheduler's ordering.
class IGlFenceHook
{
public:
  virtual ~IGlFenceHook() = default;

  /// @brief Insert/record a GPU sync fence for @p stream's just-rendered targets.
  /// Runs on the render thread, inside the batch render lock. @ref GlFenceHook
  /// implements this (glFenceSync + stash the GLsync into the stream's pending
  /// frame). No-op by default (no hook installed).
  virtual void insert_fence(OgreQarStereoStream & stream) = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__GL_FENCE_HOOK_HPP_
