# M3a road-mark completions

Goal: complete the road-mark features M2 descoped, so GS-1's markings render and
validate like real ones — the center **double-yellow** as true dual geometry,
**road-mark color**, and the object-based **stop lines** and **lane arrows**.

Baseline: M2 ships `RoadMark { s_offset, type, width }` on a lane's outer
boundary (`core/include/roadmaker/road/lane.hpp`) with `RoadMarkType` including
`SolidSolid` — but M2 renders `solid solid` as a **single** strip and has no
color (`docs/design/m2/00_overview.md` non-goals). M3a closes both.

## 1. Model additions — `<roadMark>` color and true multi-line geometry

Per OpenDRIVE §11.9 (Lane road markings), a `<roadMark>` carries `@color` and,
for multi-line marks, an explicit `<type>`/`<line>` sub-geometry giving each
stripe its own `width`, `space`, and lateral `tOffset`. M2 stored only a scalar
width. M3a extends `RoadMark`:

```cpp
enum class RoadMarkColor {          // e_roadMarkColor, §11.9
  Standard, White, Yellow, Red, Blue, Green, Orange, Other,
};

/// One painted stripe of a (possibly multi-line) road mark. Absent for the
/// simple single-line case, where `width` on RoadMark is authoritative.
struct RoadMarkLine {
  double width = 0.12;   // stripe width [m]
  double length = 0.0;   // painted length [m] (0 = continuous)
  double space  = 0.0;   // gap length [m] (0 = solid)
  double t_offset = 0.0; // lateral offset of this stripe from the mark line [m]
  double s_offset = 0.0; // longitudinal start offset within the mark [m]
};

struct RoadMark {                    // extended
  double s_offset = 0.0;
  RoadMarkType type = RoadMarkType::None;
  double width = 0.12;               // outer/simple width (kept)
  RoadMarkColor color = RoadMarkColor::Standard;   // NEW
  std::vector<RoadMarkLine> lines;   // NEW — populated for solid_solid etc.
};
```

`lines` is empty for the common single-stripe mark (M2 behavior preserved —
round-trip byte-stable for existing files). For `SolidSolid`/`SolidBroken`/
`BrokenSolid` the parser emits two `RoadMarkLine`s with symmetric `t_offset`
(±(width/2 + gap/2)); the writer round-trips whatever it parsed, and the
authoring API produces the canonical two-line form. Color defaults to
`Standard` (which the renderer resolves to white/yellow by mark type and
region) and is written explicitly only when not `Standard`.

## 2. `<roadMark>` parse / write / validate deltas

- **Parse** (`xodr/reader.cpp`): read `@color`; if a `<type>` child with
  `<line>` elements is present, populate `lines`; otherwise leave `lines` empty
  and keep the scalar `width`. Unknown colors → `RoadMarkColor::Other` + a
  diagnostic (never dropped), matching the existing `RoadMarkType::Other` path.
- **Write** (`xodr/writer.cpp`): emit `@color` when ≠ `Standard`; emit the
  `<type>`/`<line>` block when `lines` is non-empty; otherwise emit the M2
  single-`@width` form unchanged.
- **Validate:** no new normative rule is mandatory for color, but M3a adds a
  soft check that `SolidSolid`-family marks carry exactly two `lines` when
  authored (an internal consistency warning, not an ASAM rule ID, so it is
  labeled a RoadMaker advisory in the diagnostic, not spoofed as normative).

## 3. Stop lines and lane arrows — object markings, not signals

OpenDRIVE §13.1 is explicit: crosswalks and painted road markings that are not
mandatory control signals are **objects**, described via object markings
(§13.8) — closed outlines with `@fillType="paint"` or a `<markings>` element on
the outline. Only traffic-control *signs and lights* are `<signal>`s
([`01`](01_kernel_objects_signals.md) §7). So in M3a:

| GS-1 marking | Representation | Mesh |
|---|---|---|
| Crosswalk (per arm) | `<object type="crosswalk">` with a closed `<outline>` (`<cornerRoad>` rectangle across the lanes), `@fillType="paint"` | striped quad band (§4) |
| Stop line (per approach) | `<object type="roadMark">` (subtype `stopLine`), closed painted outline spanning the approach lanes | filled quad |
| Lane arrow (approach lanes) | `<object type="roadMark">` (subtype `arrow`, variant left/straight/right) point object at lane center, `hdg` along travel | generated arrow glyph (§4) |

These reuse the `Object`/`ObjectOutline` model from [`01`](01_kernel_objects_signals.md)
§2 — no new kernel type. The editor authors them through the object-placement
UX ([`05`](05_editor_and_docs.md) §2) with lane-snapped defaults (stop line
spans the approach lanes; arrow sits at lane-center `t`, `s` a fixed setback
from the stop line).

## 4. Mesh generation

New marking geometry in the mesh builder, gated by `MeshOptions.markings`
(already exists) and emitted as separate marking submeshes so the renderer can
give them the paint material and draw them coplanar-offset above the road
surface (avoiding z-fighting):

- **Dual-strip lane marks:** for a `RoadMark` with two `lines`, emit two strips
  at the stripe `t_offset`s instead of M2's single centered strip. Single-line
  marks keep the M2 path. Dashed stripes (`space > 0`) tessellate as segment
  runs of `length` on / `space` off.
- **Crosswalk / stop-line outline fill:** triangulate the closed painted
  outline (a convex quad band for GS-1) into a filled strip; crosswalk gets the
  zebra pattern as alternating painted/unpainted quads derived from the outline
  width.
- **Lane arrows:** a parametric arrow **glyph generator** (shaft + head) in road
  coordinates, sized to lane width, oriented by the object `hdg`, one of
  left/straight/right. Pure function of the object + lane geometry — no asset
  fetch (GS-1 checklist row 5 says "arrow glyph geometry (generated)").
- All marking meshes lift by a small `kMarkingLift` along the surface normal and
  carry `PrimitiveKind::Triangles` with the paint color from `RoadMarkColor` (or
  the object's paint fill), resolved to an RGBA in the scene builder, not the
  kernel (the kernel stays render-free).

Incremental re-mesh: markings belong to their lane's road, so they re-tessellate
with `remesh_roads`; object-based arrows/stop-lines belong to the object layer
and re-mesh with the object dirty set ([`01`](01_kernel_objects_signals.md)
§2.4). Determinism (stable vertex order) is required, matching the M2 mesh
tests.

## 5. Test plan

- **Round-trip:** a fixture with `solid solid` yellow center line + dashed white
  lane lines parses → writes → re-parses byte-stable; `lines` and `color`
  preserved. An existing M2 single-line fixture stays byte-identical (no
  regression from the new fields).
- **Mesh:** dual-strip mark emits two disjoint strips at the expected
  `t_offset`s; arrow glyph vertex count/orientation deterministic across runs;
  crosswalk zebra alternation correct for a known outline.
- **Validation:** authored `SolidSolid` without two lines triggers the advisory;
  crosswalk/stop-line objects validate under the object rules
  ([`01`](01_kernel_objects_signals.md) §4).
- **Golden:** GS-1 rows 4–8 (crosswalks, arrows, stop lines, double-yellow,
  dashed white) check against the fixed-camera render.
</content>
