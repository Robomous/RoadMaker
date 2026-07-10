#pragma once

#include <QString>
#include <QUndoCommand>

namespace roadmaker::editor {

// Base for every editor undo command (skeleton — see
// docs/m2/01_editing_framework.md §1.3 and §3). M2 phase 0 bridges kernel
// roadmaker::edit::Command objects through subclasses of this.
//
// QUndoStack calls redo() immediately when a command is pushed. Preview
// sessions (drag interactions) apply their work BEFORE the push, so they
// construct the command with already_applied = true: exactly that first
// redo() is skipped, while every subsequent redo() (after an undo) executes
// normally.
class EditorCommand : public QUndoCommand {
public:
  explicit EditorCommand(const QString& text,
                         bool already_applied = false,
                         QUndoCommand* parent = nullptr);

  void redo() final;
  void undo() final;

protected:
  virtual void apply() = 0;
  virtual void revert() = 0;

private:
  bool skip_next_redo_ = false;
};

} // namespace roadmaker::editor
