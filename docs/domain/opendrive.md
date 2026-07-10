# OpenDRIVE conventions

*The ASAM OpenDRIVE semantics RoadMaker implements and the conventions the
whole codebase follows. The clothoid math and meshing rules are in
[geometry](geometry.md); how to obtain and cite the spec texts is in
[references](references.md).*

## Coordinate systems

- **Inertial frame**: right-handed, Z-up, meters. Heading is measured CCW
  from +X, in radians. This is also the kernel-wide frame — see the
  [single-conversion rule](../architecture/overview.md#4-one-coordinate-frame-one-conversion-point).
- **Track coordinates (s, t)**: `s` is arc length along the road *reference
  line*, starting at 0 at the road start; `t` is the lateral offset,
  **positive to the LEFT** of the travel direction. Height `h` follows the
  elevation profile.

## Lane ids

- Lane **0** is the center lane on the reference line — it has **no width**.
- **Positive** ids grow to the **left** of the travel direction, **negative**
  ids to the **right**, ordered outward from the center.
- For right-hand traffic, driving lanes are therefore typically the
  *negative* lanes.

In the kernel: `Lane::odr_id` carries the signed id, and lane containers are
kept sorted leftmost-first (descending id).

## Reference line (planView)

A road's plan view is a contiguous sequence of `<geometry>` records, each
with a start pose `s, x, y, hdg` and a `length`, containing one primitive:

| Primitive | Shape |
|---|---|
| `line` | straight segment |
| `arc` | constant curvature (positive = left turn) |
| `spiral` | clothoid — curvature linear in s |
| `paramPoly3` | parametric cubic in a local (u, v) frame |

All four evaluate through one interface —
`ReferenceLine::evaluate(s) → {x, y, hdg, curvature}` with s clamped to
`[0, length]` — so downstream consumers (lanes, meshing, editor) never
branch on the primitive type. Clothoid evaluation and continuity rules:
[geometry](geometry.md).

## Lanes and width

- A `<laneSection>` starts at station `s0`; sections are sorted ascending
  and must have positive length.
- Lane width is a piecewise cubic: `w(ds) = a + b·ds + c·ds² + d·ds³` with
  `ds = s − sOffset`, where `sOffset` is **local to the lane section**. A
  `<width>` entry at `sOffset="0"` is required so width is defined over the
  whole section.
- `<laneOffset>` (road-level, same cubic form, global s) shifts lane 0
  laterally off the reference line.
- The t coordinate of lane boundary *k* is:
  `t_k(s) = laneOffset(s) ± Σ widths of lanes between 0 and k` — summed
  outward and signed by side (+ left, − right).
- The `<border>` alternative to `<width>` is **not supported yet**: the
  parser emits a warn-once diagnostic and ignores the element — never a
  silent drop.

## Elevation and superelevation

- `<elevationProfile>` gives z(s) as piecewise cubics in global road s.
- `<superelevation>` gives a roll angle (radians) in s, applied about the
  reference-line tangent.
- Apply order is fixed: **planView position → elevation z → superelevation
  roll**. Lane cross-sections are laid out in the rolled frame.

## Junctions

- A junction is a set of *connecting roads* plus `<connection>` records:
  each maps an incoming road onto a connecting road with per-lane
  `<laneLink>` pairs (`from` incoming lane id → `to` connecting lane id).
- A connecting road's `Road::junction` points back at its junction.
- Current scope: junctions are parsed, represented, and rendered via their
  connecting-road meshes plus a plan-view junction floor
  ([meshing](geometry.md#junction-floors)); 3D surface blending is designed
  in [junction blending](../design/m2/03_junction_blending.md).
- OpenDRIVE 1.8+ adds *virtual* and *direct* junction types; RoadMaker
  currently models the common (default) junctions.

## Reader and writer stance

- The **reader** accepts OpenDRIVE 1.6/1.7 and records the header revision.
  It never silently drops input: every skipped, coerced, or defaulted
  element becomes a structured `Diagnostic` with a location, and — whenever
  a normative checker rule exists — a rule UID such as
  `asam.net:xodr:1.4.0:ids.id_unique_in_class`
  ([citation convention](references.md#rule-id-citations)).
- The **writer** is version-explicit (it currently emits OpenDRIVE 1.7 for
  maximum ecosystem compatibility) and **validates before writing**:
  monotonic stations, reference-line continuity within
  [named tolerances](geometry.md#continuity-and-tolerances), and lane-link
  consistency. Invalid networks are refused, not written.
- Round-trip (load → save → load) must preserve geometry within
  1e-4 m position / 1e-6 rad heading
  ([tolerances](geometry.md#continuity-and-tolerances)).

Implementation entry points: `core/include/roadmaker/xodr/` —
see the [kernel tour](../architecture/kernel.md#opendrive-io).
