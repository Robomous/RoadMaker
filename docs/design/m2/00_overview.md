# M2 overview — interactive editing, junction surfaces, OpenUSD

Status: **as-built — shipped in v0.3.0** (all five phases merged to `main`;
this document was reconciled with the implementation in the M2 close-out,
issue #21). Baseline was v0.2.0 (Qt 6 Widgets editor, read-only viewer,
shared-lib kernel, Diagnostic rule-id extension merged in PR #2). Deviations
from the frozen design are recorded inline in the relevant section (search
"**As-built**"); the two substantive ones are the elevation fit (`02` §5)
and junction arm persistence (`02` §6).

M2 turns RoadMaker from a viewer into an editor: a user creates and edits a small
road network entirely inside `roadmaker-editor` (with undo/redo), junctions get
real 3D blended surfaces, and the network exports to valid OpenDRIVE plus OpenUSD —
CI-green on three platforms with sanitizers. Release target: **v0.3.0**.

## Document map

| Doc | Contents |
|---|---|
| `01_editing_framework.md` | Command/undo architecture, transactional kernel API, tool state machine, snapping, incremental re-mesh |
| `02_editing_tools.md` | Per-tool spec sheets (interaction, kernel API, undo, edge cases, test plan) |
| `03_junction_blending.md` | Junction surface geometry design, degenerate cases, ASAM alignment |
| `04_usd_export.md` | USD strategy spike results and decision record |
| `05_assets.md` | Icon/texture inventory, sources, licenses, pipeline |

## Scope

1. **Editing framework** — every mutation is a command (kernel-level delta, Qt
   `QUndoCommand` bridge onto the existing `Document::undo_stack()`), tool state
   machine with headless-testable tool controllers, kernel-side snapping queries,
   incremental re-mesh of dirty roads.
2. **Editing tools** — Select/Move, Create Road (+ lane-profile templates),
   Edit Nodes, Lane Profile editor, minimal Elevation editor, Create Junction,
   Delete, New/Save (`.xodr` is the project file — no proprietary format).
3. **Junction 3D surfaces** — watertight blended junction surface replacing the
   plan-view floors; exportable as OpenDRIVE ≥1.8 `<boundary>` + `<elevationGrid>`.
4. **OpenUSD export** — per the `04` decision record, behind `RM_BUILD_USD`.
5. **Slim OpenDRIVE fidelity** (maintainer decision, 2026-07-10): only what the
   tools force — version-explicit writer target (1.8.1 selectable; needed to emit
   junction boundary/elevation grid, both introduced in 1.8.0) and road-mark
   width editing with sensible defaults.

## Non-goals (explicit descopes)

Deferred to M3 (maintainer decision, 2026-07-10):

- Lane `<border>` support (widths only in M2).
- Multi-line road-mark geometry (solid solid renders as one strip until M3).
- Superelevation-aware lateral frame (dz/ds tilt) and superelevation *editing*.
- Full polynomial lane-width editing (M2 edits a constant width per lane).
- Sign/prop 3D assets (scouted in `05_assets.md`, nothing ships).
- GIS import, OpenSCENARIO, terrain — M3 per the standing roadmap.

## Phase map (implementation, after this plan is approved)

| Phase | Contents | Gate |
|---|---|---|
| 0 | Assets pipeline + toolbar icons; `EditorCommand` framework; transactional kernel API; incremental re-mesh | Node-drag prototype with undo on a sample network |
| 1 | Select/Move, Edit Nodes, Delete, Save | Edit-session round-trip: load → edit → save → reload → equality within `rm::tol` |
| 2 | Create Road + templates + snapping; Lane Profile editor; editable Properties panel | Author a two-road network from empty document |
| 3 | Elevation editor; Create Junction (topology + lane links, flat surface placeholder) | T-junction authored in-editor, valid on save |
| 4 | Junction 3D blending per `03` | Watertightness + continuity tests green on golden junctions |
| 5 | USD export per `04`; release workflow ships it; v0.3.0 checklist | v0.3.0 tagged on green CI |

One epic per phase (GitHub milestone **M2**, label `m2`); PRs ≤ ~500 lines where
feasible; screenshots/GIFs in PR descriptions for interactive features. Every
phase keeps the standing rules: GoogleTest/pytest with the code, kernel API
changes update `python/src/bindings.cpp` + an example in the same PR,
sanitizer run before merging geometry/parsing changes.

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Junction blending robustness** — degenerate footprints (near-parallel merges, tiny areas, self-overlap) break CDT or produce non-watertight seams | High | High | Degenerate-case table with a named test fixture each (`03` §6); watertightness + continuity invariant gtests gate the phase; flat-floor fallback path retained behind the same interface |
| 2 | **USD build weight** — Pixar OpenUSD would dominate CI time and violate the lightweight-deps posture | Medium | High | Spike-first decision (`04`); tinyusdz preferred; `RM_BUILD_USD=OFF` by default regardless; dedicated CI job if heavy |
| 3 | **Undo correctness under drag interactions** — preview mutations leaking into the stack, or stale generational IDs after undo of topology ops | Medium | High | Single-command-on-release rule (`01` §3); command round-trip equality tests against the deterministic writer; IDs never reused across undo (restore-in-place semantics, `01` §2.4) |
| 4 | Incremental re-mesh misses a dependency (junction not regenerated when incoming road edited) | Medium | Medium | Explicit dirty-propagation contract (`01` §5) + dependency tests |
| 5 | Editor-test blind spots for GUI flows (drag, rubber band) | Medium | Low | Tool controllers are headless (abstract events) — interaction logic testable without a real window; scripted QTest smoke considered in Phase 1 |

## Standards references

All standards behavior in these docs cites the local ASAM texts under
`.claude/references/asam/` (OpenDRIVE 1.9.0 primary, 1.8.1 for deltas). Rule IDs
appear inline as `asam.net:xodr:<ver>:<rule>`. Validator diagnostics must carry
these IDs via the `Diagnostic::rule_id` field added in PR #2.
