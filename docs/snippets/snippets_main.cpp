// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Entry point of the documentation snippet target. Every snippet function is declared here and
// called inside `if (argc > 1000)`: the code is compiled and linked, all references resolve, and
// nothing runs. The arguments are null pointers, which the functions would never get from a host.

#include <cstdint>
#include <memory>
#include <string>

#include "qar_ar_core/panel_input_source.hpp"
#include "qar_ar_core/pointing_ray.hpp"
#include "qar_ar_core/qar_panel_sender.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/render_thread_executor.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

// ar_core_snippets.cpp
void snippet_panel_seam(qar_ar_core::IQarPanelSenderFactory * factory, const std::uint8_t * pixels);
void snippet_pointing_ray(
  const QarDeviceHandsWithJoints & hands, const QarVector3 & eye_position, qar_ar_core::PointingRaySlot * slot);
void snippet_spatial_frames();
void snippet_pick_session(qar_ar_core::IQarPanelInputSource * source);

// ogre_streaming_snippets.cpp
void snippet_stream_config(const QarPeerId & peer);
void snippet_manager_config(
  Ogre::SceneManager * scene, std::shared_ptr<qar_ogre_streaming::IRenderThreadExecutor> executor,
  std::shared_ptr<qar_ogre_streaming::IQarRenderSenderFactory> senders,
  qar_ogre_streaming::OgreQarDiagnosticsSink * sink);
void snippet_stream_lifecycle(qar_ogre_streaming::OgreQarStreamingManager * manager, const QarPeerId & peer);
void snippet_pose_conversion(const QarPose & pose, const QarFov & fov);
void snippet_scene_lighting(Ogre::SceneManager * scene, qar_ogre_streaming::OgreQarStreamingConfig & config);

// adapter_snippets.cpp
void snippet_factory_config(
  const std::shared_ptr<qar_qar_adapter::QarSessionOwner> & owner, const std::string & host);
void snippet_factory_use(
  qar_qar_adapter::QarSessionOwner * owner, qar_qar_adapter::QarCApiSenderFactory * factory);
void snippet_panels(qar_qar_adapter::IQarSessionOwner * owner);
void snippet_url_panels(qar_qar_adapter::IQarSessionOwner * owner);
void snippet_gestures(qar_qar_adapter::QarCApiSenderFactory * factory);
void snippet_env_helpers();

// Objects for the arguments; the calls below never run, so the values do not matter.
static const QarPeerId kPeer{};
static const QarPose kPose{};
static const QarFov kFov{};
static const QarVector3 kEye{};
static const QarDeviceHandsWithJoints kHands{};
static const std::string kHost;
static qar_ogre_streaming::OgreQarStreamingConfig kConfig;

int main(int argc, char** argv) {
  (void)argv;
  if (argc > 1000) {
    snippet_panel_seam(nullptr, nullptr);
    snippet_pointing_ray(kHands, kEye, nullptr);
    snippet_spatial_frames();
    snippet_pick_session(nullptr);

    snippet_stream_config(kPeer);
    snippet_manager_config(nullptr, nullptr, nullptr, nullptr);
    snippet_stream_lifecycle(nullptr, kPeer);
    snippet_pose_conversion(kPose, kFov);
    snippet_scene_lighting(nullptr, kConfig);

    snippet_factory_config(nullptr, kHost);
    snippet_factory_use(nullptr, nullptr);
    snippet_panels(nullptr);
    snippet_url_panels(nullptr);
    snippet_gestures(nullptr);
    snippet_env_helpers();
  }
  return 0;
}
