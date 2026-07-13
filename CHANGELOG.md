# Changelog

All notable changes to RoadMaker are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.0] - Unreleased

M3a opens with a topology-editing pass — moving whole roads, inserting bend
points, splitting and merging — plus interaction polish. Every change ships
through the M2 command layer (one undo step per edit, byte-identical undo) and
is headless-testable.

### Added
- **Library panel** (UI revamp Phase 2): a searchable **Library** dock, tabbed
  with the Scene tree, holding an icon grid over the catalogue — the road
  templates and T/X assemblies the drop handler (next PR) will place. A
  `LibraryFilterProxy` filters by label, groups by class, and injects a themed
  class icon (reusing the bundled template/junction glyphs — no thumbnail
  assets needed for v1); the manifest loads from the Qt resource system so the
  built app always has it. Screenshot mode gained `--raise-dock` (CI renders the
  panel). Design + before/after: `docs/design/ui-revamp/phase2_library.md`.
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
