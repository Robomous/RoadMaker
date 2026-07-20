# GW-4 — Traffic signals at junctions

*Accepts the P4 Signal tool: auto-signalization with templates, linked
signal props, and the Signal Phase Editor in the 2D Editor pane.*

**Status: draft** — steps are refined as the owning pillar sprints land.

## Purpose

Verify that a junction can be signalized automatically from a template,
that physical signal assets are placed and linked to logical signals, and
that signal phases are editable on a timeline with per-phase maneuver
highlighting.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- A scene with a four-arm junction with maneuver roads (p4-s6) and the
  starter signal/prop assembly assets.

## Steps

1. [ ] Activate the Signal tool and select the junction. **Expected:**
   the Attributes pane offers signalization controls, including **Auto
   Signalize** with a template list.
2. [ ] Apply a four-way **protected-left** template. **Expected:** signal
   heads (prop assemblies) are placed automatically at each approach and
   each is **auto-linked** to a logical signal; the junction is marked
   dynamically signalized.
3. [ ] Verify the static alternative: on a copy of the junction, apply a
   static template (stop signs). **Expected:** stop-sign props place at
   the approaches; no phase data is created.
4. [ ] Open the **Signal Phase Editor** for the dynamic junction.
   **Expected:** it opens in the 2D Editor pane, showing phases with
   red–yellow–green intervals on a timeline bar.
5. [ ] Scrub the timeline. **Expected:** the viewport signal heads change
   state in sync with the scrubbed time.
6. [ ] Select each phase in turn. **Expected:** the roads/maneuvers that
   may move in that phase highlight in the viewport.
7. [ ] Inspect the junction gates. **Expected:** dotted links connect
   each signal to the junction movements it gates.
8. [ ] Right-click in the phase list: **add** a phase, then **duplicate**
   one. **Expected:** both appear with editable intervals.
9. [ ] Delete a phase with the Delete key. **Expected:** it is removed;
   the timeline re-flows; undo restores it.
10. [ ] Use Next/Previous Phase navigation. **Expected:** the selection
    steps through phases in order, updating highlighting each time.
11. [ ] Save, reload, and re-open the Signal Phase Editor. **Expected:**
    templates, links, phases, and intervals round-trip; the exported
    `.xodr` carries the `<signal>` data and validates.

## Pass criteria

- Every step's expected result holds; zero crashes.
- At least one template produces a working protected-left arrangement.
- Every placed signal prop is linked to a logical signal (no orphans).

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
