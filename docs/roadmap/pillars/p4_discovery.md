# P4 discovery — Junctions & Signals

*What the code actually looks like against the P4 scope, and the sprint
cut that follows. Written 2026-07-20, after p4-s1/p4-s2 (Corner tool +
materials) merged and as part of the
[2026-07 realignment](../updates/2026-07-realignment.md). Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-3](../golden_workflows/gw3_corner_materials.md) and
[GW-4](../golden_workflows/gw4_signals.md).*

> **Status (2026-07-23):** the pillar was sprint-complete; it is **reopened**
> by the [2026-07 field triage, batch 2](../updates/2026-07-field-triage-2.md)
> (#397) for one release-blocking item: bug
> [#402](https://github.com/Robomous/RoadMaker/issues/402) (junction-floor
> sidewalk band segmentation — jagged seams + zero-band collapse; lands
> **before** [#356](https://github.com/Robomous/RoadMaker/issues/356)).
> GW-3/GW-4 hand-run status is unaffected until the item lands.

## Why this document exists

P4 started (two sprints merged) without a discovery report. Read against
the code, the remaining scope splits into **three behavior areas** —
junction control, junction interior surface control, and maneuvers +
signalization + signs — and the biggest finding is what *doesn't* exist:
no stopline entity, no locked-junction concept, no maneuver entity, and no
`<controller>` support, while the `Signal` struct and its OpenDRIVE §14 IO
are already complete. The kernel gaps, not the tools, dictate the order.

## 1. What exists

- **Corners (done, p4-s1/s2).** `JunctionCorner`
  (`core/include/roadmaker/road/junction.hpp:43-66`): arm-pair identity,
  radius, extents, per-corner sidewalk/median materials;
  `default_corner_radius` + junction `material` at junction scope.
  Persisted as `rm:corners` / `rm:junction` userData.
- **Signals (kernel complete, editor partial).** `Signal`
  (`core/include/roadmaker/road/signal.hpp:24-63`) models §14 with
  verbatim `RawXml` preservation; `<signalReference>` is raw-preserved.
  Kernel ops `add/delete/move_signal` exist
  (`edit/operations.hpp:745-766`); the editor renders, selects, moves and
  deletes signals and shows s/t/hOffset attributes — but there is **no
  signal authoring tool**.
- **Junction movements.** Exported as standard `<connection>` +
  `<laneLink>` referencing real connecting roads
  (`core/src/xodr/writer.cpp:1048-1084`); regeneration re-derives them in
  place from the junction's `arms`.
- **Junction fill.** The deduplicated backend
  (`core/src/mesh/junction_surface.cpp`,
  `junction_corner_detail.cpp`) builds the floor with **no per-road-span
  structure** — nothing user-visible explains or controls interior
  triangulation.
- **Stop lines (crosswalk-only).** The only stop line today is a crosswalk
  companion: `<object type="roadMark" subtype="signalLines">` authored by
  the Crosswalk & Stop Line tool (`edit/markings.hpp:143`). It is an
  Object, not a road-end entity.

## 2. What does not exist (confirmed by search)

- **No stopline entity** — no kernel record tying a stop line to a road
  end with a distance/direction.
- **No locked/manual junction** — `Junction` has no lock state; the only
  distinction is arms-present (regenerable) vs arms-empty (foreign,
  "recreate to edit"). Regeneration always re-derives.
- **No maneuver entity** — nothing selectable/editable between
  `JunctionConnection` and the connecting road's full geometry.
- **No `<controller>`/`<control>`** anywhere in the kernel. Normatively
  (OpenDRIVE 1.9.0 §14.6) controllers map dynamic signals to signal
  groups, and *"dynamic content like the signal cycle itself is specified
  outside of this standard"* — so signal grouping/gating has a standard
  carrier to implement, while phase **timing** needs a Layer-1 carrier
  (`rm:phases`) per [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md),
  exporting to OpenSCENARIO 1.x in P8.

## 3. The stopline entity — designed once

Three consumers need the same concept, so it is specified here once and
implemented in p4-s3, not three times:

- **Definition.** A kernel record owned by a road end: **Distance**
  (meters from the road end, along s), **direction** (flippable),
  optional link to the feature that spawned it (junction arm, crosswalk).
- **Consumers.** (1) Junction **default stoplines** — derived at every
  arm of a junction, editable (typed/scrubbed Distance, flip,
  drag-to-move). (2) The P3 **crosswalk stop line** — migrates onto the
  shared entity so there is exactly one implementation. (3) **Maneuver
  termination** (p4-s6) — maneuvers end at the stopline. Corner extent
  defaults may also read it as a start-distance hint.
- **Carriers.** Geometry stays the existing Layer-0 mark
  (`<object type="roadMark" subtype="signalLines">`); the parametric link
  (owning road end, distance, direction) rides `rm:stopline` (Layer 1).

## 4. Sprint cut (remaining P4)

The docs-imposed order — stoplines → locked junctions → junction surface
→ maneuvers → signals → signs — maps to seven sprints. p4-s3/s4/s5 are
new issues; p4-s6…s9 are the pre-realignment issues retitled in place.

| Sprint | Area | One-line scope |
|---|---|---|
| p4-s3 | Junction control | Stopline entity + per-road default stoplines (Distance, flip, drag) |
| p4-s4 | Junction control | Locked junctions: manual create, convert auto↔locked, add/remove roads, merge, parallel-road s-span and single-road junctions, corner re-derivation |
| p4-s5 | Interior surface | Span visualization + Include Samples + Sort Index (Raise/Lower) over a per-span fill structure |
| p4-s6 | Maneuvers | Maneuver entity: auto-derivation + rebuild, endpoint sliding on anchor lanes, control points, Lock Geometry, Convert-to-Explicit, Turn Type |
| p4-s7 | Signals | Signal tool: auto-signalize templates (protected-left; static/dynamic), assemblies auto-linked to signals (assembly model from p6-s9); `<controller>`/`<control>` export |
| p4-s8 | Signals | Signal Phase Editor in the 2D Editor pane; `rm:phases` timing carrier |
| p4-s9 | Signs | Sign tool with editable text (text-to-texture; correct type/subtype codes) |

Dependencies from outside the pillar: `p1-s5` (toolbar information
architecture) lands before p4-s3 so the new tools arrive into a
categorized toolbar; `p6-s9` (assemblies) precedes p4-s7's linkage;
maneuver-to-elevation projection is explicitly deferred until elevation
assets exist (P5 heightmaps / P7 imports).

## 5. Known risks

- #311: pre-existing `write_xodr` segfault on a junction with a dangling
  arm road — must be fixed or worked around before p4-s4 multiplies
  membership-editing paths.
- The fill backend gaining per-span structure (p4-s5) touches the same
  code the corner detail mesher uses — sanitizer runs mandatory.
- Locked junctions change the regeneration contract
  (`edit::regenerate_junction`); the junction-regen test suite from p2-s2
  is the safety net and must grow locked cases.
