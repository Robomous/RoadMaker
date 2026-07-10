#include "document/editor_command.hpp"

namespace roadmaker::editor {

EditorCommand::EditorCommand(const QString& text, bool already_applied, QUndoCommand* parent)
    : QUndoCommand(text, parent), skip_next_redo_(already_applied) {}

void EditorCommand::redo() {
  if (skip_next_redo_) {
    skip_next_redo_ = false;
    return;
  }
  apply();
}

void EditorCommand::undo() {
  revert();
}

} // namespace roadmaker::editor
