#pragma once

#include "roadmaker/edit/command.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace roadmaker::edit {

/// Headless undo/redo stack over Command (docs/m2/01_editing_framework.md
/// §1.2). This exists for Python and headless parity ONLY — the editor's
/// single stack is Qt's QUndoStack; a document must never be driven by both.
// RM_API is applied per-method, not on the class (RoadNetwork convention):
// a class-level export would demand a DLL interface for the std::vector
// member on MSVC (C4251).
class EditStack {
public:
  EditStack() = default;
  // Not copyable (owns the recorded commands); movable. The explicit
  // deletion also keeps binding generators from instantiating the
  // ill-formed vector<unique_ptr> copy.
  EditStack(const EditStack&) = delete;
  EditStack& operator=(const EditStack&) = delete;
  EditStack(EditStack&&) = default;
  EditStack& operator=(EditStack&&) = default;
  ~EditStack() = default;

  /// Applies the command and records it on success; a failed apply returns
  /// the command's error and records nothing (network unchanged per the
  /// Command contract). Pushing truncates the redo tail.
  [[nodiscard]] RM_API Expected<void> push(RoadNetwork& network, std::unique_ptr<Command> command);

  [[nodiscard]] RM_API Expected<void> undo(RoadNetwork& network);
  [[nodiscard]] RM_API Expected<void> redo(RoadNetwork& network);

  [[nodiscard]] bool can_undo() const { return cursor_ > 0; }

  [[nodiscard]] bool can_redo() const { return cursor_ < commands_.size(); }

  /// Recorded commands (applied + redoable).
  [[nodiscard]] std::size_t size() const { return commands_.size(); }

  RM_API void clear();

  /// Caps recorded history, dropping oldest entries first (their edits stay
  /// applied — they just become un-undoable). Clamped to at least 1.
  RM_API void set_depth_limit(std::size_t limit);

  [[nodiscard]] std::size_t depth_limit() const { return depth_limit_; }

private:
  void enforce_depth_limit();

  std::vector<std::unique_ptr<Command>> commands_;
  std::size_t cursor_ = 0; // commands_[0, cursor_) are currently applied
  std::size_t depth_limit_ = 256;
};

} // namespace roadmaker::edit
