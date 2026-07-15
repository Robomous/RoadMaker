# GW-2 — Simple scene end-to-end

*The backbone workflow: build a complete small scene from an empty project
through both export previews. Touches P1–P7.*

**Status: draft** — steps are refined as the owning pillar sprints land.

## Purpose

Verify that a new user can author a believable scene end-to-end: project
and scene management, road planning with automatic junctions, elevation
and bridges, corners, crosswalks, road styles, a carved turn lane with
markings, props by all four techniques, and export previews.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- The bundled starter asset library (road styles, marking assets, arrow
  stencils, crosswalk asset, prop assets, materials).
- No open project.

## Steps

### Project and roads

1. [ ] Create a new project, then a new scene inside it. **Expected:** a
   project is a folder of shared assets; the scene lives in the project;
   both appear on the welcome screen's recent list on next launch.
2. [ ] With the Road Plan tool, click control points to lay a straight
   road; right-click to finish. **Expected:** a road with the default
   style; control points remain editable.
3. [ ] Draw a second road crossing the first. **Expected:** a junction is
   created automatically at the overlap, with connecting lanes.
4. [ ] Draw a curved road, then extend one of its endpoints by clicking
   beyond it. **Expected:** the extension fits with geometric-constraint
   continuity (no tangent kink at the join).
5. [ ] Draw roads enclosing an area. **Expected:** the enclosed area
   auto-forms a ground surface.
6. [ ] Open the Surface tool on that surface. **Expected:** the surface
   is a node graph; nodes and tangents are editable.

### Elevation and bridges

7. [ ] Select a road, open its elevation profile in the 2D Editor pane,
   and raise a span over the crossing road. **Expected:** the profile
   edits as a 2D curve; the 3D road follows; terrain follows the road.
8. [ ] Run the Road Construction tool's automatic bridge assignment on
   the raised span. **Expected:** the elevated span becomes a bridge with
   sensible span limits; a span-inflation control widens/narrows the
   bridged extent.

### Junction corner and crosswalk

9. [ ] With the Corner tool, select a junction corner and set its radius
   from 5 m to 10 m in the Attributes pane. **Expected:** the corner
   rebuilds smoothly; dragging horizontally on the attribute *name*
   scrubs the value.
10. [ ] Drag the crosswalk asset from the Library onto a junction road
    end. **Expected:** a crosswalk with stop line places along the lane
    cross-section; a chevron affordance indicates placement while
    dragging.

### Road styles

11. [ ] Drag a different road style from the Library onto an existing
    road. **Expected:** the style applies; previously edited attributes
    (e.g. the elevation profile, lane count edits) are preserved.

### Turn lane, markings, stencils

12. [ ] With the Lane Carve tool, drag along a lane approaching the
    junction. **Expected:** a tapering cut creates a turn lane.
13. [ ] Drag a solid-single-white marking asset onto the new lane's outer
    boundary, and a solid-double-yellow onto the road center. **Expected:**
    the boundary markings change accordingly.
14. [ ] Drag a turn-arrow stencil into the turn lane and adjust it with
    the Marking Point tool. **Expected:** the arrow sits flat on the lane
    surface and is positionable along/across the lane.
15. [ ] Drag a worn marking material from the Library onto the arrow's
    material slot in the Attributes pane. **Expected:** the stencil's
    material swaps; only that instance changes.

### Props

16. [ ] Prop Point: drag a single prop from the Library into the scene.
    **Expected:** it lands under the cursor and is movable.
17. [ ] Prop Curve: lay props along a curve, then **Bake**. **Expected:**
    instances distribute along the curve; baking converts them to
    individually editable props.
18. [ ] Prop Span: attach a repeating prop along a road span. **Expected:**
    props follow the road between the span's ends.
19. [ ] Prop Polygon: fill a region with a prop, adjust **Density**, then
    **Randomize**. **Expected:** the fill re-distributes accordingly.
20. [ ] Create a **Prop Set** of at least two assets with portions, and
    use it with Prop Polygon. **Expected:** the mix respects the
    configured portions.

### Export previews

21. [ ] Open the Scene Export Preview tool. **Expected:** a preview of
    the exported 3D scene (meshes/materials), without writing files.
22. [ ] Open the OpenDRIVE Export Preview tool. **Expected:** a preview
    of the OpenDRIVE data (reference lines, lanes, junctions) as it will
    export; diagnostics are visible.
23. [ ] Save, close, and reopen the scene. **Expected:** everything above
    round-trips.

## Pass criteria

- All steps complete in order in a single session; every expected result
  holds.
- Zero crashes; undo/redo works after each authoring step.
- The exported `.xodr` validates with zero errors and loads in esmini.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
