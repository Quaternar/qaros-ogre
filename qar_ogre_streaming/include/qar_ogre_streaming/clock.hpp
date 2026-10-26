// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__CLOCK_HPP_
#define QAR_OGRE_STREAMING__CLOCK_HPP_

#include <chrono>

namespace qar_ogre_streaming
{

/// @brief Monotonic time source seam for the scheduler / render budget.
///
/// The render-budget FPS pacing and time-budget shedding must be deterministically
/// unit-testable, so the budget logic never reads the wall clock directly: the
/// scheduler reads "now" ONCE per tick from an injected IClock and threads it into the budget as a
/// plain @c double (seconds). Production uses @ref SteadyClock; tests inject ManualClock from
/// qar_ogre_streaming/test_support/include/qar_ogre_streaming_test_support/manual_clock.hpp.
class IClock
{
public:
  virtual ~IClock() = default;

  /// @brief Monotonic time in seconds. Only differences are meaningful.
  virtual double now_seconds() const = 0;
};

/// @brief Default IClock backed by std::chrono::steady_clock (monotonic).
///
/// Header-only so the scheduler can default-construct one when the host does not
/// inject a clock. The epoch is the first construction; only deltas matter.
class SteadyClock : public IClock
{
public:
  double now_seconds() const override
  {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - epoch_).count();
  }

private:
  std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__CLOCK_HPP_
