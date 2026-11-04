// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The seam to the URL panels: the defaults of an adapter that cannot detach and calls QarOS at
// once, the empty common name of a description, and the hand index helper.

#include "qar_ar_core/pointing_ray.hpp"
#include "qar_ar_core/qar_url_panel.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace qar_ar_core;

namespace
{

class CountingUrlPanel : public IQarUrlPanelAdapter
{
public:
  bool navigate(const std::string &) override { return true; }
  bool set_placement(const UrlPanelPlacement &) override { return true; }
  bool set_state(QarGuiPanelState) override { return true; }
  void close() override { ++closes; }
  const std::string & last_error() const override { return error; }

  int closes = 0;
  std::string error;
};

}  // namespace

TEST(UrlPanelSeams, AUrlPanelThatCannotDetachClosesItsPanelInstead)
{
  CountingUrlPanel url_panel;
  EXPECT_FALSE(url_panel.supports_detach());
  url_panel.detach();
  EXPECT_EQ(url_panel.closes, 1);
}

TEST(UrlPanelSeams, DescriptionsCarryAnEmptyCommonNameUntilTheHostFillsIt)
{
  EXPECT_TRUE(UrlPanelDesc{}.common_name.empty());
}

TEST(HandIndex, LeftIsZeroRightIsOneUnspecifiedIsNone)
{
  EXPECT_EQ(hand_index(QAR_HANDEDNESS_LEFT), 0);
  EXPECT_EQ(hand_index(QAR_HANDEDNESS_RIGHT), 1);
  EXPECT_EQ(hand_index(QAR_HANDEDNESS_UNSPECIFIED), -1);

  HandJointsProbe probe;
  probe.right.tracked = true;
  EXPECT_TRUE(probe.hand(QAR_HANDEDNESS_RIGHT).tracked);
  EXPECT_FALSE(probe.hand(QAR_HANDEDNESS_LEFT).tracked);
  EXPECT_FALSE(probe.hand(QAR_HANDEDNESS_UNSPECIFIED).tracked);
}

TEST(UrlPanelSeams, AUrlPanelThatCallsQarOsAtOnceIsNeverPendingAndHasNoResults)
{
  std::vector<PanelOpResult> results;
  CountingUrlPanel url_panel;
  EXPECT_FALSE(url_panel.pending());
  url_panel.drain_results(results);
  EXPECT_TRUE(results.empty());
}
