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
| CI run | ✅ re-stamped after the tee visual rework (#106): PR head `b859720` (= merged `e0e3052`) — [run 29212417672](https://github.com/Robomous/RoadMaker/actions/runs/29212417672), all 14 jobs green incl. sanitizers, the seeded CI soak, and the new editor-visual-artifacts job (2026-07-12). Original evidence: `main` @ `1be3e20` — [run 29174615502](https://github.com/Robomous/RoadMaker/actions/runs/29174615502) (2026-07-11). |
| 60-min soak (seed, ops, result) | ✅ **PASS** — re-stamped after the tee visual rework: seed 421338, 22 068 ops in 60 min (13 907 commands, 4 644 previews, 6 696 undo / 3 359 redo, 1 007 saves, 2 464 rejected); final 2 274-road network save→reload clean (0 diagnostics). ASan+UBSan build @ `89dae11` (kernel/editor-lib identical to merged `e0e3052`; later commits touched only tests and the editor CLI entry point, which the soak driver does not link), macOS (`ASAN_OPTIONS=detect_leaks=0` — LSan is Linux-only; leak checking runs in the Linux CI soak). 2026-07-12. Original: seed 421337, 13 718 ops @ `1be3e20` (2026-07-11). |
| GW-1 automatable replay | ✅ re-stamped after the tee visual rework: `python3 scripts/gw1_replay.py` @ `6be6db1` (kernel identical to merged `e0e3052`) — all 7 steps PASS via the kernel command layer (side-snap T-attach, 6.0 m overpass clearance, sidewalk lane, post-drag junction regen, undo×10/redo×10 byte-identical, save→reload→save byte-identical + glTF); diagnostics at end: 0 errors, 0 warnings (2026-07-12). Original run @ `1be3e20` (2026-07-11). |

## GW-1 — First network ([spec](gw1_first_network.md))

- Date / build (commit):
- Start time: — End time: — **Within 20 min budget:** yes / no
- **Crashes:** (count; report paths; issue links)
- **Diagnostics at end:** errors — / warnings —
- esmini load: pass / fail (output tail below)

| Step | Done | Notes |
|---|---|---|
| 1. Two-lane road, 4+ waypoints, curves | | |
| 2. T against road 1 (side attach) | | |
| 3. Overpass, clearance ≥ 5 m | | |
| 4. Sidewalk lane added | | |
| 5. Drag junction incoming node → regen | | |
| 6. Undo ×10 / redo ×10 identical | | |
| 7. Save/reload; glTF + USD; esmini | | |

## GW-2 — Recover from crash ([spec](gw2_recover_from_crash.md))

- Date / build (commit):
- Autosave interval: — Kill → recovered-autosave delta:
- **Work lost (user terms):**
- Recovery offered unprompted: yes / no
- **Result:** recovered / failed

## Verdict

- [ ] **GO** — M3a (v0.5.0) opens.
- [ ] **NO-GO** — sprint extends; crashes filed:

Maintainer signature / date:
