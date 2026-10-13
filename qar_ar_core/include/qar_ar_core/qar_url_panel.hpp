// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__QAR_URL_PANEL_HPP_
#define QAR_AR_CORE__QAR_URL_PANEL_HPP_

/// @file qar_url_panel.hpp
/// @brief The seam TO QarOS for browser-backed panels — the URL sibling of
/// IQarPanelSenderAdapter (qar_panel_sender.hpp), the seam of the streamed panels.
///
/// A web panel is a QarOS GUI panel whose content is a URL rendered by the
/// runtime's own browser (`QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI`,
/// `QarGuiPanelUrlExt`). The code that serves the page and speaks the wire protocol,
/// qar_rviz_plugin's qar_web_panels, never creates the panel itself: a host hands the
/// URL to whoever implements this interface. The real implementation lives in
/// `qar_qar_adapter` (QarCApiUrlPanelAdapter), the only code that talks to the C API.
/// Deliberately Qt-free and POD-only, like every seam in this repo, so the adapter
/// does not gain a Qt dependency.

#include <memory>
#include <string>

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_ar_core
{

/// @brief Where and how big the panel is in the room. Reuses the Qar C types
/// verbatim so values reach the ABI with no conversion layer.
struct UrlPanelPlacement
{
  /// Pose of the panel in room space: {orientation, position}. It is passed unchanged
  /// to `QarGuiPanelInit::pose` and `qar_gui_panels_update_pose`, so it is the pose of
  /// QarOS panel space, whose origin is the midpoint of the panel's TOP EDGE and not
  /// its centre: the surface hangs BELOW it, because `QarGuiPanelSize` puts panel-space
  /// y in [-height_meters, 0].
  QarPose pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  /// Physical size in metres + content scale.
  ///
  /// The stream route derives the size from the widget; a URL panel has no
  /// widget, but it does have a MEASURABLE page — qar_rviz_plugin's
  /// `qar_ui_model::measure_page` says how many CSS pixels the generated
  /// content wants and `url_panel_sizing.hpp` of its qar_web_panels turns that into
  /// metres at the same arc-minutes-of-text the QWidget route uses. Callers should
  /// fill this from `size_url_panel_for(model, theme)`; the value here is only the
  /// fallback for a panel opened before anything has been measured, and is
  /// deliberately portrait because a generated form is.
  QarGuiPanelSize size{0.50f, 0.75f, 1.0f};
};

/// @brief Everything a factory needs to open one browser-backed panel.
struct UrlPanelDesc
{
  std::string url;           ///< What the panel's browser loads. Copied by QarOS at add time.
  std::string display_name;  ///< Shown by QarOS as the panel's title. Not an identifier.
  UrlPanelPlacement placement;
};

/// @brief Narrow adapter over ONE QarOS URL panel.
///
/// Lifetime is create -> N x (navigate / set_placement / set_state) -> close.
/// Every call is expected on the host's GUI thread; none of the underlying C
/// calls streams anything, so none is paced or back-pressured.
class IQarUrlPanelAdapter
{
public:
  virtual ~IQarUrlPanelAdapter() = default;

  /// @brief Point the live panel at another URL (`qar_gui_panels_navigate_to_uri`).
  virtual bool navigate(const std::string & url) = 0;

  /// @brief Push a new pose and/or physical size to the live panel.
  virtual bool set_placement(const UrlPanelPlacement & placement) = 0;

  /// @brief Show / minimise / hide the panel (`qar_gui_panels_set_state`).
  virtual bool set_state(QarGuiPanelState state) = 0;

  /// @brief Close the QarOS panel. Idempotent.
  virtual void close() = 0;

  /// @brief Reason for the most recent failure; empty when healthy.
  virtual const std::string & last_error() const = 0;
};

/// @brief Creates one IQarUrlPanelAdapter per panel.
///
/// The real implementation borrows the QarSession that `qar_qar_adapter`'s
/// QarCApiSenderFactory already owns: a panel belongs to a session, and the
/// render path's session is the one the headset is attached to.
class IQarUrlPanelFactory
{
public:
  virtual ~IQarUrlPanelFactory() = default;

  /// @return a non-null adapter on success, nullptr with @ref last_error set otherwise.
  virtual std::shared_ptr<IQarUrlPanelAdapter> create_url_panel(const UrlPanelDesc & desc) = 0;

  virtual const std::string & last_error() const = 0;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__QAR_URL_PANEL_HPP_
