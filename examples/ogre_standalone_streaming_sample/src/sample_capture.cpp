// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

// The ONE translation unit that compiles the stb_image_write implementation for
// the sample (debug CPU-readback PNG path). No rviz/Qt/ROS involved.
#define _CRT_SECURE_NO_WARNINGS 1
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "sample_capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qar_ogre_sample
{
namespace
{
// The colour viewport clears to (0.1,0.1,0.1) -> ~26 in 8-bit (see the library's
// OgreStereoRenderTarget); a pixel beyond this tolerance is scene content.
constexpr int kBackgroundLevel = 26;
constexpr int kBackgroundTolerance = 10;

bool band_centroid_x(
  std::uint32_t width, std::uint32_t height, const std::vector<float> & depth,
  float lo, float hi, double & out_mean_x, std::uint64_t & out_count)
{
  const std::size_t n = static_cast<std::size_t>(width) * height;
  if (depth.size() < n) {
    return false;
  }
  double sum_x = 0.0;
  std::uint64_t count = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float d = depth[static_cast<std::size_t>(y) * width + x];
      if (d >= lo && d <= hi && std::isfinite(d) && d > 0.0f) {
        sum_x += x;
        ++count;
      }
    }
  }
  out_count = count;
  if (count < 16) {
    return false;
  }
  out_mean_x = sum_x / static_cast<double>(count);
  return true;
}
}  // namespace

bool write_color_png(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & rgba)
{
  if (width == 0 || height == 0 ||
    rgba.size() < static_cast<std::size_t>(width) * height * 4u)
  {
    return false;
  }
  const int stride = static_cast<int>(width) * 4;
  return stbi_write_png(
    path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), stride) != 0;
}

bool write_depth_visualization_png(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<float> & depth_metres)
{
  const std::size_t n = static_cast<std::size_t>(width) * height;
  if (width == 0 || height == 0 || depth_metres.size() < n) {
    return false;
  }
  float dmin = 0.0f, dmax = 0.0f;
  bool any = false;
  for (std::size_t i = 0; i < n; ++i) {
    const float d = depth_metres[i];
    if (d > 0.0f && std::isfinite(d)) {
      if (!any) { dmin = dmax = d; any = true; }
      else { dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
    }
  }
  const float range = (dmax > dmin) ? (dmax - dmin) : 1.0f;
  std::vector<std::uint8_t> gray(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    const float d = depth_metres[i];
    if (d > 0.0f && std::isfinite(d)) {
      const float t = (d - dmin) / range;
      const float v = (1.0f - t) * 255.0f;  // near = bright, far = dark
      gray[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
    }
  }
  return stbi_write_png(
    path.c_str(), static_cast<int>(width), static_cast<int>(height), 1, gray.data(),
    static_cast<int>(width)) != 0;
}

bool write_depth_raw_f32(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<float> & depth_metres)
{
  const std::size_t n = static_cast<std::size_t>(width) * height;
  if (width == 0 || height == 0 || depth_metres.size() < n) {
    return false;
  }
  std::FILE * f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(depth_metres.data(), sizeof(float), n, f);
  std::fclose(f);
  return written == n;
}

CaptureStats compute_capture_stats(
  std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & rgba, const std::vector<float> & depth_metres)
{
  CaptureStats s;
  s.width = width;
  s.height = height;
  const std::size_t n = static_cast<std::size_t>(width) * height;
  if (rgba.size() >= n * 4u) {
    for (std::size_t i = 0; i < n; ++i) {
      const int r = rgba[i * 4 + 0];
      const int g = rgba[i * 4 + 1];
      const int b = rgba[i * 4 + 2];
      if (std::abs(r - kBackgroundLevel) > kBackgroundTolerance ||
        std::abs(g - kBackgroundLevel) > kBackgroundTolerance ||
        std::abs(b - kBackgroundLevel) > kBackgroundTolerance)
      {
        ++s.color_non_background_pixels;
      }
    }
  }
  if (depth_metres.size() >= n) {
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
      const float d = depth_metres[i];
      if (d > 0.0f && std::isfinite(d)) {
        ++s.depth_nonzero_pixels;
        if (!any) { s.depth_min = s.depth_max = d; any = true; }
        else { s.depth_min = std::min(s.depth_min, d); s.depth_max = std::max(s.depth_max, d); }
      }
    }
  }
  return s;
}

StereoParallaxStats compute_stereo_parallax(
  std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & left_rgba,
  const std::vector<std::uint8_t> & right_rgba,
  const std::vector<float> & left_depth,
  const std::vector<float> & right_depth)
{
  StereoParallaxStats s;
  const std::size_t n = static_cast<std::size_t>(width) * height;
  if (width == 0 || height == 0 || left_depth.size() < n || right_depth.size() < n) {
    return s;
  }
  float dmin = 0.0f, dmax = 0.0f;
  bool any = false;
  auto scan = [&](const std::vector<float> & d) {
    for (std::size_t i = 0; i < n; ++i) {
      const float v = d[i];
      if (v > 0.0f && std::isfinite(v)) {
        if (!any) { dmin = dmax = v; any = true; }
        else { dmin = std::min(dmin, v); dmax = std::max(dmax, v); }
      }
    }
  };
  scan(left_depth);
  scan(right_depth);
  if (!any || dmax <= dmin) {
    return s;
  }
  const float range = dmax - dmin;
  s.near_band_lo = dmin;
  s.near_band_hi = dmin + 0.34f * range;
  s.far_band_lo = dmin + 0.66f * range;
  s.far_band_hi = dmax;

  double lx = 0.0, rx = 0.0;
  std::uint64_t lc = 0, rc = 0;
  const bool near_l = band_centroid_x(width, height, left_depth, s.near_band_lo, s.near_band_hi, lx, lc);
  const bool near_r = band_centroid_x(width, height, right_depth, s.near_band_lo, s.near_band_hi, rx, rc);
  if (near_l && near_r) {
    s.near_disparity_px = std::abs(lx - rx);
    s.near_band_pixels = std::min(lc, rc);
  }
  const bool far_l = band_centroid_x(width, height, left_depth, s.far_band_lo, s.far_band_hi, lx, lc);
  const bool far_r = band_centroid_x(width, height, right_depth, s.far_band_lo, s.far_band_hi, rx, rc);
  if (far_l && far_r) {
    s.far_disparity_px = std::abs(lx - rx);
    s.far_band_pixels = std::min(lc, rc);
  }
  s.nearer_shifts_more =
    (s.near_band_pixels > 0 && s.far_band_pixels > 0 &&
     s.near_disparity_px > s.far_disparity_px);

  if (left_rgba.size() >= n * 4u && right_rgba.size() >= n * 4u) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      for (int c = 0; c < 3; ++c) {
        acc += std::abs(
          static_cast<int>(left_rgba[i * 4 + c]) - static_cast<int>(right_rgba[i * 4 + c]));
      }
    }
    s.image_mean_abs_diff = acc / (static_cast<double>(n) * 3.0);
  }
  return s;
}

}  // namespace qar_ogre_sample
