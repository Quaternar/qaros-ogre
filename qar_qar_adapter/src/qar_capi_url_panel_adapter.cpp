// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"

#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"

#include <cstdio>

namespace qar_qar_adapter
{
namespace
{

/// The line for one change of panel state, on stderr like the stream-panel adapter's: the
/// outcome of every hide and show of a URL panel, successful or not, is in the log. The panel
/// is named by its title; its URL is not printed, because it carries the access token of the
/// page. A null @p id is an adapter with no panel.
void
log_url_panel_state(
  const std::string & title, const QarGuiPanelId * id, QarGuiPanelState state,
  const std::string & error)
{
  const std::string line = url_panel_state_line(
    wall_clock_hms_ms(), title.empty() ? std::string("untitled") : title,
    id != nullptr ? panel_id_uuid(*id) : std::string("none"), state, error);
  std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
}

/// Same shape as the other adapters' helper: a readable "<call> failed: ..."
/// line, also echoed to stderr because the panel UI only shows the last error.
std::string
url_panel_result_error(const char * what, const QarResult & r)
{
  char buf[512] = {0};
  qar_result_message(r, buf, sizeof(buf) - 1);
  std::string out = std::string(what) + " failed: code=" +
    std::to_string(static_cast<long long>(r.code));
  if (buf[0] != '\0') {
    out += " msg=";
    out += buf;
  }
  std::fprintf(stderr, "[qar-url-panel-adapter] *** %s\n", out.c_str());
  std::fflush(stderr);
  return out;
}

}  // namespace

QarCApiUrlPanelAdapter::QarCApiUrlPanelAdapter(
  QarSession * session, const qar_ar_core::UrlPanelDesc & desc)
: session_(session),
  desc_(desc)
{
  bootstrap();
}

QarCApiUrlPanelAdapter::~QarCApiUrlPanelAdapter()
{
  close();
}

void
QarCApiUrlPanelAdapter::bootstrap()
{
  if (session_ == nullptr) {
    last_error_ = "no QarSession (is QAR streaming started?)";
    return;
  }
  if (desc_.url.empty()) {
    last_error_ = "panel URL must not be empty";
    return;
  }
  if (!(desc_.placement.size.width_meters > 0.0f) || !(desc_.placement.size.height_meters > 0.0f)) {
    last_error_ = "panel size must be positive";
    return;
  }

  // The panel's content IS the URL: chain QarGuiPanelUrlExt onto the init.
  // QarOS copies the string during add_panel, so a stack extension is fine.
  // Without any extension the runtime would open its default website instead.
  QarGuiPanelUrlExt url_ext = qar_gui_panel_url_ext_default();
  url_ext.url = desc_.url.c_str();

  QarGuiPanelInit panel_init = qar_gui_panel_init_default();
  panel_init.header.next = &url_ext;
  panel_init.display_name = desc_.display_name.empty() ? nullptr : desc_.display_name.c_str();
  panel_init.pose = desc_.placement.pose;
  panel_init.size = desc_.placement.size;
  // An empty list leaves the init's NULL / 0: every peer sees the panel. desc_ is a member, so
  // the array outlives the add_panel call below.
  if (!desc_.visible_to_peers.empty()) {
    panel_init.visible_to_peers = desc_.visible_to_peers.data();
    panel_init.visible_to_peer_count = desc_.visible_to_peers.size();
  }

  const QarResult r = qar_gui_panels_add_panel(session_, &panel_init, &panel_id_);
  if (qar_result_is_error(r)) {
    last_error_ = url_panel_result_error("qar_gui_panels_add_panel", r);
    return;
  }

  has_panel_ = true;
  ready_ = true;
  last_error_.clear();
}

bool
QarCApiUrlPanelAdapter::navigate(const std::string & url)
{
  if (!has_panel_ || session_ == nullptr) {
    last_error_ = "no live panel to navigate";
    return false;
  }
  if (url.empty()) {
    last_error_ = "panel URL must not be empty";
    return false;
  }
  const QarResult r = qar_gui_panels_navigate_to_uri(session_, &panel_id_, url.c_str());
  if (qar_result_is_error(r)) {
    last_error_ = url_panel_result_error("qar_gui_panels_navigate_to_uri", r);
    return false;
  }
  desc_.url = url;
  last_error_.clear();
  return true;
}

bool
QarCApiUrlPanelAdapter::set_placement(const qar_ar_core::UrlPanelPlacement & placement)
{
  if (!has_panel_ || session_ == nullptr) {
    last_error_ = "no live panel to place";
    return false;
  }

  const QarResult posed = qar_gui_panels_update_pose(session_, &panel_id_, &placement.pose);
  if (qar_result_is_error(posed)) {
    last_error_ = url_panel_result_error("qar_gui_panels_update_pose", posed);
    return false;
  }

  const QarResult sized = qar_gui_panels_change_size(session_, &panel_id_, &placement.size);
  if (qar_result_is_error(sized)) {
    last_error_ = url_panel_result_error("qar_gui_panels_change_size", sized);
    return false;
  }

  desc_.placement = placement;
  last_error_.clear();
  return true;
}

bool
QarCApiUrlPanelAdapter::set_state(QarGuiPanelState state)
{
  if (!has_panel_ || session_ == nullptr) {
    last_error_ = "no live panel to change";
    log_url_panel_state(desc_.display_name, nullptr, state, last_error_);
    return false;
  }
  const QarResult r = qar_gui_panels_set_state(session_, &panel_id_, state);
  if (qar_result_is_error(r)) {
    last_error_ = url_panel_result_error("qar_gui_panels_set_state", r);
    log_url_panel_state(desc_.display_name, &panel_id_, state, last_error_);
    return false;
  }
  last_error_.clear();
  log_url_panel_state(desc_.display_name, &panel_id_, state, std::string());
  return true;
}

void
QarCApiUrlPanelAdapter::close()
{
  if (has_panel_ && session_ != nullptr) {
    // Checked and reported like the close of a stream panel: a close that failed leaves
    // the panel in the session, and last_error() says so.
    const QarResult closed = qar_gui_panels_close_panel(session_, &panel_id_);
    if (qar_result_is_error(closed)) {
      last_error_ = url_panel_result_error("qar_gui_panels_close_panel", closed);
    }
    has_panel_ = false;
  }
  ready_ = false;
}

QarCApiUrlPanelFactory::QarCApiUrlPanelFactory(IQarSessionOwner * session_owner)
: session_owner_(session_owner)
{
}

std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter>
QarCApiUrlPanelFactory::create_url_panel(const qar_ar_core::UrlPanelDesc & desc)
{
  if (session_owner_ == nullptr) {
    last_error_ = "no QAR session owner";
    return nullptr;
  }
  if (!session_owner_->ready()) {
    last_error_ = "QAR session is not ready yet";
    return nullptr;
  }
  QarSession * session = session_owner_->session();
  if (session == nullptr) {
    last_error_ = "QAR session handle is null";
    return nullptr;
  }

  auto adapter = std::make_shared<QarCApiUrlPanelAdapter>(session, desc);
  if (!adapter->ready()) {
    last_error_ = adapter->last_error();
    return nullptr;
  }
  last_error_.clear();
  return adapter;
}

}  // namespace qar_qar_adapter
