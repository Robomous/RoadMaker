#pragma once

// Central QAction registry. Every menu/toolbar entry exists exactly once
// here; MainWindow only arranges them. Undo/redo come from the document's
// QUndoStack (M2 scaffolding — the stack is empty in M1).

#include <QAction>
#include <QObject>
#include <QUndoStack>

namespace roadmaker::editor {

class Actions : public QObject {
  Q_OBJECT

public:
  explicit Actions(QUndoStack& undo_stack, QObject* parent = nullptr);

  /// (Re)assigns the bundled palette-tinted icons to every action. Called
  /// from the constructor; call again after Icons::clear_cache() when the
  /// application palette changes so the tint follows the theme.
  void apply_icons();

  QAction* open = nullptr;
  QAction* export_glb = nullptr;
  QAction* quit = nullptr;

  QAction* undo = nullptr;
  QAction* redo = nullptr;

  QAction* reset_camera = nullptr;
  QAction* frame_selection = nullptr;
  QAction* reset_layout = nullptr;

  QAction* about = nullptr;
};

} // namespace roadmaker::editor
