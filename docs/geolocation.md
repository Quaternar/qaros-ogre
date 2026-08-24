# Geolocation: anchoring an app volume to the real world

A streamed scene has local coordinates — metres from some arbitrary origin. QarOS
can place that scene at a real position on Earth, so that an AR client standing in
the right field sees the data where it actually is. This page covers the
**adapter side**: the QarOS API involved and the geodetic conversion behind it.

For how an RViz2 panel feeds a GPS fix into this, see the `geolocation.md` in the
`qar_rviz_plugin` repository.

## What QarOS wants

Georeferencing is a property **of an app volume**, not of the session and not of a
render stream:

```c
typedef struct QarGeoAnchorFrame {
    QarWorldReferenceSystem world_ref_system;  // QAR_WORLD_REF_SYSTEM_ECEF_WGS84_METERS
    QarGeoVec3d origin_world;                  // ECEF metres (double precision)
    QarGeoVec3d axis_x_world;                  // local +X, expressed in ECEF
    QarGeoVec3d axis_y_world;                  // local +Y
    QarGeoVec3d axis_z_world;                  // local +Z
    QarHandedness handedness;
} QarGeoAnchorFrame;

qar_app_volumes_change_app_world_anchor(session, volume_id, &frame);
qar_app_volumes_clear_app_world_anchor(session, volume_id);
qar_app_volume_get_app_world_anchor(handle, &out_anchor);
```

`QarAppVolumeInit` also carries `has_app_world_anchor` / `app_world_anchor` for
setting an anchor at creation time.

**Consequence worth knowing before debugging:** because the anchor belongs to a
volume, running with the app volume disabled makes geolocation *impossible*, not
merely unhelpful. `set_geo_anchor_ecef()` returns exactly that message rather than
failing vaguely.

ECEF (Earth-Centred, Earth-Fixed) is metres from the centre of the Earth: the X
axis passes through the equator at the prime meridian, Z through the north pole.
Coordinates are ~6.4 × 10⁶ m, which is why the SDK uses `double` here — and why
storing them in `float` is not an option. Float32 carries about 7 significant
decimal digits, so across a 6.4 × 10⁶ m radius it quantises to roughly half a
metre.

## The API on this side

`QarCApiSenderFactory` exposes three entry points:

```cpp
std::string set_geo_anchor_wgs84(double lat_deg, double lon_deg, double alt_m);
std::string set_geo_anchor_ecef(const double origin[3], const double x[3],
                                const double y[3], const double z[3]);
std::string clear_geo_anchor();
bool        has_geo_anchor() const;
```

Each returns an empty string on success and a human-readable message on failure.
`set_geo_anchor_wgs84` is the convenience path for a GNSS fix; `set_geo_anchor_ecef`
is the entry point for a caller that already has an ECEF frame — for example a
double-precision `tf2` lookup of the ROS `earth` frame, which
[REP-105][rep105] defines as the ECEF origin, so such a transform *is* a
`QarGeoAnchorFrame` already with no geodetic conversion needed.

All three must be called from the same thread as the rest of the QarOS session:
the SDK is [designed for single-threaded use per runtime][conv], with callbacks as
the only cross-thread surface.

## The conversion

`qar_qar_adapter/geo_anchor.hpp` holds the arithmetic as free functions over plain
doubles, deliberately separate from the session-bound methods so that it can be
unit tested without a live QarOS runtime:

```cpp
bool geodetic_to_ecef_enu(double lat_deg, double lon_deg, double alt_m,
                          double out_origin[3], double out_east[3],
                          double out_north[3], double out_up[3]);

bool geo_fix_moved(double lat_a, double lon_a, double alt_a,
                   double lat_b, double lon_b, double alt_b,
                   double threshold_m);
```

The origin uses the standard WGS84 formulation, and the axes are the local
East/North/Up basis at that point — which maps directly onto the ROS convention of
+X East, +Y North, +Z Up ([REP-103][rep103]). ENU is right-handed, so `handedness`
is `QAR_HANDEDNESS_RIGHT`.

Two details that are easy to get wrong:

- **Altitude is height above the WGS84 ellipsoid, not above mean sea level.**
  `sensor_msgs/NavSatFix` specifies ellipsoidal height. The two differ by tens of
  metres in most of the world, so mixing them up puts the scene underground or in
  the air.
- **A GNSS receiver with no lock reports `NaN`.** NaN compares false against every
  bound, so a plain range check admits it and the anchor ends up at a NaN
  position. `geodetic_to_ecef_enu` rejects NaN and infinity explicitly, before any
  range check, and leaves the output arrays untouched when it rejects.

`geo_fix_moved` exists to throttle re-anchoring: a 10 Hz receiver would otherwise
drive one SDK call per message for what is usually sub-metre jitter. Its distance
approximation uses latitude's metres-per-degree for both axes; longitude degrees
shrink by cos(latitude), so it over-estimates east-west movement and therefore errs
towards re-anchoring rather than towards missing real movement. Any NaN input
counts as "moved", so bad data is never mistaken for standing still.

## Testing

`qar_qar_adapter/test/geo_anchor_tests.cpp` covers the conversion against
closed-form reference points (the ellipsoid axes at the poles and on the equator),
checks the ENU basis is orthonormal and right-handed, checks that stepping
north/east moves the origin along the corresponding axis, and covers
NaN/infinity/out-of-range rejection plus the re-anchor threshold. Twenty cases,
run by `colcon test` along with everything else.

This is deliberately the *only* automatically testable part of the feature: the
rest needs a live QarOS session and a GPS publisher. See [`testing.md`](testing.md).

## Status

**Built and unit tested; never exercised against live GPS data.** No rosbag
supplied so far contains a GPS topic. The first real fix is the actual test.

[rep105]: https://github.com/ros-infrastructure/rep/blob/master/rep-0105.rst
[rep103]: https://www.ros.org/reps/rep-0103.html
[conv]: https://docs.quaternar.com/developer-guide/api-conventions
