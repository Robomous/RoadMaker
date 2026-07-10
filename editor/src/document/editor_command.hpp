#pragma once

#include "roadmaker/edit/command.hpp"

#include <QString>
#include <QUndoCommand>
#include <memory>

namespace roadmaker::editor {

class Document;

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

/// Bridges one kernel roadmaker::edit::Command onto the QUndoStack
/// (docs/m2/01_editing_framework.md §1.3). Created only by
/// Document::push_command, which applies the kernel command first — so the
/// bridge is always constructed already_applied and QUndoStack's immediate
/// redo() on push is skipped. Later redo()/undo() drive the kernel command
/// and Document's re-mesh through the dirty set.
class KernelEditorCommand final : public EditorCommand {
public:
  KernelEditorCommand(Document& document, std::unique_ptr<roadmaker::edit::Command> command);

protected:
  void apply() override;
  void revert() override;

private:
  Document& document_;
  std::unique_ptr<roadmaker::edit::Command> command_;
};

} // namespace roadmaker::editor
