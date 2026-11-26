// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__GPU_PROGRESS_GATE_HPP_
#define QAR_OGRE_STREAMING__GPU_PROGRESS_GATE_HPP_

namespace qar_ogre_streaming
{

/// @brief Seam for a watch that tells whether the GPU still finishes the frames the render thread
/// issues.
///
/// While the GPU makes no progress, every GL call of the render thread that needs the GPU can wait
/// for as long as the stall lasts. A closed gate asks the render thread to issue no further GL
/// work, so the batch of the scheduler is skipped until the GPU finishes frames again.
///
/// Every method is called on the render thread and returns at once: it reads atomics or issues a
/// fence that is never waited for.
class IGpuProgressGate
{
public:
  virtual ~IGpuProgressGate() = default;

  /// @brief True while the render thread must issue no GL work.
  virtual bool gate_closed() = 0;

  /// @brief A batch was rendered and submitted: the watch tracks the GPU's progress on it.
  virtual void note_batch_issued() = 0;

  /// @brief A batch was skipped because the gate was closed.
  virtual void note_batch_skipped() = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__GPU_PROGRESS_GATE_HPP_
