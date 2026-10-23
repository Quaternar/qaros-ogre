// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__QAR_PANEL_SENDER_HPP_
#define QAR_AR_CORE__QAR_PANEL_SENDER_HPP_

#include <memory>
#include <string>

#include "qar_ar_core/panel_types.hpp"

namespace qar_ar_core
{

/// @brief What became of the QarOS GUI panel when an adapter was closed.
enum class PanelCloseOutcome
{
  /// There was no QarOS panel to close: creation never got that far, or this
  /// adapter had already been closed.
  NoPanel,
  /// `qar_gui_panels_close_panel` ran and succeeded.
  Closed,
  /// The operator had already closed the panel from the AR side, so the call was
  /// SKIPPED. A panel is closed once: asking the runtime to close one it has
  /// already removed is, at best, an error to explain away.
  AlreadyClosedInAr,
  /// `qar_gui_panels_close_panel` ran and returned an error. See
  /// PanelCloseReport::panel_message. The sender is destroyed regardless.
  Failed
};

/// @brief "none" / "ran" / "skipped (closed in AR)" / "FAILED" — for logs.
///
/// Inline: this header is the Qt-free seam that qar_qar_adapter implements and
/// qar_qt_streaming consumes, so it carries its own definitions.
inline const char *
to_string(PanelCloseOutcome outcome)
{
  switch (outcome) {
    case PanelCloseOutcome::NoPanel:
      return "none";
    case PanelCloseOutcome::Closed:
      return "ran";
    case PanelCloseOutcome::AlreadyClosedInAr:
      return "skipped (closed in AR)";
    case PanelCloseOutcome::Failed:
      return "FAILED";
  }
  return "none";
}

/// @brief What IQarPanelSenderAdapter::close actually did, step by step, so the
/// caller can log it instead of guessing.
struct PanelCloseReport
{
  /// Step 1: the panel's input/update subscriptions were cancelled (there was
  /// one to cancel).
  bool input_unsubscribed = false;
  /// Step 2: what happened to the QarOS GUI panel.
  PanelCloseOutcome panel = PanelCloseOutcome::NoPanel;
  /// The runtime's own words when @ref panel is Failed; empty otherwise.
  std::string panel_message;
  /// Step 3: a video sender existed and was destroyed.
  bool sender_destroyed = false;
};

/// @brief Narrow adapter over ONE QarOS GUI panel and its backing video stream.
///
/// qar_qt_streaming talks to QarOS ONLY through this interface, mirroring
/// qar_ogre_streaming's IQarRenderSenderAdapter. One implementation lives in
/// `qar_qar_adapter` (QarCApiPanelSenderAdapter, wrapping QarVideoSender +
/// qar_gui_panels_add_panel); a recording test double stands in for
/// unit tests.
///
/// Per-tick contract, all on the Qt GUI thread:
///   try_begin_frame(ready) -> if ready: submit_frame(pixels)
/// `try_begin_frame` maps to `qar_video_sender_try_begin_frame`, which is
/// documented as non-blocking and reports `ready == false` whenever no receiver
/// has a destination reserved. That is the entire back-pressure story, and it is
/// why this path can run on the GUI thread without ever stalling it.
class IQarPanelSenderAdapter
{
public:
  virtual ~IQarPanelSenderAdapter() = default;

  /// @brief Reserve a send cycle if QarOS has a destination ready.
  /// @param[out] out_ready true when a cycle was reserved and @ref submit_frame
  ///        must follow this tick; false to skip (poll again next tick).
  /// @return false only on a real error (the reason lands in @ref last_error).
  virtual bool try_begin_frame(bool & out_ready) = 0;

  /// @brief Why the most recent @ref try_begin_frame reported `out_ready == false`. Only
  /// meaningful right after such a call. The default, for an implementation that cannot tell,
  /// is PanelNotReadyCause::NoDestination.
  virtual PanelNotReadyCause not_ready_cause() const { return PanelNotReadyCause::NoDestination; }

  /// @brief Copy @p pixels into the reserved frame and show it.
  /// Only legal directly after a `try_begin_frame` that set `out_ready`.
  virtual bool submit_frame(const PanelPixels & pixels) = 0;

  /// @brief Push a new pose and/or physical size to the live panel.
  virtual bool set_placement(const PanelPlacement & placement) = 0;

  /// @brief Show, minimise or hide the QarOS panel (`qar_gui_panels_set_state`), keeping the
  /// panel, its video sender and its subscriptions.
  ///
  /// The default refuses, so an implementation without panel state keeps compiling and the
  /// caller falls back to closing the panel. See @ref supports_state.
  /// @return false when the state was not changed.
  virtual bool set_state(QarGuiPanelState state) { (void)state; return false; }

  /// @brief True when @ref set_state can change the state of the panel. The default is false.
  virtual bool supports_state() const { return false; }

  /// @brief Tear the panel down: retire its input subscriptions, close the QarOS
  /// panel, destroy the video sender. Idempotent.
  ///
  /// **The order is the contract**, and it is the only safe one:
  ///
  ///  1. **Cancel the panel's input/update subscriptions first.** The runtime
  ///     republishes a panel whose `close_panel` we call as state CLOSED; with the
  ///     subscription still live that echo would come back as "the operator closed
  ///     this panel in AR", and a late gesture callback could reach a slot that is
  ///     being retired. Cancelled first, neither can happen.
  ///  2. **Close the QarOS GUI panel** — *unless* @p panel_closed_in_ar. A panel
  ///     the operator already closed is closed once; this never calls
  ///     `qar_gui_panels_close_panel` a second time for it. A failure here is
  ///     reported (PanelCloseOutcome::Failed) and does NOT stop step 3.
  ///  3. **Destroy the video sender.** The panel was the sender's consumer (a
  ///     stream panel takes its receiver resolution from "the session-owned
  ///     sender identified by stream_id"), so the consumer goes first.
  ///
  /// @param panel_closed_in_ar The runtime already reported this panel closed
  ///        from the AR side; skip step 2.
  /// @return what each step did. A second call returns an empty report
  ///         (everything false / NoPanel) and touches nothing.
  virtual PanelCloseReport close(bool panel_closed_in_ar) = 0;

  /// @brief close(false): the panel is still open on the AR side.
  ///
  /// Not virtual — it only names the common case, so the callers that have no AR
  /// state to pass (a resize rebuild) read as they always did.
  PanelCloseReport close() { return close(false); }

  /// @brief Reason for the most recent failure; empty when healthy.
  virtual const std::string & last_error() const = 0;

  /// @brief Identity of the logical video stream this panel's content IS, in
  /// whatever form the runtime prints it (the real adapter returns the
  /// canonical UUID of `qar_video_sender_stream_id`). Empty when the backend
  /// has no stream identity, or before one was obtained.
  ///
  /// **Not cosmetic.** Everything the runtime says about a panel's content —
  /// the `streamCatalog/<id>` entries, the `streamRequest/<id>` queries a
  /// receiver retries, the `ReconnectableVideoReceiver` lines that name a
  /// stream it cannot reach — is keyed by this id and by nothing else.
  /// Without it printed next to the panel key, matching a runtime trace to the
  /// widget it is about means byte-comparing a UUID against a mangled id dump.
  /// See the note on retirement in qar_qt_streaming's QtPanelStreamManager.
  virtual std::string stream_id_text() const { return std::string(); }
};

/// @brief Factory that creates one IQarPanelSenderAdapter per panel.
///
/// Supplied through qar_qt_streaming's QtPanelStreamingConfig, so that library never
/// constructs QarOS objects itself. The real implementation borrows the QarSession that
/// `qar_qar_adapter`'s QarCApiSenderFactory already owns — required, not merely
/// convenient: QarGuiPanelStreamIdExt only accepts a stream id from a video
/// sender owned by the SAME session.
class IQarPanelSenderFactory
{
public:
  virtual ~IQarPanelSenderFactory() = default;

  /// @brief Create a panel sender for @p desc.
  /// @return a non-null adapter on success, or nullptr if it could not be
  ///         created (the manager surfaces this as a panel-creation error).
  virtual std::shared_ptr<IQarPanelSenderAdapter> create_panel_sender(
    const PanelDesc & desc) = 0;

  /// @brief Reason the most recent create_panel_sender() returned null.
  virtual const std::string & last_error() const = 0;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__QAR_PANEL_SENDER_HPP_
