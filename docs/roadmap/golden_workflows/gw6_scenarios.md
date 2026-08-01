# GW-6 — Scenarios end-to-end

*Accepts the P8 scenario pillar: the OpenSCENARIO data model (p8-s1), Map ↔
Scenario mode and actor placement (p8-s2), lane-anchored routes (p8-s3), the
storyboard/condition editor (p8-s4), esmini preview (p8-s5), and the
export-only OpenSCENARIO 2.x subset (p8-s6).*

**Status: refined against what shipped (p8-s5 and p8-s6, 2026-08-01); hand-runs
outstanding.** It was drafted before the pillar started — GW-1…GW-5 give their
pillars an acceptance target to build against, and without this document #248's
headline acceptance (*"a traffic-light scenario is authorable end-to-end"*)
would have shipped before the workflow that verifies it. Drafted as part of the
[P8 discovery](../pillars/p8_discovery.md) planning step (§7), which the
maintainer ruled on 2026-07-30; **p8-s1…p8-s6 have all landed** and every step
below now names the thing that actually exists.

## Purpose

Verify that a scenario can be authored on top of an existing road network —
actors placed, routes anchored to lanes, a storyboard with triggers — that it
survives edits to the network beneath it, that it round-trips through the
project container, and that an external simulator accepts the exported file.

**Validation note — and the correction measurement forced on it.** RoadMaker
does not validate `.xosc` against an XSD: the schema ships only in ASAM's gated
bundle and cannot be carried in CI. By
[maintainer ruling](https://github.com/Robomous/RoadMaker/issues/257) esmini's
parser **is** the validator.

★ THAT RULING HOLDS FOR HALF OF A SCENARIO, AND THE OTHER HALF HAS ITS OWN
CHECKER. Measured on the pinned esmini v3.5.0 (2026-07-30 and 2026-08-01, one
mutation at a time on the tracked fixtures, recorded in
`.github/workflows/ci.yml`):

| reference | esmini |
|---|---|
| a dangling `roadId` / `laneId`, an `s` past the road end | **fails the load** |
| a dangling `entityRef` (actor, triggering entity, relative target lane) | **fails the load** |
| an invalid `Event/@priority`, an invalid `@dynamicsShape` | **fails the load** |
| a dangling `trafficSignalId` | *loads, exit 0, no error* |
| a dangling `trafficSignalControllerRef` | *loads, exit 0, no error* |
| a `@phase` naming no phase | *loads, exit 0, no error* |
| a garbage `TrafficSignalState/@state` | *loads, exit 0, no error* |

So **step 14 accepts the lane-anchored half** — the cut-in, the routes, the
placements — and says nothing at all about the traffic-light half. The
traffic-light half is accepted by RoadMaker's own
`osc::validate_scenario_against_network`
([#533](https://github.com/Robomous/RoadMaker/issues/533)), surfaced live in the
Diagnostics dock, and step 11a below is where a runner reads it. Neither check
replaces the other and neither alone discharges this workflow.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- **GW-2's scene**, saved and reloadable — this workflow authors on top of it
  rather than building its own network, so a failure here is never a
  road-authoring failure.
- A junction in that scene that is **dynamically signalized** (GW-4 step 2), so
  the traffic-light half of step 8 has something to reference.
- `esmini` available on `PATH`, or its path set once through the file dialog
  **File ▸ Preview Scenario in esmini…** offers when it cannot find one
  (external tool, launched as a subprocess — never linked or bundled; see
  [dependencies](../../standards/dependencies.md)). `$ESMINI_PATH` also
  resolves, which is how a CI-like machine points at a fetched binary without
  touching user settings.

## Steps

### Mode and actors (p8-s2)

1. [ ] Open GW-2's scene and switch to **Scenario** mode (View ▸ Scenario
   Mode). **Expected:** the Actor tool becomes available and every road-editing
   tool is disabled, while the road network stays visible and
   selectable-for-reference. The map document is **not** closed — switching back
   to Map mode returns to it with the undo history intact.
   *Wording amended in `p8-s2`*: this step originally said "the scenario toolbar
   tab activates". There are no toolbar tabs — [#377](https://github.com/Robomous/RoadMaker/issues/377)
   replaced them with a single flat tool row — so the mode enables and disables
   actions in place. The gate is derived from each tool's registry
   classification, so a tool added by a later pillar is gated the day it lands.
2. [ ] Place a **vehicle actor** on a driving lane. **Expected:** it snaps to
   the lane centre and is stored as a `<LanePosition>` (roadId/laneId/s/offset),
   so it follows the road through later edits; a drop that is not on a driving
   lane is refused with a hint **while hovering**, not after the click. The two
   refusals are distinct — *no road in reach* and *this road has no driving lane
   here* are different problems.
3. [ ] Place a second vehicle actor and a **pedestrian actor**. **Expected:**
   each is drawn as a box proxy sized from its `<BoundingBox>`, oriented along
   its lane's direction of travel. Each also appears under the scene tree's
   **Scenario** branch, and selecting one fills the Attributes pane with its
   name, category, lane anchor (road / lane / s / offset), bounding box and
   initial speed — every field editable, each edit one undo step.
   *Note*: props and signals are not in the scene tree at all today, so this
   step's original "distinct from props" phrasing had nothing to contrast
   against; the Scenario branch is the contrast it was reaching for.
4. [ ] Select an actor and confirm the selection model treats it as its own
   entity kind. **Expected:** selecting an actor does not select the road
   beneath it — an actor selection carries no road id at all. This holds through
   all three routes to a selection: the **Select tool** in the viewport, the
   **scene tree**, and the **Actor tool** (which selects a placed actor rather
   than stacking a second one on it). **Delete** on an actor selection removes
   it and its `<Init>` entry as one undo step, and a mixed road+actor selection
   deletes as one step too.
   *Note*: actors are not in the `NetworkMesh`, so the viewport's ray-cast pick
   passes straight through them to the road below. The Select tool consults the
   actor hit test **first** — the same one the Actor tool grabs with, so the two
   can never disagree about what is under the cursor.

### Routes (p8-s3)

5. [ ] Give actor 1 a **route** through the junction — pick a start lane and a
   destination lane on a different arm. **Expected:** the route draws as a
   lane-anchored path through the junction's maneuvers, not a free polyline.
6. [ ] Drag a waypoint of the route. **Expected:** it re-anchors to the nearest
   lane at that station; the path re-solves; one undo restores it.
7. [ ] **Edit the network beneath the route**: move one of the junction's arm
   roads a few metres (Map mode), then return to Scenario mode. **Expected:**
   the route **follows** its lanes rather than staying behind in world space.
   ★ This is the step that distinguishes a lane-anchored route from a polyline
   that merely looked right when it was drawn.
8. [ ] Now make an edit that **invalidates** the route — delete a lane the route
   traverses. **Expected:** the route is reported as invalidated with a
   diagnostic naming it; it is **not** silently deleted and **not** silently
   re-routed. Undo restores both the lane and the route's validity.

### Storyboard (p8-s4)

9. [ ] Open the **Storyboard** page in the 2D Editor pane. **Expected:** it
   hosts beside the Signal Phase Editor and the profile editors, showing the
   scenario as a tree — Story ▸ Act ▸ ManeuverGroup ▸ Maneuver ▸ Event ▸ Action
   — with a form for whatever is selected. **Add** creates a COMPLETE subtree
   (an act needs a maneuver group, which needs a maneuver, which needs an event,
   which needs an action), so the scenario is savable after every click;
   **Remove** cascades exactly as far as the schema requires and no further.
   *Landed in `p8-s4`*: every gesture is one command on the document's single
   undo stack.
10. [ ] Author a **cut-in**: give actor 2 an event that changes lane in front of
    actor 1, triggered by a relative-distance condition. **Expected:** the
    condition's parameters are editable and the event appears on the storyboard
    in order.
11. [ ] Author a **traffic-light condition**: gate actor 1's start on the
    signalized junction's controller reaching green. **Expected:** the
    controller is selectable **by its OpenDRIVE `@id`**, and the phase list
    offered is the one GW-4 authored. ★ The states offered must be the
    Red-filled dense list, not RoadMaker's sparse Red-by-omission storage —
    a phase the editor shows as "no state" would export as a signal that is
    never red.
    *Kernel support landed in `p8-s1`*: `osc::decompose_junction_signals` and
    `osc::edit::sync_traffic_signals` (`rm.osc.edit.*`) produce exactly that
    list, so this step's UI has one source to bind to and the Red-fill is not
    re-implemented in a widget.
    ★ *And the phase list is the SYNTHESIZED one (`p8-s4`)*: `Phase::name` may
    legally be empty and the writer synthesizes names into the output only, so
    a combo populated from the model would offer a label that matches nothing
    in the saved file. Every label the phase combo shows must appear verbatim
    as a `<Phase @name>` in the exported `.xosc` — check one.
11a. [ ] Look at the **Diagnostics** dock. **Expected:** it is EMPTY for the
    scenario just authored. Now break the traffic-light condition on purpose —
    retype the controller, then delete the signalized junction in Map mode — and
    the dock names the dangling reference, live, without a save.
    ★ This step exists because esmini cannot do it (`#533`): a dangling signal
    or controller reference loads with exit 0 and no error, so step 14 passing
    is not evidence the traffic-light half is right. Undo restores both the
    junction and an empty dock.

### Round trip and export (p8-s1, p8-s5, p8-s6)

12. [ ] **Save, close, reopen** the project. **Expected:** actors, routes and
    the storyboard all return; a second save is **byte-identical** to the
    first. (Same fingerprint discipline as `write_xodr` — see
    [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md).)
13. [ ] Export the scenario as **OpenSCENARIO 1.x** (`.xosc`) and inspect it.
    **Expected:** the actors appear as entities, the routes as routes/
    trajectories, the storyboard as acts/events with their triggers, and the
    signal condition references the controller by its OpenDRIVE id. The
    referenced road network is the scene's `.xodr` via `<LogicFile>`.
14. [ ] **File ▸ Preview Scenario in esmini…**. **Expected:** the scene and its
    scenario are exported to a throwaway folder — **including edits made since
    the last save**, and without writing anything into the project — and esmini
    launches as an external process, loads them **without parser errors**, and
    the actors move along their routes; the cut-in happens; actor 1 waits for
    green. Closing esmini leaves the editor untouched.
    *Landed in `p8-s5`*: the exported scenario's `<LogicFile>` is rewritten to
    the exported network's filename, so the preview shows what is on screen
    rather than the last save (or nothing, for a scene never saved). When the
    Diagnostics dock is non-empty the status bar says so **before** the launch —
    esmini will not.
15. [ ] Re-import / re-open the saved project and confirm nothing was lost in
    the `.xosc` round trip that the project container should have preserved.
    **Expected:** anything RoadMaker cannot express in 1.x is still carried in
    the project's own layer rather than dropped (ADR-0008 Layer 2).
16. [ ] **File ▸ Export OpenSCENARIO 2.x…** (p8-s6). **Expected:** the
    documented subset emits as an `.osc` file, and anything outside it is
    **reported before the file is written** rather than silently omitted — the
    dialog lists what the subset cannot carry and the export continues only if
    you say so. *(Export-only — there is no 2.x import, and the File menu has no
    Import twin for it.)*
    *Landed in `p8-s6`*: the target is ASAM OpenSCENARIO **DSL v2.2.0**, a
    different LANGUAGE from the 1.x XML rather than another serialization of it.
    Read the result against the table in
    [docs/domain/openscenario.md](../../domain/openscenario.md) — that table is
    the acceptance, and a gtest keeps it in step with the emitter, so reviewing
    against it is reviewing against what the code actually does.

## Pass criteria

- Every step's expected result holds; zero crashes.
- **A route survives an edit to the road beneath it** (step 7) and is
  *diagnosed*, never silently mutated, when genuinely invalidated (step 8).
- **The 2.x export names what it cannot carry** (step 16) — a lossy view is
  acceptable, a silent one is not.
- **esmini loads the exported `.xosc` without parser errors** (step 14) — the
  standing substitute for schema validation, *for the lane-anchored half only*;
  see the validation note above.
- Save → reopen → save is byte-identical (step 12).
- A traffic-light condition exported from RoadMaker references a controller
  that exists in the exported `.xodr`, by the same id — **discharged by an
  automated check** (`osc::validate_scenario_against_network`, surfaced in the
  Diagnostics dock) rather than by a human reading the file, which is what
  step 11a exercises.

## Results

Three platform runs are required, one per supported OS. The rows are here
ready to fill; a run is recorded by the person who performed it.

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | macOS | — | — | not yet run |
| — | Linux | — | — | not yet run |
| — | Windows | — | — | not yet run |

Every step's sprint has now landed, so a run records all sixteen.
