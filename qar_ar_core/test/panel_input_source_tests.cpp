// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The INPUT seam: the queue the runtime's callback thread pushes into, and the
// mechanical mapping from the vendored gesture enums.
//
// Handle-READING (qar_qar_adapter's pointer_sample_from_gesture_event, which calls the
// SDK's accessors) is deliberately NOT tested here: this suite links no Qar runtime,
// which is what lets it run with neither a headset nor a runtime. That half is
// exercised only against the real QarOS runtime.

#include <gtest/gtest.h>

#include "qar_ar_core/panel_input_source.hpp"

#include <vector>

using qar_ar_core::PanelPointerSample;
using qar_ar_core::pointer_gesture_from_panel_kind;
using qar_ar_core::pointer_gesture_from_volume_kind;
using qar_ar_core::pointer_hand_from_qar;
using qar_ar_core::pointer_phase_from_qar;
using qar_ar_core::pointer_sample_from_gesture_fields;
using qar_ar_core::PointerGesture;
using qar_ar_core::PointerHand;
using qar_ar_core::PointerPhase;
using qar_ar_core::QueuedPanelInputSource;
using qar_ar_core::seconds_from_time_point;

namespace
{

/// The app-volume gesture as FIELDS — what the caller has already read off the
/// opaque handle. `with_point` is the has_action_point flag, a real
/// out-parameter of the SDK rather than a struct member.
bool
map_volume_gesture(
  QarGestureKind kind, QarGesturePhase phase, bool with_point, std::uint32_t pointer_id,
  PanelPointerSample & out)
{
  const QarVector3 action_point{0.1f, 0.2f, 0.3f};
  const QarTimePoint timestamp{1500000000};  // nanoseconds
  return pointer_sample_from_gesture_fields(
    kind, phase, with_point, action_point, timestamp, pointer_id, out);
}

}  // namespace

TEST(PointerPhase, MirrorsQarGesturePhaseValueForValue)
{
  // The phase is still a plain field copy, value for value.
  EXPECT_EQ(static_cast<int>(PointerPhase::Started), QAR_GESTURE_PHASE_STARTED);
  EXPECT_EQ(static_cast<int>(PointerPhase::Updated), QAR_GESTURE_PHASE_UPDATED);
  EXPECT_EQ(static_cast<int>(PointerPhase::Ended), QAR_GESTURE_PHASE_ENDED);
  EXPECT_EQ(static_cast<int>(PointerPhase::Instant), QAR_GESTURE_PHASE_INSTANT);
  EXPECT_EQ(static_cast<int>(PointerPhase::Canceled), QAR_GESTURE_PHASE_CANCELED);
}

TEST(PointerGestureMapping, PanelKindsNarrowOntoTheWidgetVocabulary)
{
  // The two vendor kind enums disagree, and value 0 means CLICK on the app
  // volume side and HOVER on the panel side. Nothing may cast between them;
  // this is the whole mapping.
  PointerGesture gesture = PointerGesture::Click;
  ASSERT_TRUE(pointer_gesture_from_panel_kind(QAR_GUI_PANEL_GESTURE_KIND_HOVER, gesture));
  EXPECT_EQ(gesture, PointerGesture::Hover);
  ASSERT_TRUE(pointer_gesture_from_panel_kind(QAR_GUI_PANEL_GESTURE_KIND_TAP, gesture));
  EXPECT_EQ(gesture, PointerGesture::Click);
  ASSERT_TRUE(pointer_gesture_from_panel_kind(QAR_GUI_PANEL_GESTURE_KIND_DRAG, gesture));
  EXPECT_EQ(gesture, PointerGesture::Click) << "a drag IS a press to a widget";
  ASSERT_TRUE(pointer_gesture_from_panel_kind(QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS, gesture));
  EXPECT_EQ(gesture, PointerGesture::LongPress);

  // A kind this build does not know must be refused, not silently read as
  // whatever enumerator happens to share its value.
  EXPECT_FALSE(pointer_gesture_from_panel_kind(static_cast<QarGuiPanelGestureKind>(99), gesture));
}

TEST(PointerGestureMapping, VolumeKindsKeepTheirOldMeaning)
{
  PointerGesture gesture = PointerGesture::Hover;
  ASSERT_TRUE(pointer_gesture_from_volume_kind(QAR_GESTURE_CLICK, gesture));
  EXPECT_EQ(gesture, PointerGesture::Click);
  ASSERT_TRUE(pointer_gesture_from_volume_kind(QAR_GESTURE_HOVER, gesture));
  EXPECT_EQ(gesture, PointerGesture::Hover);
  EXPECT_FALSE(pointer_gesture_from_volume_kind(QAR_GESTURE_SINGLE_POINTER_6DOF, gesture));
}

TEST(PointerGestureMapping, PhaseAndHandRefuseValuesOutsideTheEnums)
{
  PointerPhase phase = PointerPhase::Updated;
  EXPECT_FALSE(pointer_phase_from_qar(static_cast<QarGesturePhase>(42), phase));
  EXPECT_EQ(pointer_hand_from_qar(QAR_HANDEDNESS_LEFT), PointerHand::Left);
  EXPECT_EQ(pointer_hand_from_qar(QAR_HANDEDNESS_RIGHT), PointerHand::Right);
  EXPECT_EQ(pointer_hand_from_qar(QAR_HANDEDNESS_UNSPECIFIED), PointerHand::Unknown);
  EXPECT_EQ(pointer_hand_from_qar(static_cast<QarHandedness>(7)), PointerHand::Unknown);
}

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

TEST(GestureEventMapping, AnInstantClickBecomesAClickSample)
{
  PanelPointerSample sample;
  ASSERT_TRUE(map_volume_gesture(QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_INSTANT, true, 7, sample));
  EXPECT_EQ(sample.pointer_id, 7u);
  EXPECT_EQ(sample.gesture, PointerGesture::Click);
  EXPECT_EQ(sample.phase, PointerPhase::Instant);
  EXPECT_TRUE(sample.has_room_point);
  EXPECT_FALSE(sample.has_panel_uv);
  EXPECT_FLOAT_EQ(sample.room_point.z, 0.3f);
  EXPECT_DOUBLE_EQ(sample.timestamp_s, 1.5);
  // The app-volume path reports neither, and the defaults are what tell the
  // state machine to infer a double click instead of trusting a count.
  EXPECT_EQ(sample.hand, PointerHand::Unknown);
  EXPECT_EQ(sample.tap_count, 0u);
}

TEST(GestureEventMapping, HoverPhasesMapOneToOne)
{
  PanelPointerSample sample;
  ASSERT_TRUE(map_volume_gesture(QAR_GESTURE_HOVER, QAR_GESTURE_PHASE_UPDATED, true, 1, sample));
  EXPECT_EQ(sample.gesture, PointerGesture::Hover);
  EXPECT_EQ(sample.phase, PointerPhase::Updated);
}

TEST(GestureEventMapping, APressWithoutAPointIsRefusedButAnEndIsNot)
{
  PanelPointerSample sample;
  EXPECT_FALSE(map_volume_gesture(QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_STARTED, false, 1, sample));

  // The hand left tracking: no point, but the release still has to happen.
  ASSERT_TRUE(map_volume_gesture(QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_ENDED, false, 1, sample));
  EXPECT_FALSE(sample.has_room_point);
  ASSERT_TRUE(map_volume_gesture(QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_CANCELED, false, 1, sample));
}

TEST(GestureEventMapping, ManipulationGesturesAreNotPointerSamples)
{
  PanelPointerSample sample;
  EXPECT_FALSE(
    map_volume_gesture(QAR_GESTURE_SINGLE_POINTER_6DOF, QAR_GESTURE_PHASE_UPDATED, true, 1, sample));
  EXPECT_FALSE(
    map_volume_gesture(QAR_GESTURE_DUAL_POINTER_ROTATE, QAR_GESTURE_PHASE_UPDATED, true, 1, sample));
}

TEST(GestureEventMapping, TimePointsAreUnixNanoseconds)
{
  const QarTimePoint nanos{2500000000LL};
  EXPECT_DOUBLE_EQ(seconds_from_time_point(nanos), 2.5);
  const QarTimePoint epoch{0};
  EXPECT_DOUBLE_EQ(seconds_from_time_point(epoch), 0.0);
  // A present-day instant stays exact to the microsecond in a double.
  const QarTimePoint now_ish{1790000000123456000LL};
  EXPECT_NEAR(seconds_from_time_point(now_ish), 1790000000.123456, 1e-5);
}

// ---------------------------------------------------------------------------
// The kind -> action mapping, pinned
// ---------------------------------------------------------------------------
//
// The static_asserts in panel_input_source.hpp catch a RENUMBERED vendor enum
// at compile time. These catch the other half: a mapping that still compiles
// but sends the wrong value through, which is what turns a hover into a
// published robot goal. Both vendor enums are spelled out by NUMBER as well as
// by name, because a header that swaps two enumerators' values keeps every name
// valid.

TEST(PointerGestureMapping, VendorKindNumbersAreTheOnesTheMappingAssumes)
{
  EXPECT_EQ(static_cast<int>(QAR_GESTURE_CLICK), 0);
  EXPECT_EQ(static_cast<int>(QAR_GESTURE_HOVER), 1);
  EXPECT_EQ(static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_HOVER), 0);
  EXPECT_EQ(static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_TAP), 1);
  EXPECT_EQ(static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS), 2);
  EXPECT_EQ(static_cast<int>(QAR_GUI_PANEL_GESTURE_KIND_DRAG), 3);
}

TEST(PointerGestureMapping, ZeroMeansOppositeThingsOnTheTwoVendorEnums)
{
  // The trap the two enums set: value 0 is a CLICK on an app volume and a
  // HOVER on a panel. Reading one enum's 0 with the other's table publishes a
  // pick for every hover.
  PointerGesture from_volume_zero{};
  ASSERT_TRUE(
    pointer_gesture_from_volume_kind(static_cast<QarGestureKind>(0), from_volume_zero));
  EXPECT_EQ(from_volume_zero, PointerGesture::Click);

  PointerGesture from_panel_zero{};
  ASSERT_TRUE(
    pointer_gesture_from_panel_kind(
      static_cast<QarGuiPanelGestureKind>(0), from_panel_zero));
  EXPECT_EQ(from_panel_zero, PointerGesture::Hover);

  EXPECT_NE(from_volume_zero, from_panel_zero);
}

TEST(PointerGestureMapping, OnlyClickCommitsAPick)
{
  EXPECT_TRUE(qar_ar_core::gesture_commits_pick(PointerGesture::Click));
  EXPECT_FALSE(qar_ar_core::gesture_commits_pick(PointerGesture::Hover));
  // No consumer yet: treated as a preview, because "nothing happens" is the
  // safe half of that guess.
  EXPECT_FALSE(qar_ar_core::gesture_commits_pick(PointerGesture::LongPress));
}

TEST(PointerGestureMapping, EveryPanelKindThatCommitsIsAPress)
{
  // TAP and DRAG commit; HOVER and LONG_PRESS do not. Spelled out per
  // enumerator so a new kind added to the switch has to be classified here too.
  const struct
  {
    QarGuiPanelGestureKind kind;
    bool commits;
  } cases[] = {
    {QAR_GUI_PANEL_GESTURE_KIND_HOVER, false},
    {QAR_GUI_PANEL_GESTURE_KIND_TAP, true},
    {QAR_GUI_PANEL_GESTURE_KIND_LONG_PRESS, false},
    {QAR_GUI_PANEL_GESTURE_KIND_DRAG, true},
  };
  for (const auto & c : cases) {
    PointerGesture gesture{};
    ASSERT_TRUE(pointer_gesture_from_panel_kind(c.kind, gesture)) << c.kind;
    EXPECT_EQ(qar_ar_core::gesture_commits_pick(gesture), c.commits) << c.kind;
  }
}

TEST(PointerGestureMapping, VolumeHoverNeverCommitsWhateverPhaseItArrivesWith)
{
  // The live shape of the bug: the runtime sends hover as INSTANT, the same
  // phase a tap uses.
  const QarGesturePhase phases[] = {
    QAR_GESTURE_PHASE_STARTED, QAR_GESTURE_PHASE_UPDATED, QAR_GESTURE_PHASE_ENDED,
    QAR_GESTURE_PHASE_INSTANT, QAR_GESTURE_PHASE_CANCELED};
  for (const QarGesturePhase phase : phases) {
    PanelPointerSample sample;
    ASSERT_TRUE(map_volume_gesture(QAR_GESTURE_HOVER, phase, true, 1, sample)) << phase;
    EXPECT_EQ(sample.gesture, PointerGesture::Hover) << phase;
    EXPECT_FALSE(qar_ar_core::gesture_commits_pick(sample.gesture)) << phase;
  }
}

TEST(PointerSampleFromGestureFields, ActionPointIsMarkedAsASceneHit)
{
  // The runtime's action point is its own ray/scene intersection, not a hand
  // position: `qar_app_volume_gesture_get_points` documents an absent point as
  // "the recogniser had no INTERSECTION to report". Downstream needs to know,
  // or it projects an intersection onto a plane a second time.
  PanelPointerSample sample;
  ASSERT_TRUE(
    map_volume_gesture(QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_INSTANT, true, 1, sample));
  EXPECT_TRUE(sample.has_room_point);
  EXPECT_TRUE(sample.room_point_is_scene_hit);
}

TEST(PointerSampleFromGestureFields, AnActionPointEqualToTheStartPointIsNotASceneHit)
{
  // The installed runtime's header says the action point is "the intersection the
  // gesture acts on, OR THE START POINT WHEN NOTHING WAS INTERSECTED". So the
  // two being equal means the runtime handed back the HAND, and flagging that
  // as a scene hit is what puts a published marker on the operator's hand.
  const QarTimePoint timestamp{0};
  const QarVector3 hand{0.4f, 1.3f, -0.2f};

  PanelPointerSample no_hit;
  ASSERT_TRUE(
    pointer_sample_from_gesture_fields(
      QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_INSTANT, true, hand, timestamp, 1, no_hit, true,
      hand));
  EXPECT_TRUE(no_hit.has_room_point);
  EXPECT_FALSE(no_hit.room_point_is_scene_hit);

  // One that DID intersect something is still a hit, and still the normal case.
  PanelPointerSample hit;
  const QarVector3 target{0.4f, 1.3f, -9.0f};
  ASSERT_TRUE(
    pointer_sample_from_gesture_fields(
      QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_INSTANT, true, target, timestamp, 1, hit, true,
      hand));
  EXPECT_TRUE(hit.room_point_is_scene_hit);
}

TEST(PointerSampleFromGestureFields, StartPointIsCarriedWhenThePathSuppliesOne)
{
  PanelPointerSample sample;
  const QarTimePoint timestamp{0};
  const QarVector3 action{1.0f, 2.0f, 3.0f};
  const QarVector3 start{4.0f, 5.0f, 6.0f};
  ASSERT_TRUE(
    pointer_sample_from_gesture_fields(
      QAR_GESTURE_CLICK, QAR_GESTURE_PHASE_STARTED, true, action, timestamp, 1, sample, true,
      start));
  EXPECT_TRUE(sample.has_room_start_point);
  EXPECT_FLOAT_EQ(sample.room_start_point.x, 4.0f);
  // Nothing derives a direction from it: an INSTANT gesture reports it EQUAL to
  // the action point, so it is not an aiming ray.
  EXPECT_FALSE(sample.has_room_ray);
}
