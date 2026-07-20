# Roadmap realignment — 2026-07

*P4 restructure around three behavior areas, cross-cutting gap owners, and
three new product requirements: toolbar information architecture (R1),
external asset import + Library file explorer (R2), ASAM-first persistence
layers + native project format (R3).*

**Status: APPROVED — maintainer approved the full delta 2026-07-20**,
answering all nine open questions with the recommended options (see §5,
where each question now records its resolution). Tracking issue: #315.
This document is the durable record; the GitHub operation list in §3 was
executed as approved.

---

## 1. Findings (Phase A, read-only analysis)

### 1.1 Toolbar (grounds R1)

- The main toolbar is a **hardcoded action sequence**: `MainWindow::build_toolbar()`
  at `editor/src/app/main_window.cpp:701-743` spells out every button with
  `addAction`; grouping is three manual `addSeparator()` calls
  (`main_window.cpp:715,737,740`) partitioning File | Tools | Merge/Library | Camera.
- **29 actions today**: 4 file (`new_file`,`open`,`save`,`export_glb`),
  21 tools (20 exclusive `ToolId` tools + `lane_width_editor`,
  `main_window.cpp:716-736`), `merge_roads`+`add_from_library`,
  `reset_camera`+`frame_selection`. A second `QToolBar`
  (`toolbar.options`, `main_window.cpp:749-753`) is the contextual options
  strip, not a tool row.
- **No category metadata exists anywhere**: the `Tool` base class carries only
  behavioral data (`editor/src/tools/tool.hpp:109-169`), `ToolId` is a bare
  enum (`tool.hpp:19-40`), and the `QAction` metadata in
  `editor/src/app/actions.cpp` has no grouping field. The only "grouping" is
  the mutual-exclusion `QActionGroup` and the manual separators.
- **Shortcut registry** (`editor/src/app/shortcut_registry.cpp:12-193`, one
  `constexpr std::array kTable`): entries already carry a `category` string,
  but it is a *documentation section* (File/Edit/Tools/View/Help) used only
  by the generated `docs/user-guide/shortcuts.md`
  (`shortcut_registry.cpp:226-254`), and the table **omits shortcut-less
  toolbar actions** (`export_glb`, `merge_roads`, `add_from_library`,
  `reset_camera` have no `Id`). It is CI-gated by
  `editor/tests/test_shortcut_registry.cpp` (binding test at :80, doc gate
  `MarkdownMatchesCommittedShortcutsPage` at :101).
- **Overflow**: nothing custom; Qt's default extension arrow only. Both bars
  `setMovable(false)`.
- **Projection**: 29 actions today + P4 (Stopline, Maneuver affordances,
  Signal, Sign ≈ 4–5) + P5 (Surface, Road Construction, terrain brushes ≈ 3)
  + P7 (two export-preview tools) + P8 (actor/route/storyboard ≈ 3–4) ≈
  **40+ post-P8**. One row does not survive that.

**Mechanism conclusion:** the shortcut-registry pattern (single constexpr
table + binding gtest + CI doc gate) is the right one, and the table can be
**extended into a full action registry**: add `Id`s for the four uncovered
toolbar actions, add `toolbar_row` / `toolbar_group` / `toolbar_order`
fields, generate `build_toolbar()` from it, and add a gtest that fails when
any tools-group action lacks a group. A new tool then *cannot* land
uncategorized. No second table needed.

### 1.2 Library & project model (grounds R2)

- Manifest v1 `assets/library/manifest.json`, parser
  `editor/src/document/library_manifest.cpp:102-184`; items
  `{key,label,category,thumbnail,create{kind,…}}` with kinds
  `road_template/road_style/assembly/tree/signal/marking/material/crosswalk/stencil/prop_set`
  (`:19-51`); verbatim `create_raw` gives forward-compatible round-trip
  (`library_manifest.hpp:96-98`). Note: an **`assembly` kind already parses**.
- `project.json` v1 (`editor/src/document/project.cpp:38-50`):
  `project_version` + `name` only. Scenes = non-recursive top-level `*.xodr`
  glob (`project.cpp:102-115`). A **per-project library overlay path is
  already defined** (`project.cpp:117-124`) and the from-disk
  `LibraryManifest::load(path)` exists but nothing calls it — the P6
  discovery names this the seam for per-project assets.
- Thumbnails: pre-rendered qrc PNGs (`library_list_model.cpp:17-44`) plus
  runtime-painted previews for crosswalks/stencils
  (`crosswalk_item.cpp:83`, `stencil_item.cpp:68`); glyph fallback
  (`library_panel.cpp:58-65`). **No 3D-render-to-thumbnail pipeline.**
- Drag MIME: `application/x-roadmaker-library-item`, payload = item key
  (`library_list_model.hpp:21`, produced `:172-180`; consumed
  `viewport_widget.cpp:829,863,876` and Attributes `slot_widget.cpp:65,98`).
- **No filesystem watcher anywhere** in `editor/` or `core/` (zero grep
  hits); `Project::scenes()` re-globs on every call.

### 1.3 IO formats (grounds R2)

- Read+write: OpenDRIVE 1.4–1.9 (`core/src/xodr/`, `reader.hpp:27-33`,
  `writer.hpp:41-61`).
- Write-only: GLB via tinygltf (`core/src/io/gltf_exporter.cpp:304-309`,
  fully embedded); USDA via tinyusdz behind `RM_BUILD_USD`
  (`usd_exporter.cpp:175`). `core/src/io/` contains **no readers at all**.
- **No OBJ support** (the `.obj` files under `assets/library/props/` are
  generator reference exports, never loaded — `scripts/gen_prop_meshes.py:510`).
- **Props have no file→mesh load path**: geometry is compiled-in procedural
  data (`core/src/assets/prop_meshes.gen.cpp` via `props::model()`,
  `prop_library.hpp:46-50`). Import must create the file-backed path.
- **Materials are compiled-in C++** (`editor/src/render/material_catalog.cpp:11-58`;
  the header notes the manifest-v2 `materials[]` block was deferred,
  `material_catalog.hpp:8-11`). Textures decode via Qt `QImage`
  (`viewport_widget.cpp:123-135`); bundled textures are CC0 JPEG/PNG.
- Deps (cmake/deps.cmake): tinygltf 3.0.0 **MIT, already in-tree — its read
  side is simply unused**; tinyusdz 0.9.1 Apache-2.0 (USDA write); stb
  disabled; pugixml for XML.

### 1.4 Persistence (grounds R3)

- **Save/Open is a bare `.xodr`** (`document.cpp:103-131`, `:52-87`).
  Autosave exists but writes only a recovery `.xodr` + a status sidecar
  `{originalPath, dirty, saveToken, writtenMs}` (`autosave.cpp:126-157`) —
  no scene state.
- **`rm:*` userData inventory** (writer.cpp / reader.cpp): `rm:waypoints`
  (929 / 257-287), `rm:crosswalk` (469-499 / 916-943), `rm:markingCurve`
  (499-540 / 949-1003), `rm:stencil` (540-546 / 832), `rm:aux_boundary`
  (1006-1045 / 177-182), `rm:arms` (1091-1118 / 1377-1471), `rm:corners`
  (1130-1181 / 1296-1391), `rm:junction` (1183-1205 / 1397-1434),
  `rm:surface` (1547-1584 / 1474-1510), plus `rm:<name>` material ids on
  `<material>`/`<roadMark>` (`lane.hpp:101-134`). Ten codes, each invented
  ad hoc — exactly the multiplication R3 predicts.
- **Round-trip preservation gaps** (Layer-1 obligations): foreign userData
  survives via `RawXml` on lane/object/signal/roadMark
  (`reader.cpp:588,842,908,1227`) but is **dropped on `<junction>`**
  (`reader.cpp:1436-1440`) and **silently lost at root level**
  (`reader.cpp:1482`, `:1604-1612`).
- **State lost on save/reload** (seeds Layer 2): camera/orbit state
  (`nav_controller.hpp:4-6` — never persisted), selection, active tool,
  snapping settings (tool-local), undo stack (cleared on load,
  `document.cpp:72,92`), diagnostics. Per-machine QSettings only (not
  per-scene): window layout, recents, theme, autosave flag, render mode
  (`settings.hpp:15-49`). Material/style *definitions* live at app/project
  level, never in the scene.

### 1.5 Signals & junction model (grounds the P4 restructure)

- `Signal` struct with full OpenDRIVE §14 IO and verbatim preservation
  exists (`core/include/roadmaker/road/signal.hpp:24-63`; reader
  `:1135-1230`, writer `:732-789`; `<signalReference>` raw-preserved).
  Kernel ops `add/delete/move_signal` (`edit/operations.hpp:745-766`);
  editor renders, selects, moves, deletes signals
  (`properties_panel.cpp:238-240`) — but there is **no signal authoring
  tool**.
- **`<controller>`/`<control>` do not exist in the model** (zero hits in
  `core/`). Normatively (OpenDRIVE 1.9.0 §14.6): controllers map dynamic
  signals to signal groups; *"Dynamic content like the signal cycle itself
  is specified outside of this standard (i.e. in OpenSCENARIO)."* ⇒ signal
  groups/junction gates get the **standard** carrier; phase *timing* needs
  an `rm:` carrier (Layer 1) and exports to OpenSCENARIO 1.x
  (`TrafficSignalController`) in P8.
- `Junction` (`junction.hpp:68-99`): `connections`, `arms` (generator
  input), `corners` + `default_corner_radius` + `material` (p4-s1/s2).
  **No locked/manual concept** — the only distinction is arms-present
  (regenerable) vs arms-empty (foreign, "recreate to edit",
  `document.cpp:200-201`). **No Stopline entity** — today's only stop line
  is a crosswalk companion `<object type="roadMark" subtype="signalLines">`
  (`edit/markings.hpp:143`, `CrosswalkStopLineTool`). **No Maneuver
  entity** — junction movements are `<connection>`+`<laneLink>` referencing
  real connecting roads (`writer.cpp:1048-1084`), regenerated in place from
  `arms`.
- Junction fill is the deduplicated backend
  (`core/src/mesh/junction_surface.cpp`, `junction_corner_detail.cpp`) with
  **no per-road-span structure** — confirming §3.1.B opens a real black box.
- Georeferencing: `<geoReference>` lives in `<header>` per OpenDRIVE 1.9.0
  §8.5 (with `<offset>`); **no support anywhere** in reader/writer today.

### 1.6 GitHub state (verified live 2026-07-20)

- Milestones 8–15 = P1–P8. Open issues: P1 {epic #250}, P2 {#251, #297},
  P3 {#252}, P4 {#253, #227 #228 #229 #230, #311}, P5 {#254, #231–#234},
  P6 {#255, #307}, P7 {#256, #241–#244, #313}, P8 {#257, #245–#249};
  #268 un-milestoned chore.
- P4 epic checklist: #225 ✔ #226 ✔; #227–#230 open, all board-Todo. Board
  statuses are consistent (epics P1/P2/P3/P4/P6 In Progress, rest Todo).
- **GW-4 preconditions reference "maneuver roads (P4-s3)"** — must be
  renumbered by this realignment.
- **No golden workflow mentions toolbar buttons** ⇒ R1 needs no GW
  amendments.
- **GW-1's results table is a partially filled row** ("July 15, 2026 |
  MacOS | — | — | no runs yet") — the pass is not recorded (open question
  Q8).
- ADRs live in `docs/decisions/` numbered `000X-slug.md` (next free: 0008);
  there is no `docs/adr/` (open question Q1).
- There is **no P4 discovery report** (`docs/roadmap/pillars/` has p1, p2,
  p6 only) — the roadmap says one is written per pillar before its first
  sprint; P4 started without one. This realignment adds it (§2.4).

---

## 2. Docs delta (one PR, `docs/` only)

1. **`docs/roadmap/README.md`**
   - P4 scope row → the three behavior areas in their imposed order:
     *junction control* (stoplines, locked junctions, membership/merge,
     parallel-road and single-road junctions), *junction interior surface
     control* (spans, Include Samples, Sort Index), *maneuvers +
     signalization + signs* (Signal tool, Phase Editor in the 2D Editor
     pane, Sign tool).
   - P6 scope row += Library as project file explorer, texture/mesh import
     pipelines, assemblies (composite props; signal linkage stays P4).
   - P7 scope row names **georeferencing**: world CRS by WKT / proj-string,
     world origin (lat/lon), workspace extents, center/fit-to-selection;
     imports reproject into the world frame; export records
     `<header><geoReference>` per OpenDRIVE §8.5.
   - P8 scope row names **OpenSCENARIO 1.x XML first** (internal scenario
     model → `.xosc` 1.x export, validation-friendly / esmini-compatible)
     and **OpenSCENARIO 2.x as an explicit later sprint** with an honest
     v0.1.0 scope (see p8-s6 and Q6).
   - Cross-pillar workstream section: add **`fmt`** (persistence layers /
     native format; label `fmt` + owning pillar's) as the second workstream
     after `help`.
   - Mermaid: add `FMT[fmt — persistence layers]` with `P6 --> FMT`,
     `FMT --> P7`, `FMT --> P8`; note under Sequencing that `p1-s5`
     (toolbar IA) precedes the P4 tool wave.
2. **NEW `docs/decisions/0008-persistence-layers-asam-first.md`** (ADR-0008)
   — full draft in Appendix B. Resolves cross-cutting gap 3.
3. **NEW `docs/roadmap/updates/2026-07-realignment.md`** — this document.
4. **NEW `docs/roadmap/pillars/p4_discovery.md`** — P4 discovery report:
   the §1.5 findings, the sprint cut in §3, and the **design-once stopline
   entity** section (cross-cutting gap 1: one kernel entity serving
   crosswalk stop lines, junction default stoplines, maneuver termination,
   and corner start distance).
5. **`docs/roadmap/golden_workflows/gw4_signals.md`** — precondition
   "maneuver roads (P4-s3)" → "(p4-s6)". No other GW touches needed.

## 3. GitHub delta — exact operation list

**Creates** (all added to the project board as Todo; full bodies in
Appendix A):

| # | Issue | Milestone | Labels |
|---|---|---|---|
| C1 | `p1-s5: toolbar information architecture — two categorized rows` | P1 | `pillar:P1` |
| C2 | `p4-s3: stopline entity + per-road default stoplines` | P4 | `pillar:P4` |
| C3 | `p4-s4: junction control — locked junctions, membership, merge, span junctions` | P4 | `pillar:P4` |
| C4 | `p4-s5: junction interior surface control — spans, Include Samples, Sort Index` | P4 | `pillar:P4` |
| C5 | `p6-s7: Library file explorer over project asset folders` | P6 | `pillar:P6` |
| C6 | `p6-s8: texture→material and mesh→prop import pipelines` | P6 | `pillar:P6` |
| C7 | `p6-s9: assemblies — composite prop assets in the Library` | P6 | `pillar:P6` |
| C8 | `p7-s5: world georeference settings + reprojection frame` | P7 | `pillar:P7` |
| C9 | `fmt-s1: native project/scene container (persistence Layer 2)` | P6 | `fmt`, `pillar:P6` |
| C10 | `fmt-s2: round-trip hardening — foreign userData preservation + rm: registry tests` | P6 | `fmt`, `pillar:P6` |
| C11 | `p8-s6: OpenSCENARIO 2.x — export-only concrete-scenario subset` | P8 | `pillar:P8` |
| C0 | label `fmt` (repo label, color matching `help`) | — | — |

**Edits** (no closures; open unmerged sprints are rescoped in place, so no
issue numbers are burned — ground rule 4 only freezes *merged* sprints):

- **#227** → retitle `p4-s6: maneuver roads — derivation, editing, Turn
  Type`; replacement body in Appendix A (its "custom-junction groundwork"
  half moves to p4-s3/s4/s5).
- **#228** → retitle `p4-s7: Signal tool — auto-signalize templates +
  linked assemblies`; body gains: assemblies model dependency on p6-s9
  (C7); signal groups/junction gates export as standard
  `<controller>`/`<control>` (OpenDRIVE §14.6).
- **#229** → retitle `p4-s8: Signal Phase Editor in the 2D Editor pane`;
  body gains: phase timing persists as `rm:phases` per ADR-0008 (§14.6
  places signal cycles outside OpenDRIVE) and exports to OpenSCENARIO 1.x
  in P8.
- **#230** → retitle `p4-s9: Sign tool with editable text` (title only).
- **#245** (p8-s1) → body names the target explicitly: internal scenario
  model with **OpenSCENARIO 1.x XML export** as the Layer-0 format.
- **Epic #250**: add `p1-s5` to the checklist; status line notes it must
  land before p4-s3.
- **Epic #253**: scope rewritten to the three behavior areas + internal
  order; checklist becomes #225 ✔, #226 ✔, C2, C3, C4, #227(p4-s6),
  #228(p4-s7), #229(p4-s8), #230(p4-s9); explicit deliverable line: "the
  stopline entity is designed once in the P4 discovery and lands in
  p4-s3"; references ADR-0008 for every new `rm:` carrier.
- **Epic #255**: checklist adds C5, C6, C7; a Workstreams section adds C9,
  C10 (`fmt`); ownership note: "assemblies: model + Library here (p6-s9);
  signal linkage in P4 (p4-s7)".
- **Epic #256**: scope names georeferencing; checklist adds C8 with the
  execution-order note `p7-s1 → p7-s5 → p7-s2 → p7-s3 → p7-s4` (IDs keep
  creation order; the epic states execution order — see Q9).
- **Epic #257**: scope names OSC 1.x and 2.x; checklist adds C11.
- **Tracking issue** (this one): closed with a summary comment after Phase C.

## 4. Sequencing update

- **R1 before the P4 wave**: `p1-s5` is the next sprint after approval,
  before `p4-s3`.
- **P4 internal order** (docs-imposed): s3 stoplines → s4 junction control
  → s5 interior surface → s6 maneuvers → s7 signals → s8 phases → s9 signs.
  That is **7 remaining P4 sprints** — at the top of the "roughly 6–8"
  bound, flagged per instructions; Q3 offers the merge option.
- **fmt**: fmt-s1/fmt-s2 are parallel-track; fmt-s1 must land before P8's
  scenario save (scenario files live in the container) and ideally before
  p7-s5 (workspace extents are Layer-2 data); fmt-s2 is independent and can
  land any time (it is pure kernel round-trip hardening).
- **Mermaid**: add `P6 --> FMT`, `FMT --> P7`, `FMT --> P8` (the only new
  edges).
- **Revised critical path to v0.1.0**: `p1-s5 → p4-s3…s9 (7) → p8-s1…s6
  (6)` = **14 sprints on the critical path**; P5 (4), P7 (5), P6 (3), fmt
  (2) run off it. Total open sprints after this delta: 28.

## 5. Open questions — all RESOLVED 2026-07-20 with the recommended answer

Every question below was answered by the maintainer with the
recommendation (kept verbatim as the record of the alternatives that were
on the table). Net resolutions: ADR in `docs/decisions/` (Q1);
edit-in-place for #227–#230 (Q2); P4 keeps 7 remaining sprints (Q3); `fmt`
owned by P6 (Q4); directory + per-scene sidecar container (Q5);
OpenSCENARIO 2.x export-only subset (Q6); glTF/GLB + PNG/JPEG import only
(Q7); GW-1 macOS pass recorded at commit a9734d8 (Q8); P7 execution order
stated in the epic (Q9).

- **Q1 — ADR location.** The prompt says `docs/adr/ADR-XXXX-…`; the repo
  convention is `docs/decisions/000X-slug.md` (0001–0007 exist).
  **Recommend:** `docs/decisions/0008-persistence-layers-asam-first.md`.
- **Q2 — #227–#230 handling.** Edit-in-place retitle (keeps numbers,
  history, board rows; no closures) vs close-as-superseded + recreate.
  **Recommend:** edit in place, as listed in §3.
- **Q3 — P4 at 7 remaining sprints.** Within the flagged 6–8 band.
  **Recommend:** keep 7; if you want 6, merge p4-s3 into p4-s4 (stoplines +
  junction control as one large sprint).
- **Q4 — `fmt` workstream owner.** **Recommend P6** (the container extends
  p6-s1's project model; house rule gives workstreams the owning pillar's
  label, like `help` + `pillar:P2`). Alternative: P7 (it is IO-shaped).
- **Q5 — Layer-2 container shape.** **Recommend versioned project
  directory**: `project.json` v2 + a per-scene sidecar
  (`<scene>.rmscene.json` next to its `.xodr`). Git-friendly, diffable,
  keeps the `.xodr` pure and standalone-openable, matches the existing
  project-as-folder model. Alternative (rejected for now): single-file
  archive embedding the ASAM payload — revisit post-v0.1.0 as a
  "package as single file" export.
- **Q6 — OpenSCENARIO 2.x at v0.1.0.** **Recommend export-only**: a
  hand-rolled emitter for a documented concrete-scenario subset generated
  from the same internal model; **no OSC2 import** and no OSC2 parser
  dependency at v0.1.0 (avoids the ANTLR-grammar/tooling license question
  entirely; if import is ever wanted, that dependency review comes to you
  first per the dependency policy).
- **Q7 — Mesh/texture import formats (p6-s8).** **Recommend:** glTF/GLB
  read (tinygltf's read side, MIT, already in-tree — near-free) + images
  via Qt (PNG/JPEG guaranteed). **OBJ: out** unless you want a new pinned
  dep (tinyobjloader, MIT) — not "cheap" enough to smuggle in. **USD read:
  out** for v0.1.0 (tinyusdz can read, but validating arbitrary USD input
  is a project of its own).
- **Q8 — GW-1 record.** The realignment context says the GW-1 hand-run
  passed, but `gw1_camera.md`'s results table row is incomplete ("no runs
  yet"). **Recommend:** record the actual result (date/OS/commit/pass) in
  the same docs PR — or tell me it hasn't actually passed and P1 stays
  ~80%.
- **Q9 — P7 ordering.** New sprint IDs keep creation order (p7-s5) with the
  epic stating execution order (s1 → s5 → s2 → s3 → s4). Alternative:
  renumber the open, unmerged P7 sprints so IDs match execution order.
  **Recommend:** keep IDs, state order in the epic (less churn, no board
  edits).

---

## Appendix A — full issue bodies

### C1 — `p1-s5: toolbar information architecture — two categorized rows`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
The single main toolbar (29 actions, hardcoded in
`MainWindow::build_toolbar()`) becomes **two categorized rows generated
from the action registry**, so related tools cluster and later pillars
(P4/P5/P7/P8, projected 40+ actions) land without re-shuffling:

- Extend the shortcut registry (`editor/src/app/shortcut_registry.cpp`,
  the single `constexpr` table) into a full **action registry**: add `Id`s
  for the shortcut-less toolbar actions (`export_glb`, `merge_roads`,
  `add_from_library`, `reset_camera`) and add `toolbar_row` /
  `toolbar_group` / `toolbar_order` fields alongside the existing
  doc-section `category`.
- `build_toolbar()` becomes a loop over the registry; separators derive
  from group boundaries. The contextual options bar stays as-is below.
- **Row 1 — network authoring:** File (New/Open/Save/Export) · Edit
  (Select, Move, Split, Delete, Merge Roads) · Roads (Create Road, Edit
  Nodes, Create Junction, Corner, Elevation; reserves future junction
  affordances) · Lanes (Lane Profile, Lane Add, Lane Form, Lane Carve,
  Lane Width Editor).
- **Row 2 — scene layers:** Markings (Crosswalk & Stop Line, Marking
  Point, Marking Curve) · Props (Prop Point, Prop Curve, Prop Span, Prop
  Polygon) · Terrain & Structures (reserved: Surface, Road Construction,
  heightmap) · Signals & Signs (reserved: Signal, Sign, Phase Editor) ·
  Scenario (reserved: P8 tools) · Library & View (Add From Library, Reset
  Camera, Frame Selection).
- Groups reserved for future pillars render nothing until their first
  action registers — the taxonomy is fixed now so no later re-shuffle.
- A gtest (same pattern as the shortcut doc gate) fails when any
  tools-group action has no `toolbar_group` — a new tool cannot land
  uncategorized.
- Overflow: keep Qt's extension-arrow behavior per row; set a sensible
  `minimumWidth` so both rows stay usable at small window sizes.

## Refactorings
- `editor/src/app/main_window.cpp:701-743` — replace the hardcoded
  `addAction` sequence with registry-driven generation.
- `editor/src/app/shortcut_registry.*` — new fields + rows; regenerate
  `docs/user-guide/shortcuts.md` if section labels shift.

## Acceptance
- Two categorized rows render with the taxonomy above; every existing
  action, shortcut, and checkable-tool behavior is unchanged.
- The category gtest fails on an uncategorized tools-group action (verified
  by a negative test).
- Before/after screenshots in the PR (visual-quality rule).
- No golden-workflow amendments needed (no GW names toolbar buttons —
  verified 2026-07-20).

## Out of scope
New tools; user-customizable toolbars; icon redesign; options-bar changes.

## Supersedes
(none)
```

### C2 — `p4-s3: stopline entity + per-road default stoplines`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md) · designed once in [p4_discovery](../pillars/p4_discovery.md).

## Scope
A first-class kernel **stopline** entity — designed once, consumed by
crosswalk stop lines (P3), junction default stoplines (here), maneuver
termination (p4-s6), and corner start distance:

- Kernel record per road end: **Distance** attribute (meters from the road
  end), flippable direction; derived **default stoplines** appear at every
  junction arm.
- Editing: Distance in the Attributes pane (typed + scrub), flip action,
  and drag-to-move along the road (command-layer drag session; one command
  on release).
- Persistence per ADR-0008: the geometric mark stays the standard
  `<object type="roadMark" subtype="signalLines">` (Layer 0 — the carrier
  the crosswalk stop line already uses, `edit/markings.hpp:143`); the
  parametric link (owning road end, distance, direction) rides `rm:stopline`
  userData (Layer 1).
- Migrate the crosswalk-companion stop line onto the shared entity so there
  is exactly one stopline implementation.

## Refactorings
- `CrosswalkStopLineTool` + `edit::markings` stop-line authoring move onto
  the shared kernel entity.

## Acceptance
- A four-arm junction exposes four default stoplines; setting Distance,
  flipping, and dragging each round-trip through save/reload and are single
  undo steps (apply→revert leaves `write_xodr()` byte-identical).
- Existing crosswalk + stop line flows (GW-5 steps) still pass.
- Hand check: drag a stopline 5 m up an approach, flip it, save, reload —
  position and direction survive; a third-party OpenDRIVE viewer still
  shows the stop-line mark (Layer-0 degradation).
- Fuzz corpus gains an `rm:stopline` sample.

## Out of scope
Locked junctions and drag-when-locked interactions beyond a plain road end
(p4-s4); signal linkage (p4-s7).

## Supersedes
(none)
```

### C3 — `p4-s4: junction control — locked junctions, membership, merge, span junctions`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
Automatic junctions remain the default; this sprint adds explicit control:

- **Manual junction** between non-overlapping roads: pick road ends →
  confirm → junction forms.
- **Convert automatic ↔ locked**; converting back removes the junction when
  no automatic junction is derivable from the geometry.
- **Add/remove roads** to/from a locked junction; **merge** two junctions.
- **Junction between two parallel roads** defined by per-road s-spans.
- **Junction along a single road** — the enabler for standalone mid-road
  crosswalks (coordinates with the P3 Crosswalk & Stop Line tool).
- **Corner re-derivation action** for troubleshooting locked-junction
  corners.
- Stopline drag-to-move on locked junctions activates here (entity from
  p4-s3).
- Persistence per ADR-0008: locked state and s-span membership extend the
  junction's Layer-1 carriers (`rm:arms` / `rm:junction`).

## Refactorings
- `Junction` (`junction.hpp:68-99`) gains lock state + span-arm inputs;
  `edit::regenerate_junction` / `Document` regeneration
  (`document.cpp:186-244`) learns to respect the lock instead of always
  re-deriving from `arms`.

## Acceptance
- Every operation above is one `edit::Command` (apply→revert
  byte-identical; failed apply leaves the network untouched).
- Hand script: build two crossing roads (auto junction) → lock → add a
  third road → merge with a neighboring junction → convert back where
  derivable; exported junctions validate; save→reload→save byte-identical.
- Hand check: a junction spanning two parallel roads over given s-ranges,
  and a single-road junction hosting a standalone crosswalk.
- Fuzz corpus gains locked-junction samples.

## Out of scope
Interior surface spans (p4-s5); maneuvers (p4-s6); signalization.

## Supersedes
(absorbs the "custom-junction groundwork" half of #227's original scope)
```

### C4 — `p4-s5: junction interior surface control — spans, Include Samples, Sort Index`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
The junction fill backend (`core/src/mesh/junction_surface.cpp`) is a
black box today; this sprint gives users the escape valve for interior
triangulation artifacts:

- **Visualize** the individual road surface spans overlapping a junction,
  with their samples, when the junction is selected in the (new) surface
  inspection mode.
- Per-span **Include Samples** toggle.
- Per-span **Sort Index** with Raise/Lower actions; where spans overlap,
  the higher index wins.
- Persistence per ADR-0008: span records (include flag + sort index) ride a
  Layer-1 `rm:` carrier on the junction.

## Refactorings
- `junction_surface.cpp` fill backend gains per-road-span structure (it
  currently has none) feeding both the mesher and the visualization.

## Acceptance
- Hand script: construct an overlap artifact (skewed four-arm junction),
  open the span view, Raise the correct span — the artifact resolves;
  toggle Include Samples — triangulation changes accordingly; both are
  single undo steps and round-trip through save/reload.
- Meshes stay watertight (existing invariants); before/after screenshots in
  the PR.

## Out of scope
New triangulation algorithms; maneuvers; terrain coupling (P5).

## Supersedes
(none)
```

### Edited #227 — `p4-s6: maneuver roads — derivation, editing, Turn Type`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
A **maneuver** entity: an editable path through a junction that does not
affect road geometry but carries traffic semantics, exporting as standard
OpenDRIVE connecting roads + `<connection>`/`<laneLink>` (the writer's
existing representation, `writer.cpp:1048-1084`):

- Auto-created and recomputed per junction (toggleable, with an explicit
  rebuild action).
- Endpoint lines constrained to slide along their anchor lanes with fixed
  end directions; add/move/insert control points on the path.
- Per-maneuver **Lock Geometry** — implicit on manual edit, explicit
  checkbox.
- **Convert to Explicit** for U-turn refinement (the zero-width-lane
  technique via Lane Add already works and stays the mechanism).
- Semantic **Turn Type** attribute: computed, user-overridable; feeds
  signalization (p4-s7/p4-s8).
- Persistence per ADR-0008: geometry is Layer 0 (connecting roads); lock
  state, control points, and Turn Type override ride `rm:maneuver`
  (Layer 1).

## Acceptance
- GW-4 preconditions hold: a four-arm junction exposes its maneuver roads,
  selectable and highlightable.
- Exported junction connectivity validates; locked geometry survives
  junction regeneration and save/reload; every edit is one undo step.
- Hand check: refine a U-turn maneuver via Convert to Explicit; override a
  Turn Type and see it persist.

## Out of scope
Signalization (p4-s7); projecting maneuvers onto elevation data — deferred
until elevation-map assets exist (P5 heightmaps / P7 imports), sequenced
explicitly there.

## Supersedes
The original #227 scope: its "custom-junction groundwork" half moved to
p4-s3/p4-s4/p4-s5.
```

### C5 — `p6-s7: Library file explorer over project asset folders`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
The Library Browser doubles as a **file explorer over the project's asset
folders** (the p6-s1 project model already defines project-as-folder):

- A folder tree mirroring the on-disk hierarchy under the project's asset
  folders, alongside the existing manifest-driven catalog.
- A filesystem watcher (`QFileSystemWatcher` — none exists anywhere today)
  refreshing entries on create/delete/rename without restart.
- Per-type previews/thumbnails: images via `QImage` decode; existing
  runtime-painted previews (crosswalk/stencil) and qrc PNGs keep working;
  glyph fallback for unknown types.
- Read-side only; import pipelines are p6-s8.

## Refactorings
- `library_panel` / `LibraryListModel`: split the manifest-driven catalog
  model from the new filesystem-tree model; wire the currently-uncalled
  from-disk `LibraryManifest::load(path)` overlay seam
  (`project.cpp:117-124`).

## Acceptance
- Creating/deleting/renaming a file under the project's asset folders is
  reflected in the Library without restart (headless gtest with a temp
  project dir + the hand check below).
- Existing catalog assets (CC0 starter, styles, markings) keep working and
  keep the `application/x-roadmaker-library-item` drag MIME behavior.
- New item models ship their `QAbstractItemModelTester` gtest in the same
  commit (testing standard).
- Hand check: drop a PNG into the project's asset folder from the OS file
  manager; it appears in the tree with a thumbnail (importing it into a
  material is p6-s8).

## Out of scope
Import pipelines (p6-s8); assemblies (p6-s9); drag-in from the OS into the
viewport.

## Supersedes
(none)
```

### C6 — `p6-s8: texture→material and mesh→prop import pipelines`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
Users bring their **own** assets into a project:

- **Textures/images → material assets**: any Qt-decodable image (PNG and
  JPEG guaranteed) imports as a PBR-lite material in the project's library
  overlay, with thumbnail generation. Requires material definitions to
  become data-driven (the deferred manifest-v2 `materials[]` block,
  `material_catalog.hpp:8-11`) so project materials extend the compiled-in
  catalog.
- **3D models → prop assets**: **glTF/GLB read** via the in-tree tinygltf
  (MIT; its read side is currently unused) converting to the prop mesh
  representation. Requires a file-backed model path beside the compiled-in
  procedural table (`props::model()`, `prop_library.hpp:46-50`). Imported
  props work with the existing prop tools, Prop Sets, and the instanced
  rendering fast path.
- **Drag-in import**: dropping a supported file from the OS onto the
  Library panel imports it into the project. (Viewport drop: stretch,
  explicitly out unless trivial.)
- **FBX remains permanently excluded.** OBJ and USD read are out at
  v0.1.0 (maintainer question Q7 of the realignment; OBJ would need a new
  pinned dependency).

## Refactorings
- Prop model plumbing: `props::model()` id-based lookup grows a
  project-scoped, file-backed source; scene builder instanced batches key
  on it unchanged.
- Material catalog: compiled-in definitions + project-overlay definitions
  behind one lookup (`find_material`).

## Acceptance
- Hand script: import a PNG → drag the new material onto a lane → renders
  and exports (`<material>` Layer 0); import a GLB chair → place with Prop
  Point, add to a Prop Set, verify it draws in an instanced batch; both
  assets survive project close/reopen.
- Malformed inputs are rejected with structured diagnostics, never a crash
  (fuzz-adjacent tests on the importers).
- License note recorded per import (source path + user attestation field in
  the overlay manifest entry).

## Out of scope
OBJ/USD/FBX; 3D-render-to-thumbnail for props (glyph or first-frame
fallback acceptable); assemblies (p6-s9); marketplace/remote assets.

## Supersedes
(none)
```

### C7 — `p6-s9: assemblies — composite prop assets in the Library`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
Assemblies as first-class composite props (cross-cutting gap 2 — model +
Library support here; **signal linkage is P4's**, consumed by p4-s7):

- An assembly = multiple prop parts with relative transforms (e.g. pole +
  mast arm + signal heads), placed/moved/deleted as one unit.
- Library support: the manifest `assembly` kind already parses
  (`library_manifest.cpp:19-51`) — give it a real composite `create` schema,
  previews, and drag-to-place; author assemblies from existing props.
- Instanced rendering treats parts as batched instances (existing fast
  path).

## Acceptance
- Hand check: drag a signal-pole assembly into the scene — it places as one
  unit, moves as one unit, exports its parts as valid OpenDRIVE objects,
  and round-trips.
- p4-s7 can attach a logical signal to an assembly part (interface agreed
  with the P4 epic; the linkage itself ships in p4-s7).

## Out of scope
Signal linkage semantics (p4-s7); new starter-library content beyond one
demo assembly.

## Supersedes
(none)
```

### C8 — `p7-s5: world georeference settings + reprojection frame`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
World/geodetic settings as the foundation of GIS support (executes
**before p7-s2**; the epic states execution order):

- A **world projection** defined by standard CRS descriptions (WKT or
  proj-string), a **world origin** (latitude/longitude), and **workspace
  extents**; center / fit-to-selection actions operating in that frame.
- Imports (p7-s2/s3/s4) **reproject into the world frame**; exports record
  the georeference via `<header><geoReference>` (+ `<offset>`) per
  OpenDRIVE 1.9.0 §8.5 — read AND write (neither exists today).
- PROJ enters as the reprojection dependency here (permissive license;
  pinned tag + URL_HASH + THIRD_PARTY_LICENSES row in the same commit).
- Settings persist per ADR-0008: `<geoReference>` is Layer 0; workspace
  extents/UI framing are Layer 2.

## Acceptance
- Set a CRS + origin, save, reload: settings survive; exported `.xodr`
  carries a valid `<geoReference>`; importing a file with a `<geoReference>`
  adopts/reprojects per the world frame with structured diagnostics on
  mismatch.
- Hand check: fit-to-selection centers the workspace on the selection in
  the projected frame.

## Out of scope
Actual GIS layer import (p7-s2), lidar (p7-s3), OSM (p7-s4); datum
transformations beyond what PROJ provides out of the box.

## Supersedes
(none)
```

### C9 — `fmt-s1: native project/scene container (persistence Layer 2)`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md) · workstream `fmt`.

## Scope
Implement ADR-0008's Layer 2: the RoadMaker-native container for
everything that has no business inside an ASAM file.

- `project.json` v2 (versioned, forward-compatible like v1) + a per-scene
  sidecar `<scene>.rmscene.json` next to each `.xodr`.
- Sidecar carries (initial set, from the lost-state inventory): camera/view
  state, per-scene render mode, snapping settings, session hints; plus
  references — library/asset import metadata, prop-set definitions,
  material overlay references, world workspace extents (p7-s5).
- **Compatibility contract, enforced by tests**: open a pure `.xodr` →
  full editing, no sidecar required; save inside a project → sidecar
  written atomically (QSaveFile, like autosave); export ASAM → the `.xodr`
  alone is already Layers 0+1; a scene with a stale/missing sidecar opens
  cleanly with defaults.

## Refactorings
- `Document::save/load` (`document.cpp:52-131`) grow the sidecar hook;
  autosave (`autosave.cpp`) includes the sidecar in recovery.

## Acceptance
- Contract tests above (gtest, headless); save→reload restores camera and
  scene-scoped settings; deleting the sidecar loses only Layer-2 comfort,
  never scene content.
- Hand check: arrange a scene + camera, close, reopen the project — you are
  exactly where you left off; open the same `.xodr` standalone — it edits
  fine.

## Out of scope
Single-file archive packaging (post-v0.1.0 export option); cloud/sync;
undo-stack persistence.

## Supersedes
(none)
```

### C10 — `fmt-s2: round-trip hardening — foreign userData preservation + rm: registry tests`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md) · workstream `fmt`.

## Scope
Make Layer 1 airtight:

- **Preserve foreign userData everywhere.** Today it survives on
  lane/object/signal/roadMark via `RawXml` but is dropped on `<junction>`
  (`reader.cpp:1436-1440`) and silently lost at the root level
  (`reader.cpp:1482`, `:1604-1612`). Both become preserve-and-warn.
- **`rm:` code registry**: the ADR's registry table (all ten existing codes
  + new ones as they land) gets conformance tests — every code the writer
  emits has a parser, a fuzz-corpus sample, and a round-trip test; unknown
  `rm:` codes from a *newer* RoadMaker are preserved verbatim with a
  warning, never dropped.

## Acceptance
- A third-party `.xodr` with junction-level and root-level `<userData>`
  round-trips byte-preserved (new gtests + fuzz corpus entries).
- The registry conformance test fails when a new `rm:` writer lands without
  parser + corpus + round-trip coverage.
- Parser-never-silently-drops rule holds: every ignored construct emits a
  structured warning.

## Out of scope
New `rm:` carriers (their owning sprints define them); Layer-2 container
(fmt-s1).

## Supersedes
(none)
```

### C11 — `p8-s6: OpenSCENARIO 2.x — export-only concrete-scenario subset`

```markdown
Part of the [Road to Parity roadmap](https://github.com/Robomous/RoadMaker/blob/main/docs/roadmap/README.md) · [2026-07 realignment](../updates/2026-07-realignment.md).

## Scope
An honest v0.1.0 OpenSCENARIO 2.x story on top of the internal scenario
model (p8-s1, whose Layer-0 target is OpenSCENARIO 1.x XML):

- **Export-only**: emit a documented **concrete-scenario subset** of
  OpenSCENARIO 2.x from the same internal model that exports 1.x —
  a hand-rolled emitter, no parser and no new dependency.
- The supported subset (actors, placements, routes, the storyboard
  constructs the 1.x export covers) is documented in
  `docs/domain/`, with explicit "not supported at v0.1.0" list:
  **no OSC2 import**, no abstract/parameterized scenario constructs.
- Any future OSC2 *parser* dependency (grammar/tooling) goes through the
  dependency policy's stop-and-ask before it is even prototyped.

## Acceptance
- The GW-6 scenario (authored during P8) exports to both `.xosc` 1.x and
  the 2.x subset; the 1.x file validates against the schema; the 2.x file
  is reviewed against the documented subset (no schema validation without a
  parser dep — stated limitation).
- Round-trip is via the internal model + project container (ADR-0008), not
  via re-importing OSC2.

## Out of scope
OpenSCENARIO 2.x import; abstract scenarios; constraint solving; any OSC2
parser dependency.

## Supersedes
(none)
```

---

## Appendix B — ADR-0008

The accepted ADR lives at
[`docs/decisions/0008-persistence-layers-asam-first.md`](../../decisions/0008-persistence-layers-asam-first.md);
the draft below is preserved as proposed (the accepted version differs
only in status and minor wording).

```markdown
# ADR-0008: Persistence layers — ASAM first, RoadMaker enrichment on top

- **Status:** proposed
- **Date:** 2026-07-20
- **Deciders:** Armando Anaya

## Context

RoadMaker persists scenes as bare `.xodr` files; ten `rm:*` userData codes
(waypoints, crosswalk, markingCurve, stencil, aux_boundary, arms, corners,
junction, surface, material ids) have accreted one-by-one as features
needed carriers, with no written policy. P4 will add stoplines, locked
junctions, span sort indices, maneuvers, and signal phases; P6 adds
imported assets; P7 adds georeferencing; P8 adds scenarios. Meanwhile real
state is lost on reload (camera, selection, snapping, session state) or
stranded per-machine in QSettings, and foreign `<userData>` is dropped on
`<junction>` and at the root — a round-trip defect. Without a layering
decision, every sprint re-litigates "where does this datum live".

## Decision

Maximum ASAM compatibility comes first — OpenDRIVE, and OpenSCENARIO both
1.x and 2.x — with RoadMaker-specific enrichment layered on top **without
ever breaking pure-ASAM interchange**. Three layers:

**Layer 0 — pure ASAM (inviolable).** An exported `.xodr` (later `.xosc`
1.x, and the OSC 2.x subset) is always valid, self-contained, and
consumable by third-party tools with zero RoadMaker knowledge. Importing a
pure ASAM file authored elsewhere always works. Anything expressible in
the standard uses the standard: signal groups/junction gates use
`<controller>`/`<control>` (OpenDRIVE §14.6), georeference uses
`<header><geoReference>` (§8.5), maneuvers export as connecting roads.

**Layer 1 — ASAM-adjacent enrichment.** RoadMaker data annotating ASAM
entities travels inside the ASAM file via the standard extension
mechanism: namespaced `<userData code="rm:…">`. A RoadMaker export
round-trips losslessly through RoadMaker and degrades gracefully elsewhere
(other tools ignore userData). Policy:
- One `rm:` code per concern; payloads are versioned-by-shape
  (unknown fields warn, never fail).
- Every code is listed in the registry table below and ships with parser,
  writer, fuzz-corpus sample, and round-trip test (enforced by fmt-s2).
- Foreign userData (any non-`rm:` code) is preserved verbatim on every
  element — the current junction/root drops are defects to fix (fmt-s2).
- Unknown `rm:` codes (from a newer RoadMaker) are preserved verbatim with
  a warning.
- Signal-phase *timing* is Layer 1 (`rm:phases`): §14.6 places signal
  cycles outside OpenDRIVE ("specified … in OpenSCENARIO"); the phase data
  additionally exports to OpenSCENARIO 1.x traffic-signal actions in P8.

Registry (existing → planned): rm:waypoints, rm:crosswalk, rm:markingCurve,
rm:stencil, rm:aux_boundary, rm:arms, rm:corners, rm:junction, rm:surface,
rm:<material-id> → rm:stopline (p4-s3), rm:arms/rm:junction extensions for
locked state + spans (p4-s4), rm: span records (p4-s5), rm:maneuver
(p4-s6), rm:phases (p4-s8).

**Layer 2 — native project/scene container.** Everything with no business
in an ASAM file lives in the RoadMaker container: a **versioned project
directory** — `project.json` v2 plus a per-scene sidecar
`<scene>.rmscene.json` next to its `.xodr` — carrying editor/session state
(camera, snapping, render mode), library/asset references and import
metadata, prop-set and material-overlay definitions, workspace
extents/georeference framing, and (P8) scenario-editor state. Not a
single-file archive: the directory form is git-friendly, diffable,
partial-write-safe, and keeps every `.xodr` standalone-openable; a
single-file "package" can be added post-v0.1.0 as an export convenience.

**Compatibility contract** (tested, fmt-s1): open a pure `.xodr` → full
editing, no sidecar required; save inside a project → Layers 1+2 written;
export ASAM → Layers 0+1 only; a missing/stale sidecar degrades to
defaults, never blocks opening.

**OpenSCENARIO:** one internal scenario model; OSC 1.x XML export first
(validation-friendly, esmini-compatible); OSC 2.x as an export-only
concrete-scenario subset at v0.1.0 (p8-s6) with no parser dependency; OSC2
import deferred and gated on a future dependency review.

## Consequences

- Every sprint states, in its issue, which layer each new datum uses; no
  more ad-hoc carriers. The P4 epic references this ADR for all its
  carriers.
- fmt-s1 (container) and fmt-s2 (preservation hardening + registry tests)
  implement the enforcement.
- Third-party interchange can never regress silently: the contract tests
  and the registry conformance tests are CI gates.
- Cost: sidecar schema maintenance and one more file next to each scene —
  accepted as the price of keeping `.xodr` pure.
```
