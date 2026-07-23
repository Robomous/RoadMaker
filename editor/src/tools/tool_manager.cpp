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
