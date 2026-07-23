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

#include "roadmaker/edit/edit_stack.hpp"

#include <algorithm>
#include <utility>

namespace roadmaker::edit {

Expected<void> EditStack::push(RoadNetwork& network, std::unique_ptr<Command> command) {
  if (command == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "push: null command");
  }
  if (auto applied = command->apply(network); !applied.has_value()) {
    return applied;
  }
  // Truncating the redo tail destroys those commands; each is reverted (they
  // sit past the cursor), so discard releases the reserved slots their created
  // objects would otherwise leak (#271).
  for (std::size_t i = cursor_; i < commands_.size(); ++i) {
    commands_[i]->discard(network);
  }
  commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
  commands_.push_back(std::move(command));
  cursor_ = commands_.size();
  enforce_depth_limit();
  return {};
}

Expected<void> EditStack::undo(RoadNetwork& network) {
  if (!can_undo()) {
    return make_error(ErrorCode::InvalidArgument, "undo: nothing to undo");
  }
  if (auto reverted = commands_[cursor_ - 1]->revert(network); !reverted.has_value()) {
    return reverted;
  }
  --cursor_;
  return {};
}

Expected<void> EditStack::redo(RoadNetwork& network) {
  if (!can_redo()) {
    return make_error(ErrorCode::InvalidArgument, "redo: nothing to redo");
  }
  if (auto applied = commands_[cursor_]->apply(network); !applied.has_value()) {
    return applied;
  }
  ++cursor_;
  return {};
}

void EditStack::clear() {
  // No RoadNetwork& here (this signature is bound to Python), so undone
  // commands past the cursor cannot discard their reserved slots — a residual
  // leak, bounded by the redo-tail depth at clear time and accepted (#271).
  // The editor's own QUndoStack path (KernelEditorCommand's destructor) does
  // release them; this is the headless/Python parity stack only.
  commands_.clear();
  cursor_ = 0;
}

void EditStack::set_depth_limit(std::size_t limit) {
  depth_limit_ = std::max<std::size_t>(limit, 1);
  enforce_depth_limit();
}

void EditStack::enforce_depth_limit() {
  // Must NOT discard the commands it drops: these are the OLDEST entries, all
  // APPLIED (below the cursor), so their created objects are live in occupied
  // slots — discarding would try to release live slots (a no-op by the guard,
  // but the intent would be wrong) and, worse, invites the symmetric bug of
  // discarding applied history. Dropping an applied command simply forgets how
  // to undo it; its objects stay in the network. (#271)
  if (commands_.size() <= depth_limit_) {
    return;
  }
  const std::size_t excess = commands_.size() - depth_limit_;
  commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(excess));
  cursor_ -= std::min(cursor_, excess);
}

} // namespace roadmaker::edit
