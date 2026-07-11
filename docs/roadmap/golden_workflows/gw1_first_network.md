# GW-1 — First network

*The hardening sprint's gate workflow: everything a new user plausibly
tries in their first twenty minutes, executed by hand, with zero crashes.*

- **Budget:** ≤ 20 minutes wall clock, one sitting.
- **Pass:** zero crashes, zero validation **errors** in the Diagnostics
  panel at the end, all steps completed.
- **Environment:** a release-configuration build of the editor on the
  maintainer's machine; a local [esmini](https://github.com/esmini/esmini)
  install for step 7.

## Action script

1. **New project** → Create Road: author a two-lane road (one driving lane
   each side) with **4+ waypoints** including visible curvature.
2. **T-junction by side attach**: create a second road and connect it in a
   **T against the first** — the endpoint attaches to the *side* of road 1
   (side-snap indicator on road 1's reference line, release commits the
   split + junction as one undo step).
3. **Overpass**: create a third road **crossing road 1 with no junction**
   — take the cross **over** option; resulting clearance ≥ 5 m over the
   crossed road.
4. **Lane profile edit**: add a sidewalk lane on one side of one road.
5. **Junction-adjacent drag**: with the Edit Nodes tool, drag a node of
   one of the T-junction's incoming roads — the junction regenerates and
   its connecting roads follow.
6. **Undo ×10, then redo ×10** — the network is identical afterwards
   (visual check + save-compare: saving before and after produces the
   same `.xodr`).
7. **Persistence + interchange**: save; reload the file; export glTF and
   USD; export `.xodr` and load it in **esmini** (manual, local) without
   errors.

## Evidence to record (in the gate document)

- Start/end time; per-step notes where anything felt off.
- Diagnostics panel error/warning counts at the end.
- Paths of any crash reports produced (each becomes a `crash`-labeled
  issue; any crash = NO-GO for this gate).
- The esmini invocation and its output tail.
