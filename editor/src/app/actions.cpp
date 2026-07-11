#include "app/actions.hpp"

#include "app/icons.hpp"

namespace roadmaker::editor {

Actions::Actions(QUndoStack& undo_stack, QObject* parent) : QObject(parent) {
  new_file = new QAction(tr("&New"), this);
  new_file->setShortcut(QKeySequence::New);

  open = new QAction(tr("&Open…"), this);
  open->setShortcut(QKeySequence::Open);

  save = new QAction(tr("&Save"), this);
  save->setShortcut(QKeySequence::Save);

  save_as = new QAction(tr("Save &As…"), this);
  save_as->setShortcut(QKeySequence::SaveAs);

  export_glb = new QAction(tr("&Export glTF…"), this);
  export_glb->setEnabled(false); // enabled once a file is loaded

  quit = new QAction(tr("&Quit"), this);
  quit->setShortcut(QKeySequence::Quit);
  quit->setMenuRole(QAction::QuitRole);

  undo = undo_stack.createUndoAction(this, tr("&Undo"));
  undo->setShortcut(QKeySequence::Undo);
  redo = undo_stack.createRedoAction(this, tr("&Redo"));
  redo->setShortcut(QKeySequence::Redo);

  tool_group = new QActionGroup(this);
  tool_select = new QAction(tr("&Select/Move"), this);
  tool_select->setCheckable(true);
  tool_select->setChecked(true); // the default tool
  tool_select->setShortcut(Qt::Key_V);
  tool_select->setToolTip(tr("Select/Move — click picks, drag spans a rubber band, "
                             "drag a node handle moves it (V)"));
  tool_group->addAction(tool_select);

  tool_create_road = new QAction(tr("&Create Road"), this);
  tool_create_road->setCheckable(true);
  tool_create_road->setShortcut(Qt::Key_C);
  tool_create_road->setToolTip(tr("Create Road — click places waypoints, Enter or double-click "
                                  "creates the road, Esc cancels (C)"));
  tool_group->addAction(tool_create_road);

  tool_edit_nodes = new QAction(tr("Edit &Nodes"), this);
  tool_edit_nodes->setCheckable(true);
  tool_edit_nodes->setShortcut(Qt::Key_N);
  tool_edit_nodes->setToolTip(tr("Edit Nodes — drag a node to move it, click a midpoint "
                                 "marker to insert, Delete removes the active node (N)"));
  tool_group->addAction(tool_edit_nodes);

  tool_lane_profile = new QAction(tr("&Lane Profile"), this);
  tool_lane_profile->setCheckable(true);
  tool_lane_profile->setShortcut(Qt::Key_L);
  tool_lane_profile->setToolTip(tr("Lane Profile — click a lane, then edit its type, width, and "
                                   "road mark in the Properties panel (L)"));
  tool_group->addAction(tool_lane_profile);

  tool_elevation = new QAction(tr("&Elevation"), this);
  tool_elevation->setCheckable(true);
  tool_elevation->setShortcut(Qt::Key_E);
  tool_elevation->setToolTip(tr("Elevation — click a road node, then set its height in the "
                                "Properties panel; the grade re-fits smoothly (E)"));
  tool_group->addAction(tool_elevation);

  tool_delete = new QAction(tr("&Delete"), this);
  tool_delete->setCheckable(true);
  tool_delete->setShortcut(Qt::Key_X);
  tool_delete->setToolTip(tr("Delete — click a road to delete it, undo restores (X)"));
  tool_group->addAction(tool_delete);

  // Road templates for the Create Road tool (02_editing_tools.md §2):
  // exclusive and always checked; the toolbar shows them as a dropdown.
  template_group = new QActionGroup(this);
  template_rural = new QAction(tr("Two-lane &Rural"), this);
  template_rural->setCheckable(true);
  template_rural->setChecked(true); // the tool's default profile
  template_rural->setToolTip(tr("One driving lane each way, right-hand shoulder"));
  template_group->addAction(template_rural);

  template_urban = new QAction(tr("&Urban Sidewalk"), this);
  template_urban->setCheckable(true);
  template_urban->setToolTip(tr("One driving lane each way, sidewalks both sides"));
  template_group->addAction(template_urban);

  template_highway = new QAction(tr("&Highway"), this);
  template_highway->setCheckable(true);
  template_highway->setToolTip(tr("Two driving lanes each way, wide shoulders"));
  template_group->addAction(template_highway);

  reset_camera = new QAction(tr("Reset &Camera"), this);
  frame_selection = new QAction(tr("&Frame Selection"), this);
  frame_selection->setShortcut(Qt::Key_F);
  reset_layout = new QAction(tr("Reset &Layout"), this);

  about = new QAction(tr("&About RoadMaker"), this);
  about->setMenuRole(QAction::AboutRole);

  apply_icons();
}

void Actions::apply_icons() {
  // Bundled palette-tinted set (docs/design/m2/05_assets.md §1) — never
  // QIcon::fromTheme, which is empty on macOS/Windows.
  new_file->setIcon(Icons::get(QStringLiteral("file-plus")));
  open->setIcon(Icons::get(QStringLiteral("folder-open")));
  save->setIcon(Icons::get(QStringLiteral("save")));
  save_as->setIcon(Icons::get(QStringLiteral("save")));
  export_glb->setIcon(Icons::get(QStringLiteral("box")));
  undo->setIcon(Icons::get(QStringLiteral("undo-2")));
  redo->setIcon(Icons::get(QStringLiteral("redo-2")));
  tool_select->setIcon(Icons::get(QStringLiteral("mouse-pointer-2")));
  tool_create_road->setIcon(Icons::get(QStringLiteral("clothoid-road")));
  tool_edit_nodes->setIcon(Icons::get(QStringLiteral("waypoints")));
  tool_lane_profile->setIcon(Icons::get(QStringLiteral("lane-section")));
  tool_elevation->setIcon(Icons::get(QStringLiteral("mountain")));
  tool_delete->setIcon(Icons::get(QStringLiteral("trash-2")));
  template_rural->setIcon(Icons::get(QStringLiteral("template-rural")));
  template_urban->setIcon(Icons::get(QStringLiteral("template-urban")));
  template_highway->setIcon(Icons::get(QStringLiteral("template-highway")));
  reset_camera->setIcon(Icons::get(QStringLiteral("rotate-ccw")));
  frame_selection->setIcon(Icons::get(QStringLiteral("scan")));
}

} // namespace roadmaker::editor
