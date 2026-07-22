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
`rm:surface`, `rm:<material-id>`, `rm:stopline`, `rm:spans`, `rm:floor`,
`rm:maneuver`, `rm:signal`, `rm:signalmount`, `rm:phases`.
Each owning sprint defines its payload against this policy.

`rm:signal` (p4-s7, shipped) records WHICH auto-signalization template produced
a junction's signals, so the tool can show the current template and re-apply
coherently. Layer 1 on top of a full Layer 0: the `<signal>` (§14.1) and
`<controller>`/`<control>` (§14.6) elements ARE the export and a foreign reader
loses only the authoring provenance. Junction scope, one entry, fields `:`-joined:
`template=protected_left|two_phase|all_way_stop|two_way_stop[:mount=<modelId>]`.
`mount` names the prop model placed with each signal and is omitted when there
is none; the whole element is omitted when no template was applied, so an
unsignalized junction re-exports byte-identically. Degradation: a missing,
repeated or unrecognized template — or a repeated/unencodable mount — drops the
whole value with one warning (all-or-nothing, like `rm:maneuver`), while an
unknown FIELD key warns and is skipped (forward-compat, like `rm:junction`).
Nothing is re-derived on load — the `<signal>`/`<controller>` elements win.

`rm:signalmount` (p4-s7, shipped) pairs each logical signal with the physical
`<object>`s that represent it. Layer 1 with NO Layer-0 counterpart: §14.1
Table 122 gives `<signal>` only its own bounding box, and nothing in the
standard ties a signal to an object, so a foreign reader loses nothing. Junction
scope, entries `;`-joined, each `signalOdrId=objOdrId[,objOdrId…]`. The object
list is a LIST from day one so #323 (assemblies) replaces one model id with an
assembly's parts and needs no schema change; it is bounded by
`kMaxSignalMountParts` on both sides (the writer truncates to it, the reader
rejects a longer value). Stale entries — a signal or object that no longer
exists — are dropped on write like stale arms, and an empty result emits no
element. Degradation is all-or-nothing throughout: the value is a map, so it has
no field key to be forward-compatible about.

`rm:phases` (p4-s8, shipped) carries a signalized junction's phase-cycle
*timing* — the one thing OpenDRIVE deliberately omits: §14.6 states that "the
signal cycle itself is specified outside of this standard, for example, in ASAM
OpenSCENARIO", so there is no Layer-0 counterpart and a foreign reader loses
only the timing (the `<signal>`/`<controller>` wiring stays intact). Junction
scope, emitted after `rm:signalmount` and before `rm:junction`, never on a span
junction, and omitted entirely when no cycle is authored — so a junction whose
timing is still the derived default (the empty ⇔ derived invariant) re-exports
byte-identically. Entries `;`-joined in cycle order, each entry `:`-joined:
`[name=<token>:]dur=<seconds>[:st=<ctrl>,<g|y|r|o>|…]`; a state is per
controller (the §14.6 sync-group member), unstated members are Red, so an
all-red clearance phase carries no `st` field. The schema mirrors
OpenSCENARIO 1.4.0 `TrafficSignalController`/`Phase{duration,name,
trafficSignalStates}` so the P8 export to OpenSCENARIO traffic-signal actions is
near-mechanical. Degradation is all-or-nothing on the KNOWN grammar (a malformed
or duplicated field, a missing `dur`, an out-of-bounds duration, a bad state or
non-token controller, or exceeding `kMaxSignalPhases`/`kMaxSignalPhaseStates`
drops the whole value with one warning, like `rm:maneuver`), while an unknown
FIELD key warns and is skipped (forward-compat, like `rm:junction`). Controller
ids are NOT resolved on load (dormant-tolerant — a foreign file loads; the
validator advises), and dormant states are pruned on write, so a state naming a
since-deleted controller normalizes away on the next save.

`rm:maneuver` (p4-s6, shipped) carries the junction's authored maneuver
overrides — per connecting road: a geometry lock, a turn-type override, the two
endpoint slides and the interior control points of a hand-shaped path. Layer 1
with NO Layer-0 counterpart: §12.2 Table 56 gives `<connection>` exactly
`@connectingRoad`, `@contactPoint`, `@id` and `@incomingRoad`, and
§12.4/§12.4.2 describe a connecting road purely by its geometry and lane
linkage, so ASAM has nowhere to put a turn type, an endpoint slide or a control
point and a foreign reader loses nothing. Junction scope, entry form
`roadOdrId[:lock=1][:turn=left|straight|right|uturn][:so=<num>][:eo=<num>][:pts=x,y|x,y|…]`
joined with `;` — points use `,` within a point and `|` between points so no
separator collides with the `;`/`:` joins. Every field is omitted at its
default and an entry that authors nothing is dropped entirely (AUTHORS-NOTHING
⇒ ERASE), so a junction that predates the feature re-exports byte-identically
and overriding twice returns the original bytes. The point list is bounded by
`kMaxManeuverControlPoints` on both sides: the writer truncates to it (never
emit what the reader would refuse) and the reader drops a longer value.
Nothing is refitted on load — the `<planView>` is Layer 0 truth and wins.
Degradation follows the policy above: a malformed ENTRY drops the whole value
(all-or-nothing, like `rm:floor`) while an unknown FIELD key warns and is
skipped (forward-compat, like `rm:junction`).

`rm:floor` (p4-s5, shipped) carries the junction floor's per-connecting-road
surface spans — Include Samples and a sort index. It is Layer 1 with NO Layer-0
counterpart at all: §12.10 gives `<junction>` only `<boundary>` and
`<elevationGrid>`, both of them derived OUTPUT geometry with no say in how the
pavement is triangulated, so there is nothing for a foreign reader to lose.
Junction scope, entry form `roadOdrId[:inc=0][:sort=<int>]` joined with `;`,
each field omitted at its default so a junction that predates the feature
re-exports byte-identically. The code is `rm:floor` and not `rm:surface`: that
one already belongs to the P2 ground surfaces (a root-level element) and its
bytes must stay stable. Degradation follows the policy above: a malformed
ENTRY drops the whole value (all-or-nothing, like `rm:corners`) while an
unknown FIELD key warns and is skipped (forward-compat, like `rm:junction`).

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
