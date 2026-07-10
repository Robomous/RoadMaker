#include "tools/tool_manager.hpp"

namespace roadmaker::editor {

ToolManager::ToolManager(QObject* parent) : QObject(parent) {}

ToolManager::~ToolManager() = default;

void ToolManager::register_tool(ToolId id, std::unique_ptr<Tool> tool) {
  if (active_id_ == id) {
    if (Tool* current = active()) {
      current->deactivate();
    }
    active_id_.reset();
  }
  tools_[id] = std::move(tool);
}

void ToolManager::set_active(ToolId id) {
  if (active_id_ == id) {
    return;
  }
  const auto it = tools_.find(id);
  if (it == tools_.end()) {
    return;
  }
  if (Tool* current = active()) {
    current->deactivate();
  }
  active_id_ = id;
  it->second->activate();
  emit active_changed();
}

Tool* ToolManager::active() const {
  if (!active_id_) {
    return nullptr;
  }
  const auto it = tools_.find(*active_id_);
  return it == tools_.end() ? nullptr : it->second.get();
}

} // namespace roadmaker::editor
