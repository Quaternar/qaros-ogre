// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__GEO_ANCHOR_HPP_
#define QAR_QAR_ADAPTER__GEO_ANCHOR_HPP_

namespace qar_qar_adapter
{

/// @brief Geodetic -> ECEF conversion and the local tangent-plane basis.
///
/// Deliberately free functions over plain doubles, with no QAR types: this is the
/// part of georeferencing that is pure arithmetic, and keeping it separate from
/// the session-bound `set_geo_anchor_*` methods is what makes it testable at all.
/// A wrong sign here produces an anchor on the far side of the planet and nothing
/// in the pipeline would object.
///
/// Reference frames: WGS84 ellipsoid; ECEF is Earth-Centred Earth-Fixed metres,
/// the coordinate system `QarGeoAnchorFrame` expects, and also what REP-105 calls
/// the ROS `earth` frame. The returned basis is East/North/Up, which is the ROS
/// convention (+X East, +Y North, +Z Up, REP-103).

/// @brief WGS84 semi-major axis (equatorial radius), metres.
constexpr double kWgs84SemiMajorAxisM = 6378137.0;
/// @brief WGS84 flattening.
constexpr double kWgs84Flattening = 1.0 / 298.257223563;

/// @brief Convert a geodetic fix to an ECEF origin plus the local ENU basis.
///
/// @param latitude_deg  North-positive degrees, [-90, 90].
/// @param longitude_deg East-positive degrees, [-180, 180].
/// @param altitude_m    Height above the WGS84 **ellipsoid**, metres. This is what
///                      `sensor_msgs/NavSatFix` specifies, and it is NOT height
///                      above mean sea level; the two differ by tens of metres.
/// @param out_origin    ECEF position of the fix, metres.
/// @param out_east      Unit East axis, expressed in ECEF.
/// @param out_north     Unit North axis, expressed in ECEF.
/// @param out_up        Unit Up (ellipsoid normal) axis, expressed in ECEF.
/// @return false when the fix is NaN or out of range, leaving outputs untouched.
///         NaN matters in practice: NavSatFix reports NaN for "no lock", and NaN
///         compares false against every bound, so a plain range test admits it.
bool geodetic_to_ecef_enu(
  double latitude_deg, double longitude_deg, double altitude_m,
  double out_origin[3], double out_east[3], double out_north[3], double out_up[3]);

/// @brief Whether two fixes are far enough apart to justify re-anchoring.
///
/// A receiver emitting 10 Hz would otherwise drive one SDK call per message for
/// what is usually sub-metre jitter. Distance is approximated on a sphere using
/// latitude's metres-per-degree for BOTH axes: longitude degrees shrink by
/// cos(latitude), so this over-estimates east-west movement and therefore errs
/// towards re-anchoring rather than towards missing real movement.
///
/// Returns true if either fix is unusable, so a bad reading is not mistaken for
/// "hasn't moved".
bool geo_fix_moved(
  double lat_a_deg, double lon_a_deg, double alt_a_m,
  double lat_b_deg, double lon_b_deg, double alt_b_m,
  double threshold_m);

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__GEO_ANCHOR_HPP_
