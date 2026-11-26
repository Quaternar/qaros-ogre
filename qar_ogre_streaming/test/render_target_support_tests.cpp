// Copyright 2026 QuaternAR
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

using qar_ogre_streaming::ar_render_keeps;
using qar_ogre_streaming::depth_output_qar_format;
using qar_ogre_streaming::should_report_left_out;

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

// A renderable placed in screen space (RViz's selection box is an Ogre::Rectangle2D)
// would be drawn at the same screen fraction in both eyes, in colour and in depth;
// the AR render leaves it out and keeps every world-space renderable.
TEST(RenderTargetSupport, AScreenSpaceRenderableIsLeftOutOfTheArRender)
{
  EXPECT_FALSE(ar_render_keeps(true));
  EXPECT_TRUE(ar_render_keeps(false));
}

// The objects left out are announced once, when the running total first grows.
TEST(RenderTargetSupport, TheLeftOutObjectsAreReportedOnceWhenTheTotalFirstGrows)
{
  EXPECT_FALSE(should_report_left_out(0, 0, false));  // nothing left out yet
  EXPECT_TRUE(should_report_left_out(0, 1, false));   // first growth
  EXPECT_TRUE(should_report_left_out(3, 5, false));   // first growth after a quiet start
  EXPECT_FALSE(should_report_left_out(1, 2, true));   // already reported
  EXPECT_FALSE(should_report_left_out(2, 2, false));  // no growth
  EXPECT_FALSE(should_report_left_out(4, 1, false));  // total fell (targets recreated)
}

}  // namespace
