// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Snippets of API.md for the module qar_qar_adapter: the session owner, the configuration and use of the render
// factory, the panel, URL panel and gesture components, and the environment helpers. The functions are compiled
// and linked and never run. Their parameters stand for what the host owns, so each one is a pointer that the
// documentation names in its text.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/panel_types.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/spatial_frames.hpp"
#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/qar_capi_panel_input_source.hpp"
#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"
#include "qar_qar_adapter/qar_capi_spatial_gesture_source.hpp"
#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"

// [qar-snippet:session_owner]
// A host that streams panels without a 3D scene opens its own QarOS session and answers the two questions of
// the owner interface. The panel factories borrow the session; they never destroy it.
class HostSessionOwner : public qar_qar_adapter::IQarSessionOwner
{
public:
  explicit HostSessionOwner(QarSession * session) : session_(session) {}

  bool ready() const override { return session_ != nullptr; }
  QarSession * session() const override { return session_; }

private:
  QarSession * session_;   // created and joined by the host, and destroyed by it after the last panel
};
// [qar-snippet-end:session_owner]

void snippet_factory_config(const std::string & sdk_bin)
{
  // [qar-snippet:factory_config]
  qar_qar_adapter::QarCApiSenderFactory::Config config;
  config.runtime_binaries_folder_path = sdk_bin;                   // the bin folder of the SDK
  config.copy_hook = qar_qar_adapter::make_gl_copy_image_hook();   // without it QarOS shows a black scene
  config.near_far = QarNearFar{0.1f, 200.0f};                      // reported with every frame
  config.admission = qar_qar_adapter::PeerAdmission::kInvitedOnly;
  config.max_peers = 4;                                            // a fifth peer is refused with a warning
  config.invite_peers = "hololens:HOLOLENS-A2B3,quest";            // invited as soon as the session is up
  config.app_volume_name = "Fleet map";
  config.app_volume_width_m = 20.0f;                               // the box of the scene, in metres
  config.app_volume_length_m = 20.0f;
  config.app_volume_height_m = 10.0f;
  config.app_volume_center_y_m = 5.0f;                             // the centre of the box above room y = 0
  config.log_severity = QAR_LOG_SEVERITY_WARN;
  config.enable_console_logging = false;
  // [qar-snippet-end:factory_config]
  const auto factory = std::make_shared<qar_qar_adapter::QarCApiSenderFactory>(config);
  std::printf("ready: %d\n", factory->ready() ? 1 : 0);
}

void snippet_factory_use(qar_qar_adapter::QarCApiSenderFactory * factory)
{
  // [qar-snippet:factory_use]
  if (!factory->ready()) {
    std::printf("QarOS: %s\n", factory->last_error().c_str());   // the bootstrap failed: nothing else works
    return;
  }

  // Invite one more device into the running session; its peer id joins the allow-list.
  QarPeerId invited{};
  const std::string refused = factory->invite_additional_peer(
    qar_qar_adapter::PeerKind::kHoloLens, "HOLOLENS-C4D5", invited);

  // Georeference the app volume: latitude, longitude, and the height above the WGS84 ellipsoid.
  const std::string anchor_error = factory->set_geo_anchor_wgs84(47.3769, 8.5417, 408.0);
  std::printf("invite: %s, geo anchor: %s\n", refused.c_str(), anchor_error.c_str());   // empty: it worked
  // [qar-snippet-end:factory_use]
}

void snippet_panels(qar_qar_adapter::IQarSessionOwner * owner)
{
  // [qar-snippet:panels]
  // One input source serves all panels; the factory hands it to every panel that it creates.
  auto input = std::make_shared<qar_qar_adapter::QarCApiPanelInputSource>(owner->session());
  qar_qar_adapter::QarCApiPanelSenderFactory factory(owner, input);

  qar_ar_core::PanelDesc desc;
  desc.key = "status";
  desc.buffer_width = 1024;
  desc.buffer_height = 768;
  desc.content_width = 1000;                           // the part of the buffer that the widget paints;
  desc.content_height = 760;                           // zero means the whole buffer
  const std::shared_ptr<qar_ar_core::IQarPanelSenderAdapter> sender = factory.create_panel_sender(desc);
  if (!sender) {
    std::printf("panel: %s\n", factory.last_error().c_str());
    return;
  }

  // The panel is subscribed to its gestures. On the GUI thread, once for each tick:
  std::vector<qar_ar_core::PanelPointerSample> samples;
  input->drain(samples);
  std::vector<std::string> closed;
  input->drain_closed_panels(closed);                  // the keys of the panels that the operator closed in AR
  std::printf("%zu samples, %zu closed, %zu subscriptions\n", samples.size(), closed.size(),
    input->subscription_count());
  // [qar-snippet-end:panels]
}

void snippet_url_panels(qar_qar_adapter::IQarSessionOwner * owner)
{
  // [qar-snippet:url_panels]
  qar_qar_adapter::QarCApiUrlPanelFactory factory(owner);
  qar_ar_core::UrlPanelDesc desc;
  desc.url = "http://127.0.0.1:8080/controls";         // the browser of QarOS loads this page
  const std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter> panel = factory.create_url_panel(desc);
  if (panel) {
    panel->set_state(QAR_GUI_PANEL_STATE_MINIMIZED);
    panel->close();
  } else {
    std::printf("url panel: %s\n", factory.last_error().c_str());
  }
  // [qar-snippet-end:url_panels]
}

void snippet_gestures(qar_qar_adapter::QarCApiSenderFactory * factory)
{
  // [qar-snippet:gestures]
  QarAppVolumeId volume{};
  if (!factory->app_volume(volume)) {
    return;                                            // no app volume, so no gestures to subscribe to
  }
  qar_qar_adapter::QarCApiSpatialGestureSource gestures(factory->session());
  gestures.set_pointing_ray_slot(factory->pointing_ray_slot());   // the ray from the hands, as a fallback
  if (!gestures.subscribe(volume)) {
    std::printf("gestures: %s\n", gestures.last_error().c_str());
    return;
  }

  std::vector<qar_ar_core::PanelPointerSample> samples;
  gestures.drain(samples);                             // on the thread of the host, once for each tick
  const qar_qar_adapter::SpatialGestureStats stats = gestures.stats();
  std::printf("%llu events received, %llu queued\n", static_cast<unsigned long long>(stats.events_received),
    static_cast<unsigned long long>(stats.events_queued));
  gestures.unsubscribe();
  // [qar-snippet-end:gestures]
}

void snippet_env_helpers()
{
  // [qar-snippet:env_helpers]
  // The helpers take the raw text of a variable and never read the environment. A host passes the result of
  // std::getenv(name) and a null pointer means that the variable is not set.
  const bool prime = qar_qar_adapter::parse_bool_env("off", true);        // 1/true/on/yes and 0/false/off/no
  const bool trace = qar_qar_adapter::frame_trace_requested("any text");  // set to anything: requested
  const double tick_ms = qar_qar_adapter::parse_tick_ms_env("0.5", 0.1);  // the second argument is the fallback

  qar_qar_adapter::LogSeverity severity = qar_qar_adapter::LogSeverity::kTrace;
  const bool known = qar_qar_adapter::parse_log_severity(" Warn ", severity);   // case and spaces do not matter

  // The severity of the host, the severity that QarOS reads itself and the console switch decide how the
  // library of QarOS logs.
  const qar_qar_adapter::LibraryLogging logging = qar_qar_adapter::resolve_library_logging(
    "warn", "trace", nullptr, qar_qar_adapter::LogSeverity::kTrace);
  std::printf("%d %d %.1f %d | %s from %s, console %d, overwrite the variable of QarOS %d, zenoh %d\n", prime,
    trace, tick_ms, known, qar_qar_adapter::log_severity_name(logging.severity), logging.severity_source,
    logging.console, logging.environment_overridden,
    qar_qar_adapter::zenoh_logging_allowed(logging.severity));
  // [qar-snippet-end:env_helpers]
}
