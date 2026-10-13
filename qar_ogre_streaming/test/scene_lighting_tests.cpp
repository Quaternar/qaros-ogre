// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

/// @file scene_lighting_tests.cpp
/// @brief Headless tests for the Ogre-free half of the QAR scene-lighting rig
/// (mode parsing/naming + config validation), plus the per-peer head-position
/// derivation the rig is driven from.
///
/// The Ogre-touching half (light/scene-node creation, hiding RViz's headlight) needs
/// a real scene manager and is validated live in RViz — see
/// docs/ARCHITECTURE.md#scene-lighting.

#include <gtest/gtest.h>

#include <string>

#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/qar_scene_lighting.hpp"

using qar_ogre_streaming::EyeView;
using qar_ogre_streaming::Rgb;
using qar_ogre_streaming::SceneLightingConfig;
using qar_ogre_streaming::SceneLightingMode;
using qar_ogre_streaming::Status;
using qar_ogre_streaming::parse_scene_lighting_mode;
using qar_ogre_streaming::to_string;

namespace
{

SceneLightingMode parse_or(const char * text, SceneLightingMode fallback)
{
  SceneLightingMode out = fallback;
  parse_scene_lighting_mode(text, out);
  return out;
}

}  // namespace

TEST(SceneLightingMode, NamesRoundTripThroughTheParser)
{
  for (const SceneLightingMode mode : {
      SceneLightingMode::kOff, SceneLightingMode::kOmni,
      SceneLightingMode::kPerPeer, SceneLightingMode::kOmniAndPerPeer})
  {
    SceneLightingMode parsed = SceneLightingMode::kOff;
    ASSERT_TRUE(parse_scene_lighting_mode(to_string(mode), parsed))
      << "to_string produced a name the parser rejects: " << to_string(mode);
    EXPECT_EQ(parsed, mode);
  }
}

TEST(SceneLightingMode, ParsesCaseSeparatorAndWhitespaceVariants)
{
  EXPECT_EQ(parse_or("PER_PEER", SceneLightingMode::kOff), SceneLightingMode::kPerPeer);
  EXPECT_EQ(parse_or(" per-peer ", SceneLightingMode::kOff), SceneLightingMode::kPerPeer);
  EXPECT_EQ(parse_or("Omni", SceneLightingMode::kOff), SceneLightingMode::kOmni);
  EXPECT_EQ(parse_or("BOTH", SceneLightingMode::kOff), SceneLightingMode::kOmniAndPerPeer);
  EXPECT_EQ(parse_or("none", SceneLightingMode::kOmni), SceneLightingMode::kOff);
}

TEST(SceneLightingMode, UnknownTextIsRejectedAndLeavesTheOutputAlone)
{
  SceneLightingMode out = SceneLightingMode::kOmni;
  EXPECT_FALSE(parse_scene_lighting_mode("chandelier", out));
  EXPECT_EQ(out, SceneLightingMode::kOmni) << "a rejected parse must not clobber the default";
  EXPECT_FALSE(parse_scene_lighting_mode("", out));
  EXPECT_EQ(out, SceneLightingMode::kOmni);
}

TEST(SceneLightingConfig, DefaultsAreValidAndOmniIsTheLibraryDefault)
{
  const SceneLightingConfig cfg;
  EXPECT_TRUE(cfg.validate().ok()) << cfg.validate().message;
  EXPECT_EQ(cfg.mode, SceneLightingMode::kOmni);
  // The rig must leave nothing unlit: the "from below" fills are dimmer than the key
  // but never absent, or undersides go black.
  EXPECT_GT(cfg.fill_ratio, 0.0f);
  EXPECT_LE(cfg.fill_ratio, 1.0f);
  // No host light is touched unless a name is supplied — the library must not know
  // (or guess) RViz's "MainDirectional"; the RViz panel passes it in.
  EXPECT_TRUE(cfg.suppress_light_name.empty());
}

TEST(SceneLightingConfig, RejectsNegativeColoursAndRange)
{
  SceneLightingConfig cfg;
  cfg.ambient = Rgb{-0.1f, 0.0f, 0.0f};
  EXPECT_EQ(cfg.validate().status, Status::InvalidArgument);

  cfg = SceneLightingConfig{};
  cfg.key_diffuse = Rgb{0.0f, -1.0f, 0.0f};
  EXPECT_EQ(cfg.validate().status, Status::InvalidArgument);

  cfg = SceneLightingConfig{};
  cfg.fill_ratio = -0.1f;
  EXPECT_EQ(cfg.validate().status, Status::InvalidArgument);

  cfg = SceneLightingConfig{};
  cfg.peer_specular = Rgb{0.0f, 0.0f, -2.0f};
  EXPECT_EQ(cfg.validate().status, Status::InvalidArgument);

  cfg = SceneLightingConfig{};
  cfg.attenuation_range = -5.0f;
  EXPECT_EQ(cfg.validate().status, Status::InvalidArgument);
}

// --- The input the per-peer rig is driven from -----------------------------
// current_head_position() must return the MIDPOINT of the two eyes, through the same
// qar_position_to_ogre mapping the eye cameras use — otherwise a peer's light lands
// somewhere its own camera is not.

TEST(PeerHeadPosition, IsTheMidpointOfTheTwoEyesUnderTheCameraMapping)
{
  EyeView left{};
  left.pose.position = QarVector3{-0.032f, 1.5f, -4.0f};
  left.pose.orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
  EyeView right = left;
  right.pose.position = QarVector3{0.032f, 1.5f, -4.0f};

  const qar_ogre_streaming::Vec3 l =
    qar_ogre_streaming::compute_eye_render_params(left).position;
  const qar_ogre_streaming::Vec3 r =
    qar_ogre_streaming::compute_eye_render_params(right).position;

  // The midpoint the rig places the light at, expressed from the same conversion the
  // cameras were placed with. An IPD-symmetric pair must average back to eye centre.
  const qar_ogre_streaming::Vec3 mid{
    (l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
  const qar_ogre_streaming::Vec3 centre =
    qar_ogre_streaming::qar_position_to_ogre(QarVector3{0.0f, 1.5f, -4.0f});

  EXPECT_NEAR(mid.x, centre.x, 1e-5f);
  EXPECT_NEAR(mid.y, centre.y, 1e-5f);
  EXPECT_NEAR(mid.z, centre.z, 1e-5f);
}
