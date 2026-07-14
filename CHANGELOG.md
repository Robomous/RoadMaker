# Changelog

All notable changes to RoadMaker are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.6.0] - Unreleased

Placement and transform corrections found while dogfooding GS-1
([epic #178](https://github.com/Robomous/RoadMaker/issues/178)): the shared
placement path GS-1 signal placement builds on.

### Added
- **3D transform gizmo** ([#177](https://github.com/Robomous/RoadMaker/issues/177)):
  selecting an entity with the Move tool shows the classic axis gizmo — three
  colored axis arrows (X red, Y green, Z blue), a centre pad for free planar
  drag, and a yaw ring around Z — at constant screen size, always on top.
  Dragging a handle previews live through the incremental pipeline and commits
  **one undo step** with a summary toast; **Esc** cancels mid-drag; rotation
  snaps to **15° detents** (**Shift** for free). Roads: X/Y via `translate_roads`,
  yaw via `rotate_road` about the selection pivot, Z as a uniform elevation
  offset; props/signals: X/Y (re-project + `move_object`) and yaw. The gizmo math
  (screen projection, hit-testing, axis/yaw constraint) is a pure, unit-tested
  module; the soak driver learns `rotate_road`. Pitch/roll rings and prop-Z are
  future work.
- **`rotate_road` kernel op** ([#177](https://github.com/Robomous/RoadMaker/issues/177)):
  `edit::rotate_road(network, road, angle, pivot_x, pivot_y)` rigidly rotates a
  whole road about a world pivot — every geometry record's start position rotates
  and its heading gains the angle, authoring waypoints rotate too, while lengths,
  lanes, elevation (s-relative) and shape coefficients (local-frame) are
  untouched, so undo is byte-identical. Same connectivity policy as
  `translate_road` (breaks links to non-rotating roads, refuses junction roads).
  Python `edit.rotate_road` + `examples/rotate_road.py`. Foundation for the
  transform gizmo's road rotation.
- **Explicit Move tool** ([#176](https://github.com/Robomous/RoadMaker/issues/176)):
  a dedicated, discoverable **Move** tool on the toolbar (Lucide 4-arrow icon,
  shortcut **M**) — hovering a movable entity shows the 4-arrow cursor and a body
  drag moves it (Esc cancels), while a click selects it. Roads move through the
  existing `translate_roads` path; **props now move too** — dragging a prop
  re-projects it onto its owning road and previews `move_object`, committing one
  undo step on release (the kernel op existed but had no editor caller). The
  Select tool keeps drag-to-move as a power path; context menus gain a **Move**
  entry that arms the tool with the entity selected. Junction-member roads stay
  blocked with the same toast.

### Fixed
- **Drag-drop placement lands where you drop it**
  ([#175](https://github.com/Robomous/RoadMaker/issues/175)): a dragged library
  item (tree, assembly) now commits at the exact spot its ghost marks. The drag
  ghost is a world-anchored marker at the resolved landing point — projected
  from the same `resolve_library_drop` the drop commit uses — instead of a
  screen-space crosshair at the raw cursor; and the drop unprojects against the
  real surface under the cursor (road / junction / prop via `pick()`, ground
  plane only as a fallback) instead of a hidden `z = 0` plane. An off-road prop
  drop tints the ghost and is rejected with a toast rather than silently
  relocated. Controller tests assert ghost == commit across camera angles.

## [0.5.0] - Unreleased

M3a opens with a topology-editing pass — moving whole roads, inserting bend
points, splitting and merging — plus interaction polish. Every change ships
through the M2 command layer (one undo step per edit, byte-identical undo) and
is headless-testable.

### Added
- **Signal properties panel** (GS-1 WS-C): selecting a traffic light or sign
  shows a **Signal** section in the Properties panel — its kind (dynamic/static),
  type/subtype, and country as read-only rows, plus editable **s / t / heading
  offset** spinboxes that each commit one `edit::move_signal` on focus-out (with
  the same unchanged-value skip guard the other editors use, so undo-refresh
  never echoes a command back). Completes the WS-C signal authoring loop
  (place → select → edit → delete). Retyping type/subtype is a later slice. Part
  of [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Signals are selectable, hoverable, and deletable** (GS-1 WS-C): a placed
  traffic light or sign is now a first-class editor entity — click it to select
  (it highlights as a whole pole), hover shows a "signal N" readout, and
  **Delete** removes it in one undo step (`edit::delete_signal`). Picking adds a
  bounding-sphere test on the signal instances (a signal in front of a road wins
  the pick, like a prop); `SelectionModel` gains `selected_signals()` and prunes
  a signal entry when a delete command removes it; the highlight rule matches by
  `SignalId`. A body-drag on a signal no longer moves the road under it. Signal
  **properties-panel editing** follows. Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **GS-1 golden scene — "Urban intersection"** (GS-1 WS-E, acceptance artifact):
  the M3a golden scene is built and rendered. `python/examples/build_gs1.py`
  dogfoods the kernel edit layer end to end — a 4-arm urban junction
  (`x_intersection`, sidewalk profile), crosswalks / stop lines / lane arrows on
  every arm, four traffic lights + two static signs, and a line of street trees
  — saved as `assets/samples/gs1_urban_intersection.xodr` (0 diagnostics,
  esmini-loadable). A fixed **`gs1` golden camera** (eye (−55,−55,35) → origin,
  the spec's three-quarter diagonal) renders in CI at 1920×1080 in the textured
  daytime look. Per-row checklist status + baseline process in
  `docs/roadmap/golden_scenes/gs1_urban_intersection.md`.
- **Drag traffic lights & signs from the Library** (GS-1 WS-C): the Library
  panel gains a **Signals** category with a **Traffic light** and a **Traffic
  sign** — drag either onto (or beside) a road and it snaps to the nearest
  reference line and plants there as an OpenDRIVE `<signal>` in **one undo step**,
  the drag ghost marking exactly where the pole lands (ghost==commit). A light
  authors a dynamic signal (traffic-light catalog type), a sign a static German
  speed-limit plate (274/50) — both retypable later in the properties panel. A
  drop away from any road tints the ghost and is rejected with a hint. Resolver
  `resolve_library_drop` `Kind::Signal` + `edit::add_signal`; the placed signal
  renders through the WS-C signal-mesh instancing. Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Signals render as 3D instances — traffic light + sign meshes** (GS-1 WS-C):
  a placed `<signal>` now draws in the viewport (and glTF/USD exports) as an
  instance of a bundled signal model — a dynamic signal as a three-lamp traffic
  light, a static one as a sign on a pole. The models are procedurally authored
  original work (`scripts/gen_prop_meshes.py`, MIT), embedded in
  `prop_meshes.gen.cpp` alongside the trees. The mesh builder gains
  `build_signal_instances`, emitting one `SignalInstance` per signal at its
  world pose (s/t → position, road tangent + `hOffset` → heading, `zOffset`
  lift); signals rebuild on the same `DirtySet::objects` re-mesh channel as
  props, so a signal edit never re-tessellates a road surface. Python
  `NetworkMesh.signal_count` / `object_count`. Part of the GS-1 signals set.
- **Signal edit commands — add / move / delete a `<signal>`** (GS-1 WS-C): the
  kernel command layer gains `edit::add_signal` / `edit::move_signal` /
  `edit::delete_signal`, mirroring the object commands — every mutation is an
  undoable `edit::Command` (apply→revert byte-identical, restore-in-place keeps
  the `SignalId` across undo/redo, a failed apply leaves the network untouched).
  The `Values`/`GenericCommand` restore engine now carries signals as leaf
  entities. Python `edit.add_signal` / `move_signal` / `delete_signal` +
  `examples/place_signals.py`. Groundwork for placing traffic lights and signs;
  the Library **Signals** category, 3D meshes, and properties follow. Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Add lane arrows to all arms of a junction** (GS-1 WS-B): the junction
  context menu gains **Add lane arrows to all arms** — a straight lane arrow
  (`<object subtype="arrowStraight">`) on each approach lane, pointing into the
  junction and set back behind the stop line, in a **single undo step**. Kernel
  helper `edit::junction_lane_arrows` (one glyph per approach lane, centred in
  the lane, oriented by the arm's travel direction) + Python
  `edit.junction_lane_arrows`; the marks mesh through the existing arrow-glyph
  path. Per-lane left/right turn variants are a later refinement. Completes the
  GS-1 junction marks set (crosswalks + stop lines + arrows). Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Add stop lines to all arms of a junction** (GS-1 WS-B): the junction context
  menu gains **Add stop lines to all arms** — a solid stop line
  (`<object subtype="signalLines">`) across each arm's *approach* lanes, set back
  to clear the crosswalk, added in a **single undo step**. Kernel helper
  `edit::junction_stop_lines` (approach-lane span only, so a two-way road gets a
  line per side; thin along the road) + Python `edit.junction_stop_lines`; the
  marks mesh through the existing stop-line path. Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Add crosswalks to all arms of a junction** (GS-1 WS-B): the junction
  context menu gains **Add crosswalks to all arms** — one zebra crosswalk per
  arm road, spanning that arm's driving lanes just inside the junction, added as
  OpenDRIVE `<object type="crosswalk">`s in a **single undo step**. The geometry
  is a pure kernel helper `edit::junction_crosswalks` (one crosswalk `Object`
  per distinct arm, unique ids, across-span from the driving lanes, `hdg` across
  the road) that the editor adds via `edit::add_object`; the marks mesh as zebra
  bars through the existing marking path. Python `edit.junction_crosswalks` +
  `examples/junction_crosswalks.py`. Part of
  [#72](https://github.com/Robomous/RoadMaker/issues/72).
- **Textured road & sidewalk surfaces** (GS-1 WS-A part 2 + assets): in the
  opt-in Textured mode the driving surfaces now render with a real **asphalt**
  texture and
  sidewalks/curbs with **concrete** (two CC0 512² textures from Poly Haven,
  bundled in the editor), tiled at 4 m via the material UV pipe, and lane
  markings render as **bright flat paint** (unlit) instead of shaded off-white.
  Surfaces are classified per lane type (`surface_for`), resolved to a texture or
  paint material per draw; Sober mode is unchanged (flat colors). A failed
  texture load degrades gracefully to flat color (never a crash). Asset
  provenance + licenses: `assets/manifest.json`, `ASSETS_LICENSES.md`; decisions
  in `docs/design/m3a/textured_render.md`. Closes the render half of
  [#71](https://github.com/Robomous/RoadMaker/issues/71) surface work + assets
  [#70](https://github.com/Robomous/RoadMaker/issues/70) (textures).
- **Procedural grass ground under the network** (GS-1 WS-D, part 2): the opt-in
  Textured mode draws a **procedural grass ground plane** beneath the roads instead
  of floating over a bare grid — a camera-following surface (value-noise mottled,
  lit by the scene environment, fading into the sky at the horizon) at the
  network floor, so the scene finally sits on something. It renders opaque with
  depth so the roads cleanly occlude it, and it's a render-layer-only surface
  (never exported to `.xodr`). Sober mode keeps the reference grid. The flat
  plane is exact for GS-1; a bounds-fitted, elevation-following terrain skirt is
  deferred to the terrain ADR ([#83](https://github.com/Robomous/RoadMaker/issues/83)).
  Per-material texture-vs-procedural decisions: `docs/design/m3a/textured_render.md`.
  Part of [#71](https://github.com/Robomous/RoadMaker/issues/71).
- **Opt-in daytime lighting via a `View → Textured Rendering` toggle** (GS-1
  WS-D, part 1): the plain-color + reference-grid (**Sober**) look stays the
  **default**; a new opt-in **Textured** mode renders with a **hemisphere ambient
  (sky/ground by surface normal) + one directional sun** driven by the renderer
  `Environment`, giving surfaces real daytime shading, and fades the reference
  grid so it reads as scenery. Persisted via `View → Textured Rendering`
  (`view/textured_rendering`, default off). Sober reproduces the flat M2 look
  **exactly** (white ambient 0.35 + 0.65·lambert). No shadow maps (M3a decision
  3). Switching modes is instant (no re-mesh). Part of
  [#71](https://github.com/Robomous/RoadMaker/issues/71).
- **Renderer material/texture foundation + mesh texture coordinates** (GS-1
  WS-A, part 1): the `Renderer` interface gains `TextureHandle`/`TextureData`
  (RGBA8 upload), a `Material` (optional base-color texture + tint + per-meter
  `uv_scale`, default 4 m tile), per-`InstanceData` model transforms on
  `DrawItem`, and an `Environment` (hemisphere + sun) — all still GL-free. The
  GL backend uploads textures, samples them through a new texcoord vertex
  attribute, and honours a per-draw model matrix (single or instanced). The
  kernel mesh builder now emits **planar UVs (u = s, v = t in meters)** on the
  shared road grid, continuous across lane boundaries and welded seams. A
  default `Material` reproduces the pre-material flat look exactly, so this PR
  ships the plumbing with no visual change; textured surfaces, lighting, and
  terrain follow in WS-A/D. Groundwork for [#71](https://github.com/Robomous/RoadMaker/issues/71).
- **Weld road ends smoothly — no curvature kink at the joint** (gate finding 3):
  road-end adjacencies where an arc started right at the joint showed a visible
  curvature discontinuity. `edit::close_gap` now bridges a real gap with a **G2**
  connector (the Clothoids three-arc `G2solve3arc` interpolant, exposed as
  `road::fit_g2_three_arc`) that matches position, heading, **and curvature** at
  both ends. A new **Link Ends** context-menu action welds two selected roads'
  nearby free ends (pure link when they meet, G2 connector across a gap;
  enabled via `edit::check_linkable`), and `edit::create_linked_road` authors a
  road AND welds its start to a free end in one undoable command — so Create
  Road's tangent-continuation snap now produces a genuinely *linked* road, not
  merely an adjacent one. Named fixture `CloseGapNoCurvatureKinkWhenArcStartsAtJoint`
  proves the connector's curvature meets each neighbour within `tol::kWeldCurvature`.
  Python `edit.create_linked_road`. Docs: `docs/user-guide/context-menus.md`.
- **Drop a T/X intersection ONTO a road** (gate finding 1): dragging a T or X
  assembly from the Library onto an existing road now tees/crosses INTO it,
  aligned to the road tangent, instead of dropping a superimposed floating
  junction at the cursor. New kernel factories `edit::assembly::tee_onto_road`
  (a perpendicular stem via `aligned_pose_on_road` + `attach_t_junction`) and
  `cross_onto_road` (split the target at s±gap, two perpendicular stems, a 4-way
  junction on the four ends) — each one undoable command, apply→revert
  byte-identical, welds clean (`verify_junction_welds`). The editor's
  `resolve_library_drop` projects the drop onto the nearest road: on a road it
  attaches, near a road end (no room for the junction area) or in open space it
  falls back to a standalone assembly with an explanatory toast. Python
  `edit.assembly.tee_onto_road` / `cross_onto_road` + `examples/tee_onto_road.py`.
- **Connection engine — one authority for contact-and-fit geometry** (gate
  extension WS-2): the contact/fit primitives that junction generation used to
  keep to itself now live in `roadmaker/edit/connection.{hpp,cpp}` —
  `contact_state`, `contact_lateral`, `driving_lanes_at`, `aligned_pose_on_road`,
  and `fit_connector` (the G1 straight-fillet-straight clothoid fit) — so the
  junction, assembly-drop, and gap-closing consumers share one implementation
  and cannot drift. `plan_junction` is refitted onto them with **byte-identical
  goldens** as the regression gate. New queries land alongside: `junction_at_end`
  / `matching_junction` (idempotency — a repeat selection regenerates in place
  rather than overlaying a duplicate) and `verify_junction_welds` (post-regen
  position/heading coincidence, computed with the same anchor math the generator
  uses). `close_gap` / `check_linkable` weld two free road ends in one undoable
  command — a pure link when they nearly coincide, else a single-lane connector
  road (Python `edit.close_gap` / `check_linkable` / `junction_at_end` /
  `matching_junction` + `examples/close_gap.py`). New tolerances `tol::kWeld*`.
- **Duplicate-junction invariant** (gate finding 5): a road end may be an arm of
  at most one junction. `create_junction` now refuses an end already owned by a
  junction ("regenerate that junction instead"), and `validate_network` emits a
  RoadMaker-authored diagnostic `robomous.ai:rm:1.0.0:junctions.arm_single_owner`
  (Error) when two junctions claim the same arm — the vendor namespace marks it
  as a RoadMaker rule, not an ASAM one.
- **Discoverable lane removal** (gate finding 6): the properties panel's single
  context-dependent "Remove lane" button is replaced by **Remove left lane** /
  **Remove right lane** buttons that act on the outermost lane of each side of
  the selected road — no lane selection required. A button greys out (with an
  explanatory tooltip) when only the driving lane remains on that side or when
  no road is selected, protecting a side's driving floor even though the kernel
  would allow the removal. Right-clicking a lane in the viewport now also offers
  **Remove this lane** (same outermost rule), with the road's own menu items
  still reachable. Removal is one undoable `edit::remove_lane` command that
  restores the exact lane on undo; a success toast surfaces via the panel's new
  `status_message` signal. Behaviour and tests:
  `docs/user-guide/lane-profile.md`, `docs/user-guide/context-menus.md`.
- **First-run guided tour** (UI revamp Phase 4): a 5-step, skippable coach-mark
  tour runs once on a first launch — draw a road → drag in an intersection →
  plant a tree → shape the elevation → export — dimming the app and ringing the
  real toolbar button each step points at. A headless `TourController` holds the
  step logic (seam-tested like `ToastQueue`); a standalone `TourOverlay` widget
  paints it (never touches the GL viewport). It never re-shows (`tour/seen`
  QSettings flag, no telemetry) and capture windows never auto-run it;
  **Help ▸ Guided Tour** replays it. Screenshot mode gained `--show-tour` and CI
  renders the first step. Design + evidence:
  `docs/design/ui-revamp/phase4_tour.md`.
- **Drag a tree from the Library onto a road** (UI revamp Phase 3): the Library
  gains a **Props** category (pine/oak/birch/poplar/shrub, themed `trees` glyph);
  dragging one onto the viewport snaps it to the nearest road's `s`/`t` and
  places an OpenDRIVE `<object>` through one undoable command (`shrub` →
  vegetation), while a drop away from any road places nothing and shows a "drop
  near a road" hint (props are road-relative). Screenshot mode's
  `--drop-library` takes a prop key; CI renders a tree drop. *Fast-follow:*
  dragging a placed prop to a new `s`/`t` (Duplicate/Delete + re-drop cover
  repositioning today). Design + evidence:
  `docs/design/ui-revamp/phase3_props.md`.
- **Discoverability sweep** (UI revamp Phase 4): every capability now has a
  visible, labeled entry point (product-parity rule). **Add from Library** is a
  first-class toolbar button (labeled "Library") and an Edit-menu entry — it was
  reachable only from the empty-canvas context menu. Activating the **Elevation**
  tool now opens the Profile dock, surfacing the vertical-profile handles and the
  **Cross Over / Cross Under** overpass controls that were hidden behind a
  View-menu toggle. The **Merge Roads** button explains itself when greyed
  ("select two roads whose ends meet") instead of reading as broken. Audit table
  + design: `docs/design/ui-revamp/phase4_discoverability.md`.
- **Trees in the editor — render, pick, select, delete** (UI revamp Phase 3):
  placed tree props now draw in the viewport (baked per-part from the bundled
  prop meshes, tagged with their `ObjectId`), are hover-glowed and
  selection-outlined via the road accent path, are pickable (bounding-sphere
  test that shares depth with the road surface so a tree in front wins),
  deletable with the Delete key (one undo macro with any selected roads), and
  carry a right-click **Delete / Frame / Duplicate** menu. `ObjectId` is
  threaded through picking → selection → highlight → scene building, and object
  edits ride the `DirtySet::objects` re-mesh channel (no road re-tessellation).
  Dragging a prop to a new `s`/`t` follows in the placement slice; GPU
  instancing is deferred to #71. Design + evidence:
  `docs/design/ui-revamp/phase3_props.md`.
- **Tree placement through the kernel — commands, mesh, exports** (UI revamp
  Phase 3): `edit::add_object` / `delete_object` / `move_object` command
  factories place, remove, and drag OpenDRIVE `<object>` props through the M2
  command layer (undo/redo, byte-identical round-trip, restore-in-place keeps
  the `ObjectId`), exposed in the Python `edit` module with a `place_objects.py`
  example. The mesh builder emits one instanced `ObjectInstance` per placed
  tree/vegetation prop (via the `DirtySet::objects` channel, so a prop edit
  never re-tessellates a road), and both exporters carry it: glTF as a shared
  mesh per model + one instance node each, USD as a per-instance `Xform` of
  baked part meshes. Props are road-relative only (no world-xy). What
  simulators receive: `docs/domain/opendrive.md`.
- **Bundled low-poly tree props** (UI revamp Phase 3): five procedurally
  authored, license-clean props (`tree_pine`/`oak`/`birch`/`poplar` and
  `shrub`, MIT "original work") — the geometry the Library will place as
  OpenDRIVE `<object>` trees. `scripts/gen_prop_meshes.py` builds them from
  parametric
  trunks/cones/icosahedral crowns and emits both an inspectable OBJ/MTL
  reference export (`assets/library/props/`) and the embedded, flat-shaded mesh
  table the kernel compiles in (`core/src/assets/prop_meshes.gen.cpp`), exposed
  as the one canonical prop geometry through `roadmaker::props::model()`
  (`core/include/roadmaker/assets/prop_library.hpp`) for the viewport and the
  exporters alike. Design + decisions:
  `docs/design/ui-revamp/phase3_props.md`.
- **Drag-and-drop creation from the Library** (UI revamp Phase 2): drag a road
  template or a T/X intersection from the Library panel onto the viewport to
  create it. The model is a drag source (key carried as
  `application/x-roadmaker-library-item`); the viewport shows a "drop here"
  ghost and, on drop, a pure unit-tested `resolve_library_drop` dispatches —
  a road template arms Create Road with that profile at the drop point, a T/X
  assembly pushes `assembly::t/x_intersection` there (one undoable command) and
  toasts the result. The empty-viewport context menu gained **"Add from
  library…"**. Screenshot mode gained `--drop-library`. *Fast-follow:* dropping
  a T on a road tees INTO it (v1 places a standalone assembly). Design +
  before/after: `docs/design/ui-revamp/phase2_library.md`.
- **Library panel** (UI revamp Phase 2): a searchable **Library** dock, tabbed
  with the Scene tree, holding an icon grid over the catalogue — the road
  templates and T/X assemblies the drop handler (next PR) will place. A
  `LibraryFilterProxy` filters by label, groups by class, and injects a themed
  class icon (reusing the bundled template/junction glyphs — no thumbnail
  assets needed for v1); the manifest loads from the Qt resource system so the
  built app always has it. Screenshot mode gained `--raise-dock` (CI renders the
  panel). Design + before/after: `docs/design/ui-revamp/phase2_library.md`.
- **Themed hint card + transient toasts** (UI revamp Phase 1): the tool-hint
  card moved to the top-left and is now themed (bg2/border/text, 8px radius),
  fading out when idle. A greenfield **toast overlay** surfaces editor results
  (merge / save / export) as bottom-center themed cards with a severity color
  bar and auto-fade — a headless, fake-clock-tested `ToastQueue`
  (`editor/src/viewport/toast_queue.{hpp,cpp}`) that prunes, coalesces repeats,
  caps the stack, and computes fade opacity, painted by
  `ViewportWidget::show_toast`. Result messages re-routed off the status bar;
  screenshot mode gained `--toast <text>`. Design + before/after:
  `docs/design/ui-revamp/phase1_feedback.md`.
- **DPI-crisp themed tool handles** (UI revamp Phase 1): node and midpoint
  handles are now screen-space QPainter sprites with idle / hovered / grabbed
  states, replacing the world-meter GL crosses that shrank and grew with zoom.
  `PreviewGeometry` carries a typed `Handle { pos, kind, state }` list (every
  tool migrated); the viewport projects each to logical pixels via a pure,
  unit-tested `project_to_screen` and paints it in `ViewportWidget::draw_handles`
  — light idle node dots (accent on hover/grab), a hollow accent "insert-here"
  ring for midpoints, tangent whiskers still accent GL lines. Edit Nodes tracks
  the hovered node live; the profile panel's node/grade handles adopt the same
  circle language. Screenshot mode gained `--tool <id>` to render a tool's
  overlay (CI renders an Edit Nodes handles shot). Design + before/after:
  `docs/design/ui-revamp/phase1_feedback.md`.
- **Library catalogue manifest + model** (UI-revamp Phase 2 groundwork): a
  versioned runtime manifest (`assets/library/manifest.json`) and a headless
  `LibraryManifest` loader + flat `LibraryListModel` (`editor/src/document/`)
  that back the coming drag-and-drop Library panel. v1 lists the road templates
  (2-lane rural, urban w/ sidewalks, 4-lane divided) and the T/X assemblies,
  each with a `create` spec the drop handler will dispatch on. The loader is
  forward-compatible — an unknown `manifest_version` parses best-effort with a
  warning, and an item whose create kind this build doesn't know is kept as
  `Unknown` (so a Phase-3 props manifest never bricks an older editor). Ships
  with its `QAbstractItemModelTester` gtest and parse tests. (Supersedes the
  flat read-only panel of #50; as-built note in `docs/design/m3a/03_assets.md`.)
- **Parametric intersection assemblies** (kernel, UI-revamp Phase 2 groundwork):
  `edit::assembly::t_intersection(net, pose, params)` and `x_intersection(...)`
  generate a standalone 3-way tee / 4-way crossing as ONE undoable command —
  they lay down the stub roads (a `Pose` places the center, `IntersectionParams`
  tunes arm length, junction gap, and lane profile) and generate the connecting
  roads via the existing junction generator, built as a `CompositeCommand` so
  apply→revert is byte-identical. Validator-clean (fixture tests assert zero
  errors for both). Python bindings under `rm.edit.assembly`, an
  `examples/x_intersection.py`, and `t_intersection.xodr` / `x_intersection.xodr`
  fuzz-corpus seeds ship with it.
- **Viewport hover & selection feedback** (UI revamp Phase 1): the road under
  the cursor now warms subtly and the selected road(s) tint in the theme
  **accent**, both via the renderer — a per-`DrawItem` `HighlightState`
  (`None`/`Hover`/`Selected`) mapped by a pure, headless `highlight_state_for`
  and mixed toward the accent token in the mesh shader (hover subtler than
  selection; selection wins). The accent reaches the renderer as plain floats
  (`BackdropColors::highlight` from `Theme::accent`), so it retints with the
  palette and `render/` stays Qt-free. **Fix:** the old selection tint never
  actually rendered — `u_highlight`/`u_lit` (scalar uniforms) were set with
  `glUniform4f` (a no-op `GL_INVALID_OPERATION`); corrected to `glUniform1f`.
  Screenshot mode gained `--select`/`--hover <odr_id>` (forwarded by
  `editor_screenshot.py`, rendered by the CI visual-artifacts job) so the
  feedback states are captured headlessly. Chosen technique (accent surface
  tint vs. a silhouette outline) and rationale:
  `docs/design/ui-revamp/phase1_feedback.md`.
- **Move a whole road** (M3a topology UX): drag a road body with the Select
  tool to translate the whole road in plan view — auto-selecting it first, or
  moving every selected road together when several are picked. The cursor
  becomes a move cursor during the drag, a ghost tracks it, a single-road drag
  snaps its nearer endpoint to other roads, and the release is exactly one
  undo step. Kernel `edit::translate_roads` (and the single-road
  `translate_road` convenience) shift plan-view geometry and authoring
  waypoints only — headings, lengths, lanes, elevation and marks are untouched,
  so undo is byte-identical. Links between two roads moving together survive;
  a link leaving the moved set is broken on both sides in the same command
  (confirmed once, with a session-wide "don't ask again"). Roads that
  participate in a junction refuse to move (their pose is generated) with a
  toast pointing at the junction. Python `edit.translate_road` /
  `translate_roads` + `examples/move_road.py`; the interactive soak driver
  gained a move operation. *Not yet:* rotation, moving a junction as a unit,
  lateral lane-section moves, and multi-road endpoint snapping (follow-ups).
- **Insert a bend point on a road** (M3a topology UX): double-click a road body
  with the Select tool to drop a node exactly where you clicked and hand off to
  Edit Nodes with that node grabbed — double-click-then-drag bends the road in
  one motion. Edit Nodes' own double-click does the same, and its midpoint
  markers now use the shape-preserving insert too. Kernel `edit::insert_node_at`
  pins the heading at every node from the current curve, so the re-fit
  reproduces every untouched record (line/arc/spiral within tol; a paramPoly3
  covering record is re-fitted approximately with the one-time notice) and only
  the record covering the click splits — unlike the old `insert_waypoint`, which
  drifted authored roads. A node within 2 m of an existing one is refused.
  Python `edit.insert_node_at` + `examples/insert_bend.py`.
- **Split tool + right-click context menus** (M3a topology UX): a new **Split**
  tool (scissors icon, `K`) — hover a road to see the cut marker, click to cut
  it in two at that station (one undo step); both halves are selected and named
  in the toast, and the tool returns to Select. Right-clicking the viewport now
  opens a context menu (a quick right-click, distinguished from an orbit drag by
  a movement threshold): on a **road body** — insert bend point, split here,
  edit lane/elevation profile, frame, delete; on a **node** — split at this
  node, delete node, frame. The menu is built by a headless descriptor builder
  (`editor/src/app/context_menu.{hpp,cpp}`) so its logic is unit-tested without
  a QMenu — the single source of truth the guided tour (#114) will consume.
  `Document::last_dirty()` lets a tool discover a command's new ids (the split's
  tail); the shared `pick_waypoint()` node hit-test moves into
  `viewport/picking.{hpp,cpp}`. The menus are complete across all four contexts
  — road body, node, junction (Frame · Delete junction), and empty space
  (Create road here · Paste _stub_ · Frame all) — and the as-built design is
  recorded in `docs/design/m3a/06_topology_editing.md` with the connectivity
  policy, merge preconditions, and the `reverse_road` deferral inventory.
- **Merge two roads into one** (M3a topology UX): with exactly two roads
  selected that meet end-to-start, **Merge** (git-merge icon, Edit menu,
  toolbar, and the road context menu) welds them into one road. Kernel
  `edit::merge_roads` keeps the first road's id and erases the second; the
  second's geometry is re-anchored onto the first's end pose (the weld absorbs
  the ≤ 1 cm / 1 mrad residual — vertex-exact seam), profiles and lane sections
  concatenate (**not** coalesced, so split→merge is geometry-identical), and the
  far-end links and any far neighbor's back-link re-point onto the merged road.
  `edit::check_mergeable` is the enablement query and returns the verbatim reason
  when it can't (junction road, an end already connected elsewhere, meeting the
  wrong way round — "reverse one first (coming soon)", ends too far apart,
  heading mismatch, or seam lane/elevation mismatch). New tolerances
  `tol::kMergePositionGap` / `kMergeHeading`. `reverse_road` and junction-aware
  merge are deferred (follow-ups). Python `edit.merge_roads` /
  `check_mergeable` + `examples/merge_roads.py`; soak driver + fuzz corpus gain
  merge/post-split seeds.

### Fixed
- **Soak CI log no longer masquerades expected refusals as errors** (#167): the
  ASan soak fires random, mostly-invalid ops, and the command layer logged every
  expected refusal at `[error]`/`[warning]` (~460 lines per green run) — alarming
  noise that buried genuine failures. The soak runner now defaults its console
  threshold to `critical` (verdict comes from the stats/PASS line and the seed on
  failure, never spdlog); `SPDLOG_LEVEL=info` restores the full op trace for
  local seed debugging. The threshold is applied inside the editor library
  because `Document` logs through that library's spdlog registry.
- **Junction floors are selectable — no more unselectable "junction-like area"**
  (gate finding 4): the blended surface between a junction's arms now picks,
  hovers, highlights, and selects as its own entity (its `JunctionId`), from the
  viewport or the Junctions scene tree, and the properties panel shows its
  arm/connection counts. Diagnosis first confirmed the maintainer's "overpass
  produced a junction-like area" is **not** topology — `ProfilePanel::apply_overpass`
  is pure elevation and creates no junction or link (now asserted through the
  editor path), and deleting a crossed road leaves the overpass road's geometry,
  profile, and network validity fully intact (regression test). What the
  maintainer could not select was the step-2 T-junction's floor, which rendered
  but was excluded from picking; `pick()` now tests junction-floor triangles
  (nearest road/prop still wins on a tie, so arms stay grabbable). A soak
  invariant asserts every rendered junction floor maps to a live, selectable
  junction — no ghost surfaces.
- **Re-generating a junction over the same ends regenerates it in place** (gate
  finding 5): the Create Junction tool now checks `edit::matching_junction`
  before generating — an exact re-selection of an existing junction's arms
  regenerates that junction (Info toast "regenerated in place") instead of
  superimposing a duplicate, and a partial overlap (a selected end already owned
  by a junction) is refused with a Warning toast instead of a silent diagnostic.
  Backed by the kernel single-owner invariant (validator rule
  `robomous.ai:rm:1.0.0:junctions.arm_single_owner`) added earlier in the
  sprint. A new `Tool::toast_requested` signal routes tool results to the
  viewport toast overlay; the soak driver gains an `op_duplicate_junction_attempt`
  that asserts the re-attempt is refused and the network stays byte-unchanged.
- **Junction regeneration follows a dragged arm** (gate finding 2): dragging a
  node of a junction's incoming road left the connecting roads frozen —
  `regenerate_junction` matched the freshly planned turns to the existing
  connecting roads **by generation order**, so a drag that re-ordered the plan
  (e.g. a turn crossing the 10° left-turn lane-discipline threshold) made it
  refuse ("recorded connections no longer match") even though the turn *set* was
  unchanged. Matching is now **keyed** on each turn's
  `(incoming road+contact+lane, outgoing road+contact+lane)`, so a re-ordered
  plan still re-fits every connecting road in place; only a genuine turn-set
  change (a lane added/removed/retyped on an arm) refuses. When it does, the
  editor surfaces a **warning toast** ("Junction not updated: …") via the new
  `Document::regeneration_skipped` signal instead of a silent log line, and a
  debug-build assertion (`verify_junction_welds`) guards that a successful
  regeneration leaves the connecting roads coincident with their arms. Landing
  is commit-time; live mid-drag follow is filed as a follow-up (#156). Kernel
  and editor-controller tests drive the real preview→commit drag path.
- **Right-click context-menu actions no longer crash the editor** (gate
  finding 6, the GW-1 hard blocker): `MainWindow` built `ContextMenuDeps` (a
  bundle of three references) as a stack local and showed the menu with the
  non-blocking `QMenu::popup()`, while every `MenuItem` closure captured that
  bundle **by reference** — the local died as soon as `popup()` returned, so any
  later click was a use-after-free (it affected *every* context-menu action, not
  just lanes). The closures now capture the deps **by value** (a copy of three
  references to long-lived `MainWindow` members), making each menu fully
  self-contained regardless of how it is shown. A controller-level regression
  test assembles the menu from a scope whose `deps` goes out of scope and then
  triggers the action — ASan reported `stack-use-after-scope` on the old code
  and is clean on the new. Audit of the same class: the panels capture `this`
  (lifetime-tied, auto-disconnected) and re-read live state each invocation, and
  kernel commands reject stale ids, so no other instance existed.
- **Middle-mouse pan is now natural (ground-anchored)**: the old pan scaled
  screen deltas by an arbitrary `distance × 0.0016` constant that ignored
  pitch and FOV (wildly wrong speed at low pitch) and moved content *against*
  the mouse vertically. The pan now ray-casts the cursor to the ground plane
  on middle-mouse press and keeps that grabbed point pinned under the cursor
  — exact 1:1 tracking at every zoom, pitch, and yaw (the RoadRunner/CAD/maps
  standard, zero tuning constants). Near-horizon rays at low pitch fall back
  to a correctly depth-scaled view-plane pan
  (`2·distance·tan(fov/2)/viewport_height` per pixel). The shared
  `ground_point()` helper (`editor/src/viewport/picking.{hpp,cpp}`) now backs
  the hover readout, tool events, and the pan.

### Changed
- **Contributing docs gained an "Agent PR discipline" section**
  (`docs/contributing/workflow.md`): main-first branching with no stacked
  branches off unmerged work, verify-before-merge, merge-on-green, the
  maintainer look-approval gate for visual PRs, and the rule that docs, issues,
  and the roadmap are synced in the same session a PR merges.
- **Docs & README revamp close-out** (UI revamp Phase 4, closes epic #108): a
  README hero + drag-and-drop workflow GIF, the committed **golden-look**
  baseline (`docs/standards/golden-look.png`, from the new
  `assets/samples/golden_scene.xodr` T-junction-with-props scene) wired into the
  UI-design standard, a new [Library](docs/user-guide/library.md) user-guide
  page, themed screenshots on the Create Road / Junction / Elevation / Objects
  pages, and a refreshed in-editor props story on the Objects page. The M3a UI
  revamp epic is complete; remaining standards-track work (junction boundary,
  textures, instancing, object/signal placement, GS-1) carries on under M3a.

## [0.4.0] - Unreleased

The **hardening release**: maintainer dogfooding of v0.3.0 found product
gaps the milestone plans and scene-based acceptance had missed —
T-intersections were impossible, vertical design was too crude, and normal
interactive use crashed. This release exists to fix those before any new
milestone features; the roadmap records the lesson honestly and M3a shifts
to v0.5.0. Acceptance adds [golden workflows](docs/roadmap/golden_workflows/README.md)
(path-based, run by hand) alongside golden scenes; the release gate is
GW-1 + GW-2 executed by the maintainer.

### Added
- **UI revamp Phase 0 — the editor looks like a product** (#109, epic #108):
  theme token system (`editor/src/theme/`, Fusion + dark QPalette +
  generated QSS; three palettes selectable via `--theme` and persisted in
  settings — default pending the maintainer's mockup pick); labeled main
  toolbar (28 px icons, text under, grouped File | Tools | View) plus a
  contextual tool-options row (Create Road's template dropdown is now a
  visible, labeled control); welcome screen on launch (recent scenes with
  save-time thumbnails, curated sample scenes with committed thumbnails,
  New/Open, docs links); viewport backdrop rebuilt in the renderer —
  gradient sky, shader ground grid (1 m/10 m lines, distance-faded, origin
  axes) replacing the hard-edged 200 m line grid; `--screenshot-ui` captures
  the whole themed window (`-` as scene captures the welcome screen). The
  strategy shift is recorded in docs/standards/product-parity.md and the
  new docs/standards/ui-design.md.
- **Editor screenshot mode + CI visual artifacts**: `roadmaker-editor
  --screenshot <scene.xodr> <out.png> [--camera top|orbit] [--size WxH]`
  renders a scene headless and exits (`scripts/editor_screenshot.py` wraps
  binary discovery; a GL-less environment skips with a distinct exit code).
  CI renders the canonical scenes — 4-arm crossing, the tee, an overpass
  (`scripts/make_canonical_scenes.py`) — and uploads the PNGs as workflow
  artifacts for human review. Process rule: mesh/material/normal/renderer
  PRs ship editor-rendered before/after screenshots; the maintainer gates
  appearance (docs/standards/product-parity.md).
- **T-junctions — attach a road to another road's side** (#92, PR #96): the
  Create Junction tool tees one selected road end into another road's body
  (side-snap indicator at the projected station; Enter attaches as ONE undo
  step). Kernel `edit::attach_t_junction` composes split + stub-delete +
  junction generation atomically; `split_road` now splits junction-linked
  roads (successor-side junction remaps onto the tail) so the same main road
  can be teed repeatedly. All legal turns are generated by default —
  permission pruning comes later. New `edit::snap_to_road_side` query;
  Python `edit.attach_t_junction` + `examples/t_junction.py`.
- **Vertical-profile editor** (WS-C, PR #98): dockable Profile panel plotting
  z(s) of the selected road — nodes drag in z, tangent handles drag the
  grade (% readout), double-click inserts, Backspace deletes; drags are one
  preview session → one undo entry. Kernel `edit::set_elevation_profile`
  (C1 cubic through explicit nodes with lockable grades) +
  `edit::elevation_profile_points`; Python bindings +
  `examples/elevation_profile.py`.
- **Overpass workflow** (PR #98): Cross Over / Cross Under re-profile a road
  to clear every plan-view crossing by a configurable clearance (default
  5.5 m) — pure elevation, no topology; ramps respect the max grade.
- **Grade advisory** (PR #98): `validate_network` warns above a configurable
  max grade (default 12 %) — a RoadMaker advisory (empty rule id), surfaced
  in the Diagnostics panel.
- **Crash-capture infrastructure** (#84, PR #85): POSIX signal / Windows SEH
  crash handlers write local reports (version+commit, OS, stack trace,
  command-log tail) to `AppDataLocation/crash-reports`; the next launch
  points at the report and offers the folder — **no telemetry, nothing
  leaves the machine**. Per-session rolling log records every executed
  command; `.github/ISSUE_TEMPLATE/crash.yml` ships the report fields.
- **Interactive soak testing** (#86, PR #91): a seeded-random operation
  driver over the real Document/command stack with invariants after every
  operation (validation, id integrity, byte-identical round-trip);
  `roadmaker_soak` runner, fixed-seed smoke in the suite, and a ~10-minute
  seeded soak in the Linux ASan CI job. Same seed = same sequence.
- **Autosave hardening** (#53 spec gaps, PR #99): a recovery copy is written
  right before every junction regeneration, and autosave gains a persisted
  File-menu disable switch.
- **Tee preview & discoverability** (#103): the Create Junction tooltip
  documents both flows (endpoint junction and tee); with one end selected,
  hovering or anchoring a road body shows the tee overlay — anchor marker at
  the projected station, dashed ghost line from the selected end, and the
  highlighted `[s−gap, s+gap]` span the junction will replace; the status
  text also appears as a viewport-corner hint for every tool; new
  [T-junction user-guide page](docs/user-guide/t-junction.md); kernel
  `edit::t_attach_gap` (bound in Python) exposes the auto-gap the preview
  and the command share; committed tee sample `assets/samples/t_attach.xodr`.

### Fixed
- **T-junction visual quality — fillets, materials, smooth shading, seams**
  (follow-up to #103): the tee's measured geometry was fixed in PR #104 but
  the rendered junction still read as a dark rectangular patch. The junction
  surface now grows corner fillets at every re-entrant corner between arms
  (pavement-edge arcs, radius derived from the corner turn's connecting
  road, floored at 3 m, clamped to face availability), corridor edge strips
  pinning the boundary to the exact pavement edges (kills the mouth
  step/notches and highway shoulder-band gaps), a morphological closing that
  removes the 1 cm weld apron sawtooth, and hole filling (junction pavement
  is simply connected). Floors carry the driving-lane material in the editor
  and both exporters — the legacy junction-debug material is gone. Road
  normals carry the longitudinal grade (graded roads no longer lit as flat
  and creasing against the floor), and the editor viewport renders with 4x
  MSAA (jagged edge-strip silhouettes). The attach auto-gap and branch trim
  reserve fillet clearance so generated tees always have room for the arcs.
  New test gates: fillet boundary G1 + radius floor, material assertions,
  normal smoothness/weld checks, and an editor/kernel mesh-parity suite
  that kills the dual-meshing-path bug class.
- **T-junction geometry & rendering** (#103, GW-1 gate finding): tees (and
  multi-lane junctions generally) were geometrically non-conformant and
  rendered wrong. Connecting roads are now anchored on the linked lanes'
  boundaries (smooth_fit) instead of arm centers, fitted through circular
  fillet guides (drivable curvature, ≤ 1/6 m⁻¹), given elevation profiles
  matching arm z and grade (graded tees had 3 m cliffs), and generated with
  lane discipline (left turns from/to inner lanes). The auto gap is sized
  from turning geometry, not just road widths, and the branch overhang is
  trimmed back to the junction boundary. The junction surface gains per-arm
  joint closures, a micrometer-precision welded union (the 1 cm-rounding
  Clipper2 default broke every seam), exact-vertex seam stitching, and
  sliver elimination; connecting roads no longer double-draw under the
  floor (interior z-fighting). An 11-fixture quality matrix enforces the
  invariants (slivers, seams, curvature, crossings, elevation continuity,
  determinism, validator-clean export, undo byte-identity) in CI.
- Uncaught Clothoids exception on sharp waypoint turn-backs terminated the
  editor (#87, found by the soak driver; kernel exception boundary added).
- Unbounded clothoid fits: a sharp curve could balloon a road/marking to
  many times its span until the editor died (#93, maintainer CRASH-1;
  authored fits above 4× the waypoint span are refused with a clear error).
- CDT "intersecting constraint edges" exception escaped junction-floor
  meshing and terminated the editor (#88; TryResolve + graceful no-floor
  degradation).
- `delete_road` left a dangling arm reference in surviving junctions —
  regeneration then walked a dead id (#89).
- Junctions below two arms did not round-trip byte-identically (`rm:arms`
  writer/reader asymmetry, #90).
- MSVC build break on `fopen` under `/WX` (PR #94).

### Also in this release — early M3a kernel groundwork

Landed on `main` between v0.3.0 and the hardening decision, shipping here
(M3a itself resumes as v0.5.0 after the gate): OpenDRIVE `<objects>`
(#67) and `<signals>` (#68) parse/represent/write/validate; road-mark
completions — color, dual geometry, object markings (#69); junction
`<boundary>` emission for generated junctions (#62, partial — auxiliary
boundary roads remain); autosave & crash recovery (#53); the esmini
round-trip CI smoke gate (#51); and the first user guide (#52).

## [0.3.0] - 2026-07-11

Milestone 2 (M2): interactive editing core, 3D junction blending, and OpenUSD
export. This is the first release in which OpenDRIVE networks can be authored
and edited in the editor, not just viewed.

### Added
- **Interactive editing tools** (8, issues #9–#17): Select/Move (multi-pick,
  rubber band, node drag), Edit Nodes (insert/delete/drag), Delete with
  referential integrity, Create Road (lane-profile templates + snapping UX),
  Lane Profile editor (add/remove lanes, type, width, road marks), editable
  Properties panel (manual binding), Elevation editor (node z, cubic-profile
  re-fit), and Create Junction (topology, connecting roads, lane links).
- **Undo/redo architecture** — kernel command layer (`edit::Command`,
  `EditStack`, restore-in-place generational arenas); every mutation is a
  command whose apply→revert leaves `write_xodr()` byte-identical, and a
  failed apply leaves the network untouched. The editor drives a single
  `QUndoStack`; drags collapse to one command on release.
- **Incremental re-mesh** — only dirty roads are re-meshed on edit, with a
  `mesh_changed` payload so the viewport updates without a full rebuild
  (issue #4).
- **Snapping queries** — grid, endpoint, and tangent-continuation snaps in the
  kernel, surfaced through the Create Road UX (issue #5).
- **Junction 3D blended surfaces** — watertight 2.5D harmonic-elevation
  junction surface (footprint → CDT → harmonic field → stitch), replacing the
  M1 flat plan-view floors (issue #18).
- **Junction `<planView>` + `<elevationGrid>` export** (OpenDRIVE ≥1.8) — the
  blended junction surface is written as an ASAM reference line and sampled
  elevation grid (issue #19). `<boundary>` export is deferred to M3 (issue #60).
- **OpenUSD (`.usda`) export** — ASCII OpenUSD exporter behind `RM_BUILD_USD`,
  backed by tinyusdz v0.9.1, validated in CI via `pxr.UsdUtils.ComplianceChecker`
  (issue #20).
- **Lucide icon set** — toolbar/action icons fetched from Lucide plus custom
  glyphs, tinted through `Icons::get()` (issue #7).

### Changed
- Kernel shared-library `SOVERSION`/`VERSION` bumped to `0.3`.

## [0.2.0] - 2026-07-10

### Changed
- **Editor migrated to Qt 6 Widgets** (LGPLv3, dynamic linking only),
  replacing the GLFW + Dear ImGui viewer. Editor logic moved into testable
  `document/` and model classes with headless (offscreen) tests.
- **Kernel buildable as a shared library** (`RM_BUILD_SHARED`) with install
  rules and a `find_package(roadmaker)` package; wheels continue to embed the
  static kernel.

### Added
- Platform installers and packaging for the Qt editor.

## [0.1.0] - 2026-07-10

First public release (Milestone 1) — geometric and standards correctness.

### Added
- **ASAM OpenDRIVE 1.6/1.7 reader** — line/arc/spiral/paramPoly3 plan views,
  lane sections/widths/offsets, elevation & superelevation, road marks, and
  junctions with resolved links. Nothing is silently dropped: skipped or
  coerced input becomes a structured diagnostic.
- **OpenDRIVE 1.7 writer** with pre-write validation (geometry continuity,
  lane-link consistency); deterministic output.
- **Clothoid authoring API** — fit a G1 clothoid path through waypoints
  (ebertolazzi/Clothoids), apply a lane profile, emit valid OpenDRIVE;
  round-trips hold at 1e-4 m / 1e-6 rad.
- **Mesh pipeline** — curvature-adaptive sampling, watertight per-road lane
  surfaces, per-lane-type materials, lane markings as separate primitives,
  plan-view junction floors (Clipper2 + CDT).
- **glTF 2.0 (`.glb`) export** — Y-up, meters, valid accessors.
- **Python bindings** (`pip install`) — full kernel coverage with pythonic
  errors and reprs, plus runnable examples.
- **Read-only editor** — OpenDRIVE viewer with 3D viewport, scene tree,
  inspector, and diagnostics log.

[0.3.0]: https://github.com/Robomous/RoadMaker/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Robomous/RoadMaker/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Robomous/RoadMaker/releases/tag/v0.1.0
