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

- [ ] CI green on `main` (all platforms, sanitizers).
- [ ] 60-minute local ASan soak clean — seed(s) and op count recorded below.
- [ ] GW-1 steps replayed via the soak driver where automatable.

| Pre-flight item | Result / evidence |
|---|---|
| CI run | |
| 60-min soak (seed, ops, result) | |
| GW-1 automatable replay | |

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
