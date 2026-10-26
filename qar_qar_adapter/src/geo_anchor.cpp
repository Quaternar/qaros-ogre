// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/geo_anchor.hpp"

#include <cmath>

namespace qar_qar_adapter
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
/// Metres per degree of latitude on the WGS84 ellipsoid, near enough for a
/// movement threshold (it varies by ~1% between equator and pole).
constexpr double kMetresPerDegreeLat = 111320.0;
}  // namespace

bool geodetic_to_ecef_enu(
  double latitude_deg, double longitude_deg, double altitude_m,
  double out_origin[3], double out_east[3], double out_north[3], double out_up[3])
{
  // NaN first: it fails every comparison below, so a bare range check would let
  // a no-lock NavSatFix through and anchor the volume at a NaN position.
  if (std::isnan(latitude_deg) || std::isnan(longitude_deg) ||
    std::isnan(altitude_m) || std::isinf(latitude_deg) ||
    std::isinf(longitude_deg) || std::isinf(altitude_m))
  {
    return false;
  }
  if (latitude_deg < -90.0 || latitude_deg > 90.0 ||
    longitude_deg < -180.0 || longitude_deg > 180.0)
  {
    return false;
  }

  const double e2 = kWgs84Flattening * (2.0 - kWgs84Flattening);
  const double lat = latitude_deg * kDeg2Rad;
  const double lon = longitude_deg * kDeg2Rad;
  const double sin_lat = std::sin(lat), cos_lat = std::cos(lat);
  const double sin_lon = std::sin(lon), cos_lon = std::cos(lon);

  // Radius of curvature in the prime vertical.
  const double n = kWgs84SemiMajorAxisM / std::sqrt(1.0 - e2 * sin_lat * sin_lat);

  out_origin[0] = (n + altitude_m) * cos_lat * cos_lon;
  out_origin[1] = (n + altitude_m) * cos_lat * sin_lon;
  out_origin[2] = (n * (1.0 - e2) + altitude_m) * sin_lat;

  out_east[0] = -sin_lon;
  out_east[1] = cos_lon;
  out_east[2] = 0.0;

  out_north[0] = -sin_lat * cos_lon;
  out_north[1] = -sin_lat * sin_lon;
  out_north[2] = cos_lat;

  out_up[0] = cos_lat * cos_lon;
  out_up[1] = cos_lat * sin_lon;
  out_up[2] = sin_lat;
  return true;
}

bool geo_fix_moved(
  double lat_a_deg, double lon_a_deg, double alt_a_m,
  double lat_b_deg, double lon_b_deg, double alt_b_m,
  double threshold_m)
{
  if (std::isnan(lat_a_deg) || std::isnan(lon_a_deg) || std::isnan(alt_a_m) ||
    std::isnan(lat_b_deg) || std::isnan(lon_b_deg) || std::isnan(alt_b_m))
  {
    return true;  // unusable input is not evidence of "hasn't moved"
  }
  const double d_north = (lat_b_deg - lat_a_deg) * kMetresPerDegreeLat;
  const double d_east = (lon_b_deg - lon_a_deg) * kMetresPerDegreeLat;
  const double d_up = alt_b_m - alt_a_m;
  const double dist =
    std::sqrt(d_north * d_north + d_east * d_east + d_up * d_up);
  return dist >= threshold_m;
}

}  // namespace qar_qar_adapter
