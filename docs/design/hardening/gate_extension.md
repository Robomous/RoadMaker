# Gate extension — GW-1/GW-2 NO-GO (2026-07-13)

*Scope-defining design note for the gate-extension sprint. Tracks the
findings-only extension the gate rules mandate; the as-built root causes and
re-gate evidence are filled in as each workstream lands (final report at the
bottom, WS-4).*

- **Epic:** [#147](https://github.com/Robomous/RoadMaker/issues/147)
- **Verdict record:** [`../../roadmap/golden_workflows/gate-v0.4.0.md`](../../roadmap/archive/2026-07-pre-reset/golden_workflows/gate-v0.4.0.md)
- **Trigger:** first GW-1/GW-2 gate run on `main` (`de5bd7c`) → **NO-GO**.

## Rule

Per the gate criteria (fixed at sprint start in
[#81](https://github.com/Robomous/RoadMaker/issues/81)), a crash during GW-1 is
NO-GO and *"the sprint extends with those crashes only."* This extension is
**findings-only** — no new milestone features. v0.4.0 **and** v0.5.0 stay
unpublished; the maintainer re-gates **once** on post-extension `main`, and that
single run covers both releases.

## Findings (maintainer, ground truth)

| # | Finding | Workstream |
|---|---|---|
| 1 | T/X assembly drops on a road are superimposed / desynchronized (no projection, tangent alignment, or attach). | WS-2 ([#149](https://github.com/Robomous/RoadMaker/issues/149)) |
| 2 | Junction regen after node drag leaves connecting roads out of sync. | WS-2 |
| 3 | Visible discontinuity at road-end adjacencies when curvature starts right after the joint (continuity is G1-only). | WS-2 |
| 4 | An overpass produced an unselectable junction-like area that corrupts on adjacent-road delete. | WS-3 ([#150](https://github.com/Robomous/RoadMaker/issues/150)) |
| 5 | Duplicate junctions can be superimposed. | WS-2 |
| 6 | No lane-removal affordance; right-click lane delete **crashed the editor** (hard blocker). | WS-1 ([#148](https://github.com/Robomous/RoadMaker/issues/148)) |

Undo/redo and persistence passed.

## Root causes (confirmed during planning exploration)

- **Crash (6):** `main_window.cpp` builds `ContextMenuDeps` (a struct of
  references) as a **stack local**; every `MenuItem::invoke` closure captures it
  **by reference**, and the menu shows via non-blocking `QMenu::popup()` — the
  lambda outlives `deps`, so any later click is a use-after-free. Affects *every*
  context-menu action.
- **No lane-remove UX (6):** no lane item in the context menu; the only removal
  path is the `remove_lane_` button in `PropertiesPanel`.
- **Assembly drop (1):** `library_drop.cpp` uses the raw cursor as junction
  center with heading 0 — no projection / alignment / attach.
- **Regen desync (2):** `Document::push_applied_with_regeneration` skips *all*
  junction regen while `dirty.topology` is set; regen never runs during the
  preview drag; only `junctions_touching` are regenerated.
- **End adjacencies (3):** continuity is G1-only (`check_mergeable`); no
  curvature continuity checked/enforced; no general "two nearby free ends → close
  gap and link" operation.
- **Duplicate junctions (5):** the only guard is the indirect
  `ends_link_slots_free`; no explicit "end already belongs to a junction"
  invariant, no regenerate-in-place offer.
- **Overpass (4):** the overpass is already pure elevation and junction floors
  are built only per real `Junction` — so the junction-like area means a real
  junction/link was created at the crossing by another path (findings 1/5 class),
  and/or junction floors render but are not viewport-pickable while
  `deletion_closure` drags connecting roads on delete. Diagnosed in WS-3.

## Plan of record

The moat move: build `core/src/edit/connection.{hpp,cpp}` as the single
connection authority, migrate the divergent contact/fit helpers into it, and
refit the three call sites (junction connecting-road fit, assembly drop, gap
closing) onto it. Fix the crash first (hard blocker), then the engine, then
overpass purity + universal selectability, then hardening.

Delivery order (one issue per workstream tracks its PRs):
PR 0 governance → PR 1 crash → PR 2 lane UX → PR 3 engine primitives
→ PR 4 regen sync → PR 5 assembly drop → PR 6 end links → PR 7 idempotency
→ PR 8 overpass → PR 9 hardening + gate re-stamp.

Maintainer decisions (2026-07-13): regen fix is **commit-time landing only**
(live mid-drag junction follow → follow-up issue); the validator rule uses the
vendor namespace `robomous.ai:rm:1.0.0:junctions.arm_single_owner`.

## As-built root causes & re-gate evidence

*(filled in per workstream as each lands; final report WS-4.)*

### WS-3 — Overpass semantics (finding 4), PR #… (branch `fix/overpass-semantics`)

**Diagnosis (ground truth).** The reported "overpass produced an unselectable
junction-like area that corrupts on adjacent-road delete" is **two separate,
non-topological facts**, not a junction created by the overpass:

1. **The overpass creates no topology.** `ProfilePanel::apply_overpass`
   (`find_crossings` → `overpass_points` → one `edit::set_elevation_profile`)
   is pure elevation. `scripts/gw1_replay.py` already asserted
   `junction_count == 1` unchanged through step 3; this is now also asserted
   through the **editor path** by `ProfilePanel.OverpassCreatesNoTopologyAtTheCrossing`
   (junction count stays 0, neither road gains a `junction` back-reference).
2. **The "junction-like area" was the step-2 T-junction's blended floor**
   (`NetworkMesh::junction_floors`), which rendered but `pick()` deliberately
   skipped ("junction floors are not pickable in M1"). It was therefore visible
   but unselectable — read as a mysterious area. "Corrupts on delete" is
   `deletion_closure` doing the *correct* thing (a connecting road cannot outlive
   the incoming road it turns from); with the floor unselectable, the user had
   no way to inspect what changed.

**Fix.**
- **Selectable everywhere.** `PickHit`, `SelectionEntry`, `SceneItem` and the
  highlight rule gained a `JunctionId`. `pick()` now tests junction-floor
  triangles after roads/props (nearest road/prop wins on a tie, so the arms stay
  grabbable; the open floor interior becomes pickable). A viewport click selects
  the junction; the Junctions scene-tree node round-trips to the same entity
  (`SceneTreeModel::index_for_junction`); the properties panel shows the
  junction's arm/connection counts; hover readout says "junction N".
- **No implicit topology at crossings.** Confirmed none is created — the
  diagnosis showed the overpass path never makes a junction/link, so no code
  change was needed there (guarded by the new editor-path test).
- **Delete integrity.** `ProfilePanel.DeletingACrossingRoadLeavesTheOverpassIntact`
  builds an overpass, deletes the crossed road, and asserts the overpass road's
  length, elevation records, and network validity are untouched.
- **Soak invariant** (rendered primitive → selectable entity): every
  `JunctionFloor` in the mesh must resolve to a live junction, and the selection
  never holds a stale junction id (`SoakDriver::check_invariants`).

**Tests.** `test_picking` (`JunctionFloorIsSelectable`, `RoadOverFloorWinsOnTie`),
`test_selection_model` (`JunctionEntrySelectsAndClassifies`,
`StaleJunctionEntryIsDropped`), `test_highlight`
(`JunctionSelectionHighlightsOnlyThatFloor`, `HoverHighlightsTheHoveredFloor`),
`test_scene_tree_model` (`JunctionNodeRoundTripsToASelectableTarget`),
`test_profile_panel` (`OverpassCreatesNoTopologyAtTheCrossing`,
`DeletingACrossingRoadLeavesTheOverpassIntact`). Full editor suite green (286).

### WS-4 — Test & workflow hardening + gate re-stamp, PR #… (branch `test/gate-hardening`)

**Final report — root cause per finding (all fixed):**

| # | Root cause | Fix (workstream / PR) |
|---|------------|------------------------|
| 1 | Assembly drop used the raw cursor as the junction center (`heading = 0`) — no projection/alignment/attach. | `edit::assembly::tee_onto_road` / `cross_onto_road` project onto the road, align to the tangent, and attach in one command; editor `library_drop` routes on-road drops through them. WS-2 PR #158. |
| 2 | `regenerate_junction` matched planned turns to connecting roads by **generation order**, so a drag that re-ordered the plan desynced. | Keyed matching on `(incoming_road, from_lane, to_road, to_lane)`; count-changed refusal surfaces as a Warning toast, not a log line. WS-2 PR #157. |
| 3 | Continuity was G1-only — no curvature continuity, no gap-closing op. | `close_gap` / `create_linked_road` with a **local** G2 weld (three-arc Hermite; never a global refit) + "Link Ends" action. WS-2 PR #159. |
| 4 | The overpass is pure elevation (creates no topology); the "junction-like area" was the T-junction floor, which `pick()` skipped — visible but unselectable. | Junction floors are a first-class selectable entity (pick/hover/highlight/select from viewport + scene tree + properties panel); overpass-creates-no-topology and crossing-delete-integrity asserted through the editor path. WS-3 PR #161. |
| 5 | The only guard against duplicate junctions was indirect; no explicit single-owner invariant, no regenerate-in-place. | Kernel single-owner invariant + validator rule `robomous.ai:rm:1.0.0:junctions.arm_single_owner`; Create Junction regenerates in place on an exact arm-set match. WS-2 PR #155 / #160. |
| 6 | Context-menu closures captured a stack-local `ContextMenuDeps` by reference under non-blocking `popup()` → use-after-free; no lane-remove affordance. | Closures capture by value; per-side lane-remove buttons + context-menu item. WS-1 PR #153 / #154. |

**Soak driver new ops** (`kOps` table): `op_assembly_drop_on_road` (drop T/X onto
a road), `op_remove_lane` (outermost-lane UX path), `op_overpass` (the headless
`apply_overpass` path), `op_delete_crossing_road` (finding-4 integrity), joining
the earlier `op_duplicate_junction_attempt`. **Invariants** (`check_invariants`):
single-junction-per-arm-set (via `validate_network`'s `arm_single_owner`) and
**rendered primitive → selectable entity** (every `JunctionFloor` resolves to a
live junction; the selection never holds a stale junction id).

**Re-gate evidence.**

- **ASan+UBSan soak (local, macOS, `ASAN_OPTIONS=detect_leaks=0`):** seed
  20260713, **4000 ops PASS** (2507 commands, 647 previews, 862 undo / 431 redo,
  125 saves, 457 rejected) — zero sanitizer reports, every invariant held
  through the new gate-finding ops. Non-ASan multi-seed confirmation: seeds
  1 / 7 / 42 / 99 PASS at 1500 ops each. (The maintainer's re-gate includes the
  full 60-min soak, as at the original gate.)
- **CI (PR head `9a0634a`):**
  [run 29308501284](https://github.com/Robomous/RoadMaker/actions/runs/29308501284)
  — all 14 jobs green incl. sanitizers, the seeded CI soak, and
  editor-visual-artifacts.
- **`scripts/gw1_replay.py`** @ `9a0634a`: all 7 steps PASS (side-snap T-attach
  → 1 junction/3 arms/6 connections; overpass 6.00 m clearance, **no new
  junction**; sidewalk lane; post-drag regen keeps 6 connections; undo×10/redo×10
  byte-identical; save→reload + glTF byte-identical); 0 errors, 0 warnings.

**Re-gate instruction (maintainer).** One GW-1/GW-2 run on final post-extension
`main` covers **both** v0.4.0 and v0.5.0 publication (findings-only extension per
the gate rules). The GW-1 spec now carries the selectable-everything and
single-junction-per-arm-set invariants and the on-road T/X-drop acceptance.
