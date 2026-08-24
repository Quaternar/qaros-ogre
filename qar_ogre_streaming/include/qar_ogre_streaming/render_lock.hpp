// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__RENDER_LOCK_HPP_
#define QAR_OGRE_STREAMING__RENDER_LOCK_HPP_

namespace qar_ogre_streaming
{

/// @brief Generic render-lock seam used by the batched scheduler.
///
/// The canonical render_tick() sequence wraps the WHOLE batch in one
/// `context_->lockRender()` / `unlockRender()` pair so every peer samples the same
/// coherent scene snapshot. The reusable library must not
/// depend on any RViz/Qt type, so it takes the lock through this abstract seam:
///
///   * the RViz plugin implements it by forwarding to
///     `rviz_common::DisplayContext::lockRender()` / `unlockRender()`;
///   * a standalone Ogre sample (M8) implements a no-op or its own context lock;
///   * unit tests implement a recording lock that asserts the batch takes the lock
///     exactly once (nesting depth returns to zero) around the render calls.
///
/// The scheduler treats a null render-lock as "no locking required" (e.g. a
/// single-threaded sample), so the seam is optional. Implementations must be
/// re-entrant only to the degree the host requires; the scheduler never nests
/// lock_render() calls.
class IRenderLock
{
public:
  virtual ~IRenderLock() = default;

  /// @brief Acquire the host render lock before any Ogre/GL work in the batch.
  virtual void lock_render() = 0;

  /// @brief Release the host render lock after the batch completes.
  virtual void unlock_render() = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__RENDER_LOCK_HPP_
