# GW-6 — Scenarios end-to-end

*Accepts the P8 scenario pillar: the OpenSCENARIO data model (p8-s1), Map ↔
Scenario mode and actor placement (p8-s2), lane-anchored routes (p8-s3), the
storyboard/condition editor (p8-s4), esmini preview (p8-s5), and the
export-only OpenSCENARIO 2.x subset (p8-s6).*

**Status: draft, written before the pillar starts** — steps are refined as the
owning sprints land. It exists early on purpose: GW-1…GW-5 give their pillars an
acceptance target to build against, and without this document #248's headline
acceptance (*"a traffic-light scenario is authorable end-to-end"*) would ship
before the workflow that verifies it. Drafted as part of the
[P8 discovery](../pillars/p8_discovery.md) planning step (§7), which the
maintainer ruled on 2026-07-30.

## Purpose

Verify that a scenario can be authored on top of an existing road network —
actors placed, routes anchored to lanes, a storyboard with triggers — that it
survives edits to the network beneath it, that it round-trips through the
project container, and that an external simulator accepts the exported file.

**Validation note.** RoadMaker does not validate `.xosc` against an XSD: the
schema ships only in ASAM's gated bundle and cannot be carried in CI. By
[maintainer ruling](https://github.com/Robomous/RoadMaker/issues/257) esmini's
parser **is** the validator — so step 12 is not a nicety, it is the acceptance
of every export step above it.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- **GW-2's scene**, saved and reloadable — this workflow authors on top of it
  rather than building its own network, so a failure here is never a
  road-authoring failure.
- A junction in that scene that is **dynamically signalized** (GW-4 step 2), so
  the traffic-light half of step 8 has something to reference.
- `esmini` available on `PATH` (external tool, launched as a subprocess — never
  linked or bundled; see
  [dependencies](../../standards/dependencies.md)).

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

9. [ ] Open the **storyboard editor** in the 2D Editor pane. **Expected:** it
   hosts as a page beside the Signal Phase Editor and the profile editors, with
   the scenario's acts/events listed.
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
14. [ ] **Preview in esmini** from the editor. **Expected:** esmini launches as
    an external process, loads the exported scenario **without parser errors**,
    and the actors move along their routes; the cut-in happens; actor 1 waits
    for green. Closing esmini leaves the editor untouched.
15. [ ] Re-import / re-open the saved project and confirm nothing was lost in
    the `.xosc` round trip that the project container should have preserved.
    **Expected:** anything RoadMaker cannot express in 1.x is still carried in
    the project's own layer rather than dropped (ADR-0008 Layer 2).
16. [ ] Export the **OpenSCENARIO 2.x** concrete-scenario subset (p8-s6).
    **Expected:** the documented subset emits; anything outside it is reported
    rather than silently omitted. *(Export-only — there is no 2.x import.)*

## Pass criteria

- Every step's expected result holds; zero crashes.
- **A route survives an edit to the road beneath it** (step 7) and is
  *diagnosed*, never silently mutated, when genuinely invalidated (step 8).
- **esmini loads the exported `.xosc` without parser errors** (step 14) — the
  standing substitute for schema validation.
- Save → reopen → save is byte-identical (step 12).
- A traffic-light condition exported from RoadMaker references a controller
  that exists in the exported `.xodr`, by the same id.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
