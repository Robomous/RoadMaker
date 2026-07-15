# GW-1 — First network

*The hardening sprint's gate workflow: everything a new user plausibly
tries in their first twenty minutes, executed by hand, with zero crashes.*

- **Budget:** ≤ 20 minutes wall clock, one sitting.
- **Pass:** zero crashes, zero validation **errors** in the Diagnostics
  panel at the end, all steps completed. Two structural invariants added by
  the gate extension (2026-07-13 NO-GO → epic #147) must also hold throughout:
  - **Selectable-everything:** every rendered surface is selectable — including
    a junction's blended floor (click it in the viewport or the Junctions scene
    tree; the properties panel shows its arm/connection counts). No visible
    "junction-like area" that cannot be selected.
  - **Single junction per arm-set:** a set of road ends owns at most one
    junction. Re-generating over the same ends regenerates in place (never a
    superimposed duplicate); an overpass creates **no** junction at the crossing.
- **Environment:** a release-configuration build of the editor on the
  maintainer's machine; a local [esmini](https://github.com/esmini/esmini)
  install for step 7.

## Action script

1. **New project** → Create Road: author a two-lane road (one driving lane
   each side) with **4+ waypoints** including visible curvature.
2. **T-junction by side attach**: create a second road and connect it in a
   **T against the first** — the endpoint attaches to the *side* of road 1
   (side-snap indicator on road 1's reference line, release commits the
   split + junction as one undo step). **Acceptance:** the junction area
   renders correctly — one continuous surface, no sliver triangles,
   no z-fighting, no road marks running through the interior — the exported
   `.xodr` shows the same smooth turn geometry in esmini (issue #103
   regression), and clicking the junction **floor** selects the junction
   (gate finding 4). Also drag a **T/X assembly from the Library ONTO a road**:
   it aligns to the road tangent and attaches (split + junction as one command),
   never a floating superimposed junction (gate finding 1).
3. **Overpass**: create a third road **crossing road 1 with no junction**
   — take the cross **over** option; resulting clearance ≥ 5 m over the
   crossed road. **No junction is created at the crossing** (gate finding 4);
   deleting the crossed road afterwards leaves the overpass road intact.
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
