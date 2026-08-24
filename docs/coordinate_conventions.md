# Coordinate conventions (Qar ↔ Ogre)

This document is the authoritative reference for how `qar_ogre_streaming`
converts a Qar view pose and field of view into an Ogre camera transform and
projection matrix. It backs the implementation in
`qar_ogre_streaming/src/qar_pose_conversion.cpp` and the unit tests in
`qar_ogre_streaming/test/{pose_conversion,projection}_tests.cpp`.

> **Status: contractor-confirmed.** The Qar side uses the OpenXR convention.
> This was derived from the vendored `third_party/qar/include/qar_streaming.h`
> and has since been **confirmed correct by the Qar contractor** (plan §1A,
> answer C4). Treat it as fact, not assumption.

## 1. Axes and handedness

Both frames are **right-handed** and, in camera/view-local space, identical:

| Axis | Direction |
|------|-----------|
| +X   | right     |
| +Y   | up        |
| −Z   | forward (the direction the camera looks) |

- **Qar** — OpenXR convention. Evidence in `qar_streaming.h`:
  `QarHandedness` defaults to `QAR_HANDEDNESS_RIGHT`; the gesture docs put
  Y = up/down and Z = forward/back and map yaw/pitch/roll onto the local
  y/x/z axes; and `QarFov`'s default `{-45°, +45°, +45°, -45°}` matches
  `XrFovf` exactly (left/down negative, right/up positive).
- **Ogre** — an `Ogre::Camera` in its own local space also looks down **−Z**
  with **+X right, +Y up**, right-handed. Same frame.

Because the two frames coincide, **the pose mapping is the identity mapping**:
no axis swap, no sign flip, no handedness change.

## 2. Quaternion component order and storage

| Type | Storage order | Identity |
|------|---------------|----------|
| `QarQuaternion` | **x, y, z, w** (xyzw) | `{0, 0, 0, 1}` |
| `qar_ogre_streaming::Quat` | **w, x, y, z** (wxyz) | `{1, 0, 0, 0}` |
| `Ogre::Quaternion` | **w, x, y, z** (wxyz) | `{1, 0, 0, 0}` |

The conversion is a **pure storage reindex plus normalization**. It represents
the *same rotation* — there is no conjugation and no axis remap, precisely
because the two coordinate frames are identical (§1):

```
Quat out;
out.w = in.w;  out.x = in.x;  out.y = in.y;  out.z = in.z;   // xyzw -> wxyz
// then divide all four components by sqrt(w²+x²+y²+z²)
```

`qar_ogre_streaming::Quat` deliberately uses Ogre's `w,x,y,z` field order so it
drops straight into `Ogre::Quaternion(q.w, q.x, q.y, q.z)`.

### Normalization and degenerate input
- Non-unit (but non-degenerate) quaternions are normalized to unit length.
- A quaternion whose length is `< kMinQuaternionNorm` (1e-6), including the zero
  quaternion and any NaN component, is **degenerate**:
  - `qar_orientation_to_ogre()` (convenience form) falls back to identity.
  - `qar_pose_to_ogre()` (checked form) returns
    `Status::InvalidArgument` and leaves its outputs at identity.

### Rotation sanity checks (right-handed, +angle by right-hand rule)
| Rotation | Maps |
|----------|------|
| yaw +90° about +Y  | +X → −Z |
| pitch +90° about +X | +Y → +Z |
| roll +90° about +Z  | +X → +Y |

## 3. Position

`QarVector3{x, y, z}` copies component-for-component to
`qar_ogre_streaming::Vec3{x, y, z}`. Identity mapping; cannot fail.

## 4. Matrix storage

`qar_ogre_streaming::Mat4` holds 16 floats in **row-major** order,
`m[row * 4 + col]`. This is byte-for-byte compatible with `Ogre::Matrix4`
(also row-major, `m[i][j]` = row i, col j), so the 16-float row-major
`Ogre::Matrix4` constructor reproduces it directly, and it can be handed to
`Ogre::Camera::setCustomProjectionMatrix(true, that_matrix)`.

## 5. Projection matrix

`make_projection_from_qar_fov(fov, near, far, out)` builds the classic OpenGL
**off-axis (glFrustum)** perspective matrix. It is identical to
`Ogre::Frustum::setFrustumExtents(left, right, top, bottom)` followed by
`Ogre::Frustum::getProjectionMatrix()`:
right-handed, view space looking down −Z, clip-space depth in **[−1, +1]**.

Frustum extents are taken on the near plane from the signed half-angles:

```
left   = near * tan(fov.angle_left)    (typically < 0)
right  = near * tan(fov.angle_right)   (typically > 0)
bottom = near * tan(fov.angle_down)    (typically < 0)
top    = near * tan(fov.angle_up)      (typically > 0)
```

Row-major entries (n = near, f = far; all other entries 0, **including m[15]**):

```
m[0]  = 2n / (right - left)
m[2]  = (right + left) / (right - left)     // horizontal off-axis shear
m[5]  = 2n / (top - bottom)
m[6]  = (top + bottom) / (top - bottom)     // vertical off-axis shear
m[10] = -(f + n) / (f - n)
m[11] = -2 f n / (f - n)
m[14] = -1                                   // perspective divide by -z
```

Depth behavior (verified in tests): a point at view-space `z = −near` maps to
NDC `z = −1`; a point at `z = −far` maps to NDC `z = +1` (after the perspective
divide). The homogeneous `w` output equals the positive view-space depth `−z`.

### Rejected (degenerate) input → `Status::InvalidArgument`
- `near ≤ 0`
- `far ≤ near`
- `angle_right ≤ angle_left`
- `angle_up ≤ angle_down`
- any `|angle| ≥ π/2` (tan undefined / non-monotonic there)

## 6. Worked asymmetric-FOV example

Inputs (radians): `angle_left = −0.6`, `angle_right = 0.4`,
`angle_up = 0.5`, `angle_down = −0.3`; `near = 0.1`, `far = 100`.

Extents on the near plane (`near * tan(angle)`):

```
left   = 0.1 * tan(-0.6) = 0.1 * -0.68413681 = -0.068413681
right  = 0.1 * tan( 0.4) = 0.1 *  0.42279322 =  0.042279322
bottom = 0.1 * tan(-0.3) = 0.1 * -0.30933625 = -0.030933625
top    = 0.1 * tan( 0.5) = 0.1 *  0.54630249 =  0.054630249

right - left  = 0.110693003
top   - bottom = 0.085563874
```

Resulting matrix entries:

```
m[0]  = 2n/(r-l)     = 0.2 / 0.110693003        =  1.8068013
m[2]  = (r+l)/(r-l)  = -0.026134359 / 0.110693003 = -0.2361072
m[5]  = 2n/(t-b)     = 0.2 / 0.085563874        =  2.3374010
m[6]  = (t+b)/(t-b)  =  0.023696624 / 0.085563874 =  0.2769419
m[10] = -(f+n)/(f-n) = -100.1 / 99.9            = -1.0020020
m[11] = -2fn/(f-n)   = -20 / 99.9               = -0.2002002
m[14] = -1
```

Shear signs are a useful mental check: the right edge is wider than the left
(`|right| < |left|` here → `right + left < 0` → `m[2] < 0`), and the up edge is
wider than the down edge (`top + bottom > 0` → `m[6] > 0`).

This exact case is asserted in
`qar_ogre_streaming/test/projection_tests.cpp`
(`Projection.AsymmetricFovHandComputedEntries`) and cross-checked against an
independent replica of Ogre's own `Frustum::updateFrustumImpl()` perspective
formula (`Projection.MatchesOgreFrustumAsymmetric`).
