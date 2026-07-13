# Phase 2 — Library panel & drag-and-drop creation (design notes)

Part of the M3a UI revamp (epic
[#108](https://github.com/Robomous/RoadMaker/issues/108), phase
[#112](https://github.com/Robomous/RoadMaker/issues/112)). Phase 2 pulls the
M4 Library Browser foundation forward and supersedes the flat read-only panel
of #50. Built in slices:

1. **Manifest + model** (shipped, #128): `assets/library/manifest.json` +
   headless `LibraryManifest` / `LibraryListModel`.
2. **Kernel assemblies** (shipped, #126): `edit::assembly::t_intersection` /
   `x_intersection`.
3. **Library panel** (this PR): the browsable dock.
4. **Drag-and-drop creation** (next, P2.4): `QDrag` source + viewport drop
   handler + the behaviour matrix.

## Library panel (this PR)

**Delivered:** a **Library** dock, tabbed with the Scene tree on the left
(Scene raised by default), holding a searchable icon grid over the
`LibraryListModel`:

- `editor/src/panels/library_panel.{hpp,cpp}` — a `QLineEdit` search box over a
  `QListView` in icon mode. A `LibraryFilterProxy` (`QSortFilterProxyModel`)
  filters case-insensitively on the label, sorts by category then label so the
  classes cluster (Assemblies, then Road templates), and injects a **themed
  class icon** for the grid's `DecorationRole` — reusing the bundled
  `template-rural/urban/highway` and `junction-connect` glyphs (tinted to the
  palette by `Icons::get`), so no thumbnail assets are needed for v1.
- The manifest is loaded from the Qt resource system (`:/library/manifest.json`,
  aliased to the committed data file) so it is always available in the built
  app, then handed to the `LibraryListModel` the dock renders.
- Wiring: `main_window.cpp` `build_docks` creates `dock.library`,
  `tabifyDockWidget(scene_dock_, library_dock_)`, adds a View-menu toggle, and
  the objectName lets the layout persist.

Screenshot mode gained `--raise-dock <objectName>` (`MainWindow::
raise_dock_for_capture`) so a whole-window capture can bring the Library tab to
the front; the CI `visual-artifacts` job renders it.

### Evidence

![The Library dock: searchable icon grid tabbed with the Scene tree](phase2_library_panel.png)

*T/X intersection assemblies and the three road templates, grouped by class
with a search box, tabbed with the Scene tree.*

## Drag-and-drop creation (P2.4)

**Delivered:** dragging a library item onto the viewport creates geometry.

- **Drag source** — `LibraryListModel` is drag-enabled (`Qt::ItemIsDragEnabled`
  + `mimeTypes`/`mimeData`); a dragged item carries its `key` as
  `application/x-roadmaker-library-item`. The panel's `QListView` is
  `DragOnly`.
- **Drop target** — `ViewportWidget` accepts that MIME type, shows a themed
  "drop here" crosshair ghost at the cursor during the drag, and on drop
  resolves the cursor to the ground plane and emits
  `library_item_dropped(key, world_x, world_y)`.
- **Behaviour matrix** — a pure, unit-tested `resolve_library_drop(item,
  network, x, y)` (`editor/src/document/library_drop.{hpp,cpp}`) maps the drop
  to an action, which `MainWindow` carries out:
  - **Road template →** arms Create Road with that profile and places the
    first waypoint at the drop point (`CreateRoadTool::begin_at`).
  - **T assembly →** `assembly::t_intersection` at the drop point (one undoable
    command) → success toast.
  - **X assembly →** `assembly::x_intersection` at the drop point → success
    toast.
- **Discoverability** — the empty-viewport context menu gained **"Add from
  library…"** (`Actions::add_from_library`), which raises the Library dock.

Screenshot mode gained `--drop-library <key>` (drives the real drop path); the
CI `visual-artifacts` job renders a drop.

### Evidence

![Dropping an X-intersection assembly onto the scene](phase2_dragdrop.png)

*An X-intersection dropped below an existing tee — the 4-way is created and a
"Placed X-intersection" success toast confirms it.*

### Deferred (fast-follow)

- **Tee INTO an existing road** — dropping a T on a road body should split it
  and tee a branch in (via `attach_t_junction`); v1 places a standalone
  assembly at the drop point. Needs a kernel `tee_road` factory (create branch
  stub + attach) — a follow-up.
- **Pre-rendered thumbnails** — the grid uses monochrome class glyphs;
  photographic per-item thumbnails can replace the `DecorationRole` later.
  Distinct T vs. X glyphs too (they share the junction glyph now).
- **Live ghost of the item shape** — the drag ghost is a crosshair; a T/X-shaped
  ghost is a polish follow-up.
