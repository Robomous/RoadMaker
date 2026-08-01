/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Central QAction registry. Every menu/toolbar entry exists exactly once
// here; MainWindow only arranges them. Undo/redo come from the document's
// QUndoStack (M2 scaffolding — the stack is empty in M1).

#include <QAction>
#include <QActionGroup>
#include <QObject>
#include <QUndoStack>

#include "app/shortcut_registry.hpp"

namespace roadmaker::editor {

class Actions : public QObject {
  Q_OBJECT

public:
  explicit Actions(QUndoStack& undo_stack, QObject* parent = nullptr);

  /// The QAction a registry Id names. This is what lets MainWindow GENERATE
  /// the toolbar from shortcut_registry's taxonomy instead of hand-placing
  /// buttons. Implemented as an exhaustive switch with NO `default:`, so
  /// CI's -Wswitch -Werror is the gate: a new Id fails the build until it is
  /// mapped. Returns nullptr only for the kIdCount sentinel.
  [[nodiscard]] QAction* action(shortcuts::Id id) const;

  /// (Re)assigns the bundled palette-tinted icons to every action. Called
  /// from the constructor; call again after Icons::clear_cache() when the
  /// application palette changes so the tint follows the theme.
  void apply_icons();

  QAction* new_file = nullptr;
  QAction* open = nullptr;
  QAction* save = nullptr;
  QAction* save_as = nullptr;
  QAction* export_glb = nullptr;
  /// Only constructed when the kernel is built with RM_BUILD_USD=ON; stays
  /// nullptr otherwise so MainWindow can skip wiring it.
  QAction* export_usd = nullptr;

  /// Export previews (p7-s1, #241) — GW-2 steps 21 and 22. Enabled always,
  /// unlike the export actions: previewing "nothing to export" on an empty
  /// scene is precisely the value.
  QAction* export_preview_scene = nullptr;
  QAction* export_preview_xodr = nullptr;

  /// File ▸ Preview Scenario in esmini… (p8-s5, #249, GW-6 step 14). Menu-only
  /// (no registry id, so no shortcut): a preview is an occasional gesture, and
  /// keeping it out of the registry keeps the generated shortcuts page — and
  /// its CI enforcer — untouched.
  ///
  /// ★ esmini is launched as a SUBPROCESS and never linked or bundled, which is
  /// what keeps it inside its MPL-2.0 external-tool entry
  /// (docs/standards/dependencies.md).
  QAction* preview_esmini = nullptr;
  QAction* quit = nullptr;

  QAction* undo = nullptr;
  QAction* redo = nullptr;

  /// Editing tools: exclusive, checkable — one active tool at a time (more
  /// join the group in later M2 phases).
  QActionGroup* tool_group = nullptr;
  QAction* tool_select = nullptr;
  QAction* tool_move = nullptr;
  QAction* tool_create_road = nullptr;
  QAction* tool_edit_nodes = nullptr;
  QAction* tool_lane_profile = nullptr;
  QAction* tool_elevation = nullptr;
  QAction* tool_create_junction = nullptr;
  QAction* tool_split = nullptr;
  QAction* tool_delete = nullptr;
  QAction* tool_lane_add = nullptr;
  QAction* tool_lane_form = nullptr;
  QAction* tool_lane_carve = nullptr;
  QAction* tool_crosswalk = nullptr;
  QAction* tool_marking_point = nullptr;
  QAction* tool_marking_curve = nullptr;
  QAction* tool_prop_point = nullptr;
  QAction* tool_prop_curve = nullptr;
  QAction* tool_prop_span = nullptr;
  QAction* tool_prop_polygon = nullptr;
  QAction* tool_corner = nullptr;
  QAction* tool_stopline = nullptr;
  QAction* tool_junction_span = nullptr;
  QAction* tool_junction_surface = nullptr;
  QAction* tool_surface = nullptr;
  QAction* tool_terrain_brush = nullptr;
  QAction* tool_actor_place = nullptr;
  QAction* tool_route = nullptr;
  QAction* scenario_mode = nullptr;
  QAction* tool_maneuver = nullptr;
  QAction* tool_signal = nullptr;
  QAction* tool_sign = nullptr;

  /// Not a tool: raises the 2D Editor's Lane Width tab for the selected lane
  /// (⇧L). Standalone so it works from any active tool.
  QAction* lane_width_editor = nullptr;

  /// Not a tool — a command that surfaces the 2D Editor's Signal Phases tab for
  /// the selected junction. Standalone so ⇧G works whatever tool is active.
  QAction* signal_phase_editor = nullptr;

  /// Create Road cross-section templates (exclusive, one always checked);
  /// the toolbar presents them as a dropdown next to the tool button.
  QActionGroup* template_group = nullptr;
  QAction* template_freeway = nullptr;
  QAction* template_arterial = nullptr;
  QAction* template_collector = nullptr;
  QAction* template_local = nullptr;

  QAction* reset_camera = nullptr;
  QAction* frame_selection = nullptr;
  QAction* frame_cursor = nullptr;
  QAction* add_from_library = nullptr;

  /// View ▸ Centre on World Origin (p7-s5, #324). A pivot move, not a dolly —
  /// the same semantics as frame_cursor, so the zoom the user set survives.
  QAction* center_world_origin = nullptr;

  /// Projection: exclusive and checkable, Perspective checked at startup.
  QActionGroup* projection_group = nullptr;
  QAction* view_perspective = nullptr;
  QAction* view_orthographic = nullptr;

  /// Cardinal views. Bound to the numpad digits, with the top-row digits as
  /// numpad-less alternates (setShortcuts, not setShortcut).
  QAction* view_north = nullptr;
  QAction* view_south = nullptr;
  QAction* view_west = nullptr;
  QAction* view_east = nullptr;
  QAction* view_top = nullptr;

  /// Checkable View toggle for the viewport corner hint (#333). Persisted in
  /// QSettings by MainWindow; the status-bar instruction is unaffected.
  QAction* viewport_hints = nullptr;

  QAction* merge_roads = nullptr;

  // Terrain (p5-s2, #232): Edit ▸ Terrain menu entries, enabled by whether a
  // height field currently exists.
  QAction* terrain_create = nullptr;
  QAction* terrain_remove = nullptr;

  /// Edit ▸ World Georeference… (p7-s5, #324). Opens the tool window that
  /// authors <header><geoReference>/<offset> and the workspace box.
  QAction* world_georeference = nullptr;

  // DEM import (p5-s4, #234): Edit ▸ Terrain ▸ Import DEM — reads an ESRI ASCII
  // grid (.asc) and installs it as the scene field. Menu-only (no registry id).
  QAction* terrain_import = nullptr;

  // GIS import (p7-s2, #242). File ▸ Import gets the two REFERENCE imports —
  // Layer-2 backdrop that never enters the .xodr — while the elevation raster
  // joins Edit ▸ Terrain, because it becomes real scene content through the
  // same command a .asc DEM does. The menu split follows what the data
  // BECOMES, not what format it arrived in.
  QAction* import_gis_vector = nullptr;
  QAction* import_gis_raster = nullptr;
  QAction* import_point_cloud = nullptr;
  QAction* terrain_import_raster = nullptr;
  QAction* terrain_seed_point_cloud = nullptr;

  // OSM import (p7-s4, #244). NOT a reference layer: an OSM extract becomes
  // real network content, so it goes through the command layer and lands as
  // ONE undoable edit — the same side of the menu split the terrain imports
  // are on, for the same reason.
  QAction* import_osm = nullptr;
  /// File ▸ Import ▸ Asset… — a user's own image becomes a project material
  /// (p6-s8, #322). A THIRD category beside the two the comment above describes:
  /// an imported asset is neither a reference layer nor scene content, it is
  /// LIBRARY content, so it lives in File ▸ Import next to them rather than under
  /// Edit ▸ Terrain. The menu split still follows what the data becomes.
  QAction* import_asset = nullptr;

  // Bridges (p5-s3, #233): the Road Construction tool's automatic bridge
  // assignment — a menu action (no registry id, so no shortcut) that detects
  // grade-separated crossings and builds a bridge over each. The interactive
  // span-inflation control is a follow-up.
  QAction* bridge_generate = nullptr;

  /// Clears every span whose crossing has gone (cascade-s3, #463). A move
  /// relocates a span onto its crossing where it can and reports the rest as
  /// orphaned; this is the only way to act on that report, since a bridge is
  /// not selectable and there is no span control yet.
  QAction* bridge_remove_orphans = nullptr;

  /// Moves every obstructed prop to the nearest clear place on its own anchor
  /// road (cascade-s4, #464). Invoking it IS the consent: a move reports an
  /// obstruction and never corrects it, so this menu item is the whole of the
  /// offered fix. No registry id — menu-only, like the bridge pair above.
  QAction* props_relocate_obstructed = nullptr;

  QAction* reset_layout = nullptr;

  /// Opens the in-app user guide (Help menu, F1).
  QAction* help_contents = nullptr;
  QAction* about = nullptr;
};

} // namespace roadmaker::editor
