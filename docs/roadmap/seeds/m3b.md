# M3b seed — real-world import

*Scope sketch for M3b. A seed, not a design: full design docs are written
when M3b's planning task runs.*

- **Theme:** the world comes in — real road networks become editable
  RoadMaker documents.
- **Golden scene:** [GS-2 "Imported district"](../golden_scenes/gs2_imported_district.md)
- **Release target:** v0.5.0
- **Gap coverage:** enables scale; no direct gap, but GS-2 proves M3a's
  visual stack on non-authored data.

## Scope sketch

### Kernel

- **GIS/vector + raster import:** GDAL (MIT) for vector/raster formats,
  PROJ (MIT) for CRS transforms; geo-anchoring via `<geoReference>` in the
  OpenDRIVE header.
- **Lidar/point-cloud reference:** PDAL (BSD-3) — point clouds as read-only
  reference layers for tracing, not as mesh input.
- **OSM road-network extraction:** ways/relations → reference-line fitting
  (polyline → clothoid smoothing), lane counts/types from tags with
  documented defaults, junction construction at shared nodes. Structured
  diagnostics for every dropped or defaulted element — the parser-never-
  silently-drops rule extends to importers.
- License audit of all transitive deps before pinning
  ([policy](../../standards/dependencies.md)).

### Quality gate: scale targets

M3b owns the roadmap's
[scale-targets gate](../roadmap.md#cross-cutting-quality-gates) (added
2026-07-10; measured from M3b on): open + edit a **1,000-road network**
and import a **~50 km² OSM district**. The metrics are load time,
node-drag latency at scale, and a memory ceiling; concrete numbers are
*proposed* for maintainer sign-off during the M3b planning task. A
measurement harness for these metrics is part of M3b scope.

### Editor

- Import dialog/flow (source selection, extent, CRS report, import stats).
- Reference-layer display (imagery underlay, point-cloud display) —
  read-only layers under the road network.
- Post-import cleanup workflow using the M2 editing tools.

### Assets

- None beyond M3a's; GS-2 renders with the M3a stack. OSM extract data
  carries ODbL attribution (recorded with the scene, not an asset row).

## Dependencies on prior milestones

- M2 editing tools (post-import cleanup is editing).
- M3a textured rendering + terrain (GS-2's acceptance is "renders as a
  coherent scene").
- M3a's junction/mark features exercised at import scale.

## Top 3 risks

1. **Dependency weight & licensing surface** — GDAL/PDAL/PROJ drag large
   transitive trees; mitigation: import support behind a build option if
   needed, full transitive license audit before pinning, wheels/installers
   decide inclusion explicitly.
2. **OSM data quality → junction construction** — real intersections are
   messy (missing lane tags, odd angles, dual carriageways); mitigation:
   topological-correctness stats as the acceptance gate (GS-2), extensive
   diagnostic coverage, cleanup workflow as a first-class deliverable.
3. **CRS/geodesy correctness** — projection errors are silent and
   embarrassing; mitigation: landmark round-trip test in GS-2 acceptance,
   PROJ for all transforms, no hand-rolled geodesy.

## Open questions for the maintainer

1. Is lidar/PDAL in the M3b critical path, or optional behind a build flag
   with OSM/GDAL as the core deliverable?
2. Imagery underlay licensing/provider stance (OSM raster tiles have usage
   policies; local user-supplied imagery only?).
3. How much automatic lane inference from OSM tags vs. defaulting +
   manual cleanup?
