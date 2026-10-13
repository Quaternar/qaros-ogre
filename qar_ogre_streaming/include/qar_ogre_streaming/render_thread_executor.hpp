// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__RENDER_THREAD_EXECUTOR_HPP_
#define QAR_OGRE_STREAMING__RENDER_THREAD_EXECUTOR_HPP_

#include <functional>

namespace qar_ogre_streaming
{

/// @brief Generic interface used by qar_ogre_streaming to ensure Ogre/OpenGL
/// work runs on the correct (render) thread.
///
/// The signature below is the seam every host implements — keep it stable. RViz
/// provides an implementation backed by RViz/Qt scheduling; a standalone Ogre
/// application provides its own. Qar networking callbacks must never render
/// directly: they enqueue work through post_to_render_thread(), and the scheduler
/// asserts is_render_thread() before touching Ogre.
class IRenderThreadExecutor
{
public:
  virtual ~IRenderThreadExecutor() = default;
  virtual void post_to_render_thread(std::function<void()> work) = 0;
  virtual bool is_render_thread() const = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__RENDER_THREAD_EXECUTOR_HPP_
