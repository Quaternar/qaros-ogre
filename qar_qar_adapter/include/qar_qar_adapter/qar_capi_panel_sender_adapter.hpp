// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The QarOS side of the Qt GUI-panel path — sibling of
// QarCApiRenderSenderAdapter, which does the same job for the stereo 3D stream.
//
// Two SDK objects back one AR panel:
//   * QarVideoSender  — a producer-owned CPU video stream, explicitly described
//     in qar_streaming.h as being "for application-owned video such as GUI panel
//     content" (as opposed to QarRenderSender's receiver-driven stereo frames).
//   * QarGuiPanel     — the panel itself, created with a QarGuiPanelStreamIdExt
//     chained onto QarGuiPanelInit so its content IS that video stream.
//
// Both take a QarSession*, and the header requires the video sender to be owned
// by the SAME session as the panel ("The stream must identify a video sender
// owned by the same session"). So this borrows the session QarCApiSenderFactory
// already owns rather than standing up a second one — sharing is mandatory here,
// not just economical.
//
// This file stays Qt-FREE: it implements qar_ar_core's POD-only panel seam
// (IQarPanelSenderAdapter), which qar_qt_streaming drives, the same way the render
// adapter implements qar_ogre_streaming's Ogre-free seam.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_PANEL_SENDER_ADAPTER_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_PANEL_SENDER_ADAPTER_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/qar_capi_panel_input_source.hpp"
#include "qar_ar_core/qar_panel_sender.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief One QarOS GUI panel fed by one producer-owned CPU video stream.
///
/// Lifetime is create -> N x (try_begin_frame + submit_frame) -> close. Every
/// call happens on the Qt GUI thread (see qt_widget_capture.hpp in qar_qt_streaming).
/// `qar_video_sender_try_begin_frame` is documented non-blocking, so the per-frame
/// calls never stall it; the constructor and close() make synchronous QarOS calls.
class QarCApiPanelSenderAdapter : public qar_ar_core::IQarPanelSenderAdapter
{
public:
  /// @param session Borrowed, owned by QarCApiSenderFactory; must outlive this.
  /// @param desc    Buffer size + placement, fixed for the sender's lifetime.
  /// @param input   Optional. When given, this adapter subscribes the new
  ///        panel to QarOS's pointer (gesture) events on creation and unsubscribes in
  ///        @ref close — tying the subscription's life to the panel's exactly,
  ///        which is what keeps a callback from arriving for a panel that no
  ///        longer exists. Shared, because the source outlives any one panel.
  QarCApiPanelSenderAdapter(
    QarSession * session,
    const qar_ar_core::PanelDesc & desc,
    std::shared_ptr<QarCApiPanelInputSource> input = nullptr);
  ~QarCApiPanelSenderAdapter() override;

  QarCApiPanelSenderAdapter(const QarCApiPanelSenderAdapter &) = delete;
  QarCApiPanelSenderAdapter & operator=(const QarCApiPanelSenderAdapter &) = delete;

  /// @brief True once the video sender AND the panel both exist.
  bool ready() const { return ready_; }

  /// @brief The panel id QarOS assigned, valid only when @ref ready.
  const QarGuiPanelId & panel_id() const { return panel_id_; }

  bool try_begin_frame(bool & out_ready) override;

  /// @brief Which of the three answers the last @ref try_begin_frame gave when it reported no
  /// destination: success without a reserved cycle, no active transfers (604) or a transfer that
  /// is reconnecting (603). The first time a sender that has shown frames hears 603, one line
  /// says so, with the number of frames shown.
  qar_ar_core::PanelNotReadyCause not_ready_cause() const override { return not_ready_cause_; }

  bool submit_frame(const qar_ar_core::PanelPixels & pixels) override;
  bool set_placement(const qar_ar_core::PanelPlacement & placement) override;

  /// @brief One `qar_gui_panels_set_state` on this panel: hide, show or minimise it while the
  /// panel, the video sender and the subscriptions all stay. Refused (false, @ref last_error
  /// says why) when there is no live panel, or when the runtime returns an error. Each change
  /// that succeeds is printed to stderr in the style of the creation line.
  bool set_state(QarGuiPanelState state) override;
  bool supports_state() const override { return true; }

  /// @brief The one teardown, in the one safe order — see
  /// IQarPanelSenderAdapter::close for the contract, and the body for the lines it
  /// prints:
  ///
  ///  1. `QarCApiPanelInputSource::unsubscribe_panel` — the gesture AND the
  ///     panel-update subscription, cancelled first so our own close is not
  ///     mistaken for an AR-side close and no callback reaches a retiring slot;
  ///  2. `qar_gui_panels_close_panel` — skipped when @p panel_closed_in_ar, and a
  ///     failure is reported rather than hidden;
  ///  3. `qar_video_sender_handle_destroy` — always, whatever step 2 did.
  ///
  /// Idempotent: the destructor calls it again and finds nothing to do. That
  /// matters beyond tidiness — a SECOND unsubscribe for this key would tear down
  /// the subscription of a newer adapter that has since re-used it.
  using qar_ar_core::IQarPanelSenderAdapter::close;
  qar_ar_core::PanelCloseReport close(bool panel_closed_in_ar) override;
  const std::string & last_error() const override { return last_error_; }
  /// @brief Canonical UUID of `qar_video_sender_stream_id`, the id the runtime
  /// names this panel's content by everywhere. Empty until the sender exists;
  /// kept after close() so a retired panel can still be identified.
  std::string stream_id_text() const override { return stream_id_text_; }

private:
  /// Run the create chain: video_sender_create -> stream_id -> add_panel.
  /// Records the failing step in last_error_ and leaves ready_ false.
  void bootstrap();

  QarSession * session_ = nullptr;
  qar_ar_core::PanelDesc desc_;
  /// Null when the host wants a stream-only panel (input off).
  std::shared_ptr<QarCApiPanelInputSource> input_;

  QarVideoSender * sender_ = nullptr;
  QarGuiPanelId panel_id_{};
  /// Set once at bootstrap, never cleared: see stream_id_text().
  std::string stream_id_text_;
  /// close() is idempotent and runs from the destructor too; this keeps the
  /// retirement line to one per panel.
  bool retired_logged_ = false;
  /// Set by the first close(). Every later one returns an empty report without
  /// touching the runtime or the input source.
  bool closed_ = false;
  bool has_panel_ = false;
  bool ready_ = false;
  bool frame_reserved_ = false;
  std::string last_error_;
  /// The answer of the last try_begin_frame when it reported no destination.
  qar_ar_core::PanelNotReadyCause not_ready_cause_ = qar_ar_core::PanelNotReadyCause::NoDestination;
  /// Frames this sender has shown (a show_frame that succeeded).
  std::uint64_t frames_shown_ = 0;
  /// Lets the first frame shown print its line, and the first 603 after a frame print its line.
  LogOnce first_show_noted_;
  LogOnce reconnect_noted_;
};

/// @brief Mints QarCApiPanelSenderAdapters against a live QarCApiSenderFactory.
///
/// Holds only a raw pointer to that factory: the RViz panel owns both, and the
/// factory always outlives the panel streams (it is torn down last).
class QarCApiPanelSenderFactory : public qar_ar_core::IQarPanelSenderFactory
{
public:
  /// @param session_owner Borrowed; must outlive this factory.
  /// @param input Optional; handed to every panel this factory mints, so each
  ///        one subscribes itself to QarOS's pointer (gesture) events. Null gives
  ///        stream-only panels: they stream out and take no input.
  explicit QarCApiPanelSenderFactory(
    IQarSessionOwner * session_owner,
    std::shared_ptr<QarCApiPanelInputSource> input = nullptr);

  std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> create_panel_sender(
    const qar_ar_core::PanelDesc & desc) override;

  const std::string & last_error() const override { return last_error_; }

  /// @brief The input source this factory subscribes its panels through, if any.
  const std::shared_ptr<QarCApiPanelInputSource> & input() const { return input_; }

private:
  IQarSessionOwner * session_owner_ = nullptr;
  std::shared_ptr<QarCApiPanelInputSource> input_;
  std::string last_error_;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_PANEL_SENDER_ADAPTER_HPP_
