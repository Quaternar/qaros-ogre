// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The reattach decision (pure, from probe values), its log line and the outcome record of a panel
// operation.

#include "qar_ar_core/panel_attach.hpp"
#include "qar_ar_core/panel_op_result.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace qar_ar_core;

namespace
{

PanelProbe
found_probe(QarGuiPanelState state, bool content_differs, bool size_differs)
{
  PanelProbe probe;
  probe.found = true;
  probe.state = state;
  probe.content_differs = content_differs;
  probe.size_differs = size_differs;
  return probe;
}

}  // namespace

TEST(PanelAttach, ANewPanelIsCreatedWithNothingToCarryOut)
{
  const PanelAttachDecision decision =
    decide_panel_attach(found_probe(QAR_GUI_PANEL_STATE_VISIBLE, false, false), 0);
  EXPECT_EQ(decision.kind, PanelAttachKind::Created);
  EXPECT_FALSE(decision.swap_content);
  EXPECT_FALSE(decision.change_size);
  EXPECT_FALSE(decision.show);

  PanelProbe not_listed;
  EXPECT_EQ(decide_panel_attach(not_listed, 0).kind, PanelAttachKind::Created);
}

TEST(PanelAttach, AHiddenPanelOfAnEarlierSessionIsReattachedAndShown)
{
  const PanelAttachDecision hidden =
    decide_panel_attach(found_probe(QAR_GUI_PANEL_STATE_HIDDEN, true, false), 0);
  EXPECT_EQ(hidden.kind, PanelAttachKind::Reattached);
  EXPECT_TRUE(hidden.swap_content);
  EXPECT_FALSE(hidden.change_size);
  EXPECT_TRUE(hidden.show);

  const PanelAttachDecision minimized =
    decide_panel_attach(found_probe(QAR_GUI_PANEL_STATE_MINIMIZED, true, true), 0);
  EXPECT_EQ(minimized.kind, PanelAttachKind::Reattached);
  EXPECT_TRUE(minimized.change_size);
  EXPECT_TRUE(minimized.show);

  // A visible panel is not shown again, but its stream is replaced.
  const PanelAttachDecision visible =
    decide_panel_attach(found_probe(QAR_GUI_PANEL_STATE_VISIBLE, true, false), 0);
  EXPECT_EQ(visible.kind, PanelAttachKind::Reattached);
  EXPECT_TRUE(visible.swap_content);
  EXPECT_FALSE(visible.show);
}

TEST(PanelAttach, APanelOfTheOtherContentTypeIsRetriedOnceWithV2ThenRefused)
{
  PanelProbe web = found_probe(QAR_GUI_PANEL_STATE_VISIBLE, true, false);
  web.content_mismatch = true;
  const PanelAttachDecision first = decide_panel_attach(web, 0);
  EXPECT_EQ(first.kind, PanelAttachKind::RetryWithSuffix);
  EXPECT_EQ(first.retry_suffix, "-v2");
  EXPECT_EQ(first.reason, "other content type");

  const PanelAttachDecision second = decide_panel_attach(web, 1);
  EXPECT_EQ(second.kind, PanelAttachKind::Refused);
  EXPECT_TRUE(second.retry_suffix.empty());
}

TEST(PanelAttach, AClosedPanelWalksR1ToR8ThenRefuses)
{
  const PanelProbe closed = found_probe(QAR_GUI_PANEL_STATE_CLOSED, true, false);
  const PanelAttachDecision first = decide_panel_attach(closed, 0);
  EXPECT_EQ(first.kind, PanelAttachKind::RetryWithSuffix);
  EXPECT_EQ(first.retry_suffix, "-r1");
  EXPECT_EQ(first.reason, "closed");
  EXPECT_EQ(decide_panel_attach(closed, 1).retry_suffix, "-r2");
  EXPECT_EQ(decide_panel_attach(closed, 7).retry_suffix, "-r8");
  const PanelAttachDecision last = decide_panel_attach(closed, kMaxClosedPanelRetries);
  EXPECT_EQ(last.kind, PanelAttachKind::Refused);
  EXPECT_TRUE(last.retry_suffix.empty());
  EXPECT_EQ(last.reason, "closed");

  // The content type is checked before the state.
  PanelProbe closed_web = closed;
  closed_web.content_mismatch = true;
  EXPECT_EQ(decide_panel_attach(closed_web, 0).retry_suffix, "-v2");
  EXPECT_EQ(decide_panel_attach(closed_web, 1).kind, PanelAttachKind::Refused);
}

namespace
{

/// What an adapter ends with when the first @p closed_names names it tries (the base name, then
/// "-r1", "-r2", ...) belong to CLOSED panels and the next one to a free name. The loop is the
/// adapters': ask, probe, decide, and try the base name plus the suffix again.
struct WalkResult
{
  PanelAttachKind kind = PanelAttachKind::Created;
  std::string name;
  unsigned tries = 0;
};

WalkResult
walk(unsigned closed_names)
{
  WalkResult result;
  for (unsigned retries = 0;; ++retries) {
    ++result.tries;
    PanelProbe probe;
    if (retries < closed_names) {
      probe = found_probe(QAR_GUI_PANEL_STATE_CLOSED, true, false);
    }
    const PanelAttachDecision decision = decide_panel_attach(probe, retries);
    if (decision.kind != PanelAttachKind::RetryWithSuffix) {
      result.kind = decision.kind;
      return result;
    }
    result.name = "base" + decision.retry_suffix;
  }
}

}  // namespace

TEST(PanelAttach, TheWalkOverClosedNamesEndsAtTheFirstFreeNameWithinTheBound)
{
  const WalkResult none = walk(0);
  EXPECT_EQ(none.kind, PanelAttachKind::Created);
  EXPECT_EQ(none.name, "");
  EXPECT_EQ(none.tries, 1u);

  const WalkResult one = walk(1);
  EXPECT_EQ(one.kind, PanelAttachKind::Created);
  EXPECT_EQ(one.name, "base-r1");

  const WalkResult two = walk(2);
  EXPECT_EQ(two.kind, PanelAttachKind::Created);
  EXPECT_EQ(two.name, "base-r2");

  const WalkResult eight = walk(8);
  EXPECT_EQ(eight.kind, PanelAttachKind::Created);
  EXPECT_EQ(eight.name, "base-r8") << "eight closed names still leave the eighth suffix";
  EXPECT_EQ(eight.tries, 9u);

  const WalkResult nine = walk(9);
  EXPECT_EQ(nine.kind, PanelAttachKind::Refused) << "nine closed names exhaust the bound";
  EXPECT_EQ(nine.tries, 9u);
}

TEST(PanelAttach, TheVisibleToListIsBroughtToTheWantedOne)
{
  QarPeerId a{};
  QarPeerId b{};
  QarPeerId c{};
  a.data[0] = 1;
  b.data[0] = 2;
  c.data[0] = 3;

  EXPECT_TRUE(plan_visible_to({a, b}, {b, a}).empty());
  EXPECT_TRUE(plan_visible_to({}, {}).empty());

  const VisibleToPlan narrowed = plan_visible_to({a, b}, {b});
  EXPECT_TRUE(narrowed.additions.empty());
  ASSERT_EQ(narrowed.removals.size(), 1u);
  EXPECT_EQ(narrowed.removals[0].data[0], 1);

  const VisibleToPlan widened = plan_visible_to({a}, {a, c});
  ASSERT_EQ(widened.additions.size(), 1u);
  EXPECT_EQ(widened.additions[0].data[0], 3);
  EXPECT_TRUE(widened.removals.empty());

  const VisibleToPlan from_everyone = plan_visible_to({}, {a, a});
  ASSERT_EQ(from_everyone.additions.size(), 1u) << "a peer named twice is added once";

  const VisibleToPlan to_everyone = plan_visible_to({a, b}, {});
  EXPECT_TRUE(to_everyone.additions.empty());
  EXPECT_EQ(to_everyone.removals.size(), 2u) << "an empty wanted list removes every peer";
}

TEST(PanelAttach, TheLogLineHasTheDocumentedFormat)
{
  PanelAttachLogFields fields;
  fields.time = "12:34:56.789";
  fields.key = "hidden:Displays";
  fields.reattached = true;
  fields.common_name = "hidden-displays-0123abcd.host-plugin.devpc.qaros-ros";
  fields.panel_id = "f75868aa-9802-40b3-92c1-10438545623f";
  fields.state_was = panel_state_name(QAR_GUI_PANEL_STATE_HIDDEN);
  fields.stream_swapped = true;
  fields.size_changed = false;
  EXPECT_EQ(
    panel_attach_log_line(fields),
    "[qar-panel-adapter] 12:34:56.789 panel 'hidden:Displays' reattached "
    "name=hidden-displays-0123abcd.host-plugin.devpc.qaros-ros "
    "id=f75868aa-9802-40b3-92c1-10438545623f state_was=HIDDEN stream_swapped=y size_changed=n");

  fields.reattached = false;
  EXPECT_NE(panel_attach_log_line(fields).find("' created name="), std::string::npos);
  EXPECT_EQ(panel_state_name(QAR_GUI_PANEL_STATE_CLOSED), "CLOSED");
  EXPECT_EQ(panel_state_name(static_cast<QarGuiPanelState>(9)), "state 9");
}

TEST(PanelSeams, AnOutcomeStartsAsASuccessfulCreate)
{
  const PanelOpResult result;
  EXPECT_EQ(result.op, PanelOpResult::Op::Create);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.reattached);
  EXPECT_TRUE(result.error.empty());
}
