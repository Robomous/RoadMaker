#include "app/actions.hpp"

#include "app/icons.hpp"

namespace roadmaker::editor {

Actions::Actions(QUndoStack& undo_stack, QObject* parent) : QObject(parent) {
  open = new QAction(tr("&Open…"), this);
  open->setShortcut(QKeySequence::Open);

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

  tool_edit_nodes = new QAction(tr("Edit &Nodes"), this);
  tool_edit_nodes->setCheckable(true);
  tool_edit_nodes->setShortcut(Qt::Key_N);
  tool_edit_nodes->setToolTip(tr("Edit Nodes — drag a node to move it, click a midpoint "
                                 "marker to insert, Delete removes the active node (N)"));
  tool_group->addAction(tool_edit_nodes);

  tool_delete = new QAction(tr("&Delete"), this);
  tool_delete->setCheckable(true);
  tool_delete->setShortcut(Qt::Key_X);
  tool_delete->setToolTip(tr("Delete — click a road to delete it, undo restores (X)"));
  tool_group->addAction(tool_delete);

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
  open->setIcon(Icons::get(QStringLiteral("folder-open")));
  export_glb->setIcon(Icons::get(QStringLiteral("box")));
  undo->setIcon(Icons::get(QStringLiteral("undo-2")));
  redo->setIcon(Icons::get(QStringLiteral("redo-2")));
  tool_select->setIcon(Icons::get(QStringLiteral("mouse-pointer-2")));
  tool_edit_nodes->setIcon(Icons::get(QStringLiteral("waypoints")));
  tool_delete->setIcon(Icons::get(QStringLiteral("trash-2")));
  reset_camera->setIcon(Icons::get(QStringLiteral("rotate-ccw")));
  frame_selection->setIcon(Icons::get(QStringLiteral("scan")));
}

} // namespace roadmaker::editor
