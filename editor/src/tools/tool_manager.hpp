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

#include <QObject>
#include <map>
#include <memory>
#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

// Owns the tool set and the single active tool (skeleton — see
// docs/m2/01_editing_framework.md §4). ViewportWidget forwards translated
// events to active(); toolbar QActions call set_active(). Listeners react to
// active_changed() and query state (same convention SelectionModel moves to
// in M2: signals carry no payload, state is pulled).
class ToolManager : public QObject {
  Q_OBJECT

public:
  explicit ToolManager(QObject* parent = nullptr);
  ~ToolManager() override;

  // Replaces any tool previously registered under the same id.
  void register_tool(ToolId id, std::unique_ptr<Tool> tool);

  // Deactivates the current tool and activates the one registered under id.
  // Unknown ids and re-activation of the current tool are no-ops.
  void set_active(ToolId id);

  [[nodiscard]] Tool* active() const;

  [[nodiscard]] std::optional<ToolId> active_id() const { return active_id_; }

signals:
  void active_changed();

private:
  std::map<ToolId, std::unique_ptr<Tool>> tools_;
  std::optional<ToolId> active_id_;
};

} // namespace roadmaker::editor
