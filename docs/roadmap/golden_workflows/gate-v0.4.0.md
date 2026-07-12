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
| CI run | ✅ `main` @ `1be3e20` — [run 29174615502](https://github.com/Robomous/RoadMaker/actions/runs/29174615502), all jobs green incl. sanitizers + the seeded CI soak (2026-07-11). |
| 60-min soak (seed, ops, result) | ✅ **PASS** — seed 421337, 13 718 ops in 60 min (9 155 commands, 3 166 previews, 4 150 undo / 2 011 redo, 631 saves, 885 rejected); final 1 436-road network save→reload clean (0 diagnostics). ASan build @ `1be3e20`, macOS (`ASAN_OPTIONS=detect_leaks=0` — LSan is Linux-only; leak checking runs in the Linux CI soak). 2026-07-11. |
| GW-1 automatable replay | ✅ `python3 scripts/gw1_replay.py` @ `1be3e20` — all 7 steps PASS via the kernel command layer (side-snap T-attach, 6.0 m overpass clearance, sidewalk lane, post-drag junction regen, undo×10/redo×10 byte-identical, save→reload→save byte-identical + glTF); diagnostics at end: 0 errors, 0 warnings (2026-07-11). |

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
