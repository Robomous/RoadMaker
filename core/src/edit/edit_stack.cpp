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
  commands_.clear();
  cursor_ = 0;
}

void EditStack::set_depth_limit(std::size_t limit) {
  depth_limit_ = std::max<std::size_t>(limit, 1);
  enforce_depth_limit();
}

void EditStack::enforce_depth_limit() {
  if (commands_.size() <= depth_limit_) {
    return;
  }
  const std::size_t excess = commands_.size() - depth_limit_;
  commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(excess));
  cursor_ -= std::min(cursor_, excess);
}

} // namespace roadmaker::edit
