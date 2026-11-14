// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Streams the Ogre sample scene to a target of the QAROS Hub (a headset or a Visualizer). The
// program owns an Ogre::Root with a hidden GL window (sample_scene.cpp, shared with the headless
// sample), joins the QAROS Hub with a QarSessionOwner and renders through
// OgreQarStreamingManager for the first target that comes up. The program joins the Hub of its
// own PC: QAROS asks once to allow it (and again after each rebuild of it). To reach a Hub on
// another PC, make the Hub of this PC a member of that Hub in QAROS.

// [qar-snippet:includes]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qar_ar_core/common_name.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ar_core/version.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"
#include "../ogre_standalone_streaming_sample/src/sample_scene.hpp"
// [qar-snippet-end:includes]

namespace qos = qar_ogre_streaming;
namespace qqa = qar_qar_adapter;

namespace
{

constexpr const char * kProgramTag = "ogre-live-stream";

// [qar-snippet:executor]
// The main thread is the render thread: the library only asks is_render_thread() and posts nothing.
struct MainThreadExecutor : qos::IRenderThreadExecutor
{
  void post_to_render_thread(std::function<void()> work) override { work(); }
  bool is_render_thread() const override { return std::this_thread::get_id() == thread; }
  const std::thread::id thread = std::this_thread::get_id();
};
// [qar-snippet-end:executor]

void print_usage()
{
  std::printf(
    "Usage: qar_ogre_live_stream_example [--local] [--connect-only] [--seconds N] | --version\n"
    "  --local        join the QAROS Hub of this PC (the only mode, accepted for older\n"
    "                  scripts). QAROS asks once to allow this program, and again after each\n"
    "                  rebuild of it: click Allow in the QAROS window. For a Hub on another PC,\n"
    "                  make the Hub of this PC a member of that Hub in QAROS\n"
    "  --connect-only  join QAROS, print the runtime, the mode, this program's peer, the app\n"
    "                  volume and the targets, and exit; no window, no stream\n"
    "  --seconds N     otherwise: wait for a target (a headset or a Visualizer), stream the\n"
    "                  scene to the first one that comes up, stop after N s (default 60)\n"
    "  --version       print the qar_ogre_adapter version and exit\n");
}

// Prints what the Hub-facing events say, the plain-words status first.
void print_events(const std::vector<qqa::SessionEvent> & events)
{
  for (const qqa::SessionEvent & event : events) {
    if (event.kind == qqa::SessionEvent::Kind::kState && !event.status.message.empty()) {
      std::printf("example: QAROS: %s\n", event.status.message.c_str());
    } else if (event.kind == qqa::SessionEvent::Kind::kProgress && !event.text.empty()) {
      std::printf("example: QAROS: %s\n", event.text.c_str());
    }
  }
}

std::string host_name()
{
  std::string name = qqa::read_environment("COMPUTERNAME");
  if (name.empty()) { name = qqa::read_environment("HOSTNAME"); }
  return name.empty() ? std::string("this-pc") : name;
}

}  // namespace

int main(int argc, char ** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool connect_only = false;
  int seconds = 60;
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::printf("qar_ogre_live_stream_example %s\n", qar_ar_core::kLibraryVersion);
    return 0;
  }
  const bool help = argc == 2 && std::string(argv[1]) == "--help";
  bool bad_usage = false;
  for (int i = 1; i < argc && !help && !bad_usage; ++i) {
    const std::string arg = argv[i];
    if (arg == "--connect-only") { connect_only = true; }
    else if (arg == "--local") { /* the only mode */ }
    else if (arg == "--seconds" && i + 1 < argc) { seconds = std::atoi(argv[++i]); }
    else { bad_usage = true; }
  }
  if (!help && !bad_usage && seconds < 1) {
    bad_usage = true;
  }
  if (help || bad_usage) {
    print_usage();
    return help ? 0 : 2;
  }

  // [qar-snippet:session]
  const std::string host = host_name();
  qqa::SessionOwnerConfig owner_config;
  owner_config.program_tag = kProgramTag;
  owner_config.display_name = std::string("Ogre live stream example on ") + host;
  owner_config.log_severity = QAR_LOG_SEVERITY_WARN;
  owner_config.enable_console_logging = false;
  auto owner = std::make_shared<qqa::QarSessionOwner>(owner_config);  // starts its own thread
  // [qar-snippet-end:session]
  // [qar-snippet:factory_config]
  qqa::QarCApiSenderFactory::Config config;
  config.copy_hook = qqa::make_gl_copy_image_hook();  // copies our textures into those of QAROS
  config.max_peers = 1;                               // one stream: the first target that comes up
  config.name_scope = qar_ar_core::make_name_scope(kProgramTag, "", host);
  // [qar-snippet-end:factory_config]

  // [qar-snippet:connect_check]
  std::printf("example: joining the QAROS Hub of this PC; click Allow if QAROS asks\n");
  auto factory = std::make_shared<qqa::QarCApiSenderFactory>(owner, config);
  owner->connect();
  std::vector<qqa::SessionEvent> seen;
  // The approval prompt waits 120 s by default; the wait here is a little longer.
  const qqa::SessionState state = qqa::wait_for_session(*owner, std::chrono::seconds(150), &seen);
  print_events(seen);
  if (state != qqa::SessionState::kRunning) {
    std::printf("example: QAROS connect failed (state %d)\n", static_cast<int>(state));
    return 1;
  }
  QarAppVolumeId volume{};
  if (!factory->attach() || !factory->app_volume(volume)) {
    std::printf("example: QAROS connect failed: %s\n",
      factory->attached() ? "no app volume (see the lines above)" : factory->last_error().c_str());
    return 1;
  }
  std::printf("example: connected to QarOS: ready=1 app_volume=");
  for (const auto byte : volume.data) { std::printf("%02x", byte); }
  std::printf("\n");
  // [qar-snippet-end:connect_check]
  if (connect_only) {
    // The peer list arrives right after the join; give it a moment before it is printed.
    std::vector<qqa::SessionEvent> late;
    for (int i = 0; i < 75; ++i) {  // 1.5 s
      owner->poll(late);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("example: runtime: %s\n", owner->runtime_description().c_str());
    std::printf("example: mode: local\n");
    std::printf("example: this program is peer %s\n",
      qos::peer_id_to_hex(owner->self_peer()).c_str());
    const std::vector<qqa::TargetInfo> targets = owner->targets();
    std::printf("example: %u target(s)\n", static_cast<unsigned>(targets.size()));
    for (const qqa::TargetInfo & target : targets) {
      std::printf("example:   target %s '%s'%s\n", qos::peer_id_to_hex(target.id).c_str(),
        target.device_name.empty() ? target.display_name.c_str() : target.device_name.c_str(),
        target.served ? "" : " (not served)");
    }
    factory->detach();
    return 0;
  }

  qar_ogre_sample::SampleScene scene;  // declared before the manager, so it is destroyed after it
  std::string error;
  if (!scene.initialize(QAR_OGRE_PLUGIN_DIR, qar_ogre_sample::RenderSystemChoice::Auto,
      "qar_live_stream_ogre.log", error)) {
    std::printf("example: Ogre failed to start: %s\n", error.c_str());
    factory->detach();
    return 1;
  }

  // [qar-snippet:manager_setup]
  qos::OgreQarStreamingConfig streaming;
  streaming.scene_manager = scene.scene_manager();
  streaming.render_thread_executor = std::make_shared<MainThreadExecutor>();
  streaming.qar_sender_factory = factory;
  auto manager = std::make_unique<qos::OgreQarStreamingManager>(streaming);
  streaming.qar_sender_factory.reset();  // the manager holds the factory now
  manager->set_streaming_active(true);
  // [qar-snippet-end:manager_setup]

  std::printf("example: waiting for a target (a headset or a Visualizer) to join QAROS\n");
  // [qar-snippet:frame_loop]
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<qqa::SessionEvent> events;  // the owner's news: a lost Hub, a target that went
    owner->poll(events);
    print_events(events);
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
  manager.reset();    // the streams and their senders go first,
  factory->detach();  // then the factory lets go of the app volume,
  factory.reset();
  owner.reset();      // then the QAROS session, runtime and library,
  scene.shutdown();   // then Ogre.
  // [qar-snippet-end:shutdown]
  std::printf("example: done\n");
  return 0;
}
