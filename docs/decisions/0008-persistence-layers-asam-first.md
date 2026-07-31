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
  fuzz-corpus sample, and round-trip test (enforced since `fmt-s2` #326 by
  `core/tests/test_rm_registry.cpp`, which fails when the code list in
  `core/include/roadmaker/xodr/rm_codes.hpp`, this registry block, the
  writer, the reader, the fuzz corpus, or the round-trip tests fall out of
  step).
- Foreign userData (any non-`rm:` code) is preserved verbatim on every
  element. The historical drops on `<road>`, `<junction>` and at the root
  were fixed by `fmt-s2` (#326): preserve-and-warn, byte-verbatim re-emit.
- Unknown `rm:` codes (from a newer RoadMaker) are preserved verbatim with
  a structured warning, never dropped (implemented by `fmt-s2` #326 at
  every preserved tier).
- Signal-phase *timing* is Layer 1 (`rm:phases`): §14.6 places signal
  cycles outside OpenDRIVE ("specified … in OpenSCENARIO"); phase data
  additionally exports to OpenSCENARIO 1.x traffic-signal actions in P8.

**Registry** — every `rm:` userData code the writer emits, by scope
(amended by `fmt-s2` #326: `rm:terrain` and `rm:material.bridge_deck` were
emitted but unlisted; the block below is now generated and CI-gated):

<!-- rm-registry:begin — mirrors core/include/roadmaker/xodr/rm_codes.hpp; regenerate via core/tests/test_rm_registry.cpp -->
- road: `rm:waypoints`, `rm:aux_boundary`
- object: `rm:crosswalk`, `rm:markingCurve`, `rm:assembly`, `rm:stencil`, `rm:stopline`, `rm:material.bridge_deck`
- junction: `rm:arms`, `rm:corners`, `rm:floor`, `rm:maneuver`, `rm:signal`, `rm:signalmount`, `rm:phases`, `rm:junction`, `rm:spans`
- root: `rm:surface`, `rm:terrain`
<!-- rm-registry:end -->

The `rm:<material-id>` namespace (`rm:asphalt`, `rm:paint_white`, …) is
NOT a userData code: those are material ids carried in attribute values
(lane `<material @surface>`, `<roadMark @material>`, `rm:junction` `mat=`
fields, `<userData code="rm:surface" material=…>`), so they live outside
this table. Each owning sprint defines its payload against this policy.

`rm:signal` (p4-s7, shipped) records WHICH auto-signalization template produced
a junction's signals, so the tool can show the current template and re-apply
coherently. Layer 1 on top of a full Layer 0: the `<signal>` (§14.1) and
`<controller>`/`<control>` (§14.6) elements ARE the export and a foreign reader
loses only the authoring provenance. Junction scope, one entry, fields `:`-joined:
`template=protected_left|two_phase|all_way_stop|two_way_stop[:mount=<modelId>]`.
`mount` names the prop model — or, since `p6-s9` (#323), the composite
**assembly** — placed with each signal, and is omitted when there is none. The
token's shape is identical either way, because `signalize_junction` resolves it
against `props::model()` first and `props::assembly()` second. The whole element
is omitted when no template was applied, so an unsignalized junction re-exports
byte-identically. Degradation: a missing,
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

`rm:assembly` (p6-s9, shipped) records that an `<object>` is one PART of a
composite prop assembly — a mast-arm traffic signal is a pole, an arm and two
heads — and where in that assembly it sits. Layer 1 with NO Layer-0 counterpart:
OpenDRIVE has no object grouping at all. §13.10's `<objectReference>` is
cross-road *identity* for one physical object spanning several roads (Table 102
gives it `@id`, `@s`, `@t`, `@zOffset`, `@validLength`, `@orientation` and nothing
relative), and §13.3's `<skeleton>`/`<polyline>` describes an object's own shape
volumes rather than a set of asset instances — so there is nothing for a foreign
reader to lose beyond the grouping itself: it gets N valid, individually-placed
props. Object scope, attribute form: `asset` (the `props::assembly()` id),
`inst` (the token shared by one placement's parts) and `part` (0-based) are all
REQUIRED — they are the record, and a partial one would present as a one-part
assembly — plus `du`/`dv`/`dz`/`dyaw`, the part's pose in the assembly's local
frame, each omitted at its default of zero. So the anchor part writes three
attributes, and an assembly that is placed and never re-posed re-exports
byte-identically. `part` is bounded by `props::kMaxAssemblyParts` on BOTH sides
(the writer drops a record it cannot express rather than emitting a value the
reader would refuse). The pose is duplicated onto every part rather than re-read
from the catalogue on purpose: a scene opened without its project has no overlay
catalogue (#508), and re-deriving would let an edit to an asset definition drag
every placed instance with it — so the record is authoritative for the INSTANCE
and the catalogue for the ASSET. Degradation follows the policy above: a malformed
or missing required field drops the record but keeps the object live (Layer 0
survives), and an unknown attribute warns without costing the record.

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

Refined by
[ADR-0014](./0014-scenario-model-kernel-side-osc-1x.md) (`p8-s1`,
[#245](https://github.com/Robomous/RoadMaker/issues/245)) — see the
amendment at the end of this record.

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

## Amendment (2026-07-30, `p8-s1` / #245)

The *OpenSCENARIO* section above was written before any scenario code
existed, and left four things open that the first P8 sprint had to
settle. [ADR-0014](./0014-scenario-model-kernel-side-osc-1x.md) settles
them, and that section is read with it:

- **"One internal scenario model" means one *kernel-side* model.** It
  lives under `core/include/roadmaker/osc/` in namespace
  `roadmaker::osc` and is mutated only through kernel command factories
  (`osc::edit`, per ADR-0014's own 2026-07-31 amendment — this line
  originally said `edit::Command`), because the Python module links
  `roadmaker::core` alone
  (`python/CMakeLists.txt:36`) and a model in `editor/src/document/`
  could never be replayed headlessly.
- **"1.x" now names a revision.** The writer is revision-targetable and
  **defaults to 1.2**, the shape `WriterOptions::target_version` already
  uses for OpenDRIVE, because validation means "esmini accepts the file"
  and CI pins that binary.
- **"Export first" is also an import.** #245's acceptance is
  read/write/validate, so the never-drop contract this ADR states for
  OpenDRIVE governs `.xosc` identically — a preserved tier plus
  structured diagnostics, with `fmt-s2`'s re-canonicalization caveat.
- **"Validation-friendly" is now defined.** There is no XSD in CI and
  esmini's parser is the validator (ruling on #257); ADR-0014 adds
  `asam.net:xosc:` checker-rule ids cited through `Diagnostic::rule_id`,
  additive to that ruling rather than a substitute.

`rm:phases`' promise above — that phase timing exports to OpenSCENARIO
traffic-signal actions in P8 — is discharged, but not one-for-one:
RoadMaker's cycle is per junction and OSC's `TrafficSignalController` is
per `<controller>`, so one timeline decomposes into N phase lists.

A `.xosc` is a **second Layer-0 file**, not a Layer-2 sidecar: top-level
in the project directory beside its `.xodr`, stem-matched, and
standalone-openable in any tool. The three-layer contract is unchanged.
