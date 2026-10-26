// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__FRAME_STALL_PROBE_HPP_
#define QAR_QAR_ADAPTER__FRAME_STALL_PROBE_HPP_

/// @file frame_stall_probe.hpp
/// @brief The pure parts of the log lines of the 3D stream: the one line for the first frame
/// after a stall (finding the stall, averaging the colour that was read back, the words of the
/// line) and the lines that name the threads and the textures of a sender when it starts.
///
/// A viewer that stops asking for frames (it is restarted, or paused) and then asks again leaves
/// a gap in the frames the stream shows. Whether the scene looks dark or empty when it comes back
/// is a question about the first frame after the gap and about where the viewer's head is then,
/// so the stream reads the colour of that one frame back and says so once. Nothing here touches
/// the GPU or the runtime, which keeps it testable on plain numbers.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace qar_qar_adapter
{

/// A gap between two frames of at least this many seconds is a stall.
constexpr double kStallThresholdSeconds = 2.0;

/// A point in the space the frame information is given in.
struct StallPoint
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

/// What is known about a stall when the first frame after it is ready.
struct StallReport
{
  /// Seconds between the last frame shown before the stall and the frame that ends it.
  double stall_seconds = 0.0;
  /// The midpoint of the two eye positions of the frame that ends it.
  StallPoint head;
};

/// @brief Notices that a frame follows a gap of at least @c threshold seconds in the frames shown.
///
/// Call @ref frame_ready when a frame is ready to be rendered, @ref take before it is shown to
/// ask whether it is the first one after a stall, and @ref frame_shown once it is shown. The
/// first frame of a stream never follows a stall, because there is no earlier frame to be apart
/// from.
class FrameStallDetector
{
public:
  explicit FrameStallDetector(double threshold_seconds = kStallThresholdSeconds)
  : threshold_seconds_(threshold_seconds)
  {
  }

  /// @brief A frame is ready to be rendered at @p now_seconds (any monotonic clock, in seconds),
  /// with its eyes at @p left_eye and @p right_eye. If it follows a stall, that is remembered
  /// for @ref take. Called again for the same frame, or for a later one before any frame was
  /// shown, it replaces what was remembered: the stall is measured to the frame that is shown.
  void frame_ready(double now_seconds, const StallPoint & left_eye, const StallPoint & right_eye)
  {
    if (!have_shown_) {
      return;
    }
    const double gap = now_seconds - last_shown_seconds_;
    if (gap < threshold_seconds_) {
      return;
    }
    pending_ = true;
    report_.stall_seconds = gap;
    report_.head.x = 0.5f * (left_eye.x + right_eye.x);
    report_.head.y = 0.5f * (left_eye.y + right_eye.y);
    report_.head.z = 0.5f * (left_eye.z + right_eye.z);
  }

  /// @brief True, once, when the frame that is about to be shown follows a stall; @p out says
  /// how long the stall was and where the head is.
  bool take(StallReport & out)
  {
    if (!pending_) {
      return false;
    }
    out = report_;
    pending_ = false;
    return true;
  }

  /// @brief A frame was shown at @p now_seconds: the gap is measured from here on.
  void frame_shown(double now_seconds)
  {
    have_shown_ = true;
    last_shown_seconds_ = now_seconds;
    pending_ = false;
  }

private:
  double threshold_seconds_;
  bool have_shown_ = false;
  double last_shown_seconds_ = 0.0;
  bool pending_ = false;
  StallReport report_;
};

/// @brief The mean colour of an R8G8B8A8 image, each channel 0 to 255, over every @p step-th
/// pixel in x and in y (rows are tightly packed, @p width * 4 bytes apart). Alpha is ignored.
/// @return false, with @p rgb zero, for no data, a zero size or a zero step.
inline bool
mean_rgba8(
  const std::uint8_t * rgba, std::uint32_t width, std::uint32_t height, std::uint32_t step,
  float (& rgb)[3])
{
  rgb[0] = rgb[1] = rgb[2] = 0.0f;
  if (rgba == nullptr || width == 0 || height == 0 || step == 0) {
    return false;
  }
  double sum[3] = {0.0, 0.0, 0.0};
  std::uint64_t count = 0;
  for (std::uint32_t y = 0; y < height; y += step) {
    const std::uint8_t * row = rgba + static_cast<std::size_t>(y) * width * 4u;
    for (std::uint32_t x = 0; x < width; x += step) {
      const std::uint8_t * pixel = row + static_cast<std::size_t>(x) * 4u;
      sum[0] += pixel[0];
      sum[1] += pixel[1];
      sum[2] += pixel[2];
      ++count;
    }
  }
  for (int i = 0; i < 3; ++i) {
    rgb[i] = static_cast<float>(sum[i] / static_cast<double>(count));
  }
  return true;
}

/// @brief The line for the first frame after a stall, without a time stamp:
/// `LIVE: first 3D frame after a <s> s stall: mean colour (r, g, b), head app (x, y, z)`. The
/// colour is 0 to 255 per channel; when it could not be read back (@p have_colour false) it
/// reads `mean colour (unavailable)`. The head is in the space of the frame information, which
/// is the app space of a stream bound to an app volume.
inline std::string
format_stall_line(const StallReport & report, bool have_colour, const float (& rgb)[3])
{
  char colour[96] = {0};
  if (have_colour) {
    std::snprintf(
      colour, sizeof(colour) - 1, "(%.1f, %.1f, %.1f)", static_cast<double>(rgb[0]),
      static_cast<double>(rgb[1]), static_cast<double>(rgb[2]));
  } else {
    std::snprintf(colour, sizeof(colour) - 1, "(unavailable)");
  }
  char line[256] = {0};
  std::snprintf(
    line, sizeof(line) - 1,
    "LIVE: first 3D frame after a %.1f s stall: mean colour %s, head app (%.2f, %.2f, %.2f)",
    report.stall_seconds, colour, static_cast<double>(report.head.x),
    static_cast<double>(report.head.y), static_cast<double>(report.head.z));
  return std::string(line);
}

/// The names (GL texture ids) of the four textures of a stereo frame, in the order of the frame:
/// the colour of the left and right eye, then their depth, and the size of the first one. An id of
/// 0 is a texture the frame does not have.
struct SenderTextureIds
{
  unsigned int colour[2] = {0u, 0u};
  unsigned int depth[2] = {0u, 0u};
  unsigned int width = 0u;
  unsigned int height = 0u;
};

/// @brief A handle as `0x<hex>`, the same text on every platform (a printf pointer conversion
/// differs between compilers).
inline std::string
format_handle(const void * handle)
{
  char text[32] = {0};
  std::snprintf(
    text, sizeof(text) - 1, "0x%llx",
    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle)));
  return std::string(text);
}

/// @brief The line before a sender is asked for, without a time stamp:
/// `peer <peer> create_async tid=<n>` (the thread that asks).
inline std::string
format_create_async_line(const std::string & peer, unsigned long long thread)
{
  return "peer " + peer + " create_async tid=" + std::to_string(thread);
}

/// @brief The line when the sender is handed over, without a time stamp:
/// `peer <peer> created (callback tid=<n>) after <s> s` (the thread that ran the runtime's
/// callback, and the seconds since the request).
inline std::string
format_created_line(const std::string & peer, unsigned long long thread, double seconds)
{
  char text[96] = {0};
  std::snprintf(
    text, sizeof(text) - 1, " created (callback tid=%llu) after %.2f s", thread, seconds);
  return "peer " + peer + text;
}

/// @brief The line for the first frame a sender shows, without a time stamp:
/// `peer <peer> first show_frame tid=<n> tex=c<l>/<r> d<l>/<r> <w>x<h>`.
inline std::string
format_first_show_line(
  const std::string & peer, unsigned long long thread, const SenderTextureIds & ids)
{
  char text[160] = {0};
  std::snprintf(
    text, sizeof(text) - 1, " first show_frame tid=%llu tex=c%u/%u d%u/%u %ux%u", thread,
    ids.colour[0], ids.colour[1], ids.depth[0], ids.depth[1], ids.width, ids.height);
  return "peer " + peer + text;
}

/// @brief The line for the first time the runtime's graphics thread makes a sender's context
/// current, without a time stamp: `peer <peer> SDK bound ctx <hglrc> hdc=<hdc> tid=<n>`.
inline std::string
format_sdk_bound_line(
  const std::string & peer, const void * context, const void * device_context,
  unsigned long long thread)
{
  return "peer " + peer + " SDK bound ctx " + format_handle(context) + " hdc=" +
         format_handle(device_context) + " tid=" + std::to_string(thread);
}

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__FRAME_STALL_PROBE_HPP_
