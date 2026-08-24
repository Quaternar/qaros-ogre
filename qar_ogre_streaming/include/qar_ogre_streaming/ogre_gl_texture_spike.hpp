// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_STREAMING__OGRE_GL_TEXTURE_SPIKE_HPP_
#define QAR_OGRE_STREAMING__OGRE_GL_TEXTURE_SPIKE_HPP_

#include <cstdint>
#include <string>

#include "qar_ogre_streaming/gl_interop.hpp"

namespace qar_ogre_streaming
{

class OgreStereoRenderTarget;

/// @brief Outcome of the M7 zero-copy texture-handoff spike (Gate B evidence).
///
/// Every field is measured inside the live GL context on the render thread; the
/// crucial one is @ref roundtrip_match: an INDEPENDENT raw-GL readback of the
/// extracted native id (via `glGetTexImage`, not through Ogre) reproduces the same
/// pixels Ogre's own `blitToMemory` reads back — proving the id truly refers to the
/// rendered content and can be handed to Qar without a CPU copy.
struct GlTextureRoundtripResult
{
  bool ran = false;                 ///< The spike actually executed (target created).

  std::string render_system_name;   ///< Active Ogre render system (GL backend).
  uint32_t texture_id = 0;          ///< Native GL texture id extracted from Ogre.
  bool gl_is_texture = false;       ///< glIsTexture(id) == GL_TRUE.
  uint32_t queried_width = 0;       ///< glGetTexLevelParameteriv WIDTH.
  uint32_t queried_height = 0;      ///< glGetTexLevelParameteriv HEIGHT.
  uint32_t expected_width = 0;      ///< RTT width.
  uint32_t expected_height = 0;     ///< RTT height.
  bool dims_match = false;          ///< queried dims == RTT dims.

  std::size_t ogre_byte_count = 0;  ///< bytes read back via Ogre blitToMemory.
  std::size_t raw_byte_count = 0;   ///< bytes read back via raw glGetTexImage.
  std::size_t total_pixels = 0;
  std::size_t matching_pixels_direct = 0;   ///< pixels equal (same row order).
  std::size_t matching_pixels_flipped = 0;  ///< pixels equal (vertically flipped).
  double match_fraction = 0.0;      ///< best of direct/flipped, in [0,1].
  bool flipped = false;             ///< true if the flipped comparison matched best.
  bool roundtrip_match = false;     ///< match_fraction >= threshold (zero-copy proven).

  bool fence_created = false;                                   ///< glFenceSync succeeded.
  gl_interop::FenceWaitStatus fence_wait = gl_interop::FenceWaitStatus::NoFence;
  bool fence_signaled = false;      ///< wait returned ALREADY_SIGNALED/CONDITION_SATISFIED.

  bool opengl_frame_filled = false; ///< QarVideoFrameOpenGl marshalling succeeded.

  std::string message;              ///< human-readable summary / first failure.
};

/// @brief Run the zero-copy handoff spike on a rendered color render target.
///
/// MUST be called on the render/GL thread with a current context (in RViz: inside
/// DisplayContext::lockRender()), AFTER @p target has been created and rendered at
/// least once. It: extracts the native id, validates it (glIsTexture + dims),
/// raw-reads it back independently of Ogre, compares against Ogre's readback, then
/// creates + client-waits + deletes a GL fence, and finally marshals a
/// QarVideoFrameOpenGl. It allocates and frees its own GL fence; it does not retain
/// GL state. Pass/fail detail is in the returned struct.
GlTextureRoundtripResult run_color_texture_roundtrip_spike(const OgreStereoRenderTarget & target);

/// @brief One-line, greppable summary of a spike result (for the scripted proof):
/// `texture_id=.. glIsTexture=1 dims=WxH dims_match=1 roundtrip_match=1 match=1.000
///  fence=GL_ALREADY_SIGNALED opengl_frame=1`.
std::string format_spike_log_line(const GlTextureRoundtripResult & r);

}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING__OGRE_GL_TEXTURE_SPIKE_HPP_
