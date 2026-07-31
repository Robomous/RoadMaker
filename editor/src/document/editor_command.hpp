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

#include "roadmaker/edit/command.hpp"
#include "roadmaker/osc/edit.hpp"

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

  // Releases the kernel command's reserved slots when it is destroyed in the
  // REVERTED (undone) state — QUndoStack truncating the redo tail on a new
  // push, clear(), or Document teardown. A command destroyed while applied
  // keeps its slots legitimately (its objects are live). (#271)
  ~KernelEditorCommand() override;

protected:
  void apply() override;
  void revert() override;

private:
  Document& document_;
  std::unique_ptr<roadmaker::edit::Command> command_;
  // Whether the kernel command is currently in its APPLIED state. Constructed
  // already-applied; flipped only on a SUCCESSFUL redo()/undo() (the failure
  // branches return early, leaving it correct).
  bool applied_ = true;
};

/// Bridges one kernel `osc::edit::Command` onto the SAME QUndoStack
/// (p8-s2, #246) — the scenario twin of KernelEditorCommand, created only by
/// Document::push_scenario_command.
///
/// ★ ONE STACK, NOT TWO. Map and scenario entries interleave on Document's
/// QUndoStack, which is what makes "switching back to Map mode returns to it
/// with the undo history intact" (GW-6 step 1) true by construction. The
/// kernel's `osc::edit::ScenarioStack` is Python/headless parity ONLY and never
/// drives this document.
///
/// TWO DIFFERENCES FROM ITS TWIN, both consequences of a scenario holding no
/// arena content:
///   * NO DESTRUCTOR. There are no reserved slots to release, because
///     `osc::edit::Command` has no `discard()` — so the whole #271 hazard this
///     class's sibling exists to handle simply does not arise here.
///   * NO DIRTY SET AND NO RE-MESH. Nothing in a scenario is tessellated, so
///     redo/undo emit `scenario_changed()` rather than driving
///     `after_kernel_mutation`.
class ScenarioEditorCommand final : public EditorCommand {
public:
  ScenarioEditorCommand(Document& document, std::unique_ptr<roadmaker::osc::edit::Command> command);

protected:
  void apply() override;
  void revert() override;

private:
  Document& document_;
  std::unique_ptr<roadmaker::osc::edit::Command> command_;
};

} // namespace roadmaker::editor
