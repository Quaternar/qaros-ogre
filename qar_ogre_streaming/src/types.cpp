// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include "qar_ogre_streaming/types.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace qar_ogre_streaming
{

const char * to_string(Status status)
{
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::NotImplemented:
      return "NotImplemented";
    case Status::InvalidArgument:
      return "InvalidArgument";
    case Status::InvalidState:
      return "InvalidState";
    case Status::AlreadyExists:
      return "AlreadyExists";
    case Status::NotFound:
      return "NotFound";
    case Status::RenderThreadRequired:
      return "RenderThreadRequired";
  }
  return "Unknown";
}

bool PeerIdLess::operator()(const QarPeerId & a, const QarPeerId & b) const noexcept
{
  return std::memcmp(a.data, b.data, QAR_MAX_ID_LENGTH) < 0;
}

bool peer_id_equal(const QarPeerId & a, const QarPeerId & b) noexcept
{
  return std::memcmp(a.data, b.data, QAR_MAX_ID_LENGTH) == 0;
}

std::string peer_id_to_hex(const QarPeerId & id)
{
  static const char * kHex = "0123456789abcdef";
  std::string out;
  out.reserve(QAR_MAX_ID_LENGTH * 2);
  for (unsigned char byte : id.data) {
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

Result StereoStreamConfig::validate() const
{
  if (width == 0 || height == 0) {
    return Result::error(
      Status::InvalidArgument, "StereoStreamConfig: width and height must be non-zero");
  }
  if (max_fps == 0) {
    return Result::error(
      Status::InvalidArgument, "StereoStreamConfig: max_fps must be non-zero");
  }
  // Only the separated-textures layout is supported.
  if (layout != QAR_FRAME_LAYOUT_SEPARATED_TEXTURES) {
    return Result::error(
      Status::InvalidArgument,
      "StereoStreamConfig: only QAR_FRAME_LAYOUT_SEPARATED_TEXTURES is supported");
  }
  return Result::success();
}

}  // namespace qar_ogre_streaming
