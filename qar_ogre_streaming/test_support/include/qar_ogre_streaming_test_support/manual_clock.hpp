// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING_TEST_SUPPORT__MANUAL_CLOCK_HPP_
#define QAR_OGRE_STREAMING_TEST_SUPPORT__MANUAL_CLOCK_HPP_

#include "qar_ogre_streaming/clock.hpp"

namespace qar_ogre_streaming
{
namespace test_support
{

/// @brief IClock whose time is advanced by hand, so scheduler/budget FPS pacing
/// and time-budget shedding are deterministically testable with no wall clock.
class ManualClock : public IClock
{
public:
  explicit ManualClock(double start_seconds = 0.0) : now_(start_seconds) {}

  double now_seconds() const override { return now_; }

  void set(double seconds) { now_ = seconds; }
  void advance(double seconds) { now_ += seconds; }

private:
  double now_ = 0.0;
};

}  // namespace test_support
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING_TEST_SUPPORT__MANUAL_CLOCK_HPP_
