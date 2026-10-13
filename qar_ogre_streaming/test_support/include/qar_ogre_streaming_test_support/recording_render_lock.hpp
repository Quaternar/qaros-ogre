// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING_TEST_SUPPORT__RECORDING_RENDER_LOCK_HPP_
#define QAR_OGRE_STREAMING_TEST_SUPPORT__RECORDING_RENDER_LOCK_HPP_

#include "qar_ogre_streaming/render_lock.hpp"

namespace qar_ogre_streaming
{
namespace test_support
{

/// @brief IRenderLock that records lock/unlock activity so a test can assert the
/// batch takes the lock EXACTLY once around the whole batch.
///
/// Tracks the current nesting depth, the max depth ever reached (must be 1 — the
/// scheduler never nests), and the total lock/unlock counts (which must be equal
/// and, for a non-empty batch, exactly one each).
class RecordingRenderLock : public IRenderLock
{
public:
  void lock_render() override
  {
    ++depth_;
    ++lock_calls_;
    if (depth_ > max_depth_) {
      max_depth_ = depth_;
    }
  }

  void unlock_render() override
  {
    --depth_;
    ++unlock_calls_;
  }

  int depth() const { return depth_; }
  int max_depth() const { return max_depth_; }
  int lock_calls() const { return lock_calls_; }
  int unlock_calls() const { return unlock_calls_; }
  bool balanced() const { return depth_ == 0 && lock_calls_ == unlock_calls_; }

private:
  int depth_ = 0;
  int max_depth_ = 0;
  int lock_calls_ = 0;
  int unlock_calls_ = 0;
};

}  // namespace test_support
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING_TEST_SUPPORT__RECORDING_RENDER_LOCK_HPP_
