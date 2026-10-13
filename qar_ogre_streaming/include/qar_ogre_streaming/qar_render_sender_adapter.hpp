// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__QAR_RENDER_SENDER_ADAPTER_HPP_
#define QAR_OGRE_STREAMING__QAR_RENDER_SENDER_ADAPTER_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{

/// @brief Narrow adapter over one peer's Qar render sender.
///
/// qar_ogre_streaming talks to Qar ONLY through this interface, which keeps the
/// rendering library decoupled from the QarOS C API. Two implementations exist: a
/// recording test double (test_support) that records calls, and the real adapter
/// of qar_qar_adapter (QarCApiRenderSenderAdapter) wrapping QarRenderSender +
/// qar_render_sender_frame_opengl.
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

  /// @brief Human-readable account of the LAST begin_frame() that did not hand
  /// back a frame — the adapter's side of a skip.
  ///
  /// begin_frame() collapses several very different situations into one "no
  /// frame this tick" answer: the Qar begin-frame wait is still pending, the SDK
  /// returned an error, or there is no stream handle at all. From the counter
  /// alone they are indistinguishable: a stall presents as nothing but `skip=`
  /// climbing at the tick rate. The stream copies this into
  /// StreamDiagnostics::last_skip_detail so the heartbeat can name it.
  ///
  /// Must stay valid until the next begin_frame() call. Empty is fine (the
  /// default) — the SkipReason alone is already a diagnosis for simple adapters.
  virtual const char * last_skip_detail() const { return ""; }

  /// @brief Live switch (QAR_ADAPTER_PRIME): enable/disable starting the next
  /// frame's begin-frame wait right after this one is shown (see
  /// QarCApiRenderSenderAdapter::prime_next_frame). Applies at the next show.
  /// Default no-op — adapters with no such optimization (e.g. a test double) ignore it.
  virtual void set_prime_enabled(bool enabled) { (void)enabled; }

  /// @brief Informational only (QAR_PANEL_TICK_MS): records the host's currently
  /// configured render-tick period, in milliseconds (fractional; sub-1ms periods are
  /// realized by the panel as a zero-interval QTimer + steady-clock throttle) for
  /// diagnostics (the frame-trace CSV's tick_ms column). Does NOT affect scheduling —
  /// the scheduler paces off the render budget, not this value. Default no-op.
  virtual void set_tick_ms(double tick_ms) { (void)tick_ms; }

  /// @brief Full path of this adapter's per-frame trace CSV (QAR_ADAPTER_FRAME_TRACE),
  /// or empty if tracing is off / unsupported. Default empty.
  virtual std::string frame_trace_path() const { return {}; }

  /// @brief Total frame-trace CSV rows written so far (0 when tracing is off /
  /// unsupported). Feeds StreamDiagnostics::frame_trace_rows.
  virtual std::uint64_t frame_trace_rows() const { return 0; }

  /// @brief Of those, how many had a changed pre-show pose re-read. Feeds
  /// StreamDiagnostics::frame_trace_changed_rows.
  virtual std::uint64_t frame_trace_changed_rows() const { return 0; }
};

/// @brief Factory that creates one IQarRenderSenderAdapter per peer.
///
/// Supplied through OgreQarStreamingConfig. The manager
/// asks the factory for a sender when a stream is created for a peer, so the
/// library never constructs Qar objects itself. A test harness provides a
/// test-double factory; RViz/real integration provides one backed by the Qar runtime.
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
