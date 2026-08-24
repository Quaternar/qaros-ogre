// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__QAR_RENDER_SENDER_ADAPTER_HPP_
#define QAR_OGRE_STREAMING__QAR_RENDER_SENDER_ADAPTER_HPP_

#include <memory>

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Narrow adapter over one peer's Qar render sender.
///
/// qar_ogre_streaming talks to Qar ONLY through this interface, which keeps the
/// rendering library decoupled from the (still-evolving) Qar OpenGL producer
/// path. Two implementations exist: a mock (test_support) that records calls,
/// and — once Qar ships the real OpenGL path — a thin real adapter wrapping
/// QarRenderSender + qar_render_sender_frame_opengl.
///
/// The two methods below are named exactly as in the original API design.
class IQarRenderSenderAdapter
{
public:
  virtual ~IQarRenderSenderAdapter() = default;

  /// @brief Begin a frame; fills @p out_info with the sender's desired per-eye
  /// pose/FOV, frame index and predicted display time.
  /// @return true if a frame should be rendered/submitted this tick; false to
  ///         skip (e.g. the sender is not ready or does not want a frame).
  virtual bool begin_frame(QarFrameInfo & out_info) = 0;

  /// @brief Submit a rendered separated-texture stereo OpenGL frame.
  /// @return true if Qar accepted the frame.
  virtual bool submit_opengl_stereo_frame(const QarOpenGlStereoFrame & frame) = 0;
};

/// @brief Factory that creates one IQarRenderSenderAdapter per peer.
///
/// Supplied through OgreQarStreamingConfig. The manager
/// asks the factory for a sender when a stream is created for a peer, so the
/// library never constructs Qar objects itself. A test harness provides a mock
/// factory; RViz/real integration provides one backed by the Qar runtime.
class IQarRenderSenderFactory
{
public:
  virtual ~IQarRenderSenderFactory() = default;

  /// @brief Create a render-sender adapter for @p peer_id.
  /// @return a non-null adapter on success, or nullptr if the sender could not
  ///         be created (the manager surfaces this as a stream-creation error).
  virtual std::shared_ptr<IQarRenderSenderAdapter> create_sender(
    const QarPeerId & peer_id) = 0;
};

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__QAR_RENDER_SENDER_ADAPTER_HPP_
