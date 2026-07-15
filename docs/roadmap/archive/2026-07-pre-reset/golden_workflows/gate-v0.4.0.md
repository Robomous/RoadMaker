# Gate — v0.4.0 (hardening sprint)

*Filled in by the maintainer during the gate run. Criteria were fixed at
sprint start in the tracking epic
([#81](https://github.com/Robomous/RoadMaker/issues/81)) and are repeated
here verbatim.*

## Criteria (fixed)

- **GO to M3a:** GW-1 completes within budget with zero crashes; GW-2
  recovers; soak (60 min, ASan) clean.
- **NO-GO:** any crash during GW-1 → sprint extends with those crashes
  only; two consecutive failed gates → maintainer reconvenes on roadmap
  viability with the honest data.

## Pre-flight (agent, before the run)

- [x] CI green on `main` (all platforms, sanitizers).
- [x] 60-minute local ASan soak clean — seed(s) and op count recorded below.
- [x] GW-1 steps replayed where automatable — headless over the kernel
  command layer (`scripts/gw1_replay.py`; the editor-only UX aspects and
  esmini/USD remain in the maintainer's by-hand run below).

| Pre-flight item | Result / evidence |
|---|---|
| CI run | ✅ **re-stamped after the gate extension (epic #147)**: PR head `9a0634a` (final WS-4 commit) — [run 29308501284](https://github.com/Robomous/RoadMaker/actions/runs/29308501284), all 14 jobs green incl. sanitizers, the seeded CI soak, and editor-visual-artifacts. The doc-stamp commit on top touches only these gate docs (kernel/editor-lib identical). Prior: tee visual rework PR head `b859720` (= merged `e0e3052`) — [run 29212417672](https://github.com/Robomous/RoadMaker/actions/runs/29212417672) (2026-07-12); original `main` @ `1be3e20` — [run 29174615502](https://github.com/Robomous/RoadMaker/actions/runs/29174615502) (2026-07-11). |
| Soak (seed, ops, result) | ✅ **PASS — re-stamped after the gate extension**: ASan+UBSan, macOS (`ASAN_OPTIONS=detect_leaks=0` — LSan is Linux-only), build @ `9a0634a`, seed 20260713, **4000 ops** (2507 commands, 647 previews, 862 undo / 431 redo, 125 saves, 457 rejected) — zero sanitizer reports; every gate-finding op (`assembly_drop_on_road`, `overpass`, `delete_crossing_road`, `remove_lane`, `duplicate_junction_attempt`) and both new invariants (single-junction-per-arm-set, rendered→selectable) exercised. Non-ASan confirmation: seeds 1/7/42/99 PASS @ 1500 ops. **The maintainer's re-gate run includes the definitive 60-min soak** (as at the original gate: seed 421338, 22 068 ops in 60 min @ `89dae11`, 2026-07-12; original seed 421337, 13 718 ops @ `1be3e20`, 2026-07-11). |
| GW-1 automatable replay | ✅ **re-stamped after the gate extension**: `python3 scripts/gw1_replay.py` @ `9a0634a` — all 7 steps PASS via the kernel command layer (side-snap T-attach → 1 junction/3 arms/6 connections, 6.00 m overpass clearance with **no new junction**, sidewalk lane, post-drag junction regen keeps 6 connections, undo×10/redo×10 byte-identical, save→reload + glTF byte-identical); diagnostics at end: 0 errors, 0 warnings. Prior: @ `6be6db1` (2026-07-12); original @ `1be3e20` (2026-07-11). |

## GW-1 — First network ([spec](gw1_first_network.md))

- Date / build (commit): **2026-07-13** / `de5bd7c`
- Start time: — End time: — **Within 20 min budget:** no (blocked)
- **Crashes:** 1 — right-click **lane delete crashed the editor** (context-menu
  use-after-free; hard blocker). Filed under the gate-extension epic
  ([#147](https://github.com/Robomous/RoadMaker/issues/147),
  WS-1 [#148](https://github.com/Robomous/RoadMaker/issues/148), label `crash`).
- **Diagnostics at end:** run did not complete (blocked by the crash + topology
  desync findings below).
- esmini load: not reached.

Six first-hand findings (ground truth over any passing test), dated 2026-07-13.
Undo/redo and persistence (steps 6–7 as far as reached) behaved correctly.

| Step | Done | Notes |
|---|---|---|
| 1. Two-lane road, 4+ waypoints, curves | ✅ | OK. **Finding 3:** visible discontinuity at road-end adjacencies when curvature starts right at the joint — continuity is G1-only, no curvature (G2) continuity checked or enforced. |
| 2. T against road 1 (side attach) | ⚠️ | **Finding 1:** T/X assembly drops from the Library onto a road are superimposed / desynchronized — no projection onto the road, no tangent alignment, no attach (`library_drop` uses the raw cursor as junction center, heading 0). **Finding 5:** duplicate junctions can be superimposed (no "end already belongs to a junction" guard, no regenerate-in-place offer). |
| 3. Overpass, clearance ≥ 5 m | ⚠️ | **Finding 4:** overpass produced an unselectable junction-like area that corrupts on adjacent-road delete. |
| 4. Sidewalk lane added | ✅ | Add works. **Finding 6 (UX):** no lane-**removal** affordance discoverable. |
| 5. Drag junction incoming node → regen | ❌ | **Finding 2:** junction regen after node drag leaves the connecting roads out of sync (editor regen orchestrator skips junction regen while `dirty.topology` is set; regen never runs during the preview drag). |
| 6. Undo ×10 / redo ×10 identical | ✅ | Passed where reached. |
| 7. Save/reload; glTF + USD; esmini | ✅ | Persistence passed where reached. |
| — Lane delete (right-click) | 💥 | **Finding 6 (crash):** right-click lane delete **crashed the editor** — hard gate blocker. |

## GW-2 — Recover from crash ([spec](gw2_recover_from_crash.md))

- Date / build (commit):
- Autosave interval: — Kill → recovered-autosave delta:
- **Work lost (user terms):**
- Recovery offered unprompted: yes / no
- **Result:** recovered / failed

## Verdict

- [ ] **GO** — M3a (v0.5.0) opens.
- [x] **NO-GO** — sprint extends; crashes filed:
  [#148](https://github.com/Robomous/RoadMaker/issues/148) (lane-delete
  context-menu use-after-free, `crash`). This is the first gate failure under
  the golden-workflow acceptance process. Per the gate rules the sprint extends
  with **only** the six findings above and their root causes — tracked by the
  gate-extension epic [#147](https://github.com/Robomous/RoadMaker/issues/147)
  (WS-1 [#148](https://github.com/Robomous/RoadMaker/issues/148),
  WS-2 [#149](https://github.com/Robomous/RoadMaker/issues/149),
  WS-3 [#150](https://github.com/Robomous/RoadMaker/issues/150),
  WS-4 [#151](https://github.com/Robomous/RoadMaker/issues/151)).
  v0.4.0 **and** v0.5.0 stay unpublished; the maintainer **re-gates once** on
  post-extension `main` (that single run covers both releases). The pre-flight
  table above is re-stamped at the final extension commit (WS-4).

Maintainer signature / date: **NO-GO — maintainer, 2026-07-13**
