// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Snippets of API.md for the module qar_ogre_streaming: the configuration of a stream and of the manager, the
// life of a stream, the pose conversion, the lighting rig and the diagnostics sink. The functions are compiled
// and linked and never run. Their parameters stand for what the host owns, so each one is a pointer or a shared
// pointer that the documentation names in its text.

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_ogre_streaming/qar_pose_conversion.hpp"
#include "qar_ogre_streaming/qar_scene_lighting.hpp"
#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"
#include "qar_ogre_streaming/render_thread_executor.hpp"
#include "qar_ogre_streaming/types.hpp"

void snippet_stream_config(const QarPeerId & peer)
{
  // [qar-snippet:stream_config]
  qar_ogre_streaming::StereoStreamConfig config;
  config.peer_id = peer;
  config.width = 1920;                 // pixels of each eye
  config.height = 1080;
  config.max_fps = 60;                 // the cap of this stream; the default is 72
  const qar_ogre_streaming::Result valid = config.validate();
  if (!valid.ok()) {
    std::printf("stream config refused: %s (%s)\n", valid.message.c_str(),
      qar_ogre_streaming::to_string(valid.status));
  }
  // [qar-snippet-end:stream_config]
}

// [qar-snippet:diagnostics_sink]
// The library reports through this interface, on the render thread: a sink must not block.
class StderrSink : public qar_ogre_streaming::OgreQarDiagnosticsSink
{
public:
  void on_message(
    qar_ogre_streaming::DiagnosticSeverity severity, const std::string & category,
    const std::string & message, const std::string & peer_id_hex) override
  {
    std::fprintf(stderr, "[%s] %s %s %s\n", qar_ogre_streaming::to_string(severity), category.c_str(),
      peer_id_hex.c_str(), message.c_str());
  }
};
// [qar-snippet-end:diagnostics_sink]

void snippet_manager_config(
  Ogre::SceneManager * scene, std::shared_ptr<qar_ogre_streaming::IRenderThreadExecutor> executor,
  std::shared_ptr<qar_ogre_streaming::IQarRenderSenderFactory> senders,
  qar_ogre_streaming::OgreQarDiagnosticsSink * sink)
{
  // [qar-snippet:manager_config]
  qar_ogre_streaming::OgreQarStreamingConfig config;
  config.scene_manager = scene;                        // required
  config.render_thread_executor = executor;            // required
  config.qar_sender_factory = senders;                 // required
  config.diagnostics = sink;                           // optional
  config.render_budget.max_batch_milliseconds = 12.0;  // optional: the time budget of one batch
  config.fence_hook = std::make_shared<qar_ogre_streaming::GlFenceHook>();   // optional

  try {
    qar_ogre_streaming::OgreQarStreamingManager manager(config);
    manager.set_streaming_active(true);
  } catch (const std::invalid_argument & refused) {
    std::printf("manager: %s\n", refused.what());      // a required field was null
  }
  // [qar-snippet-end:manager_config]
}

void snippet_stream_lifecycle(qar_ogre_streaming::OgreQarStreamingManager * manager, const QarPeerId & peer)
{
  // [qar-snippet:stream_lifecycle]
  qar_ogre_streaming::StereoStreamConfig stream;
  stream.peer_id = peer;
  const qar_ogre_streaming::Result created = manager->create_stream(stream);
  if (!created.ok()) {
    std::printf("create_stream: %s (%s)\n", created.message.c_str(),
      qar_ogre_streaming::to_string(created.status));  // for example AlreadyExists: one stream for each peer
    return;
  }
  manager->get_stream(peer)->set_active(true);        // a stream renders only while it is active

  // On the render thread, once for each tick of the host:
  const qar_ogre_streaming::Result ticked = manager->render_tick();
  const qar_ogre_streaming::DiagnosticsSnapshot snapshot = manager->diagnostics_snapshot();
  for (const qar_ogre_streaming::StreamDiagnostics & item : snapshot.streams) {
    std::printf("%s: %llu frames, %.1f fps, last skip: %s\n", item.peer_id_hex.c_str(),
      static_cast<unsigned long long>(item.frames_submitted), item.achieved_fps,
      qar_ogre_streaming::to_string(item.last_skip_reason));
  }
  if (!ticked.ok()) {
    std::printf("render_tick: %s\n", ticked.message.c_str());
  }

  manager->destroy_stream(peer);
  // [qar-snippet-end:stream_lifecycle]
}

void snippet_pose_conversion(const QarPose & pose, const QarFov & fov)
{
  // [qar-snippet:pose_conversion]
  // One eye of a frame, in the two calls that the stream makes: the pose, then the off-axis projection.
  qar_ogre_streaming::Vec3 position;
  qar_ogre_streaming::Quat orientation;                // w, x, y, z: the order of Ogre
  const qar_ogre_streaming::Result posed = qar_ogre_streaming::qar_pose_to_ogre(pose, position, orientation);
  qar_ogre_streaming::Mat4 projection;                 // row-major, the depth range of OpenGL
  const qar_ogre_streaming::Result projected =
    qar_ogre_streaming::make_projection_from_qar_fov(fov, 0.1f, 500.0f, projection);
  if (!posed.ok() || !projected.ok()) {
    std::printf("degenerate eye: %s %s\n", posed.message.c_str(), projected.message.c_str());
  }

  // Or both at once, as plain values and without Ogre:
  qar_ogre_streaming::EyeView eye;
  eye.pose = pose;
  eye.fov = fov;
  eye.near_far = QarNearFar{0.1f, 500.0f};
  const qar_ogre_streaming::EyeRenderParams params = qar_ogre_streaming::compute_eye_render_params(eye);
  std::printf("pose valid: %d, projection valid: %d\n", params.pose_valid ? 1 : 0, params.projection_valid ? 1 : 0);
  // [qar-snippet-end:pose_conversion]
}

void snippet_scene_lighting(Ogre::SceneManager * scene, qar_ogre_streaming::OgreQarStreamingConfig & config)
{
  // [qar-snippet:scene_lighting]
  qar_ogre_streaming::SceneLightingConfig lighting;
  qar_ogre_streaming::parse_scene_lighting_mode("both", lighting.mode);   // off, omni, per-peer or both
  lighting.suppress_light_name = "MainDirectional";    // the headlight of the host, hidden while streaming
  config.scene_lighting = std::make_shared<qar_ogre_streaming::QarSceneLighting>(scene, lighting);
  // [qar-snippet-end:scene_lighting]
}
