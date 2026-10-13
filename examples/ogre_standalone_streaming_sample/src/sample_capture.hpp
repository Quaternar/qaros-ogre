// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_OGRE_SAMPLE__SAMPLE_CAPTURE_HPP_
#define QAR_OGRE_SAMPLE__SAMPLE_CAPTURE_HPP_

#include <cstdint>
#include <string>
#include <vector>

// Self-contained CPU-readback PNG + content/parallax helpers for the standalone
// sample. This mirrors the qar_offscreen_capture of the RViz plugin (qar_rviz_plugin), but
// lives in the sample so it links NO rviz/Qt code — the sample must be independent. The
// one stb_image_write implementation is compiled in sample_capture.cpp.
namespace qar_ogre_sample
{

/// @brief Content statistics of one captured eye.
struct CaptureStats
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t color_non_background_pixels = 0;
  std::uint64_t depth_nonzero_pixels = 0;
  float depth_min = 0.0f;
  float depth_max = 0.0f;
};

/// @brief Left/right parallax measurement (near band must shift more than far).
struct StereoParallaxStats
{
  float near_band_lo = 0.0f;
  float near_band_hi = 0.0f;
  float far_band_lo = 0.0f;
  float far_band_hi = 0.0f;
  std::uint64_t near_band_pixels = 0;
  std::uint64_t far_band_pixels = 0;
  double near_disparity_px = 0.0;
  double far_disparity_px = 0.0;
  bool nearer_shifts_more = false;
  double image_mean_abs_diff = 0.0;
};

/// @brief Write an RGBA8 buffer (row-major, byte order R,G,B,A) as a PNG.
bool write_color_png(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & rgba);

/// @brief Write an 8-bit grayscale PNG of metric depth (near = bright, far = dark,
/// background black). For human/debug inspection.
bool write_depth_visualization_png(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<float> & depth_metres);

/// @brief Write raw metric depth as little-endian float32, one per pixel.
bool write_depth_raw_f32(
  const std::string & path, std::uint32_t width, std::uint32_t height,
  const std::vector<float> & depth_metres);

CaptureStats compute_capture_stats(
  std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & rgba, const std::vector<float> & depth_metres);

StereoParallaxStats compute_stereo_parallax(
  std::uint32_t width, std::uint32_t height,
  const std::vector<std::uint8_t> & left_rgba,
  const std::vector<std::uint8_t> & right_rgba,
  const std::vector<float> & left_depth,
  const std::vector<float> & right_depth);

}  // namespace qar_ogre_sample

#endif  // QAR_OGRE_SAMPLE__SAMPLE_CAPTURE_HPP_
