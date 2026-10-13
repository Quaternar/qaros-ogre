// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Streams the Ogre sample scene to a real QarOS Visualizer. The program owns an Ogre::Root with a
// hidden GL window (sample_scene.cpp, shared with the headless sample), opens a QarOS session with
// QarCApiSenderFactory, invites one Visualizer and renders for it through OgreQarStreamingManager.

// [qar-snippet:includes]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "../ogre_standalone_streaming_sample/src/sample_scene.hpp"
// [qar-snippet-end:includes]

namespace qos = qar_ogre_streaming;
namespace qqa = qar_qar_adapter;

// [qar-snippet:executor]
// The main thread is the render thread: the library only asks is_render_thread() and posts nothing.
struct MainThreadExecutor : qos::IRenderThreadExecutor
{
  void post_to_render_thread(std::function<void()> work) override { work(); }
  bool is_render_thread() const override { return std::this_thread::get_id() == thread; }
  const std::thread::id thread = std::this_thread::get_id();
};
// [qar-snippet-end:executor]

int main(int argc, char ** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string sdk_bin;
  bool connect_only = false;
  int seconds = 60;
  const bool help = argc == 2 && std::string(argv[1]) == "--help";
  for (int i = 1; i < argc && !help; ++i) {
    const std::string arg = argv[i];
    if (arg == "--connect-only") { connect_only = true; }
    else if (arg == "--sdk-bin" && i + 1 < argc) { sdk_bin = argv[++i]; }
    else if (arg == "--seconds" && i + 1 < argc) { seconds = std::atoi(argv[++i]); }
    else { sdk_bin.clear(); break; }
  }
  if (help || sdk_bin.empty() || seconds < 1) {
    std::printf(
      "Usage: qar_ogre_live_stream_example --sdk-bin DIR [--connect-only] [--seconds N]\n"
      "  --sdk-bin DIR   bin folder of the QarOS SDK (required)\n"
      "  --connect-only  open a QarOS session, print its app volume and exit; no window, no peer\n"
      "  --seconds N     otherwise: invite a Visualizer (it opens a window) and stream for N s (default 60)\n");
    return help ? 0 : 2;
  }
  qar_ogre_sample::SampleScene scene;  // declared first, so it is destroyed last
  std::string error;
  if (!connect_only && !scene.initialize(QAR_OGRE_PLUGIN_DIR, qar_ogre_sample::RenderSystemChoice::Auto,
      "qar_live_stream_ogre.log", error)) {
    std::printf("example: Ogre failed to start: %s\n", error.c_str());
    return 1;
  }

  // [qar-snippet:factory_config]
  qqa::QarCApiSenderFactory::Config config;
  config.runtime_binaries_folder_path = sdk_bin;      // QarOS starts its launcher and Visualizer from here
  config.copy_hook = qqa::make_gl_copy_image_hook();  // copies our textures into the ones QarOS hands out
  config.log_severity = QAR_LOG_SEVERITY_WARN;
  config.enable_console_logging = false;
  // [qar-snippet-end:factory_config]

  // [qar-snippet:connect_check]
  auto factory = std::make_shared<qqa::QarCApiSenderFactory>(config);
  QarAppVolumeId volume{};
  if (!factory->ready() || !factory->app_volume(volume)) {
    std::printf("example: QarOS connect failed: %s\n",
      factory->ready() ? "no app volume (see the lines above)" : factory->last_error().c_str());
    return 1;
  }
  std::printf("example: connected to QarOS: ready=1 app_volume=");
  for (const auto byte : volume.data) { std::printf("%02x", byte); }
  std::printf("\n");
  // [qar-snippet-end:connect_check]
  if (connect_only) { return 0; }

  QarPeerId invited{};
  const std::string invite_error = factory->invite_additional_visualizer(invited);
  if (!invite_error.empty()) {
    std::printf("example: could not invite a Visualizer: %s\n", invite_error.c_str());
    return 1;
  }
  std::printf("example: invited Visualizer %s\n", qos::peer_id_to_hex(invited).c_str());

  // [qar-snippet:manager_setup]
  qos::OgreQarStreamingConfig streaming;
  streaming.scene_manager = scene.scene_manager();
  streaming.render_thread_executor = std::make_shared<MainThreadExecutor>();
  streaming.qar_sender_factory = factory;
  auto manager = std::make_unique<qos::OgreQarStreamingManager>(streaming);
  streaming.qar_sender_factory.reset();  // the manager holds the factory now
  manager->set_streaming_active(true);
  // [qar-snippet-end:manager_setup]

  // [qar-snippet:frame_loop]
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    factory->capture_gl_context();  // latch the GL context that is current on this thread
    std::vector<QarPeerId> requested;
    factory->poll_new_peer_requests(requested);
    for (const QarPeerId & id : requested) {
      qos::StereoStreamConfig stream;
      stream.peer_id = id;
      const qos::Result created = manager->create_stream(stream);
      if (created.ok()) {
        manager->get_stream(id)->set_active(true);
        std::printf("example: stream open for peer %s\n", qos::peer_id_to_hex(id).c_str());
      } else if (created.status != qos::Status::AlreadyExists) {
        factory->on_render_request(id);  // the sender is still being created: ask again next pass
      }
    }
    if (const qos::Result ticked = manager->render_tick(); !ticked.ok()) {
      std::printf("example: render_tick failed: %s\n", ticked.message.c_str());
      return 1;
    }
    // [qar-snippet:closed_peers]
    std::vector<QarPeerId> closed;
    factory->poll_closed_peers(closed);
    for (const QarPeerId & id : closed) {
      manager->destroy_stream(id);
      factory->forget(id);
      std::printf("example: stream closed for peer %s\n", qos::peer_id_to_hex(id).c_str());
    }
    // [qar-snippet-end:closed_peers]
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // [qar-snippet-end:frame_loop]

  // [qar-snippet:shutdown]
  manager.reset();   // the streams and their senders go first,
  factory.reset();   // then the QarOS session, runtime and library,
  scene.shutdown();  // then Ogre.
  // [qar-snippet-end:shutdown]
  std::printf("example: done\n");
  return 0;
}
