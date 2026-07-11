# ADR-0006: Terrain scope

- **Status:** PROPOSED — maintainer decides; nothing is implemented until
  this ADR is accepted
- **Date:** 2026-07-11
- **Deciders:** Armando Anaya

## Context

Maintainer dogfooding of v0.3.0 surfaced an expectation gap: building a
real map raises the question of "mountain-type" ground — terrain the roads
sit *in*, not just the roads themselves. Today the
[gap analysis](../roadmap/gap_analysis.md#known-exclusions) records
"terrain sculpting beyond the road corridor" as a **backlog exclusion**,
and M3a's scope is limited to a terrain *skirt* plus procedural ground
around the network. Road **vertical profile** UX (hills, grades,
overpasses along the road) is separate and is being fixed in the hardening
sprint's elevation workstream; this ADR is only about the ground surface.

The question matters long-term because it decides whether RoadMaker's
answer to the RoadRunner-visual expectation is "roads carry their own
elevation, the ground is presentation" or "there is a real height field
the network conforms to" — with import (M3b, GDAL/PDAL/PROJ) being the
natural place a height field would come from anyway.

## Options

### A — Stay excluded (status quo)

Roads carry their own elevation; ground remains the M3a flat/skirt visual.

- **Cost:** zero new work.
- **Impact:** weakest look; scenes with sustained grades float over flat
  ground; the dogfooding expectation stays unmet. Honest and cheap.
- **Roadmap edits:** none (gap-analysis row stands as-is).

### B — Heightmap terrain under the network *(recommended)*

A single height field per scene: import a DEM (raster — ties directly
into M3b's GDAL work) plus a simple raise/lower/smooth brush; roads
cut/conform to the field via the existing skirt logic. No materials
system, no arbitrary meshes — one grid, one texture set.

- **Cost:** moderate — a heightmap data model + sampler in the kernel
  frame, a GDAL raster ingest path (M3b already pays the GDAL licensing
  and build cost), brush tools as commands, and a mesh/skirt integration
  pass. Roughly one M3b work package.
- **Impact:** covers the RoadRunner-visual expectation for imported and
  authored scenes; GS-2 ("imported district") gets believable ground for
  free from the same DEM the import brings in.
- **Roadmap edits:** add a "heightmap terrain (DEM import + basic brush)"
  line to the M3b seed and GS-2's checklist; flip the gap-analysis row
  from "backlog" to "scheduled (M3b, heightmap only)"; sculpting beyond a
  height field stays excluded.

### C — Full sculpting/materials terrain system

General terrain modeling: multi-layer materials, arbitrary sculpting,
holes/overhangs, terrain LOD.

- **Cost:** large — a second product surface with its own asset pipeline,
  serialization, and render path; competes directly with the core mission
  (standards-correct road/scenario editing) for multiple milestones.
- **Impact:** best visuals, worst opportunity cost; none of the golden
  scenes requires it, and simulators consuming our exports do not read a
  proprietary terrain format anyway.
- **Roadmap edits:** would need its own milestone between M3b and M4 and
  a new golden scene; not compatible with the current sequence without
  displacing scenario work.

## Recommendation

**B, scheduled with M3b.** It answers the actual dogfooding expectation
(ground that isn't a flat plate) at the moment the enabling dependency
(GDAL) arrives for import reasons regardless; it keeps sculpting-as-a-
product excluded, which the gap analysis already argues is a different
product surface; and it upgrades GS-2's acceptance from "renders as a
district" to "renders as a district on its real ground" with data the
import pipeline already touches. Option A remains the fallback if M3b's
dependency work runs over budget — the ADR would then be re-dated, not
rewritten.

## Consequences

- If accepted as B: M3b's seed and GS-2 gain a terrain work package; the
  kernel gains a height-field concept that mesh/skirt code must respect;
  reversal cost before M3b starts is one seed edit, afterwards it is a
  feature removal.
- If A: no code consequences; the expectation gap is documented as a
  deliberate trade, and this ADR is the record to point to.
- If C: roadmap renegotiation — out of scope for a hardening sprint to
  estimate honestly beyond "multiple milestones".

## References

- [Gap analysis — known exclusions](../roadmap/gap_analysis.md#known-exclusions)
- [M3b seed](../roadmap/seeds/m3b.md) (GDAL/PDAL/PROJ import stack)
- [GS-2 "Imported district"](../roadmap/golden_scenes/gs2_imported_district.md)
- Hardening sprint tracking epic:
  [#81](https://github.com/Robomous/RoadMaker/issues/81)
