// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

/// @file render_target_support_tests.cpp
/// @brief Headless (Ogre-free) unit tests for the OgreStereoRenderTarget support
/// layer — specifically the depth-format decision.
///
/// The real create()/render()/readback paths need a live GL context and are
/// therefore exercised by the standalone sample and in RViz, NOT here. These tests
/// deliberately reference ONLY the pure free function depth_output_qar_format(),
/// which lives in its own translation unit that calls no Ogre runtime function — so
/// this test binary stays free of any OgreMain.dll import.

#include <gtest/gtest.h>

#include "qar_ogre_streaming/ogre_stereo_render_target.hpp"

namespace
{

using qar_ogre_streaming::depth_output_qar_format;

// Metric linear eye-space depth is produced as a single-channel F32 texture,
// so the transmitted depth format is ALWAYS R32_FLOAT, independent of the request.
TEST(RenderTargetSupport, DepthOutputIsAlwaysMetricR32Float)
{
  EXPECT_EQ(depth_output_qar_format(QAR_PIXEL_FORMAT_D32_FLOAT), QAR_PIXEL_FORMAT_R32_FLOAT);
  EXPECT_EQ(depth_output_qar_format(QAR_PIXEL_FORMAT_R32_FLOAT), QAR_PIXEL_FORMAT_R32_FLOAT);
  EXPECT_EQ(depth_output_qar_format(QAR_PIXEL_FORMAT_R8G8B8A8), QAR_PIXEL_FORMAT_R32_FLOAT);
}

// The hardware depth-buffer format is explicitly NOT what we produce (it is
// non-linear and not portably sampleable through Ogre 1.12 GL3+).
TEST(RenderTargetSupport, HardwareDepthFormatIsNotProduced)
{
  EXPECT_NE(depth_output_qar_format(QAR_PIXEL_FORMAT_D32_FLOAT), QAR_PIXEL_FORMAT_D32_FLOAT);
}

}  // namespace
