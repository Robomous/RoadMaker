# GW-4 — Traffic signals at junctions

*Accepts the P4 Signal tool: auto-signalization with templates, linked
signal props, and the Signal Phase Editor in the 2D Editor pane — plus the
Sign tool and editable sign-face text (p4-s9).*

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
   dynamically signalized. Every head **faces its own approach** — fly down
   each arm toward the junction and the lenses look back at you (p6-s14,
   #416).
3. [ ] Verify the static alternative: on a copy of the junction, apply a
   static template (stop signs). **Expected:** stop-sign props place at
   the approaches, each facing its approaching traffic; no phase data is
   created.
4. [ ] Open the **Signal Phase Editor** for the dynamic junction — via
   **⇧G**, by activating the Signal tool, or the junction's right-click
   **Signal Phases…**. **Expected:** it opens as a page in the 2D Editor
   pane, with one row per signal controller and phase columns whose widths
   track their durations, coloured red–yellow–green (an all-red clearance
   phase shows red across every row).
5. [ ] Scrub the timeline (drag the playhead). **Expected:** the viewport
   signal heads change state in sync with the scrubbed time.
6. [ ] Select each phase in turn. **Expected:** the connecting roads that
   may move in that phase brighten in the viewport.
7. [ ] Inspect the junction gates. **Expected:** dotted links connect the
   phase's green heads to the junction movements they gate, tracking the
   active phase as the timeline is scrubbed; they show without the Signal
   tool active.
8. [ ] Right-click in the phase list: **add** a phase, then **duplicate**
   one. **Expected:** both appear with editable intervals.
9. [ ] Delete a phase with the Delete key. **Expected:** it is removed;
   the timeline re-flows; undo restores it.
10. [ ] Use Next/Previous Phase navigation. **Expected:** the selection
    steps through phases in order, updating highlighting each time.
11. [ ] Save the file, reload it, and re-select the junction with the
    Signal tool. **Expected:** the applied template and the signal↔mount
    links round-trip (the Signalization pane shows the same template); the
    exported `.xodr` carries the `<signal>` and `<controller>` data and
    validates. *(Authored phase timing round-trips too, as of p4-s8: the
    cycle rides `<userData code="rm:phases">` and survives save→reload→save
    byte-identically; a junction still on its derived default cycle writes
    no `rm:phases` at all.)*
12. [ ] Activate the **Sign tool** (**B**) and click a road away from the
    junction, **on the right-hand side**. **Expected:** a street-name blade
    places on the road and is selected (added as of p4-s9, #230; the pack
    default moved to the US catalogue in p6-s12, #414). It **faces the
    traffic coming toward it**, not along the road (p6-s14, #416). Edit its
    **Text** row in the Attributes pane to a two-line name (e.g. `MAIN ST` /
    `W 4TH`). **Expected:** the plate shows the text in the viewport; the
    edit is one undo step.
12b. [ ] Click again on the **left-hand** side of the same road.
    **Expected:** the second sign faces the opposite way — it is aimed at the
    traffic on that side. Now scrub the first sign's **Heading offset** to an
    obviously wrong angle, then move that sign along the road with **s**.
    **Expected:** the hand-set heading is **kept** — nothing re-derives it.
    Press **Auto facing** in the Attributes pane. **Expected:** it returns to
    facing its traffic, in one undo step, and **Ctrl+Z** brings the hand-set
    heading back.
12c. [ ] With that sign selected, switch to the **Move** tool. **Expected:**
    the gizmo appears **at the sign**, not out at the road's midpoint, and it
    has no Z arrow (p6-s15, #417). Drag the **yaw ring**. **Expected:** the
    sign turns and the road underneath it does **not** move at all; the
    Heading offset row follows the drag and lands on a 15° multiple; the
    Attributes pane's **Applies to** row still reports the same traffic
    direction — turning a sign never changes which traffic it applies to
    (p6-s16, #418, which added the row this step checks). Now move the sign
    with the centre pad. **Expected:** it slides along the road with the
    dragged heading untouched, and **Auto facing** is still the only way back.
    Finally, click into another field without typing anything. **Expected:**
    the undo history does **not** grow and the Heading offset keeps the exact
    angle the drag produced — leaving a field is not an edit (p6-s16, #418).
12d. [ ] With the sign still selected, drag the **Mounting height** label and
    then type an exact value (p6-s16, #418). **Expected:** the sign rises and
    falls on its post, each gesture is one undo step, and the **Applies to**
    row and Heading offset are untouched — raising a sign never re-aims it.
    A negative value is accepted (for a sign hung below the reference line).
12e. [ ] Place a **Speed Limit (R2-1)** sign, note its position and raise its
    mounting height, then change its **Designation** to **Stop (R1-1)** (#429).
    **Expected:** ONE undo step; the model in the viewport becomes an octagon
    where the plate stood — same position, same mounting height, same facing,
    same Text; the **Speed limit** row disappears with the posted value it
    carried, and **Face width** becomes 0.75 m. Undo. **Expected:** the speed
    plate returns complete, posted value and all. Now edit **Face height**
    alone. **Expected:** one undo step, and **Face length** stays **not set** —
    editing one dimension never invents another. Finally step **Face width**
    below its minimum until it reads **not set**. **Expected:** one undo step,
    and the saved `.xodr`'s `<signal>` carries no `@width` at all rather than
    `width="0"`. Select a sign placed from a *left* **One Way (R6-1)** and check
    the viewport. **Expected:** it draws the LEFT arrow and the Designation row
    names the left variant (before #429 both variants drew the right arrow).
13. [ ] Save, reload, and re-select the sign. **Expected:** the face text
    round-trips (the Text row shows the same multi-line value; the exported
    `.xodr` carries `@text`), and so does the facing — including the hand-set
    heading from step 12b, which the file carries as `@orientation` plus
    `@hOffset` and which reload must not recompute. The designation from step
    12e and its face size round-trip too (`@type`/`@subtype`/`@country` and the
    declared dimensions come back exactly, and a dimension left **not set**
    comes back not set). Export glTF (`.glb`) and open it. **Expected:**
    the sign's face texture carries the text. *(USD export keeps the flat
    plate — single-file USDA cannot embed textures, #364 — but the `@text` is
    still written.)*

## Pass criteria

- Every step's expected result holds; zero crashes.
- At least one template produces a working protected-left arrangement.
- Every placed signal prop is linked to a logical signal (no orphans).
- Every sign and signal head — placed by hand or by a template — faces the
  traffic it governs, and a hand-set heading survives every later edit and a
  save/reload round trip.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
