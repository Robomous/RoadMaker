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

**Amendments note (P2 discovery, 2026-07-15).** Two steps were corrected
against the code rather than left describing a product that does not exist
— see the [P2 discovery report](../pillars/p2_discovery.md):

- **Step 3** claimed a junction is created *"automatically at the overlap"*.
  Nothing does that today: the Create Road tool only authors a road, and the
  snap set has no side or crossing candidate, so junctions are made solely
  with the Create Junction tool. The kernel operations to form one on commit
  do exist, so P2 wires them up
  ([#214](https://github.com/Robomous/RoadMaker/issues/214)) and the step now
  says *when* the junction forms.
- **Step 11** was self-contradictory: a road style *defines* the lane
  profile, yet the step asked for *"lane count edits"* to be preserved
  through it. The preservation contract is now explicit — a style replaces
  the cross section and keeps everything orthogonal to it
  ([#219](https://github.com/Robomous/RoadMaker/issues/219)).

**Amendment (field triage, 2026-07-21).** Step 3 was extended by maintainer
decision ([#351](https://github.com/Robomous/RoadMaker/issues/351) item 3,
implemented by [#354](https://github.com/Robomous/RoadMaker/issues/354)):
the single-crossing wording was the p2-s3 delivery; the step now requires a
junction at **every** crossing and T-intent detection when an endpoint lands
on another road's side — all interactions of one stroke in one undoable
commit.

**Amendment (realism batch, 2026-07-24).** With
[#413](https://github.com/Robomous/RoadMaker/issues/413) the create-road
templates and Library road styles are the four road classes of
[realism_defaults.md](../../domain/realism_defaults.md) §1.2 (freeway /
arterial / collector / local street), all widths and markings deriving from
the defaults registry. Step 2's "default style" is now the **local street**
(one lane each way between sidewalks, no painted lines) and step 11's style
choices are the four class styles; a pending hand-run sees the new cross
sections. The headless replay below is unaffected — it pins the collector
(`two_lane_default`) profile explicitly.

**Amendment (p7-s2, 2026-07-28).** Step 27 promised that *"PROJ arrives with
p7-s2"*. It does not:
[ADR-0010](../../decisions/0010-gis-ingest-bounded-crs.md) rules that GIS
ingest computes a bounded family of projections in closed form and refuses
everything else by name, so a CRS outside that family stays opaque
permanently rather than until the next sprint. The step's *observable*
expectation is unchanged — a custom CRS is still carried verbatim and never
reinterpreted — only the promise about the future was removed.

**Closeout self-check (P2 sprint 9, 2026-07-17).** A headless replay,
`scripts/gw2_replay.py`, now drives the automatable slice through the kernel
command layer and asserts each outcome: step 2 (Create Road, default two-lane
profile, editable authoring waypoints), step 3 (a crossing forms one junction
with connections and consumed lane links via `cross_roads`), step 4 (a curved
road extended at BOTH endpoints with heading + curvature continuity at each
join), step 5 (an enclosing ring derives one ground surface with a non-empty
surface mesh channel), step 11 (a road style replaces the profile while name,
elevation, links, and a placed object are preserved), step 12 (Lane Carve tapers
0 → full and holds to the terminus), and the step-23 persistence slice (save →
reload → write is byte-identical; `validate_network(V1_8_1)` reports zero
errors). The P2 lane tools the official steps don't touch — Lane
(type + direction), Lane Width, Lane Add, and Lane Form across a seam — are
exercised as supplementary evidence. The replay runs undo ×10 / redo ×10
byte-identically, exits 0, and its exported `.xodr` loads cleanly in esmini.
One authoring nicety surfaced: a placed `<object>` needs a real `@type` (not an
empty one) for esmini to accept it without an error line — the replay sets it,
and this is a scene-authoring note, not a kernel defect. The user-guide tool
pages (Lane, Lane Width, Lane Add, Lane Form, Lane Carve, Road Styles) landed in
the same change. `Road Plan tool` in step 2 was corrected to its real name,
`Create Road tool`.

### Project and roads

1. [ ] Create a new project, then a new scene inside it. **Expected:** a
   project is a folder of shared assets; the scene lives in the project;
   both appear on the welcome screen's recent list on next launch.
2. [ ] With the Create Road tool, click control points to lay a straight
   road; right-click to finish. **Expected:** a road with the default
   style; control points remain editable.
3. [ ] Draw a second road across the first, finishing beyond it. Then draw a
   third road that crosses two roads and ends on the side of another.
   **Expected:** on commit, a junction forms at EVERY crossing with
   connecting lanes, and an endpoint landing on a road's side forms a
   T junction — all in one undoable commit. Detection happens when the road
   is finished, not while it is being drawn; T-intent is indicated while
   drawing.
4. [ ] Draw a curved road, then extend one of its endpoints by clicking
   beyond it. **Expected:** the extension fits with geometric-constraint
   continuity (no tangent kink at the join).
5. [ ] Draw roads enclosing an area. **Expected:** the enclosed area
   auto-forms a ground surface.
6. [ ] Select that surface and open the Surface tool (**U**). **Expected:**
   the surface is a node graph; nodes and tangent handles are editable, a
   midpoint marker inserts a node and Delete removes one, each gesture is a
   single undo step, and the Attributes pane reports the boundary as
   *Authored* after the first edit with "Revert to derived" offered to undo
   the detach. The headless slice of this step (`scripts/gw2_replay.py`,
   step 6) covers the edit, the detach, the single undo step, the
   round-trip, and the revert; the hand-run is the gestures themselves.

### Elevation and bridges

7. [ ] Create a terrain field (**Edit ▸ Terrain ▸ Create Terrain Field**),
   then select a road, open its elevation profile in the 2D Editor pane,
   and raise a span over the crossing road. **Expected:** the profile
   edits as a 2D curve; the 3D road follows; the ground within a skirt band
   of the road rises with it while ground farther away stays flat, and the
   two meet at the kerb with no step. The field saves to a `.asc` sidecar
   beside the `.xodr` and reloads with the scene. The headless slice of
   this step (`scripts/gw2_replay.py`, step 7) covers creating the field,
   the road-driven re-mesh, undo/redo of both edits, and the sidecar
   round-trip; the hand-run is watching the ground follow the road in 3D.
   Then sculpt the ground directly with the **Terrain Brush** (**⇧B**):
   raise, lower and smooth strokes each land as one undo step, and
   **Edit ▸ Terrain ▸ Import DEM…** brings in an ESRI ASCII (`.asc`) grid as
   the field (p5-s4). **Expected:** a brush stroke deforms the ground live and
   the roads keep meeting the kerb; an imported DEM gives believable ground the
   roads conform to. The headless slice (`scripts/gw2_replay.py`, step 7b)
   covers the brush command's one-undo edit, the DEM `.asc` round-trip, and that
   the sculpted scene still validates and round-trips; the hand-run is watching
   the hill form under the brush in 3D.
8. [ ] Run the Road Construction tool's automatic bridge assignment on
   the raised span (**Edit ▸ Bridge ▸ Generate Bridge Structures**).
   **Expected:** the elevated span becomes a bridge — a deck with piers,
   abutments and guardrails — with sensible span limits; a span-inflation
   control widens/narrows the bridged extent. The headless slice of this step
   (`scripts/gw2_replay.py`, step 8) covers the grade-separation detection, the
   author-bridge command building a watertight solid, the span-inflation command
   changing the extent, and the `<bridge>` record round-tripping byte-identically
   while the solids stay derived. The hand-run is watching the deck, piers and
   guardrails appear over the crossing in 3D, plus the interactive span-inflation
   handle (a follow-up UI; the span-inflation *command* is covered headlessly).

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
    road. **Expected:** the style replaces the lane profile and the
    boundary markings, while everything orthogonal to the cross section
    survives — the reference-line geometry, the elevation profile edited
    in step 7, superelevation, junction connectivity, the road's name,
    and any props or signals already placed on it.

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
16b. [ ] Switch to the **Move** tool with that prop still selected.
    **Expected:** the gizmo appears **at the prop**. Drag the **yaw ring**
    slowly: the prop turns in 15° steps and stops on exact multiples of 15°
    measured from the road, not 15° from where the drag started (p6-s15,
    #417) — so from an odd starting angle the first step *tidies* it. Hold
    **Shift** while dragging: the angle goes free. Release. **Expected:** one
    toast, one undo step, and **Ctrl+Z** restores the exact prior heading.
    Now drag the ring on a **Prop Span**. **Expected:** every instance in the
    series turns by the same angle relative to the road.
16c. [ ] Select a single prop and read the Attributes pane (p6-s16, #418).
    **Expected:** **s**, **t**, **Heading** and **Z offset** are all editable,
    seeded from the prop, with a read-only **World** row beneath them. Type a
    **Heading** and a **Z offset**. **Expected:** the prop turns and lifts, one
    undo step each, the World row follows, and the viewport agrees with it.
    Drag the **s** label. **Expected:** the prop slides along its road in one
    undo step. Now select a **crosswalk**. **Expected:** the pose rows are
    gone — a marking's shape is stored as an outline, so its origin is not
    typeable here.
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

21. [ ] Open **File ▸ Scene Export Preview…**. **Expected:** a per-channel
    manifest of the 3D export (elements, triangles, materials, extents) with
    nothing written to disk; switching **Format** between glTF and OpenUSD
    changes the triangle count, because glTF shares one mesh per prop model
    while USD bakes every instance. The **surfaces** and **terrain** rows both
    read *Exported*, with non-zero triangles and a `ground_…` material each —
    the ground from steps 5–7 goes into the file
    ([#390](https://github.com/Robomous/RoadMaker/issues/390)); the two ground
    rows carry the same triangle counts in both formats, since neither shares
    nor bakes the ground. The only *Not supported* row is the sign faces in
    OpenUSD ([#364](https://github.com/Robomous/RoadMaker/issues/364)).
22. [ ] Open **File ▸ OpenDRIVE Export Preview…**. **Expected:** the `.xodr`
    exactly as it would be written, its structural counts (roads, reference
    length, junctions, lane sections, lanes, objects, signals), the `rm:`
    extension records it carries — and no Layer-2 scene state among them — and
    the checker's findings, which are published to the Diagnostics panel
    **without saving**. Before georeferencing (step 25) the header
    line reads *No georeference — the file describes a local Cartesian frame.*
23. [ ] Save, close, and reopen the scene. **Expected:** everything above
    round-trips.
24. [ ] Before closing in step 23, orbit somewhere distinctive and toggle
    **View ▸ Textured Rendering**; after reopening, close the scene again and
    reopen the PROJECT (welcome tile or **File ▸ Open Project…**).
    **Expected:** the project reopens on that same scene, at the camera and
    render mode you left — the Layer-2 container of
    [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md) (fmt-s1,
    [#325](https://github.com/Robomous/RoadMaker/issues/325)). Then delete the
    scene's `.rmscene.json` and reopen it: the scene is intact and simply
    frames itself as before — Layer 2 is comfort, never content.

### Georeferencing

25. [ ] Open **Edit ▸ World Georeference…**, leave **World origin** selected,
    enter a latitude and longitude, and press **Apply** (p7-s5,
    [#324](https://github.com/Robomous/RoadMaker/issues/324)). **Expected:**
    ONE undo entry for the whole form. Re-open **File ▸ OpenDRIVE Export
    Preview…**: it now names that world origin, and the XML shows a
    `<geoReference><![CDATA[+proj=tmerc …]]></geoReference>` inside `<header>`,
    which also carries derived `west`/`south`/`east`/`north` attributes.
26. [ ] Press **Fit workspace to selection** (with a road selected), then save,
    close and reopen the scene. **Expected:** the workspace read-out comes back
    unchanged, and the latitude and longitude are the ones you typed — not
    rounded. Press **View ▸ Centre on World Origin**: the pivot moves to the
    scene origin and the zoom is unchanged.
27. [ ] Re-open the georeference window, switch to **Custom CRS**, paste
    `+proj=utm +zone=31 +ellps=GRS80 +units=m +no_defs`, and **Apply**.
    **Expected:** the window reports it as a projection carried verbatim rather
    than naming a world origin — the scene's own frame is only ever the
    Transverse Mercator on the origin that step 25 writes, and a foreign CRS is
    exported untouched and never reinterpreted. Reopening the scene now
    discards the workspace box with a warning in the Diagnostics panel, because
    it was framed in the previous frame. Undo twice to get back to step 25's
    state.

### Importing GIS data

28. [ ] With the world origin from step 25 still applied, open **File ▸ Import ▸
    GIS Raster…** and pick a GeoTIFF, or a PNG with a world file beside it,
    covering roughly the same area (p7-s2,
    [#242](https://github.com/Robomous/RoadMaker/issues/242)). **Expected:** the
    imagery appears UNDER the network, hiding the procedural grass where it
    covers, with the roads drawn over it. The Diagnostics panel names the
    coordinate system it read, and says whether the image was **placed** (its
    own pixels) or **resampled** (reprojected).
29. [ ] **File ▸ Import ▸ GIS Vector…** a shapefile or GeoJSON of the same area.
    **Expected:** its lines land on top of the imagery, in a colour no authored
    geometry uses. A shapefile with no `.prj` beside it warns that it did not
    state a coordinate system.
30. [ ] Save, close and reopen the scene. **Expected:** both layers come back in
    the same place. Then move one of the source files away and reopen again: the
    layer is still listed but does not draw, with a warning — a reference you
    added is never silently forgotten because a drive was not mounted.
31. [ ] Re-open **Edit ▸ World Georeference…** and change the latitude and
    longitude. **Expected:** the imported layers RE-PLACE themselves in the new
    frame rather than staying where the old one put them or vanishing — unlike
    the workspace box in step 27, a reference layer has a source file to
    re-derive from.
32. [ ] Try **File ▸ Import ▸ GIS Vector…** on a file in an unsupported
    coordinate system (a national grid, or anything on a datum other than
    WGS 84). **Expected:** it is refused with a message that NAMES the
    coordinate system it read and points at the issue tracking the limitation —
    not a generic failure.
33. [ ] **Edit ▸ Terrain ▸ Import Elevation Raster…** with a single-band
    elevation GeoTIFF. **Expected:** it becomes the scene's terrain — the ground
    deforms and the roads conform to it — in exactly ONE undo entry, and Undo
    puts the previous terrain back. Importing an ordinary colour image here is
    refused, saying it needs a single-band raster.

### Importing lidar

34. [ ] **File ▸ Import ▸ Point Cloud…** with an ASPRS `.las` or `.laz` tile
    covering the same area as step 28's imagery (p7-s3,
    [#243](https://github.com/Robomous/RoadMaker/issues/243)). **Expected:** the
    cloud draws ON TOP OF the imagery, in the same place — both went through the
    same coordinate transform, so a cloud that lands somewhere else means one of
    them was mis-projected. It is coloured by height rather than flat, and
    unlike the imagery it is NOT flattened onto the ground: orbit the camera and
    the building returns stand above the street ones.
35. [ ] Read the layer's status line and the Diagnostics panel. **Expected:**
    they name the coordinate system the tile declared AND, if the tile was too
    large for the read budget, the decimation ratio ("1 in 12 points").
36. [ ] **Edit ▸ Terrain ▸ Seed from Point Cloud…** with the same tile.
    **Expected:** the ground deforms to the tile's own surface and the roads
    conform to it, in exactly ONE undo entry; Undo puts the previous terrain
    back. A diagnostic names WHICH estimator ran — the returns the tile
    classified as bare ground, or the lowest return where it classified none.
37. [ ] Try **File ▸ Import ▸ Point Cloud…** on a tile in an unsupported
    coordinate system. **Expected:** refused with the coordinate system NAMED,
    in the same words the GIS importers use for the same CRS — the refusal is
    shared code, and three importers disagreeing about one file would be worse
    than any of them refusing it.

### Importing OSM

38. [ ] **File ▸ Import ▸ OSM Road Network…** with an `.osm` extract of a small
    district covering the same area as step 28's imagery (p7-s4,
    [#244](https://github.com/Robomous/RoadMaker/issues/244)). **Expected:** the
    roads land ON the imagery, in the right places — the extract, the
    orthophoto and the lidar tile all went through the same transform, so a
    district that lands elsewhere means one of the four importers disagrees
    about where this place is.
39. [ ] Look at the Diagnostics panel. **Expected:** every way that did not
    become a road is named **by its OSM id** with the reason
    (`highway=footway is not a road classification this build imports`), and
    every simplified road quotes the **measured** node counts and the **actual**
    deviation in metres. An aggregate count with no ids fails this step: the
    point is that a user can look the element up in their own file.
40. [ ] Count the roads that produced NO diagnostic. **Expected:** most of a
    real district imports without compromise, and those roads are **silent**. A
    panel with one row per imported road fails this step just as surely as an
    empty one would — warning about everything says exactly as much as warning
    about nothing.
41. [ ] Zoom to a crossing and orbit. **Expected:** the intersections are real
    junctions with connecting roads, not merely roads that touch. Then find a
    place where the extract has a bridge (`layer=1`) over another road.
    **Expected:** they are **NOT joined** — no junction, no link — and the
    Diagnostics panel says so, naming both layers. A junction there would look
    fine from directly above and be wrong in every 3D consumer downstream.
42. [ ] Press **Undo once**. **Expected:** the entire district disappears in a
    single step, and the scene is exactly what it was before the import. Redo
    brings all of it back.
43. [ ] Import the same extract a second time. **Expected:** nothing is
    duplicated, and the summary says how many roads it skipped because they
    were already present.
44. [ ] Try **File ▸ Import ▸ OSM Road Network…** on a scene with **no world
    origin** (a fresh, ungeoreferenced scene). **Expected:** refused, naming
    the coordinate system, **in the same words the GIS and lidar importers
    use** — and the scene is still ungeoreferenced afterwards. An importer that
    helpfully picked an origin would have given the scene a projection the user
    never chose.
45. [ ] Try it on a `.osm.pbf`. **Expected:** refused with a message that names
    **zlib** as the reason and offers the `osmium` conversion. A message
    blaming Protocol Buffers is wrong and fails this step — it would send the
    next reader down the wrong path.

## Pass criteria

- All steps complete in order in a single session; every expected result
  holds.
- Zero crashes; undo/redo works after each authoring step.
- The exported `.xodr` validates with zero errors and loads in esmini.
- **An OSM import is one undo entry.** Step 42 must restore the pre-import
  scene in a single Undo — a district that takes hundreds of presses to remove
  is not one edit however many roads it made.
- **Every dropped element is named by id.** Step 39 fails on an aggregate
  count alone, and step 40 fails if a road imported without compromise
  produced a diagnostic anyway.
- **An overpass is not welded to the road beneath it** (step 41).
- **The no-origin refusal names the coordinate system**, in the same words the
  GIS and lidar importers use (step 44).
- The ground reaches the 3D export: in step 21 the **surfaces** and **terrain**
  rows read *Exported* with non-zero triangles, in BOTH formats. A ground row
  that reads *Not written* is a regression of
  [#390](https://github.com/Robomous/RoadMaker/issues/390) and fails the run.
- The saved `.xodr` is byte-identical whether or not its `.rmscene.json`
  companion exists: no editor state may leak into the ASAM layer.
- The world origin survives the file EXACTLY: the latitude and longitude shown
  after step 26's reopen are the digits typed in step 25, not a rounding of
  them. A drifted origin means the projection string is being written lossily
  and fails the run.
- The workspace box in step 27 is discarded, not silently kept: a box framed in
  one georeference must never be reused under another
  ([#324](https://github.com/Robomous/RoadMaker/issues/324)).
- An imported layer is never silently mis-placed. In step 28 the Diagnostics
  panel must say **placed** or **resampled** — a reprojected image is no longer
  pixel-for-pixel its source file, and a run where that goes unreported fails
  ([#242](https://github.com/Robomous/RoadMaker/issues/242)).
- Step 32's refusal NAMES the coordinate system. "Unsupported" on its own is a
  regression: the whole point of computing a bounded family is being able to say
  precisely what fell outside it.
- Steps 30 and 31 distinguish the two Layer-2 rules that look alike: a reference
  layer is RE-DERIVED when the frame changes (it has a source), while the
  workspace box is DISCARDED (it has none). A build that drops reference layers
  on a georeference change fails the run.
- In step 34 the cloud and the imagery must LAND ON EACH OTHER. They are read by
  different parsers but share one `CrsTransform`, so a visible offset between
  them is a projection bug in one of the two, not a data difference — and it is
  the cheapest place in the whole workflow to notice one
  ([#243](https://github.com/Robomous/RoadMaker/issues/243)).
- The cloud in step 34 must have VISIBLE RELIEF when the camera orbits. A tile
  drawn flat at one height has been treated as a backdrop, which throws away the
  only thing it was imported for.
- Step 36's diagnostic must NAME the estimator. "Classified as bare ground" and
  "lowest return" disagree under a bridge or a canopy, and a user reading the
  terrain has to know which answer they are looking at; an unlabelled fit fails
  the run.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
