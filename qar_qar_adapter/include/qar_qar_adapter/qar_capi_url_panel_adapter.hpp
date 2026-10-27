// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The QarOS side of the web-panel path — sibling of
// QarCApiPanelSenderAdapter, which does the same job for streamed Qt widgets.
//
// One SDK object backs one AR panel here: a QarGuiPanel created with a
// QarGuiPanelUrlExt chained onto QarGuiPanelInit, so its content is a URL that
// QarOS renders in its OWN browser (QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI). The panel is
// found or created under its common name: one that an earlier session left behind is
// reattached (navigated to the URL when that differs, sized, posed and shown again).
// There is no video sender and no frame loop: the panel's browser fetches the
// page from qar_rviz_plugin's qar_web_panels server and handles pointer input itself, and every
// interaction reaches us over that server's WebSocket rather than through any
// QarOS input API. That is what makes a web panel interactive.
//
// The panel borrows the session QarCApiSenderFactory already owns: a panel
// belongs to a session, and the render path's session is the one the headset
// is attached to. This file stays Qt-FREE, like the rest of the adapter: the
// seam it implements (qar_ar_core/qar_url_panel.hpp) is POD-only.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_URL_PANEL_ADAPTER_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_URL_PANEL_ADAPTER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/qar_url_panel.hpp"
#include "qar_qar_adapter/panel_state_guard.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief One QarOS GUI panel whose content is a URL.
///
/// Lifetime is create -> N x (navigate / set_placement / set_state) -> close. Every call of this
/// class is for the host's GUI thread, and none of them waits for the Hub: the panel is made and
/// changed by tasks on the thread that owns the session (see
/// IQarSessionOwner::post_session_task), in the order the calls were made, and the outcomes come
/// back through @ref drain_results. None of the C calls involved streams anything, so there is
/// nothing to pace.
class QarCApiUrlPanelAdapter : public qar_ar_core::IQarUrlPanelAdapter
{
public:
  /// @param owner Borrowed; must outlive this. The panel is made on the session it owns.
  QarCApiUrlPanelAdapter(IQarSessionOwner * owner, const qar_ar_core::UrlPanelDesc & desc);
  ~QarCApiUrlPanelAdapter() override;

  QarCApiUrlPanelAdapter(const QarCApiUrlPanelAdapter &) = delete;
  QarCApiUrlPanelAdapter & operator=(const QarCApiUrlPanelAdapter &) = delete;

  /// @brief True when the constructor refused the panel (no owner, a bad description, or no
  /// session accepting calls); @ref last_error says why. A panel that was accepted and fails
  /// later reports it through @ref drain_results.
  bool refused() const { return refused_; }

  /// @brief True once the panel exists in the session.
  bool ready() const;

  /// @brief True while the panel is still being made.
  bool pending() const override;

  /// @brief The panel id QarOS assigned, valid only when @ref ready.
  QarGuiPanelId panel_id() const;

  /// @brief True when the panel was found under its common name, left behind by an earlier
  /// session, and not created by this adapter. Valid when @ref ready.
  bool reattached() const;

  /// @brief The common name the panel has: the one of the description, or that with a suffix
  /// when the name was taken by a panel that could not be reused.
  std::string common_name() const;

  /// @brief The URL the panel currently shows (as last accepted by QarOS).
  std::string url() const;

  /// @brief Each of the next four queues the call and returns at once; true means accepted. They
  /// are refused (false, @ref last_error says why) when the panel is closed or failed. The outcome
  /// arrives as PanelOpResult::Op::Navigate, Placement and State.
  bool navigate(const std::string & url) override;
  /// Of several placements asked for before the task runs, the last one is applied.
  bool set_placement(const qar_ar_core::UrlPanelPlacement & placement) override;
  /// The adapter remembers the state it asked for. When the runtime reports the opposite one
  /// (HIDDEN for VISIBLE or the reverse) shortly after, it asks once more, at most 3 times within
  /// 10 seconds and only within 5 seconds of its own change; a state the operator sets later in
  /// AR, and MINIMIZED or CLOSED, are never contested.
  bool set_state(QarGuiPanelState state) override;

  /// @brief Replaces the clock by which the adapter decides whether a report of the runtime that
  /// contradicts the state it set is still contested. The steady clock is the default. For tests.
  void set_state_guard_clock(PanelStateKeeper::ClockFn now);

  /// @brief Queues the close of the QarOS panel. Idempotent.
  void close() override;

  /// @brief Lets go of the adapter and leaves the QarOS panel as it is, so the next session
  /// reattaches it. The caller hides the panel first; that call, queued ahead, still runs.
  /// Idempotent.
  void detach() override;
  bool supports_detach() const override { return true; }

  const std::string & last_error() const override { return last_error_; }

  /// @brief Moves the outcomes of the create, navigate, placement and state tasks since the last
  /// call into @p out. The consumer calls it on every tick; it also lets the state guard ask again
  /// when the hold of its last ask has ended (see panel_state_guard.hpp).
  void drain_results(std::vector<qar_ar_core::PanelOpResult> & out) override;

private:
  /// What the tasks and the GUI thread share. Defined in the .cpp.
  struct Core;

  /// The tasks. Each runs on the thread that owns the session.
  static void run_create(QarSession * session, const std::shared_ptr<Core> & core);
  static void run_navigate(
    QarSession * session, const std::shared_ptr<Core> & core, const std::string & url);
  static void run_place(QarSession * session, const std::shared_ptr<Core> & core);
  static void run_state(
    QarSession * session, const std::shared_ptr<Core> & core, QarGuiPanelState state);
  static void run_close(QarSession * session, const std::shared_ptr<Core> & core);

  /// True while the panel can take a call; else records @p what as the last error.
  bool live(const char * what);
  void refuse(const std::string & reason);

  IQarSessionOwner * owner_ = nullptr;
  /// The title of the panel, for the names of the tasks and the log lines.
  std::string title_;
  std::shared_ptr<Core> core_;

  // The members below belong to the GUI thread.
  bool refused_ = false;
  bool closed_ = false;
  std::string last_error_;
};

/// @brief Mints QarCApiUrlPanelAdapters against a live QarCApiSenderFactory.
///
/// Holds only a raw pointer to that factory: the RViz panel owns both, and the
/// factory always outlives the panels (it is torn down last).
class QarCApiUrlPanelFactory : public qar_ar_core::IQarUrlPanelFactory
{
public:
  /// @param session_owner Borrowed; must outlive this factory.
  explicit QarCApiUrlPanelFactory(IQarSessionOwner * session_owner);

  std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter> create_url_panel(
    const qar_ar_core::UrlPanelDesc & desc) override;

  const std::string & last_error() const override { return last_error_; }

private:
  IQarSessionOwner * session_owner_ = nullptr;
  std::string last_error_;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_URL_PANEL_ADAPTER_HPP_
