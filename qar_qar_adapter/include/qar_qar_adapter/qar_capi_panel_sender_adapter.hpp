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
//   * QarGuiPanel     - the panel itself, found or created under its common name with a
//     QarGuiPanelStreamIdExt chained onto QarGuiPanelInit so its content IS that video
//     stream. A panel that an earlier session left behind under the same name is reattached:
//     it gets the stream of the current sender, its size and pose, and is shown again.
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
#include "qar_qar_adapter/panel_state_guard.hpp"
#include "qar_qar_adapter/qar_capi_panel_input_source.hpp"
#include "qar_ar_core/qar_panel_sender.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief One QarOS GUI panel fed by one producer-owned CPU video stream.
///
/// Lifetime is create -> N x (try_begin_frame + submit_frame) -> close. Every call of this class
/// is for the Qt GUI thread, and none of them waits for the Hub. The panel and its video sender
/// are made, changed and destroyed by tasks on the thread that owns the session (see
/// IQarSessionOwner::post_session_task), in the order the calls were made; the outcomes come back
/// through @ref drain_results. `qar_video_sender_try_begin_frame` is documented non-blocking, so
/// the per-frame calls stay on the GUI thread and never stall it.
class QarCApiPanelSenderAdapter : public qar_ar_core::IQarPanelSenderAdapter
{
public:
  /// @param owner   Borrowed; must outlive this. The panel and its video sender are made on the
  ///        session it owns.
  /// @param desc    Buffer size + placement, fixed for the sender's lifetime, and the
  ///        common name of the panel (an empty one is refused).
  /// @param input   Optional. When given, the create task subscribes the new
  ///        panel to QarOS's pointer (gesture) events and the teardown unsubscribes -
  ///        tying the subscription's life to the panel's exactly,
  ///        which is what keeps a callback from arriving for a panel that no
  ///        longer exists. Shared, because the source outlives any one panel.
  QarCApiPanelSenderAdapter(
    IQarSessionOwner * owner,
    const qar_ar_core::PanelDesc & desc,
    std::shared_ptr<QarCApiPanelInputSource> input = nullptr);
  ~QarCApiPanelSenderAdapter() override;

  QarCApiPanelSenderAdapter(const QarCApiPanelSenderAdapter &) = delete;
  QarCApiPanelSenderAdapter & operator=(const QarCApiPanelSenderAdapter &) = delete;

  /// @brief True when the constructor refused the panel (no owner, a bad description, or no
  /// session accepting calls); @ref last_error says why. A panel that was accepted and fails
  /// later reports it through @ref drain_results.
  bool refused() const { return refused_; }

  /// @brief True once the creation is complete: the video sender and the panel exist, and the
  /// attach probe, a reattach and the input subscription have run.
  bool ready() const;

  /// @brief True while the panel and its video sender are still being made and no frame may be
  /// sent. It turns false as soon as the panel exists and shows the stream of the sender, which
  /// is before the attach probe and the reattach have finished: the receiver shows the panel from
  /// that moment, so the frames must not wait for them.
  bool pending() const override;

  /// @brief The panel id QarOS assigned, valid only when @ref ready.
  QarGuiPanelId panel_id() const;

  /// @brief True when the panel was found under its common name, left behind by an earlier
  /// session, and not created by this adapter. Valid when @ref ready.
  bool reattached() const;

  /// @brief The common name the panel has: the one of the description, or that with a suffix
  /// when the name was taken by a panel that could not be reused.
  std::string common_name() const;

  /// @brief Reserves a frame cycle once @ref pending is false: while the creation is still
  /// running (probe, reattach) as well as after it. A creation that fails ends the frames at the
  /// next call.
  bool try_begin_frame(bool & out_ready) override;

  /// @brief Which of the five answers the last @ref try_begin_frame gave when it reported no
  /// destination: success without a reserved cycle, no active transfers (604), a transfer that
  /// is reconnecting (603), a stream the receiver paused (602), or a panel that is still being
  /// made. The first time a sender that has shown frames hears 603, one line says so, with the
  /// number of frames shown; the first 602 is noted in one line as well.
  qar_ar_core::PanelNotReadyCause not_ready_cause() const override { return not_ready_cause_; }

  bool submit_frame(const qar_ar_core::PanelPixels & pixels) override;

  /// @brief Queues the pose and size for the panel; of several placements asked for before the
  /// task runs, the last one is applied. Refused (false, @ref last_error says why) when the panel
  /// is closed or failed. The outcome arrives as PanelOpResult::Op::Placement.
  bool set_placement(const qar_ar_core::PanelPlacement & placement) override;

  /// @brief Queues one `qar_gui_panels_set_state` on this panel: hide, show or minimise it while
  /// the panel, the video sender and the subscriptions all stay. Refused (false, @ref last_error
  /// says why) when the panel is closed or failed. The outcome arrives as
  /// PanelOpResult::Op::State; each change that succeeds is printed to stderr in the style of
  /// the creation line. The adapter remembers the state it asked for. When the runtime reports
  /// the opposite one (HIDDEN for VISIBLE or the reverse) shortly after, it asks once more, at
  /// most 3 times within 10 seconds and only within 5 seconds of its own change; a state the
  /// operator sets later in AR, and MINIMIZED or CLOSED, are never contested.
  bool set_state(QarGuiPanelState state) override;
  bool supports_state() const override { return true; }

  /// @brief Replaces the clock by which the adapter decides whether a report of the runtime that
  /// contradicts the state it set is still contested (5 seconds after its own change, at most
  /// 3 asks within 10 seconds). The steady clock is the default. For tests.
  void set_state_guard_clock(PanelStateKeeper::ClockFn now);

  /// @brief The one teardown, in the one safe order — see
  /// IQarPanelSenderAdapter::close for the contract, and the body of the task for the lines it
  /// prints. From this call on the adapter makes no frame call; the teardown itself runs as a
  /// task and the report says `Queued`. The adapter's own watch of the panel's state (see
  /// @ref set_state) ends before the three steps, so the republishing that the close causes asks
  /// for nothing:
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

  /// @brief Steps 1 and 3 of @ref close without step 2: the input subscriptions are cancelled
  /// and the video sender is destroyed, and the QarOS panel is left as it is (the caller hides
  /// it first), so the next session reattaches it under its common name. Idempotent. The tasks
  /// of a later adapter for the same panel run after this one's.
  void detach() override;
  bool supports_detach() const override { return true; }
  const std::string & last_error() const override { return last_error_; }
  /// @brief Canonical UUID of `qar_video_sender_stream_id`, the id the runtime
  /// names this panel's content by everywhere. Empty until the sender exists;
  /// kept after close() so a retired panel can still be identified.
  std::string stream_id_text() const override;

  /// @brief Moves the outcomes of the create, placement and state tasks since the last call
  /// into @p out. The consumer calls it on every tick; it also lets the state guard ask again
  /// when the hold of its last ask has ended (see panel_state_guard.hpp).
  void drain_results(std::vector<qar_ar_core::PanelOpResult> & out) override;

private:
  /// What the tasks and the GUI thread share. Defined in the .cpp.
  struct Core;

  /// The tasks. Each runs on the thread that owns the session.
  static void run_create(QarSession * session, const std::shared_ptr<Core> & core);
  static void run_place(QarSession * session, const std::shared_ptr<Core> & core);
  static void run_state(
    QarSession * session, const std::shared_ptr<Core> & core, QarGuiPanelState state);
  static void run_teardown(
    QarSession * session, const std::shared_ptr<Core> & core, bool panel_closed_in_ar,
    bool keep_panel);

  /// The one teardown behind close() and detach(); @p keep_panel skips step 2.
  qar_ar_core::PanelCloseReport teardown(bool panel_closed_in_ar, bool keep_panel);

  /// Marks the panel as refused by the constructor.
  void refuse(const std::string & reason);

  IQarSessionOwner * owner_ = nullptr;
  qar_ar_core::PanelDesc desc_;
  /// Null when the host wants a stream-only panel (input off).
  std::shared_ptr<QarCApiPanelInputSource> input_;
  std::shared_ptr<Core> core_;

  // The members below belong to the GUI thread.
  /// The video sender, as the GUI thread learned it when the panel became ready.
  QarVideoSender * sender_ = nullptr;
  bool refused_ = false;
  /// Set by the first close(). Every later one returns an empty report without
  /// touching the runtime or the input source.
  bool closed_ = false;
  bool frame_reserved_ = false;
  std::string last_error_;
  /// The answer of the last try_begin_frame when it reported no destination.
  qar_ar_core::PanelNotReadyCause not_ready_cause_ = qar_ar_core::PanelNotReadyCause::NoDestination;
  /// Frames this sender has shown (a show_frame that succeeded).
  std::uint64_t frames_shown_ = 0;
  /// Lets the first frame shown print its line, and the first 603 after a frame print its line.
  LogOnce first_show_noted_;
  LogOnce reconnect_noted_;
  LogOnce paused_noted_;
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
