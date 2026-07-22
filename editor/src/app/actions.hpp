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
  QAction* tool_maneuver = nullptr;
  QAction* tool_signal = nullptr;

  /// Not a tool: raises the 2D Editor's Lane Width tab for the selected lane
  /// (⇧L). Standalone so it works from any active tool.
  QAction* lane_width_editor = nullptr;

  /// Not a tool — a command that surfaces the 2D Editor's Signal Phases tab for
  /// the selected junction. Standalone so ⇧G works whatever tool is active.
  QAction* signal_phase_editor = nullptr;

  /// Create Road cross-section templates (exclusive, one always checked);
  /// the toolbar presents them as a dropdown next to the tool button.
  QActionGroup* template_group = nullptr;
  QAction* template_rural = nullptr;
  QAction* template_urban = nullptr;
  QAction* template_highway = nullptr;

  QAction* reset_camera = nullptr;
  QAction* frame_selection = nullptr;
  QAction* frame_cursor = nullptr;
  QAction* add_from_library = nullptr;

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
  QAction* reset_layout = nullptr;

  /// Opens the in-app user guide (Help menu, F1).
  QAction* help_contents = nullptr;
  QAction* about = nullptr;
};

} // namespace roadmaker::editor
