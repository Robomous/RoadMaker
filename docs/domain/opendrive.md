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

## Georeferencing (§8.5)

A scene's place on the earth travels in `<header>`, as pure Layer-0 data:

- **`<geoReference>`** carries a PROJ string, written as CDATA because it may
  contain characters that would break an XML attribute. It is the **only**
  CDATA node RoadMaker emits. At most one is allowed
  (`asam.net:xodr:1.9.0:header.proj.max_one_proj` — named in 1.9.0's catalog
  only, but the 0..1 multiplicity is identical in 1.8.1, so RoadMaker cites the
  1.9.0 UID at every revision); a second is reported and ignored, first wins.
- **`<offset>`** is a whole-dataset affine — `x`, `y`, `z`, `hdg`, **all four
  required** — applied to the local coordinates *before* any datum conversion.
  A partial element is reported and dropped, never completed with zeros. An
  identity offset is not written: absence and all-zero mean the same thing, and
  absence is the smaller file. `validate_network` raises
  `asam.net:xodr:1.7.0:header.offset.centered_coords` when a network sits far
  from the origin with no offset to bring it back.
- **`north`/`south`/`east`/`west`** (§6.4.1 Table 8) are **derived** from the
  network being written, never carried over from the input — a bounding box
  read from a file becomes a lie the moment anything moves.
- Everything else in `<header>` — `<license>` (§6.4.2), `<dataQuality>`,
  `<include>`, foreign `<userData>`, unknown attributes — is **preserved
  verbatim** and re-emitted after the modeled children, matching the `t_header`
  sequence.

**No PROJ dependency.** Converting between two geodetic datums needs the PROJ
library, which arrives with GIS import (p7-s2). Until then RoadMaker authors
the construction §8.5 itself recommends — "alternatively define a custom
projection using Transverse Mercator" — centred on the scene's own origin with
unit scale and no false easting or northing, so the local OpenDRIVE coordinates
*are* the projected coordinates and nothing is ever transformed. A projection
RoadMaker did not author is carried verbatim and reported as opaque:
`tmerc_origin()` answers only for that family, because guessing at a UTM zone's
origin would be wrong by its 500 km false easting.

**The generated string carries no `+no_defs`**, even though §8.5's own example
does: esmini v3.5.0 rejects it outright (*"Unsupported geo reference attr:
+no_defs"*), and the flag is a PROJ.4-era instruction not to read `proj_def.dat`
— a file removed in PROJ 6, so no PROJ this project targets does anything with
it. A default RoadMaker *generates* must not carry a token a shipping consumer
refuses. Caught by the [esmini smoke gate](../contributing/ci.md).

Workspace extents and UI framing are **Layer 2**, not here — see
[persistence](../architecture/persistence.md#scenermscenejson-v1).

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

## Road marks

- A `<roadMark>` styles the line on its lane's **outer** border (rule
  `asam.net:xodr:1.9.0:road.lane.road_mark.only_outer`); the center line
  between left and right lanes is lane 0's road mark. RoadMaker stores one
  mark list per lane, so the rule holds by construction.
- The spec's `@width` is optional and has **no normative numeric values**
  (`@weight` standard/bold is its coarse axis). RoadMaker's *conventions*:
  the [realism defaults](realism_defaults.md) §1.3 normal-line width
  (`defaults::kLineWidth`, the kernel default since #413) for a
  standard-weight line and **0.25 m** for a bold one — these are the
  editor's road-mark width presets, not spec requirements.
- Multiple `<roadMark>` records per lane (ascending `sOffset`, rule
  `asam.net:xodr:1.4.0:road.lane.road_mark.elem_asc_order`) are preserved in
  data; the M2 editor edits the first (`sOffset` 0) record only.

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
- Current scope: junctions are parsed, represented, and rendered via a
  blended 2.5D junction surface stitched to the incoming arms
  ([meshing](geometry.md#junction-surfaces)), designed in
  [junction blending](../design/m2/03_junction_blending.md).
- OpenDRIVE 1.8+ adds *virtual* and *direct* junction types; RoadMaker
  currently models the common (default) junctions.

## Objects (props: trees, poles, crosswalks)

- `<object>` elements (spec chapter 13) are parsed, represented
  (`roadmaker::Object`), written, and validated. An object is **road-relative**:
  it lives under a `<road>` and is located by `s`/`t`, `zOffset`, and `hdg` —
  OpenDRIVE has no world-placed object, so RoadMaker never invents one. The
  editor snaps a placed prop to the nearest road's `s`/`t` and refuses drops
  with no road nearby.
- Object mutation goes through the command layer (`edit::add_object` /
  `delete_object` / `move_object`), so placement participates in undo/redo with
  the byte-identical-round-trip contract; the raw `RoadNetwork::add_object`
  arena API stays available for headless authoring.
- **Bundled tree/vegetation props** carry a prop-model id in `@name` (e.g.
  `tree_pine`). The mesh builder resolves that to a bundled mesh
  (`roadmaker::props`) and emits one `ObjectInstance` (model id + world
  transform) per placed prop — the single geometry source shared by the
  viewport and both exporters.
- **What exporters emit for props** (so a downstream simulator receives real
  geometry, not just an OpenDRIVE record):
  - **glTF (.glb):** one shared `mesh` per prop model (trunk + crown
    primitives, flat materials) referenced by one `node` per instance carrying
    the world translation/rotation — idiomatic glTF instancing (many trees stay
    one mesh in the file). Y-up per the export boundary.
  - **USD (.usda):** one `Xform` per instance (`prop_<n>_<model>`) containing
    the part `Mesh` prims baked into world space, each bound to a flat prop
    `Material` under `/Looks` (tinyusdz has no ergonomic prototype instancing,
    so USD bakes rather than instances).
  - Non-prop objects (crosswalks, stop lines, arrows) still mesh as flat paint
    submeshes; other object types round-trip in `.xodr` but carry no 3D export
    geometry yet.

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
