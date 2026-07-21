# ADR-0008: Persistence layers — ASAM first, RoadMaker enrichment on top

- **Status:** ACCEPTED — maintainer approved 2026-07-20
  (via the [2026-07 roadmap realignment](../roadmap/updates/2026-07-realignment.md))
- **Date:** 2026-07-20
- **Deciders:** Armando Anaya

## Context

RoadMaker persists scenes as bare `.xodr` files; ten `rm:*` userData codes
(waypoints, crosswalk, markingCurve, stencil, aux_boundary, arms, corners,
junction, surface, and the `rm:<name>` material ids) have accreted
one-by-one as features needed carriers, with no written policy. P4 will
add stoplines, locked junctions, span sort indices, maneuvers, and signal
phases; P6 adds imported user assets; P7 adds georeferencing; P8 adds
scenarios. Meanwhile real state is lost on reload (camera, selection,
snapping, session state) or stranded per-machine in QSettings, and foreign
`<userData>` is dropped on `<junction>` and at the document root — a
round-trip defect. Without a layering decision, every sprint re-litigates
"where does this datum live".

## Decision

Maximum ASAM compatibility comes first — OpenDRIVE, and OpenSCENARIO both
1.x and 2.x — with RoadMaker-specific enrichment layered on top **without
ever breaking pure-ASAM interchange**. Three layers:

### Layer 0 — pure ASAM (inviolable)

An exported `.xodr` (later `.xosc` 1.x, and the OpenSCENARIO 2.x subset)
is always valid, self-contained, and consumable by third-party tools with
zero RoadMaker knowledge. Importing a pure ASAM file authored elsewhere
always works. Anything expressible in the standard uses the standard:
signal groups and junction gates use `<controller>`/`<control>`
(OpenDRIVE 1.9.0 §14.6), georeference uses `<header><geoReference>`
(§8.5), maneuvers export as connecting roads with
`<connection>`/`<laneLink>`.

### Layer 1 — ASAM-adjacent enrichment

RoadMaker data that annotates ASAM entities travels inside the ASAM file
via the standard extension mechanism: namespaced `<userData code="rm:…">`.
A RoadMaker export round-trips losslessly through RoadMaker and degrades
gracefully in other tools (they ignore `userData`). Policy:

- One `rm:` code per concern; payloads are versioned-by-shape — unknown
  fields warn, never fail.
- Every code appears in the registry below and ships with parser, writer,
  fuzz-corpus sample, and round-trip test (enforced by `fmt-s2`).
- Foreign userData (any non-`rm:` code) is preserved verbatim on every
  element. The current drops on `<junction>` and at the root are defects,
  fixed by `fmt-s2`.
- Unknown `rm:` codes (from a newer RoadMaker) are preserved verbatim with
  a structured warning, never dropped.
- Signal-phase *timing* is Layer 1 (`rm:phases`): §14.6 places signal
  cycles outside OpenDRIVE ("specified … in OpenSCENARIO"); phase data
  additionally exports to OpenSCENARIO 1.x traffic-signal actions in P8.

**Registry** — existing: `rm:waypoints`, `rm:crosswalk`, `rm:markingCurve`,
`rm:stencil`, `rm:aux_boundary`, `rm:arms`, `rm:corners`, `rm:junction`,
`rm:surface`, `rm:<material-id>`, `rm:stopline`. Planned:
`rm:arms`/`rm:junction` extensions for locked state and s-spans (p4-s4),
per-span surface records (p4-s5), `rm:maneuver` (p4-s6), `rm:phases`
(p4-s8). Each owning sprint defines its payload against this policy.

`rm:stopline` (p4-s3, shipped) is the worked example of a **materialized**
record: the Layer-0 carrier is not something the user placed but an
`<object type="roadMark" subtype="signalLines">` the writer synthesizes per
junction arm (§13.7 Table 117 — a bounding-volume road-marking object, no
`<outline>`, so it serializes identically under 1.8.1 and 1.9.0). Object
scope, attribute form: `contact="start|end"` (required — the junction-facing
end of the enclosing road, which IS the record's identity), plus `distance`,
`flipped` and `crosswalk`, each omitted at its default. A foreign reader gets
a valid, placed stop line; RoadMaker absorbs the tagged objects back into
`Junction::stoplines` on load, so they are never live arena objects and a
round trip neither duplicates them nor loses the authoring. Degradation
follows the policy above: a malformed field drops the record but keeps the
object live (Layer 0 survives), and an unknown attribute warns without
costing the record.

### Layer 2 — native project/scene container

Everything with no business inside an ASAM file lives in the RoadMaker
container: a **versioned project directory** — `project.json` v2 plus a
per-scene sidecar `<scene>.rmscene.json` next to its `.xodr` — carrying
editor/session state (camera, snapping, per-scene render mode),
library/asset references and import metadata, prop-set and
material-overlay definitions, workspace extents and georeference framing,
and (P8) scenario-editor state. Deliberately **not** a single-file
archive: the directory form is git-friendly, diffable, partial-write-safe,
and keeps every `.xodr` standalone-openable. A single-file "package"
export may be added post-v0.1.0 as a convenience.

### Compatibility contract (tested, `fmt-s1`)

- Open a pure `.xodr` → full editing, no sidecar required.
- Save inside a project → Layers 1+2 written (sidecar atomically).
- Export ASAM → Layers 0+1 only.
- A missing or stale sidecar degrades to defaults; it never blocks
  opening and never loses scene content.

### OpenSCENARIO

One internal scenario model; OpenSCENARIO **1.x XML export first**
(validation-friendly, esmini-compatible); OpenSCENARIO **2.x as an
export-only concrete-scenario subset** at v0.1.0 (`p8-s6`) with no parser
dependency; OSC2 import is deferred and gated on a future dependency
review per the [dependency policy](../standards/dependencies.md).

## Consequences

- Every sprint states in its issue which layer each new datum uses; no
  more ad-hoc carriers. The P4 epic references this ADR for all its
  carriers.
- `fmt-s1` (container) and `fmt-s2` (preservation hardening + registry
  conformance tests) implement the enforcement; both carry the `fmt`
  workstream label under P6.
- Third-party interchange can never regress silently: the contract tests
  and registry conformance tests are CI gates.
- Cost: sidecar schema maintenance and one more file next to each scene —
  accepted as the price of keeping `.xodr` pure.
