# Gate extension — GW-1/GW-2 NO-GO (2026-07-13)

*Scope-defining design note for the gate-extension sprint. Tracks the
findings-only extension the gate rules mandate; the as-built root causes and
re-gate evidence are filled in as each workstream lands (final report at the
bottom, WS-4).*

- **Epic:** [#147](https://github.com/Robomous/RoadMaker/issues/147)
- **Verdict record:** [`../../roadmap/golden_workflows/gate-v0.4.0.md`](../../roadmap/golden_workflows/gate-v0.4.0.md)
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
