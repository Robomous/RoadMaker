# GS-2 — Imported district

*Golden scene for M3b (real-world import): a ~500 m × 500 m real-world
road-network extract imported from OSM, meshed, and rendered with M3a's
visual stack.*

Specified from OpenDRIVE and OpenStreetMap concepts only, per the
[product-parity rules](../../../../standards/product-parity.md).

## Scene definition

A ~500 m × 500 m extract of a real urban district, imported via the M3b
OSM road-network pipeline:

- **Area selection:** a district with a mixed street grid — at least 15
  intersections (3-arm and 4-arm), one multi-lane arterial, and residential
  streets. The exact extract (bounding box + retrieval date) is fixed in
  `assets/golden_scenes/gs2/` when M3b starts, so the import is
  reproducible against archived source data.
- **Data licensing:** OpenStreetMap data is © OpenStreetMap contributors,
  **ODbL** — ODbL applies to the *data*, not to RoadMaker. The scene, any
  distributed extract, and every render published from it carry the
  attribution "© OpenStreetMap contributors" and a link to the ODbL. The
  attribution requirement is recorded in `ASSETS_LICENSES.md` alongside the
  extract.
- **Import result:** OSM ways/relations → RoadMaker road network —
  reference lines fit (clothoid smoothing of polylines), lane counts and
  types from OSM tags where present (defaults where absent), junctions
  built at shared nodes, geo-anchoring via `<geoReference>` in the
  OpenDRIVE header.
- **Presentation:** M3a's textured mode, terrain skirt, and sky — the
  district renders as a coherent scene, not gray ribbons.

## Acceptance — topological correctness stats

The import is accepted on measured statistics over the extract, reported by
the importer and archived with the scene:

| Statistic | Target |
|---|---|
| OSM drivable ways imported | ≥ 95 % (each drop emits a structured diagnostic, never silent) |
| Junctions with coherent lane links | 100 % of built junctions validate |
| Reference-line continuity | G1 at all record joints within `rm::tol` |
| Exported `.xodr` validation | zero errors; warnings each carry a rule id |
| Round-trip | write → parse → write stable within `rm::tol` |
| Geo-reference | round-trips through the header; a known landmark's projected position within 1 m of its source coordinate |

## Visual checklist

| # | Element | Depends on |
|---|---|---|
| 1 | ☐ District renders fully textured (asphalt/sidewalk/grass) | M3a textured mode |
| 2 | ☐ Junction surfaces watertight across the grid | M2 junction blending at import scale |
| 3 | ☐ Lane markings present where OSM tags imply them | importer marking inference |
| 4 | ☐ Terrain skirt covers the extract without gaps | M3a terrain |
| 5 | ☐ No z-fighting or cracked seams at any intersection from the fixed camera | mesh robustness |
| 6 | ☐ OSM attribution visible in the scene's About/metadata | ODbL compliance |
| 7 | ☐ Buildings give the district real density (not bare road ribbons) | **[Materials & Structures](../seeds/materials-structures.md)** — building props |
| 8 | ☐ Material variety reads across surfaces (asphalt variants, sidewalk, structure) | **[Materials & Structures](../seeds/materials-structures.md)** — material library |

> **Dependency note.** GS-2's approval requires buildings and material variety,
> which are now scheduled in the **Materials & Structures (v0.7.0)** milestone
> (its prerequisite). GS-2's own spec is otherwise unchanged.

## Fixed camera

Kernel frame: right-handed, Z-up, meters; extract centered at the origin.

| Parameter | Value |
|---|---|
| Position | (−350.0, −350.0, 250.0) |
| Target | (0.0, 0.0, 0.0) |
| Up | +Z |
| Vertical FOV | 40° |
| Aspect | 16:9 (render at 1920×1080) |

A high oblique over the district diagonal: the whole extract in frame,
street grid and junction surfaces readable.

## History

- 2026-07-10 — initial spec (this document). Extract area intentionally not
  chosen yet — fixed when M3b starts, with the archived source data.
