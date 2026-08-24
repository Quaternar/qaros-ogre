// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING_TEST_SUPPORT__IMMEDIATE_RENDER_THREAD_EXECUTOR_HPP_
#define QAR_OGRE_STREAMING_TEST_SUPPORT__IMMEDIATE_RENDER_THREAD_EXECUTOR_HPP_

#include <cstddef>
#include <functional>
#include <utility>

#include "qar_ogre_streaming/render_thread_executor.hpp"

namespace qar_ogre_streaming
{
namespace test_support
{

/// @brief IRenderThreadExecutor that runs posted work inline (immediately) and
/// reports is_render_thread() == true.
///
/// Lets unit tests and the standalone sample exercise the scheduler's
/// render-thread-affinity path without any real threading. Counts how many work
/// items were posted so tests can assert enqueue behavior.
class ImmediateRenderThreadExecutor : public IRenderThreadExecutor
{
public:
  void post_to_render_thread(std::function<void()> work) override
  {
    ++posted_count_;
    if (work) {
      work();  // Immediate/inline execution.
    }
  }

  bool is_render_thread() const override { return is_render_thread_; }

  /// @brief Flip the reported affinity to exercise the "wrong thread" refusal
  /// path in the scheduler.
  void set_is_render_thread(bool value) { is_render_thread_ = value; }

  std::size_t posted_count() const { return posted_count_; }

private:
  bool is_render_thread_ = true;
  std::size_t posted_count_ = 0;
};

}  // namespace test_support
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING_TEST_SUPPORT__IMMEDIATE_RENDER_THREAD_EXECUTOR_HPP_
