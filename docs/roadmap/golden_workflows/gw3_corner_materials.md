# GW-3 — Corner reshaping & materials

*Accepts the P4 Corner tool and the P6 material-assignment interaction on
junction corners.*

**Status: draft** — steps are refined as the owning pillar sprints land.

## Purpose

Verify corner selection and reshaping via control vertices and extents,
per-corner attributes, junction-wide attributes, and material assignment
by dragging from the Library Browser onto Attributes-pane slots.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- A scene with a four-arm junction with sidewalks and a median on at least
  one approach.
- The starter material library (incl. distinct sidewalk, median, and
  carriageway materials).

## Steps

1. [ ] Activate the Corner tool and click a junction corner.
   **Expected:** the corner highlights (selection tint), showing its
   control vertices as distinct points and its extents as dashed lines.
2. [ ] Hover a control vertex. **Expected:** hover feedback (highlight)
   before any click.
3. [ ] Drag a control vertex. **Expected:** the corner reshapes live; the
   junction surface and markings rebuild on release as one undo step.
4. [ ] Drag an extent. **Expected:** the corner's reach along the
   approach road changes; geometry stays watertight.
5. [ ] With the corner selected, set **Corner Radius** in the Attributes
   pane (typed value and scrub-drag on the attribute name). **Expected:**
   both input paths work; the corner rebuilds.
6. [ ] Drag a material from the Library onto the corner's **Sidewalk
   Material** slot image in the Attributes pane. **Expected:** the
   Library auto-navigates to Materials when the slot is engaged; the
   sidewalk of that corner re-renders with the material.
7. [ ] Drag a material onto the corner's **Median Material** slot.
   **Expected:** the median of that approach re-renders.
8. [ ] Select the junction itself (not a corner). **Expected:** the
   Attributes pane shows junction-wide attributes: Corner Radius (all
   corners) and Carriageway Material.
9. [ ] Set the junction-wide Corner Radius. **Expected:** all corners
   rebuild to the new radius; per-corner overrides are re-applicable
   afterwards.
10. [ ] Drag a material onto **Carriageway Material**. **Expected:** the
    junction's paved surface re-renders with it.
11. [ ] Undo all steps back to the start. **Expected:** each step reverts
    in order; the final state matches the initial scene; save/reload
    round-trips the materials.

## Pass criteria

- Every step's expected result holds; zero crashes.
- Each reshape/assignment is exactly one undo step.
- Materials persist through save/reload. Lane-scope materials appear in the
  OpenDRIVE export as standard `<material>` data. **Corner- and
  junction-scope materials have no ASAM carrier** — OpenDRIVE 1.9.0 §12.10
  gives `<junction>`/`<boundary>` no material or corner-radius attribute — so
  they route through `<userData>` (`rm:corners` fields `sw=`/`md=`, and
  `rm:junction` `mat=`), the route sanctioned for exactly this case in
  `docs/design/materials-structures/04_phases.md` WS-2 ("Centre lane /
  junction floor route through `<userData>`"). Save → reload → save is
  byte-identical either way.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
