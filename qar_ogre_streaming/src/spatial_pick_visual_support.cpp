// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The Ogre-FREE half of spatial_pick_visual.hpp: where the feedback lines go.
// Kept in its own translation unit so the unit tests exercise it with no Ogre
// and no GL context, exactly as qar_scene_lighting_support.cpp does.

#include "qar_ogre_streaming/spatial_pick_visual.hpp"

#include <cmath>

namespace qar_ogre_streaming
{
namespace
{

constexpr float kEpsilon = 1e-6f;
constexpr float kPi = 3.14159265358979323846f;

bool finite(const Vec3 & v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool normalized(const Vec3 & v, Vec3 & out)
{
  if (!finite(v)) {
    return false;
  }
  const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (!(length > kEpsilon)) {
    return false;
  }
  out = Vec3{v.x / length, v.y / length, v.z / length};
  return true;
}

void add(PickVisualGeometry & out, const Vec3 & from, const Vec3 & to, const Rgb & color)
{
  out.segments.push_back(PickVisualSegment{from, to, color});
}

}  // namespace

void append_ray(
  PickVisualGeometry & out, const Vec3 & origin, const Vec3 & direction, float length_m,
  const Rgb & color)
{
  Vec3 unit{};
  if (!finite(origin) || !normalized(direction, unit) || !std::isfinite(length_m) ||
    !(length_m > 0.0f))
  {
    return;  // nothing truthful to draw
  }
  add(
    out, origin,
    Vec3{origin.x + unit.x * length_m, origin.y + unit.y * length_m,
      origin.z + unit.z * length_m},
    color);
}

void append_point_marker(
  PickVisualGeometry & out, const Vec3 & position, float size_m, const Rgb & color)
{
  if (!finite(position) || !std::isfinite(size_m) || !(size_m > 0.0f)) {
    return;
  }
  const float half = size_m * 0.5f;
  add(
    out, Vec3{position.x - half, position.y, position.z},
    Vec3{position.x + half, position.y, position.z}, color);
  add(
    out, Vec3{position.x, position.y - half, position.z},
    Vec3{position.x, position.y + half, position.z}, color);
  add(
    out, Vec3{position.x, position.y, position.z - half},
    Vec3{position.x, position.y, position.z + half}, color);
}

void append_pose_marker(
  PickVisualGeometry & out, const Vec3 & position, float yaw_rad, const PickVisualStyle & style,
  const Rgb & color)
{
  if (!finite(position) || !std::isfinite(yaw_rad)) {
    return;
  }
  // The position still has to read when the heading is barely established, so
  // the cross is drawn whether or not the arrow is.
  append_point_marker(out, position, style.marker_size_m, color);
  if (!std::isfinite(style.arrow_length_m) || !(style.arrow_length_m > 0.0f)) {
    return;
  }
  const float c = std::cos(yaw_rad);
  const float s = std::sin(yaw_rad);
  const Vec3 tip{
    position.x + c * style.arrow_length_m, position.y + s * style.arrow_length_m, position.z};
  add(out, position, tip, color);
  if (!std::isfinite(style.arrow_head_m) || !(style.arrow_head_m > 0.0f)) {
    return;
  }
  // Two barbs at +/-150 degrees from the shaft direction: an arrowhead that
  // reads from either side without needing a filled triangle.
  const float barb = 5.0f * kPi / 6.0f;
  for (const float sign : {1.0f, -1.0f}) {
    const float angle = yaw_rad + sign * barb;
    add(
      out, tip,
      Vec3{
        tip.x + std::cos(angle) * style.arrow_head_m,
        tip.y + std::sin(angle) * style.arrow_head_m, tip.z},
      color);
  }
}

void append_polygon(
  PickVisualGeometry & out, const std::vector<Vec3> & vertices, bool closed, const Rgb & color)
{
  if (vertices.size() < 2u) {
    return;
  }
  for (const auto & vertex : vertices) {
    if (!finite(vertex)) {
      return;  // one bad vertex makes the whole outline a lie
    }
  }
  for (std::size_t i = 0; i + 1 < vertices.size(); ++i) {
    add(out, vertices[i], vertices[i + 1], color);
  }
  if (closed && vertices.size() >= 3u) {
    add(out, vertices.back(), vertices.front(), color);
  }
}

void append_circle(
  PickVisualGeometry & out, const Vec3 & center, float radius_m, std::uint32_t segments,
  const Rgb & color)
{
  if (!finite(center) || !std::isfinite(radius_m) || !(radius_m > 0.0f) || segments < 3u) {
    return;
  }
  Vec3 previous{center.x + radius_m, center.y, center.z};
  const Vec3 first = previous;
  for (std::uint32_t i = 1; i <= segments; ++i) {
    const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
    const Vec3 next =
      i == segments
      ? first  // close exactly, so no sliver survives floating-point rounding
      : Vec3{center.x + std::cos(angle) * radius_m, center.y + std::sin(angle) * radius_m,
      center.z};
    add(out, previous, next, color);
    previous = next;
  }
}

}  // namespace qar_ogre_streaming
