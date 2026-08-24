// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "qar_ogre_streaming/render_budget.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;

TEST(StereoStreamConfig, DefaultsAreValid)
{
  qos::StereoStreamConfig cfg;
  cfg.peer_id = qos::test::make_peer_id(1);
  EXPECT_TRUE(cfg.validate().ok());
}

TEST(StereoStreamConfig, RejectsZeroDimensions)
{
  qos::StereoStreamConfig cfg;
  cfg.width = 0;
  const qos::Result r = cfg.validate();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status, qos::Status::InvalidArgument);

  cfg.width = 640;
  cfg.height = 0;
  EXPECT_EQ(cfg.validate().status, qos::Status::InvalidArgument);
}

TEST(StereoStreamConfig, RejectsZeroFps)
{
  qos::StereoStreamConfig cfg;
  cfg.max_fps = 0;
  EXPECT_EQ(cfg.validate().status, qos::Status::InvalidArgument);
}

TEST(StereoStreamConfig, RejectsNonSeparatedLayout)
{
  qos::StereoStreamConfig cfg;
  cfg.layout = QAR_FRAME_LAYOUT_SIDE_BY_SIDE;
  EXPECT_EQ(cfg.validate().status, qos::Status::InvalidArgument);

  cfg.layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;
  EXPECT_TRUE(cfg.validate().ok());
}

TEST(PeerId, EqualityAndHex)
{
  const QarPeerId a = qos::test::make_peer_id(0xAB);
  const QarPeerId b = qos::test::make_peer_id(0xAB);
  const QarPeerId c = qos::test::make_peer_id(0x01);
  EXPECT_TRUE(qos::peer_id_equal(a, b));
  EXPECT_FALSE(qos::peer_id_equal(a, c));
  // 16 bytes of 0xAB -> 32 hex chars: "ab" repeated 16 times.
  std::string expected;
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    expected += "ab";
  }
  EXPECT_EQ(qos::peer_id_to_hex(a), expected);
}

TEST(RenderBudget, FpsGate)
{
  qos::RenderBudget budget;
  // No cap => always due.
  EXPECT_TRUE(budget.is_frame_due(0, 100.0, 100.0));
  // 60 fps => min interval ~0.0167s.
  EXPECT_FALSE(budget.is_frame_due(60, 1.000, 1.000));
  EXPECT_FALSE(budget.is_frame_due(60, 1.010, 1.000));
  EXPECT_TRUE(budget.is_frame_due(60, 1.020, 1.000));

  EXPECT_EQ(budget.skipped_frames(), 0u);
  budget.note_skipped_frame();
  EXPECT_EQ(budget.skipped_frames(), 1u);
}
