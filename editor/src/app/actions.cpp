#include "app/actions.hpp"

namespace roadmaker::editor {

Actions::Actions(QUndoStack& undo_stack, QObject* parent) : QObject(parent) {
  open = new QAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("&Open…"), this);
  open->setShortcut(QKeySequence::Open);

  export_glb =
      new QAction(QIcon::fromTheme(QStringLiteral("document-save-as")), tr("&Export glTF…"), this);
  export_glb->setEnabled(false); // enabled once a file is loaded

  quit = new QAction(tr("&Quit"), this);
  quit->setShortcut(QKeySequence::Quit);
  quit->setMenuRole(QAction::QuitRole);

  undo = undo_stack.createUndoAction(this, tr("&Undo"));
  undo->setShortcut(QKeySequence::Undo);
  redo = undo_stack.createRedoAction(this, tr("&Redo"));
  redo->setShortcut(QKeySequence::Redo);

  reset_camera =
      new QAction(QIcon::fromTheme(QStringLiteral("view-restore")), tr("Reset &Camera"), this);
  frame_selection =
      new QAction(QIcon::fromTheme(QStringLiteral("zoom-fit-best")), tr("&Frame Selection"), this);
  frame_selection->setShortcut(Qt::Key_F);
  reset_layout = new QAction(tr("Reset &Layout"), this);

  about = new QAction(tr("&About RoadMaker"), this);
  about->setMenuRole(QAction::AboutRole);
}

} // namespace roadmaker::editor
