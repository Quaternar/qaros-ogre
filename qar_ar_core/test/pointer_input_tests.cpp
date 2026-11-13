// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The pointer vocabulary: the queue a source pushes into and the rule for which gesture may
// commit.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "qar_ar_core/pointer_input.hpp"

using qar_ar_core::PanelPointerSample;
using qar_ar_core::PointerGesture;
using qar_ar_core::QueuedPanelInputSource;

TEST(QueuedInputSource, DrainsInOrderAndEmpties)
{
  QueuedPanelInputSource source;
  for (std::uint32_t i = 0; i < 3; ++i) {
    PanelPointerSample sample;
    sample.pointer_id = i;
    source.push(sample);
  }
  EXPECT_EQ(source.pending(), 3u);

  std::vector<PanelPointerSample> out;
  source.drain(out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].pointer_id, 0u);
  EXPECT_EQ(out[2].pointer_id, 2u);
  EXPECT_EQ(source.pending(), 0u);

  source.drain(out);
  EXPECT_EQ(out.size(), 3u) << "a second drain appends nothing";
}

TEST(PointerGestureMapping, OnlyClickCommitsAPick)
{
  EXPECT_TRUE(qar_ar_core::gesture_commits_pick(PointerGesture::Click));
  EXPECT_FALSE(qar_ar_core::gesture_commits_pick(PointerGesture::Hover));
  // No consumer yet: treated as a preview, because "nothing happens" is the
  // safe half of that guess.
  EXPECT_FALSE(qar_ar_core::gesture_commits_pick(PointerGesture::LongPress));
}
