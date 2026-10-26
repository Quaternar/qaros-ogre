// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Standalone Ogre streaming sample.
//
// (getenv/fopen/snprintf are used deliberately for a portable, dependency-free
// sample; silence MSVC's CRT deprecation warnings.)
#define _CRT_SECURE_NO_WARNINGS 1
//
// Proves that the reusable `qar_ogre_streaming` library works in a plain Ogre
// application with NO RViz / ROS / Qt: this program owns its Ogre::Root, a GL
// render system, a hidden GL context and a SceneManager; builds a simple lit scene
// (grid + coloured cubes + a directional light); instantiates the library through
// exactly the same public API the RViz plugin uses (OgreQarStreamingConfig +
// IRenderThreadExecutor + IQarRenderSenderFactory + OgreQarStreamingManager); drives
// ≥2 synthetic stereo peers through manager.render_tick(); writes per-peer left/right
// colour + depth PNGs with frame metadata; then exercises the zero-copy GL bridge
// (native texture id + OpenGL frame marshalling) outside RViz.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qar_ogre_streaming/diagnostics.hpp"
#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"
#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "qar_ogre_streaming_test_support/immediate_render_thread_executor.hpp"

#include "sample_capture.hpp"
#include "sample_mock.hpp"
#include "sample_scene.hpp"

#ifndef QAR_OGRE_PLUGIN_DIR
#  define QAR_OGRE_PLUGIN_DIR ""
#endif

namespace qos = qar_ogre_streaming;
using qar_ogre_sample::SampleMockSenderFactory;

namespace
{

struct Options
{
  int ticks = 120;
  int peers = 3;
  int capture_every = 0;  // 0 = only the final tick
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  float ipd_metres = 0.064f;
  std::string out_dir = "qar_sample_out";
  std::string render_system = "auto";
  std::string plugin_dir;  // resolved below
};

void print_usage(const char * argv0)
{
  std::printf(
    "Usage: %s [options]\n"
    "  --ticks N            render N batched ticks (default 120)\n"
    "  --peers K            number of synthetic in-process stereo peers, >=2 (default 3)\n"
    "  --capture-every M    also capture every M ticks (0 = final only; default 0)\n"
    "  --width W            per-eye texture width (default 1280)\n"
    "  --height H           per-eye texture height (default 720)\n"
    "  --ipd-mm X           inter-pupillary distance in mm (default 64)\n"
    "  --out-dir DIR        output directory for PNGs/reports (default qar_sample_out)\n"
    "  --render-system S    auto|gl3plus|gl (default auto: legacy GL, else GL3Plus)\n"
    "  --ogre-plugin-dir D  dir with RenderSystem_GL*/OgreMain (default: env/baked)\n"
    "  --help               show this help\n",
    argv0);
}

std::string resolve_default_plugin_dir()
{
  if (const char * env = std::getenv("ROS2_INSTALL_ROOT")) {
    if (env[0] != '\0') {
      return std::string(env) + "/opt/rviz_ogre_vendor/bin";
    }
  }
  return std::string(QAR_OGRE_PLUGIN_DIR);
}

bool parse_args(int argc, char ** argv, Options & o)
{
  o.plugin_dir = resolve_default_plugin_dir();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char * name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (a == "--ticks") { const char * v = next("--ticks"); if (!v) return false; o.ticks = std::atoi(v);
    } else if (a == "--peers") { const char * v = next("--peers"); if (!v) return false; o.peers = std::atoi(v);
    } else if (a == "--capture-every") { const char * v = next("--capture-every"); if (!v) return false; o.capture_every = std::atoi(v);
    } else if (a == "--width") { const char * v = next("--width"); if (!v) return false; o.width = static_cast<std::uint32_t>(std::atoi(v));
    } else if (a == "--height") { const char * v = next("--height"); if (!v) return false; o.height = static_cast<std::uint32_t>(std::atoi(v));
    } else if (a == "--ipd-mm") { const char * v = next("--ipd-mm"); if (!v) return false; o.ipd_metres = static_cast<float>(std::atof(v)) / 1000.0f;
    } else if (a == "--out-dir") { const char * v = next("--out-dir"); if (!v) return false; o.out_dir = v;
    } else if (a == "--render-system") { const char * v = next("--render-system"); if (!v) return false; o.render_system = v;
    } else if (a == "--ogre-plugin-dir") { const char * v = next("--ogre-plugin-dir"); if (!v) return false; o.plugin_dir = v;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      print_usage(argv[0]);
      return false;
    }
  }
  if (o.peers < 2) {
    std::fprintf(stderr, "note: --peers raised to 2 (the sample needs >=2 synthetic stereo peers)\n");
    o.peers = 2;
  }
  if (o.ticks < 1) o.ticks = 1;
  return true;
}

QarPeerId make_peer_id(std::uint32_t index)
{
  QarPeerId id{};
  id.data[0] = static_cast<std::uint8_t>((index >> 0) & 0xFF);
  id.data[1] = static_cast<std::uint8_t>((index >> 8) & 0xFF);
  id.data[2] = static_cast<std::uint8_t>((index >> 16) & 0xFF);
  id.data[3] = static_cast<std::uint8_t>((index >> 24) & 0xFF);
  id.data[15] = 0xEE;  // marker byte of a synthetic peer (the RViz plugin uses the same one)
  return id;
}

// Per-peer capture result (for the aggregate summary + exit code).
struct PeerCaptureResult
{
  std::string peer_hex;
  bool ok = false;
  std::uint64_t frames_submitted = 0;
  bool both_eyes_content = false;
  bool depth_ok = false;
  bool eyes_differ = false;
  bool nearer_shifts_more = false;
  std::string png_left;
  std::string png_right;
};

// Read a peer's left/right eye targets, write PNGs + a per-peer report, and return
// a pass/fail summary. Single-threaded: the GL context is current on this thread,
// so no render lock is needed (the RViz plugin wraps the equivalent in lockRender).
PeerCaptureResult capture_peer(
  qos::OgreQarStreamingManager & manager,
  const SampleMockSenderFactory & factory,
  const QarPeerId & peer_id,
  const std::string & prefix)
{
  PeerCaptureResult r;
  r.peer_hex = qos::peer_id_to_hex(peer_id);

  qos::OgreQarStereoStream * stream = manager.get_stream(peer_id);
  if (stream == nullptr) {
    std::fprintf(stderr, "capture: no stream for peer %s\n", r.peer_hex.c_str());
    return r;
  }
  if (!stream->targets_ready()) {
    const auto ir = stream->initialize_targets();
    if (!ir.ok()) {
      std::fprintf(stderr, "capture: initialize_targets failed: %s\n", ir.message.c_str());
      return r;
    }
  }
  qos::OgreStereoRenderTarget * lt = stream->left_target();
  qos::OgreStereoRenderTarget * rt = stream->right_target();
  if (lt == nullptr || rt == nullptr || !lt->is_created() || !rt->is_created()) {
    std::fprintf(stderr, "capture: eye targets not created for %s\n", r.peer_hex.c_str());
    return r;
  }

  const std::uint32_t w = lt->width();
  const std::uint32_t h = lt->height();
  std::vector<std::uint8_t> l_rgba, r_rgba;
  std::vector<float> l_depth, r_depth;
  lt->readColorRgba(l_rgba);
  lt->readDepthMetric(l_depth);
  rt->readColorRgba(r_rgba);
  rt->readDepthMetric(r_depth);

  r.png_left = prefix + "_left_color.png";
  r.png_right = prefix + "_right_color.png";
  const bool lc = qar_ogre_sample::write_color_png(r.png_left, w, h, l_rgba);
  const bool rc = qar_ogre_sample::write_color_png(r.png_right, w, h, r_rgba);
  qar_ogre_sample::write_depth_visualization_png(prefix + "_left_depth.png", w, h, l_depth);
  qar_ogre_sample::write_depth_visualization_png(prefix + "_right_depth.png", w, h, r_depth);
  qar_ogre_sample::write_depth_raw_f32(prefix + "_left_depth.f32", w, h, l_depth);
  qar_ogre_sample::write_depth_raw_f32(prefix + "_right_depth.f32", w, h, r_depth);

  const qar_ogre_sample::CaptureStats ls = qar_ogre_sample::compute_capture_stats(w, h, l_rgba, l_depth);
  const qar_ogre_sample::CaptureStats rs = qar_ogre_sample::compute_capture_stats(w, h, r_rgba, r_depth);
  const qar_ogre_sample::StereoParallaxStats sp =
    qar_ogre_sample::compute_stereo_parallax(w, h, l_rgba, r_rgba, l_depth, r_depth);

  const auto diag = stream->diagnostics();
  r.frames_submitted = diag.frames_submitted;

  // Reproduce the exact frame metadata the stream last rendered (deterministic
  // from that peer's own animated source and the submitted-frame count).
  auto source = factory.source_for(peer_id);
  const std::uint64_t last_idx = diag.frames_submitted > 0 ? diag.frames_submitted - 1 : 0;
  qos::QarFrameInfo info{};
  if (source) {
    info = source->frame_at(last_idx);
  }

  r.both_eyes_content = ls.color_non_background_pixels > 0 && rs.color_non_background_pixels > 0;
  r.depth_ok = ls.depth_nonzero_pixels > 0 && ls.depth_max > ls.depth_min &&
    rs.depth_nonzero_pixels > 0 && rs.depth_max > rs.depth_min;
  r.eyes_differ = sp.image_mean_abs_diff > 0.5;
  r.nearer_shifts_more = sp.nearer_shifts_more;
  r.ok = lc && rc && r.both_eyes_content && r.depth_ok && r.eyes_differ && r.nearer_shifts_more;

  // Per-peer report.
  std::string report;
  char buf[512];
  auto add = [&](const char * fmt, auto... args) {
    std::snprintf(buf, sizeof(buf), fmt, args...);
    report += buf;
  };
  add("# Qar standalone Ogre sample — per-peer stereo capture\n");
  add("peer_id=%s\n", r.peer_hex.c_str());
  add("layout=SEPARATED_TEXTURES\n");
  add("width=%u\nheight=%u\n", w, h);
  add("frames_submitted=%llu\n", static_cast<unsigned long long>(diag.frames_submitted));
  add("source_frame_index=%llu\n", static_cast<unsigned long long>(info.frame_index));
  add("left_eye_pos=%.4f,%.4f,%.4f\n",
    info.left_eye.pose.position.x, info.left_eye.pose.position.y, info.left_eye.pose.position.z);
  add("right_eye_pos=%.4f,%.4f,%.4f\n",
    info.right_eye.pose.position.x, info.right_eye.pose.position.y, info.right_eye.pose.position.z);
  add("left_eye_fov_lrud=%.4f,%.4f,%.4f,%.4f\n",
    info.left_eye.fov.angle_left, info.left_eye.fov.angle_right,
    info.left_eye.fov.angle_up, info.left_eye.fov.angle_down);
  add("right_eye_fov_lrud=%.4f,%.4f,%.4f,%.4f\n",
    info.right_eye.fov.angle_left, info.right_eye.fov.angle_right,
    info.right_eye.fov.angle_up, info.right_eye.fov.angle_down);
  add("left_color_content_px=%llu\n", static_cast<unsigned long long>(ls.color_non_background_pixels));
  add("right_color_content_px=%llu\n", static_cast<unsigned long long>(rs.color_non_background_pixels));
  add("left_depth_range_m=%.3f..%.3f\n", ls.depth_min, ls.depth_max);
  add("right_depth_range_m=%.3f..%.3f\n", rs.depth_min, rs.depth_max);
  add("image_mean_abs_diff=%.3f\n", sp.image_mean_abs_diff);
  add("near_band_m=%.3f..%.3f\n", sp.near_band_lo, sp.near_band_hi);
  add("far_band_m=%.3f..%.3f\n", sp.far_band_lo, sp.far_band_hi);
  add("near_disparity_px=%.3f\n", sp.near_disparity_px);
  add("far_disparity_px=%.3f\n", sp.far_disparity_px);
  add("nearer_shifts_more=%s\n", sp.nearer_shifts_more ? "true" : "false");
  add("overall_stereo_pass=%s\n", r.ok ? "true" : "false");

  std::FILE * f = std::fopen((prefix + "_stereo.txt").c_str(), "wb");
  if (f != nullptr) {
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
  }

  std::printf(
    "  peer %s  %ux%u  submitted=%llu  L_px=%llu R_px=%llu  diff=%.2f  "
    "near_disp=%.2fpx far_disp=%.2fpx  nearer_more=%s  pass=%s\n",
    r.peer_hex.substr(0, 8).c_str(), w, h,
    static_cast<unsigned long long>(diag.frames_submitted),
    static_cast<unsigned long long>(ls.color_non_background_pixels),
    static_cast<unsigned long long>(rs.color_non_background_pixels),
    sp.image_mean_abs_diff, sp.near_disparity_px, sp.far_disparity_px,
    sp.nearer_shifts_more ? "true" : "false", r.ok ? "true" : "false");
  return r;
}

// Exercise the zero-copy GL bridge on a rendered target, outside RViz.
bool run_bridge_demo(
  qos::OgreQarStreamingManager & manager,
  const SampleMockSenderFactory & factory,
  const QarPeerId & peer_id,
  const std::string & out_dir)
{
  qos::OgreQarStereoStream * stream = manager.get_stream(peer_id);
  if (stream == nullptr || stream->left_target() == nullptr ||
    !stream->left_target()->is_created())
  {
    std::fprintf(stderr, "bridge demo: peer 0 left target not ready\n");
    return false;
  }
  qos::OgreStereoRenderTarget & lt = *stream->left_target();

  std::printf("\n== OpenGL zero-copy bridge demo (outside RViz) ==\n");
  std::printf("  active render system: %s\n",
    qos::OgreOpenGlTextureBridge::active_render_system_name().c_str());
  std::printf("  color target native GL id (render-target accessor): %u\n", lt.colorGlTextureId());

  // The bridge API directly: marshal the last submitted separated frame into the
  // real Qar QarVideoFrameOpenGl (pure).
  qos::OgreOpenGlTextureBridge bridge;  // ZeroCopyGl by default
  std::string fill_detail = "no submitted frame";
  bool frame_filled = false;
  std::size_t tex_count = 0, view_count = 0;
  auto src = factory.source_for(peer_id);
  (void)src;
  for (const auto & ps : factory.sources()) {
    if (qos::peer_id_equal(ps.peer_id, peer_id) && ps.sender &&
      !ps.sender->submitted_frames().empty())
    {
      const qos::QarOpenGlStereoFrame & last = ps.sender->submitted_frames().back();
      QarVideoFrameOpenGl out{};
      const qos::Result fr = qos::OgreOpenGlTextureBridge::fill_opengl_frame(last, out);
      frame_filled = fr.ok();
      tex_count = out.textures_count;
      view_count = out.texture_views_count;
      fill_detail = frame_filled ? "ok" : fr.message;
      break;
    }
  }
  std::printf(
    "  fill_opengl_frame: %s  textures=%zu views=%zu\n",
    fill_detail.c_str(), tex_count, view_count);

  // Report file.
  std::string report;
  char buf[512];
  auto add = [&](const char * fmt, auto... args) {
    std::snprintf(buf, sizeof(buf), fmt, args...);
    report += buf;
  };
  add("# Qar standalone Ogre sample — OpenGL zero-copy bridge demo\n");
  add("render_system=%s\n", qos::OgreOpenGlTextureBridge::active_render_system_name().c_str());
  add("texture_id=%u\n", lt.colorGlTextureId());
  add("bridge_fill_opengl_frame=%d\n", frame_filled ? 1 : 0);
  add("bridge_textures=%zu\n", tex_count);
  add("bridge_views=%zu\n", view_count);

  const bool bridge_ran = lt.colorGlTextureId() != 0 && frame_filled;
  const bool bridge_pass = bridge_ran;
  add("bridge_ran=%d\n", bridge_ran ? 1 : 0);
  add("bridge_pass=%s\n", bridge_pass ? "true" : "false");

  std::FILE * f = std::fopen((out_dir + "/bridge_demo.txt").c_str(), "wb");
  if (f != nullptr) {
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
  }
  std::printf("  bridge_ran=%s bridge_pass=%s (report: %s/bridge_demo.txt)\n",
    bridge_ran ? "true" : "false", bridge_pass ? "true" : "false", out_dir.c_str());
  return bridge_ran;
}

}  // namespace

int main(int argc, char ** argv)
{
  Options o;
  if (!parse_args(argc, argv, o)) {
    return 2;
  }

  std::error_code ec;
  std::filesystem::create_directories(o.out_dir, ec);

  std::printf("== Qar standalone Ogre streaming sample ==\n");
  std::printf(
    "ticks=%d peers=%d capture_every=%d size=%ux%u ipd=%.1fmm out=%s rs=%s\n",
    o.ticks, o.peers, o.capture_every, o.width, o.height, o.ipd_metres * 1000.0f,
    o.out_dir.c_str(), o.render_system.c_str());
  std::printf("ogre_plugin_dir=%s\n", o.plugin_dir.c_str());

  // --- 1) Our own Ogre::Root + GL context + scene (no RViz/ROS/Qt) -----------
  qar_ogre_sample::SampleScene scene;
  std::string err;
  if (!scene.initialize(
      o.plugin_dir, qar_ogre_sample::parse_render_system_choice(o.render_system),
      o.out_dir + "/qar_sample_ogre.log", err))
  {
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    return 2;
  }
  std::printf("Ogre up. render system = \"%s\"\n", scene.render_system_name().c_str());

  int exit_code = 0;
  {
    // --- 2) Instantiate the library through the SAME public API as the plugin --
    auto executor = std::make_shared<qos::test_support::ImmediateRenderThreadExecutor>();
    auto diag_sink = std::make_shared<qar_ogre_sample::StdoutDiagnosticsSink>();

    qos::MockQarFrameInfoSource::Config base_cfg;  // library defaults: Z-up orbit
    base_cfg.ipd_metres = o.ipd_metres;
    base_cfg.aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
    auto factory = std::make_shared<SampleMockSenderFactory>(base_cfg);

    qos::OgreQarStreamingConfig cfg;
    cfg.scene_manager = scene.scene_manager();
    cfg.render_thread_executor = executor;
    cfg.qar_sender_factory = factory;
    cfg.diagnostics = diag_sink.get();
    // Leave cfg.clock null -> the scheduler uses a real SteadyClock; the render loop
    // below paces itself to the peer FPS so every peer is due each tick (real
    // wall-clock batch timing in the diagnostics).
    // Single-threaded sample: no host render lock needed (the RViz plugin injects
    // one that forwards to DisplayContext::lockRender()). The scheduler treats a
    // null lock as "no locking required".
    cfg.render_lock = nullptr;
    // Generous batch budget so every peer renders every tick for clean captures
    // (FPS/time-budget shedding under load is covered by the budget unit tests); keep
    // the 30 FPS floor so the same budgeting machinery is exercised.
    cfg.render_budget.max_batch_milliseconds = 200.0;
    cfg.render_budget.floor_fps = 30;
    // Zero-copy GL fence seam — the SAME hook the plugin installs.
    cfg.fence_hook = std::make_shared<qos::GlFenceHook>();

    qos::OgreQarStreamingManager manager(cfg);

    // --- 3) Create ≥2 synthetic stereo peers (distinct source config per peer) -
    std::vector<QarPeerId> peers;
    const std::uint32_t max_fps = 60;
    for (int i = 0; i < o.peers; ++i) {
      const QarPeerId id = make_peer_id(static_cast<std::uint32_t>(i));
      qos::StereoStreamConfig sc;
      sc.peer_id = id;
      sc.width = o.width;
      sc.height = o.height;
      sc.max_fps = max_fps;
      const qos::Result cr = manager.create_stream(sc);
      if (!cr.ok()) {
        std::fprintf(stderr, "create_stream(%d) failed: %s\n", i, cr.message.c_str());
        exit_code = 1;
        continue;
      }
      manager.get_stream(id)->set_active(true);
      peers.push_back(id);
    }
    manager.set_streaming_active(true);
    std::printf("created %zu stereo peers; streaming active.\n", peers.size());

    // --- 4) Drive N batched ticks through the SAME render path as the plugin ---
    // Pace the loop to the peer FPS (busy-wait to each deadline, like the plugin's
    // multi-peer driver) so the scheduler's real SteadyClock sees every peer due
    // each tick -> full batches with real per-batch wall-clock timing.
    const auto period =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(max_fps)));
    auto next_tick = std::chrono::steady_clock::now();
    std::printf("\n== rendering %d ticks ==\n", o.ticks);
    for (int t = 0; t < o.ticks; ++t) {
      const qos::Result rr = manager.render_tick();
      if (!rr.ok()) {
        std::fprintf(stderr, "render_tick(%d) error: %s\n", t, rr.message.c_str());
        exit_code = 1;
        break;
      }
      const bool final_tick = (t == o.ticks - 1);
      const bool periodic = (o.capture_every > 0) && ((t % o.capture_every) == 0);
      if (periodic && !final_tick) {
        for (std::size_t p = 0; p < peers.size(); ++p) {
          const std::string prefix =
            o.out_dir + "/tick" + std::to_string(t) + "_peer" + std::to_string(p);
          capture_peer(manager, *factory, peers[p], prefix);
        }
      }
      // Pace to the next FPS deadline (busy-wait; Windows sleep granularity is too
      // coarse for 60 Hz). Never try to catch up if a tick already overran.
      next_tick += period;
      const auto now_tp = std::chrono::steady_clock::now();
      if (next_tick < now_tp) {
        next_tick = now_tp;
      } else {
        while (std::chrono::steady_clock::now() < next_tick) {
          std::this_thread::yield();
        }
      }
    }

    // --- 5) Final per-peer stereo capture (PNGs + metadata) --------------------
    std::printf("\n== final per-peer stereo capture ==\n");
    std::vector<PeerCaptureResult> results;
    for (std::size_t p = 0; p < peers.size(); ++p) {
      const std::string prefix = o.out_dir + "/peer" + std::to_string(p);
      results.push_back(capture_peer(manager, *factory, peers[p], prefix));
    }

    // --- 6) Zero-copy GL bridge demo (native id + fence, outside RViz) ---------
    const bool bridge_ran = !peers.empty() && run_bridge_demo(manager, *factory, peers[0], o.out_dir);

    // --- 7) Diagnostics snapshot (same struct the plugin surfaces) -------------
    const qos::DiagnosticsSnapshot snap = manager.diagnostics_snapshot();
    std::printf("\n== diagnostics snapshot ==\n");
    std::printf(
      "streaming=%d streams=%u batches=%llu last_batch=%.3fms avg_batch=%.3fms "
      "served=%u skipped=%u budget_skipped=%llu\n",
      snap.streaming_active ? 1 : 0, snap.stream_count,
      static_cast<unsigned long long>(snap.batches_rendered),
      snap.last_batch_ms, snap.avg_batch_ms,
      snap.last_batch_streams_served, snap.last_batch_streams_skipped,
      static_cast<unsigned long long>(snap.budget_skipped_frames));
    for (const auto & sd : snap.streams) {
      std::printf(
        "  peer %s %ux%u @%ufps submitted=%llu skipped=%llu achieved_fps=%.2f "
        "L=%.2fms R=%.2fms total=%.2fms\n",
        sd.peer_id_hex.substr(0, 8).c_str(), sd.width, sd.height, sd.max_fps,
        static_cast<unsigned long long>(sd.frames_submitted),
        static_cast<unsigned long long>(sd.frames_skipped),
        sd.achieved_fps, sd.last_left_render_ms, sd.last_right_render_ms, sd.last_render_ms);
    }

    // --- 8) Aggregate acceptance + summary file --------------------------------
    bool all_peers_ok = !results.empty();
    for (const auto & r : results) {
      if (!r.ok) all_peers_ok = false;
    }
    const bool core_pass =
      static_cast<int>(peers.size()) == o.peers && all_peers_ok && snap.batches_rendered > 0;

    std::string summary;
    char buf[512];
    auto add = [&](const char * fmt, auto... args) {
      std::snprintf(buf, sizeof(buf), fmt, args...);
      summary += buf;
    };
    add("# Qar standalone Ogre streaming sample — run summary\n");
    add("render_system=%s\n", scene.render_system_name().c_str());
    add("ticks=%d peers=%d size=%ux%u ipd_mm=%.1f\n",
      o.ticks, o.peers, o.width, o.height, o.ipd_metres * 1000.0f);
    add("batches_rendered=%llu\n", static_cast<unsigned long long>(snap.batches_rendered));
    add("avg_batch_ms=%.3f\n", snap.avg_batch_ms);
    for (const auto & r : results) {
      add("peer %s submitted=%llu both_content=%d depth_ok=%d eyes_differ=%d "
        "nearer_shifts_more=%d pass=%d\n",
        r.peer_hex.substr(0, 8).c_str(),
        static_cast<unsigned long long>(r.frames_submitted),
        r.both_eyes_content ? 1 : 0, r.depth_ok ? 1 : 0, r.eyes_differ ? 1 : 0,
        r.nearer_shifts_more ? 1 : 0, r.ok ? 1 : 0);
    }
    add("bridge_ran=%d\n", bridge_ran ? 1 : 0);
    add("overall_pass=%s\n", core_pass ? "true" : "false");

    std::FILE * f = std::fopen((o.out_dir + "/summary.txt").c_str(), "wb");
    if (f != nullptr) {
      std::fwrite(summary.data(), 1, summary.size(), f);
      std::fclose(f);
    }
    std::printf("\n%s", summary.c_str());
    std::printf("outputs written under: %s\n", o.out_dir.c_str());

    if (!core_pass) {
      exit_code = 1;
    }

    // The manager (and its streams/targets) must be destroyed BEFORE the scene
    // (Ogre::Root). This scope ends here, releasing the manager first.
  }

  scene.shutdown();
  std::printf("\nsample %s (exit %d)\n", exit_code == 0 ? "PASSED" : "FAILED", exit_code);
  return exit_code;
}
