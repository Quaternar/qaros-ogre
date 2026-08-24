// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

/// @file ogre_stereo_render_target_support.cpp
/// @brief Pure (Ogre-free) helpers for OgreStereoRenderTarget.
///
/// These live in their own translation unit deliberately: they call NO Ogre
/// runtime function, so a unit test that references them does not drag the real
/// Ogre-calling code (createManual/render/blit) — and therefore OgreMain.dll —
/// onto the test executable's import table. That keeps the headless unit-test
/// binary Ogre-free while still covering the M4 C3 depth-format decision.

#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"

namespace qar_ogre_streaming
{

QarPixelFormat depth_output_qar_format(QarPixelFormat /*requested*/) noexcept
{
  // C3 (see docs/api_gaps.md): metric linear eye-space depth is produced as a
  // shader-written single-channel 32-bit float texture, so the transmitted depth
  // format is ALWAYS R32_FLOAT, independent of what the caller requested (e.g.
  // the hardware-depth QAR_PIXEL_FORMAT_D32_FLOAT is deliberately NOT used, as it
  // is non-linear and not portably sampleable through Ogre 1.12 GL3+).
  return QAR_PIXEL_FORMAT_R32_FLOAT;
}

}  // namespace qar_ogre_streaming
