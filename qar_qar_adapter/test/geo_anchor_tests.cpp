// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Tests for the geodetic -> ECEF conversion that georeferences the QarOS app
// volume (QarGeoAnchorFrame). This is the one part of the geolocation path that
// can be checked without a QarOS session, and it is also the part that fails
// SILENTLY: a wrong sign or a swapped axis produces a perfectly well-formed
// anchor on the far side of the planet, and neither RViz nor QarOS would object.

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "qar_qar_adapter/geo_anchor.hpp"

namespace
{
using qar_qar_adapter::geo_fix_moved;
using qar_qar_adapter::geodetic_to_ecef_enu;
using qar_qar_adapter::kWgs84Flattening;
using qar_qar_adapter::kWgs84SemiMajorAxisM;

/// WGS84 semi-minor axis, i.e. the polar radius. Derived, not hard-coded, so the
/// expectation tracks the constants the implementation actually uses.
double semi_minor_axis()
{
  return kWgs84SemiMajorAxisM * (1.0 - kWgs84Flattening);
}

double dot(const double a[3], const double b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double norm(const double a[3])
{
  return std::sqrt(dot(a, a));
}

struct Anchor
{
  double origin[3];
  double east[3];
  double north[3];
  double up[3];
};

Anchor anchor_at(double lat, double lon, double alt)
{
  Anchor a{};
  EXPECT_TRUE(
    geodetic_to_ecef_enu(lat, lon, alt, a.origin, a.east, a.north, a.up));
  return a;
}
}  // namespace

// --- Origin against closed-form reference points ---------------------------
// At these four points the ECEF coordinates are exactly the ellipsoid axes, so
// they pin down the conversion without trusting a second implementation.

TEST(GeoAnchor, EquatorPrimeMeridianIsSemiMajorAxisOnX)
{
  const Anchor a = anchor_at(0.0, 0.0, 0.0);
  EXPECT_NEAR(a.origin[0], kWgs84SemiMajorAxisM, 1e-6);
  EXPECT_NEAR(a.origin[1], 0.0, 1e-6);
  EXPECT_NEAR(a.origin[2], 0.0, 1e-6);
}

TEST(GeoAnchor, NorthPoleIsSemiMinorAxisOnZ)
{
  const Anchor a = anchor_at(90.0, 0.0, 0.0);
  EXPECT_NEAR(a.origin[0], 0.0, 1e-6);
  EXPECT_NEAR(a.origin[1], 0.0, 1e-6);
  EXPECT_NEAR(a.origin[2], semi_minor_axis(), 1e-6);
}

TEST(GeoAnchor, EquatorNinetyEastIsSemiMajorAxisOnY)
{
  const Anchor a = anchor_at(0.0, 90.0, 0.0);
  EXPECT_NEAR(a.origin[0], 0.0, 1e-6);
  EXPECT_NEAR(a.origin[1], kWgs84SemiMajorAxisM, 1e-6);
  EXPECT_NEAR(a.origin[2], 0.0, 1e-6);
}

TEST(GeoAnchor, SouthPoleIsNegativeZ)
{
  const Anchor a = anchor_at(-90.0, 0.0, 0.0);
  EXPECT_NEAR(a.origin[2], -semi_minor_axis(), 1e-6);
}

TEST(GeoAnchor, AltitudeRaisesAlongTheEllipsoidNormal)
{
  const Anchor ground = anchor_at(0.0, 0.0, 0.0);
  const Anchor high = anchor_at(0.0, 0.0, 1000.0);
  EXPECT_NEAR(high.origin[0] - ground.origin[0], 1000.0, 1e-6);
}

// --- The local basis --------------------------------------------------------

TEST(GeoAnchor, EnuBasisIsOrthonormal)
{
  const Anchor a = anchor_at(50.0755, 14.4378, 200.0);
  EXPECT_NEAR(norm(a.east), 1.0, 1e-12);
  EXPECT_NEAR(norm(a.north), 1.0, 1e-12);
  EXPECT_NEAR(norm(a.up), 1.0, 1e-12);
  EXPECT_NEAR(dot(a.east, a.north), 0.0, 1e-12);
  EXPECT_NEAR(dot(a.east, a.up), 0.0, 1e-12);
  EXPECT_NEAR(dot(a.north, a.up), 0.0, 1e-12);
}

TEST(GeoAnchor, EnuBasisIsRightHanded)
{
  // east x north must equal up. A left-handed basis would mirror the scene, which
  // is exactly the kind of fault that looks plausible until someone reads a sign.
  const Anchor a = anchor_at(50.0755, 14.4378, 200.0);
  const double cross[3] = {
    a.east[1] * a.north[2] - a.east[2] * a.north[1],
    a.east[2] * a.north[0] - a.east[0] * a.north[2],
    a.east[0] * a.north[1] - a.east[1] * a.north[0]};
  EXPECT_NEAR(cross[0], a.up[0], 1e-12);
  EXPECT_NEAR(cross[1], a.up[1], 1e-12);
  EXPECT_NEAR(cross[2], a.up[2], 1e-12);
}

TEST(GeoAnchor, UpPointsAwayFromTheEarthCentre)
{
  const Anchor a = anchor_at(50.0755, 14.4378, 200.0);
  // Not exactly 1: the ellipsoid normal is not radial except at pole and equator.
  EXPECT_GT(dot(a.up, a.origin) / norm(a.origin), 0.999);
}

TEST(GeoAnchor, EastIsPurelyHorizontalEverywhere)
{
  // East always lies in the equatorial plane, so its ECEF Z component is zero.
  for (const double lat : {-80.0, -30.0, 0.0, 12.5, 60.0, 89.0}) {
    const Anchor a = anchor_at(lat, 33.0, 0.0);
    EXPECT_NEAR(a.east[2], 0.0, 1e-15) << "latitude " << lat;
  }
}

TEST(GeoAnchor, NorthPointsTowardsIncreasingLatitude)
{
  // Stepping north by a small amount must move the origin along +north.
  const double lat = 20.0, lon = -75.0;
  const Anchor a = anchor_at(lat, lon, 0.0);
  const Anchor b = anchor_at(lat + 0.001, lon, 0.0);
  const double delta[3] = {
    b.origin[0] - a.origin[0], b.origin[1] - a.origin[1], b.origin[2] - a.origin[2]};
  EXPECT_GT(dot(delta, a.north), 0.0);
  EXPECT_NEAR(dot(delta, a.east), 0.0, 1e-6);
}

TEST(GeoAnchor, EastPointsTowardsIncreasingLongitude)
{
  const double lat = 20.0, lon = -75.0;
  const Anchor a = anchor_at(lat, lon, 0.0);
  const Anchor b = anchor_at(lat, lon + 0.001, 0.0);
  const double delta[3] = {
    b.origin[0] - a.origin[0], b.origin[1] - a.origin[1], b.origin[2] - a.origin[2]};
  EXPECT_GT(dot(delta, a.east), 0.0);
}

// --- Rejecting unusable fixes ----------------------------------------------

TEST(GeoAnchor, RejectsNanBecauseNavSatFixReportsItForNoLock)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  double o[3], e[3], n[3], u[3];
  EXPECT_FALSE(geodetic_to_ecef_enu(nan, 0.0, 0.0, o, e, n, u));
  EXPECT_FALSE(geodetic_to_ecef_enu(0.0, nan, 0.0, o, e, n, u));
  EXPECT_FALSE(geodetic_to_ecef_enu(0.0, 0.0, nan, o, e, n, u));
}

TEST(GeoAnchor, RejectsInfinity)
{
  const double inf = std::numeric_limits<double>::infinity();
  double o[3], e[3], n[3], u[3];
  EXPECT_FALSE(geodetic_to_ecef_enu(inf, 0.0, 0.0, o, e, n, u));
  EXPECT_FALSE(geodetic_to_ecef_enu(0.0, 0.0, -inf, o, e, n, u));
}

TEST(GeoAnchor, RejectsOutOfRangeAnglesButAcceptsTheBounds)
{
  double o[3], e[3], n[3], u[3];
  EXPECT_FALSE(geodetic_to_ecef_enu(90.001, 0.0, 0.0, o, e, n, u));
  EXPECT_FALSE(geodetic_to_ecef_enu(-90.001, 0.0, 0.0, o, e, n, u));
  EXPECT_FALSE(geodetic_to_ecef_enu(0.0, 180.001, 0.0, o, e, n, u));
  EXPECT_TRUE(geodetic_to_ecef_enu(90.0, 180.0, 0.0, o, e, n, u));
  EXPECT_TRUE(geodetic_to_ecef_enu(-90.0, -180.0, 0.0, o, e, n, u));
}

TEST(GeoAnchor, LeavesOutputsUntouchedWhenRejecting)
{
  double o[3] = {1.0, 2.0, 3.0};
  double e[3] = {4.0, 5.0, 6.0};
  double n[3] = {7.0, 8.0, 9.0};
  double u[3] = {10.0, 11.0, 12.0};
  EXPECT_FALSE(geodetic_to_ecef_enu(
      std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, o, e, n, u));
  EXPECT_DOUBLE_EQ(o[0], 1.0);
  EXPECT_DOUBLE_EQ(u[2], 12.0);
}

// --- Re-anchor throttle -----------------------------------------------------

TEST(GeoFixMoved, IdenticalFixDoesNotMove)
{
  EXPECT_FALSE(geo_fix_moved(50.0, 14.0, 200.0, 50.0, 14.0, 200.0, 2.0));
}

TEST(GeoFixMoved, SubThresholdJitterIsIgnored)
{
  // ~0.5 m north: well inside a 2 m threshold.
  EXPECT_FALSE(geo_fix_moved(50.0, 14.0, 200.0, 50.0000045, 14.0, 200.0, 2.0));
}

TEST(GeoFixMoved, RealMovementCrossesTheThreshold)
{
  // 0.001 deg latitude is ~111 m.
  EXPECT_TRUE(geo_fix_moved(50.0, 14.0, 200.0, 50.001, 14.0, 200.0, 2.0));
}

TEST(GeoFixMoved, AltitudeAloneCanTripIt)
{
  EXPECT_TRUE(geo_fix_moved(50.0, 14.0, 200.0, 50.0, 14.0, 210.0, 2.0));
  EXPECT_FALSE(geo_fix_moved(50.0, 14.0, 200.0, 50.0, 14.0, 200.5, 2.0));
}

TEST(GeoFixMoved, UnusableInputCountsAsMovedRatherThanStationary)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(geo_fix_moved(nan, 14.0, 200.0, 50.0, 14.0, 200.0, 2.0));
  EXPECT_TRUE(geo_fix_moved(50.0, 14.0, 200.0, 50.0, nan, 200.0, 2.0));
}
