// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The pure parts of the one line the 3D stream prints for the first frame after a stall: when a
// frame follows a stall, what the colour read back averages to, and the words of the line. The
// read-back itself needs a GL context and a QarOS session.

#include <gtest/gtest.h>

#include "qar_qar_adapter/frame_stall_probe.hpp"

#include <cstdint>
#include <vector>

using qar_qar_adapter::FrameStallDetector;
using qar_qar_adapter::StallPoint;
using qar_qar_adapter::StallReport;
using qar_qar_adapter::format_stall_line;
using qar_qar_adapter::kStallThresholdSeconds;
using qar_qar_adapter::mean_rgba8;

namespace
{

const StallPoint kLeft{-0.03f, 1.0f, 2.0f};
const StallPoint kRight{0.03f, 3.0f, 4.0f};

}  // namespace

TEST(FrameStall, TheThresholdIsTwoSeconds)
{
  EXPECT_DOUBLE_EQ(kStallThresholdSeconds, 2.0);
}

TEST(FrameStall, NothingFollowsAStallBeforeTheFirstFrameWasShown)
{
  FrameStallDetector detector;
  StallReport report;
  // However long the stream has existed, a first frame has no earlier frame to be apart from.
  detector.frame_ready(500.0, kLeft, kRight);
  EXPECT_FALSE(detector.take(report));
}

TEST(FrameStall, AFrameAfterAShortGapIsNotAStall)
{
  FrameStallDetector detector;
  StallReport report;
  detector.frame_shown(10.0);
  detector.frame_ready(10.016, kLeft, kRight);
  EXPECT_FALSE(detector.take(report));
  detector.frame_ready(11.999, kLeft, kRight);
  EXPECT_FALSE(detector.take(report));
}

TEST(FrameStall, AFrameAfterTwoSecondsOrMoreIsTheFirstAfterAStallAndSaysWhereTheHeadIs)
{
  FrameStallDetector detector;
  StallReport report;
  detector.frame_shown(10.0);

  detector.frame_ready(12.0, kLeft, kRight);   // exactly at the threshold counts
  ASSERT_TRUE(detector.take(report));
  EXPECT_DOUBLE_EQ(report.stall_seconds, 2.0);
  EXPECT_FLOAT_EQ(report.head.x, 0.0f);
  EXPECT_FLOAT_EQ(report.head.y, 2.0f);
  EXPECT_FLOAT_EQ(report.head.z, 3.0f);
}

TEST(FrameStall, TheStallIsReportedOnceAndEndsWithTheFrameThatIsShown)
{
  FrameStallDetector detector;
  StallReport report;
  detector.frame_shown(1.0);
  detector.frame_ready(8.2, kLeft, kRight);
  ASSERT_TRUE(detector.take(report));
  EXPECT_NEAR(report.stall_seconds, 7.2, 1e-9);
  EXPECT_FALSE(detector.take(report)) << "once";

  // The frame is shown; the next one follows it closely and is an ordinary frame.
  detector.frame_shown(8.2);
  detector.frame_ready(8.22, kLeft, kRight);
  EXPECT_FALSE(detector.take(report));

  // A second stall later is reported again, on its own.
  detector.frame_shown(8.22);
  detector.frame_ready(20.22, kLeft, kRight);
  ASSERT_TRUE(detector.take(report));
  EXPECT_NEAR(report.stall_seconds, 12.0, 1e-9);
}

TEST(FrameStall, AFrameThatWasReadyButNotShownLeavesTheStallForTheNextOne)
{
  FrameStallDetector detector;
  StallReport report;
  detector.frame_shown(1.0);

  // Ready at 4.0 s but never shown (the copy failed, say); the next frame is ready at 4.5 s.
  // The stall is measured to the frame that is shown, with that frame's head.
  detector.frame_ready(4.0, kLeft, kRight);
  detector.frame_ready(4.5, StallPoint{1.0f, 1.0f, 1.0f}, StallPoint{3.0f, 3.0f, 3.0f});
  ASSERT_TRUE(detector.take(report));
  EXPECT_NEAR(report.stall_seconds, 3.5, 1e-9);
  EXPECT_FLOAT_EQ(report.head.x, 2.0f);
  EXPECT_FLOAT_EQ(report.head.y, 2.0f);
  EXPECT_FLOAT_EQ(report.head.z, 2.0f);
}

TEST(FrameStall, ADifferentThresholdIsHonoured)
{
  FrameStallDetector detector(0.5);
  StallReport report;
  detector.frame_shown(0.0);
  detector.frame_ready(0.4, kLeft, kRight);
  EXPECT_FALSE(detector.take(report));
  detector.frame_ready(0.5, kLeft, kRight);
  EXPECT_TRUE(detector.take(report));
}

TEST(FrameStall, TheMeanColourIsTheAverageOfTheSampledPixelsPerChannel)
{
  // 4 x 2 pixels, rows tightly packed: red, green, blue, white / four black ones.
  std::vector<std::uint8_t> rgba = {
    255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 255, 255,
    0, 0, 0, 255,     0, 0, 0, 255,     0, 0, 0, 255,     0, 0, 0, 255};
  float rgb[3] = {-1.0f, -1.0f, -1.0f};
  ASSERT_TRUE(mean_rgba8(rgba.data(), 4, 2, 1, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 2.0f * 255.0f / 8.0f);
  EXPECT_FLOAT_EQ(rgb[1], 2.0f * 255.0f / 8.0f);
  EXPECT_FLOAT_EQ(rgb[2], 2.0f * 255.0f / 8.0f);
}

TEST(FrameStall, OnlyEveryStepthPixelInXAndYIsAveraged)
{
  // 8 x 8, all black except the pixels a step of 4 reads: (0,0), (4,0), (0,4), (4,4).
  std::vector<std::uint8_t> rgba(8 * 8 * 4, 0);
  for (const std::uint32_t y : {0u, 4u}) {
    for (const std::uint32_t x : {0u, 4u}) {
      std::uint8_t * p = rgba.data() + (static_cast<std::size_t>(y) * 8 + x) * 4;
      p[0] = 200;
      p[1] = 100;
      p[2] = 40;
    }
  }
  float rgb[3] = {};
  ASSERT_TRUE(mean_rgba8(rgba.data(), 8, 8, 4, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 200.0f);
  EXPECT_FLOAT_EQ(rgb[1], 100.0f);
  EXPECT_FLOAT_EQ(rgb[2], 40.0f);
  ASSERT_TRUE(mean_rgba8(rgba.data(), 8, 8, 1, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 200.0f * 4.0f / 64.0f);
}

TEST(FrameStall, NoDataOrNoSizeIsNoColour)
{
  float rgb[3] = {9.0f, 9.0f, 9.0f};
  std::vector<std::uint8_t> rgba(4 * 4 * 4, 255);
  EXPECT_FALSE(mean_rgba8(nullptr, 4, 4, 1, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 0.0f);
  rgb[0] = 9.0f;
  EXPECT_FALSE(mean_rgba8(rgba.data(), 0, 4, 1, rgb));
  EXPECT_FALSE(mean_rgba8(rgba.data(), 4, 0, 1, rgb));
  EXPECT_FALSE(mean_rgba8(rgba.data(), 4, 4, 0, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 0.0f);
  EXPECT_TRUE(mean_rgba8(rgba.data(), 4, 4, 1, rgb));
}

TEST(FrameStall, TheLineHasTheWordsTheHumanCheckLooksFor)
{
  StallReport report;
  report.stall_seconds = 7.2;
  report.head = StallPoint{0.0f, -1.0f, 1.0f};
  const float rgb[3] = {12.34f, 45.67f, 78.9f};
  EXPECT_EQ(
    format_stall_line(report, true, rgb),
    "LIVE: first 3D frame after a 7.2 s stall: mean colour (12.3, 45.7, 78.9), "
    "head app (0.00, -1.00, 1.00)");
}

TEST(FrameStall, AColourThatCouldNotBeReadBackIsSaidSo)
{
  StallReport report;
  report.stall_seconds = 2.04;
  report.head = StallPoint{1.234f, 0.0f, -5.0f};
  const float none[3] = {0.0f, 0.0f, 0.0f};
  EXPECT_EQ(
    format_stall_line(report, false, none),
    "LIVE: first 3D frame after a 2.0 s stall: mean colour (unavailable), "
    "head app (1.23, 0.00, -5.00)");
}

// ---------------------------------------------------------------------------
// The lines that name the threads and the textures of a sender when it starts. The calls
// themselves need a QarOS session; what is pinned here is the words, which the log scorecard reads.
// ---------------------------------------------------------------------------

TEST(SenderStartLines, TheCreateRequestNamesThePeerAndTheAskingThread)
{
  EXPECT_EQ(
    qar_qar_adapter::format_create_async_line("9733c6bf", 14972ull),
    "peer 9733c6bf create_async tid=14972");
}

TEST(SenderStartLines, TheHandOverNamesTheCallbackThreadAndTheWait)
{
  EXPECT_EQ(
    qar_qar_adapter::format_created_line("9733c6bf", 20468ull, 5.7049),
    "peer 9733c6bf created (callback tid=20468) after 5.70 s");
  EXPECT_EQ(
    qar_qar_adapter::format_created_line("9733c6bf", 0ull, 0.0),
    "peer 9733c6bf created (callback tid=0) after 0.00 s");
}

TEST(SenderStartLines, TheFirstFrameNamesTheThreadTheFourTexturesAndTheSize)
{
  qar_qar_adapter::SenderTextureIds ids;
  ids.colour[0] = 4;
  ids.colour[1] = 5;
  ids.depth[0] = 6;
  ids.depth[1] = 7;
  ids.width = 1920;
  ids.height = 1080;
  EXPECT_EQ(
    qar_qar_adapter::format_first_show_line("9733c6bf", 14972ull, ids),
    "peer 9733c6bf first show_frame tid=14972 tex=c4/5 d6/7 1920x1080");
}

TEST(SenderStartLines, AFrameWithoutDepthShowsZerosForTheDepthTextures)
{
  qar_qar_adapter::SenderTextureIds ids;
  ids.colour[0] = 11;
  ids.colour[1] = 12;
  ids.width = 640;
  ids.height = 480;
  EXPECT_EQ(
    qar_qar_adapter::format_first_show_line("ab", 1ull, ids),
    "peer ab first show_frame tid=1 tex=c11/12 d0/0 640x480");
}

TEST(SenderStartLines, TheBindNamesTheContextTheDeviceContextAndTheBindingThread)
{
  const int context_marker = 0;
  const int device_marker = 0;
  const std::string bound = qar_qar_adapter::format_sdk_bound_line(
    "9733c6bf", &context_marker, &device_marker, 20468ull);
  const std::string expected = "peer 9733c6bf SDK bound ctx " +
    qar_qar_adapter::format_handle(&context_marker) + " hdc=" +
    qar_qar_adapter::format_handle(&device_marker) + " tid=20468";
  EXPECT_EQ(bound, expected);
  EXPECT_EQ(bound.find("SDK bound ctx 0x"), std::string("peer 9733c6bf ").size());
}

TEST(SenderStartLines, AHandleIsLowerCaseHexWithThePrefixOnEveryPlatform)
{
  EXPECT_EQ(qar_qar_adapter::format_handle(nullptr), "0x0");
  const std::uintptr_t address = 0xABCDEF;
  EXPECT_EQ(
    qar_qar_adapter::format_handle(reinterpret_cast<const void *>(address)), "0xabcdef");
}
