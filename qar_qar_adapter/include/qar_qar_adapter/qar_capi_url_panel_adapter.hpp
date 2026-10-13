// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// The QarOS side of the web-panel path — sibling of
// QarCApiPanelSenderAdapter, which does the same job for streamed Qt widgets.
//
// One SDK object backs one AR panel here: a QarGuiPanel created with a
// QarGuiPanelUrlExt chained onto QarGuiPanelInit, so its content is a URL that
// QarOS renders in its OWN browser (QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI).
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

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/qar_url_panel.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief One QarOS GUI panel whose content is a URL.
///
/// Lifetime is create -> N x (navigate / set_placement / set_state) -> close.
/// Every call happens on the host's GUI thread and is a synchronous QarOS call; none
/// of the C calls involved streams anything, so there is nothing to pace.
class QarCApiUrlPanelAdapter : public qar_ar_core::IQarUrlPanelAdapter
{
public:
  /// @param session Borrowed, owned by QarCApiSenderFactory; must outlive this.
  QarCApiUrlPanelAdapter(QarSession * session, const qar_ar_core::UrlPanelDesc & desc);
  ~QarCApiUrlPanelAdapter() override;

  QarCApiUrlPanelAdapter(const QarCApiUrlPanelAdapter &) = delete;
  QarCApiUrlPanelAdapter & operator=(const QarCApiUrlPanelAdapter &) = delete;

  /// @brief True once the panel exists in the session.
  bool ready() const { return ready_; }

  /// @brief The panel id QarOS assigned, valid only when @ref ready.
  const QarGuiPanelId & panel_id() const { return panel_id_; }

  /// @brief The URL the panel currently shows (as last accepted by QarOS).
  const std::string & url() const { return desc_.url; }

  bool navigate(const std::string & url) override;
  bool set_placement(const qar_ar_core::UrlPanelPlacement & placement) override;
  bool set_state(QarGuiPanelState state) override;
  void close() override;
  const std::string & last_error() const override { return last_error_; }

private:
  /// Chain QarGuiPanelUrlExt onto QarGuiPanelInit and add the panel. Records
  /// the failing step in last_error_ and leaves ready_ false.
  void bootstrap();

  QarSession * session_ = nullptr;
  qar_ar_core::UrlPanelDesc desc_;

  QarGuiPanelId panel_id_{};
  bool has_panel_ = false;
  bool ready_ = false;
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
