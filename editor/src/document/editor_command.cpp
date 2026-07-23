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

#include "document/editor_command.hpp"

#include <spdlog/spdlog.h>

#include <utility>

#include "document/document.hpp"

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

KernelEditorCommand::KernelEditorCommand(Document& document,
                                         std::unique_ptr<roadmaker::edit::Command> command)
    : EditorCommand(
          QString::fromUtf8(command->name().data(), static_cast<qsizetype>(command->name().size())),
          /*already_applied=*/true),
      document_(document), command_(std::move(command)) {}

KernelEditorCommand::~KernelEditorCommand() {
  // Destroyed in the reverted (undone) state — its created objects sit in
  // reserved slots awaiting a re-apply that will never come. Release them
  // (#271). Network outlives the stack: Document declares network_ before
  // undo_stack_, and load()/reset() clear the stack before swapping network_.
  if (!applied_ && command_ != nullptr) {
    command_->discard(document_.network_);
  }
}

void KernelEditorCommand::apply() {
  // Failures here are broken linear-history invariants, not user errors —
  // push_command already vetted the first apply. Log, leave the document
  // untouched (the kernel guarantees that on failure).
  if (auto applied = command_->apply(document_.network_); !applied.has_value()) {
    spdlog::error("redo '{}' failed: {}", command_->name(), applied.error().message);
    return;
  }
  applied_ = true;
  spdlog::info("redo: {}", command_->name());
  document_.after_kernel_mutation(command_->dirty());
}

void KernelEditorCommand::revert() {
  if (auto reverted = command_->revert(document_.network_); !reverted.has_value()) {
    spdlog::error("undo '{}' failed: {}", command_->name(), reverted.error().message);
    return;
  }
  applied_ = false;
  spdlog::info("undo: {}", command_->name());
  document_.after_kernel_mutation(command_->dirty());
}

} // namespace roadmaker::editor
