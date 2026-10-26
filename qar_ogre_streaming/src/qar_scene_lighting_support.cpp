// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

/// @file qar_scene_lighting_support.cpp
/// @brief Pure (Ogre-free) helpers for the QAR scene-lighting rig.
///
/// Same split as ogre_stereo_render_target_support.cpp / ogre_qar_stereo_stream_
/// support.cpp: mode parsing, naming and config validation call NO Ogre runtime
/// function, so the headless unit tests can exercise them without OgreMain.dll ever
/// being loaded. The Ogre-touching light creation lives in qar_scene_lighting.cpp
/// and is validated live in RViz.

#include "qar_ogre_streaming/qar_scene_lighting.hpp"

#include <cctype>
#include <string>

namespace qar_ogre_streaming
{
namespace
{

bool rgb_valid(const Rgb & c)
{
  return c.r >= 0.0f && c.g >= 0.0f && c.b >= 0.0f;
}

/// Lower-case, drop whitespace and map '_' to '-', so `PER_PEER`, `per-peer` and
/// ` Per Peer ` all name the same mode.
std::string canonical(const std::string & text)
{
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    if (std::isspace(ch) != 0) {
      continue;
    }
    out.push_back(raw == '_' ? '-' : static_cast<char>(std::tolower(ch)));
  }
  return out;
}

}  // namespace

const char * to_string(SceneLightingMode mode)
{
  switch (mode) {
    case SceneLightingMode::kOff:
      return "off";
    case SceneLightingMode::kOmni:
      return "omni";
    case SceneLightingMode::kPerPeer:
      return "per-peer";
    case SceneLightingMode::kOmniAndPerPeer:
      return "both";
  }
  return "unknown";
}

bool parse_scene_lighting_mode(const std::string & text, SceneLightingMode & out)
{
  const std::string t = canonical(text);
  if (t == "off" || t == "none") {
    out = SceneLightingMode::kOff;
  } else if (t == "omni" || t == "point") {
    out = SceneLightingMode::kOmni;
  } else if (t == "per-peer" || t == "peer") {
    out = SceneLightingMode::kPerPeer;
  } else if (t == "both" || t == "omni-and-per-peer") {
    out = SceneLightingMode::kOmniAndPerPeer;
  } else {
    return false;
  }
  return true;
}

Result SceneLightingConfig::validate() const
{
  if (!rgb_valid(ambient)) {
    return Result::error(Status::InvalidArgument, "ambient colour has a negative component");
  }
  if (!rgb_valid(key_diffuse) || !rgb_valid(omni_specular)) {
    return Result::error(Status::InvalidArgument, "omni colour has a negative component");
  }
  if (fill_ratio < 0.0f) {
    return Result::error(Status::InvalidArgument, "fill_ratio must not be negative");
  }
  if (!rgb_valid(peer_diffuse) || !rgb_valid(peer_specular)) {
    return Result::error(Status::InvalidArgument, "per-peer colour has a negative component");
  }
  if (attenuation_range < 0.0f) {
    return Result::error(Status::InvalidArgument, "attenuation_range must not be negative");
  }
  return Result::success();
}

}  // namespace qar_ogre_streaming
