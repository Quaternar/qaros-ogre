// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/ogre_gl_texture_spike.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "qar_ogre_streaming/gl_interop.hpp"
#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"
#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"
#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{
namespace
{

/// Count pixels (RGBA quads) that are byte-identical between two buffers, comparing
/// @p b either directly or with its rows vertically flipped relative to @p a.
std::size_t count_matching_pixels(
  const std::vector<uint8_t> & a, const std::vector<uint8_t> & b,
  uint32_t width, uint32_t height, bool flip_b)
{
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
  const std::size_t need = row_bytes * height;
  if (a.size() < need || b.size() < need) {
    return 0;
  }
  std::size_t matches = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t by = flip_b ? (height - 1u - y) : y;
    const uint8_t * pa = a.data() + static_cast<std::size_t>(y) * row_bytes;
    const uint8_t * pb = b.data() + static_cast<std::size_t>(by) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      const std::size_t o = static_cast<std::size_t>(x) * 4u;
      if (pa[o] == pb[o] && pa[o + 1] == pb[o + 1] &&
        pa[o + 2] == pb[o + 2] && pa[o + 3] == pb[o + 3])
      {
        ++matches;
      }
    }
  }
  return matches;
}

}  // namespace

GlTextureRoundtripResult run_color_texture_roundtrip_spike(const OgreStereoRenderTarget & target)
{
  GlTextureRoundtripResult r;

  if (!target.is_created()) {
    r.message = "render target not created (create + render before running the spike)";
    return r;
  }
  r.ran = true;
  r.render_system_name = OgreOpenGlTextureBridge::active_render_system_name();

  const QarTextureSize size = target.colorTextureSize();
  r.expected_width = size.width;
  r.expected_height = size.height;
  r.total_pixels = static_cast<std::size_t>(size.width) * size.height;

  // --- 1. Native GL id extraction (via the bridge / getCustomAttribute("GLID")) --
  r.texture_id = target.colorGlTextureId();
  if (r.texture_id == 0u) {
    r.message = "native GL id extraction failed (colorGlTextureId()==0; not GL3+?)";
    return r;
  }

  if (!gl_interop::ensure_loaded()) {
    r.message = "GL entry points unavailable (no current context on this thread?)";
    return r;
  }

  // --- 2. Validate the id against the live context ---------------------------
  r.gl_is_texture = gl_interop::is_texture(r.texture_id);
  gl_interop::texture_2d_dimensions(r.texture_id, r.queried_width, r.queried_height);
  r.dims_match = r.gl_is_texture &&
    r.queried_width == r.expected_width && r.queried_height == r.expected_height;

  // --- 3. Two independent readbacks of the SAME texture ----------------------
  //   (a) Ogre's own path (HardwarePixelBuffer::blitToMemory).
  //   (b) A raw glGetTexImage on the extracted id, NOT through Ogre.
  std::vector<uint8_t> ogre_rgba;
  const Result ogre_read = target.readColorRgba(ogre_rgba);
  r.ogre_byte_count = ogre_rgba.size();

  std::vector<uint8_t> raw_rgba;
  const bool raw_ok =
    gl_interop::read_texture_2d_rgba8(r.texture_id, r.expected_width, r.expected_height, raw_rgba);
  r.raw_byte_count = raw_rgba.size();

  if (ogre_read.ok() && raw_ok && r.total_pixels > 0) {
    r.matching_pixels_direct =
      count_matching_pixels(ogre_rgba, raw_rgba, r.expected_width, r.expected_height, false);
    r.matching_pixels_flipped =
      count_matching_pixels(ogre_rgba, raw_rgba, r.expected_width, r.expected_height, true);
    const std::size_t best = std::max(r.matching_pixels_direct, r.matching_pixels_flipped);
    r.flipped = r.matching_pixels_flipped > r.matching_pixels_direct;
    r.match_fraction = static_cast<double>(best) / static_cast<double>(r.total_pixels);
    // The two readbacks are of the same GL texture object; expect a near-perfect
    // match (identical GPU->CPU conversion). 0.999 tolerates only trivial noise.
    r.roundtrip_match = r.match_fraction >= 0.999;
  }

  // --- 4. GL fence: create, client-wait, observe signaled, delete ------------
  OgreOpenGlTextureBridge bridge;
  GlFrameSync sync{};
  const Result fence_res = bridge.insert_fence(sync);
  r.fence_created = fence_res.ok() && sync.fence_sync != nullptr;
  if (r.fence_created) {
    // 1 second timeout: the RTT draw commands are already done, so this returns
    // ALREADY_SIGNALED (or CONDITION_SATISFIED after an implicit flush) immediately.
    r.fence_wait = gl_interop::fence_client_wait(sync.fence_sync, 1000000000ull);
    r.fence_signaled = r.fence_wait == gl_interop::FenceWaitStatus::AlreadySignaled ||
      r.fence_wait == gl_interop::FenceWaitStatus::ConditionSatisfied;
    gl_interop::fence_delete(sync.fence_sync);
  }

  // --- 5. Marshal a real Qar QarVideoFrameOpenGl (exercise the fill path) ----
  GlVideoTexture tex{};
  tex.size = size;
  tex.texture_id = r.texture_id;
  tex.target = gl_interop::gl_texture_2d();
  QarOpenGlStereoFrame sf{};
  sf.layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;
  sf.left_color = tex;
  sf.right_color = tex;  // single-target spike reuses the id for both eyes.
  sf.has_depth = false;
  sf.views[0] = QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 0u};
  sf.views[1] =
    QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 1u};
  sf.views_count = 2;
  sf.synchronization = sync;
  QarVideoFrameOpenGl qframe{};
  const Result fill = OgreOpenGlTextureBridge::fill_opengl_frame(sf, qframe);
  r.opengl_frame_filled =
    fill.ok() && qframe.textures_count == 2 && qframe.texture_views_count == 2;

  r.message = r.roundtrip_match
    ? "zero-copy handoff PROVEN (raw-GL readback of the extracted id matches Ogre)"
    : "roundtrip mismatch or a validation step failed (see fields)";
  return r;
}

std::string format_spike_log_line(const GlTextureRoundtripResult & r)
{
  auto b = [](bool v) { return v ? "1" : "0"; };
  char frac[32];
  std::snprintf(frac, sizeof(frac), "%.4f", r.match_fraction);
  std::string line;
  line += "ran=" + std::string(b(r.ran));
  line += " rs=\"" + r.render_system_name + "\"";
  line += " texture_id=" + std::to_string(r.texture_id);
  line += " glIsTexture=" + std::string(b(r.gl_is_texture));
  line += " dims=" + std::to_string(r.queried_width) + "x" + std::to_string(r.queried_height);
  line += " expected=" + std::to_string(r.expected_width) + "x" + std::to_string(r.expected_height);
  line += " dims_match=" + std::string(b(r.dims_match));
  line += " roundtrip_match=" + std::string(b(r.roundtrip_match));
  line += " match_fraction=" + std::string(frac);
  line += " flipped=" + std::string(b(r.flipped));
  line += " fence_created=" + std::string(b(r.fence_created));
  line += " fence=" + std::string(gl_interop::to_string(r.fence_wait));
  line += " fence_signaled=" + std::string(b(r.fence_signaled));
  line += " opengl_frame=" + std::string(b(r.opengl_frame_filled));
  return line;
}

}  // namespace qar_ogre_streaming
