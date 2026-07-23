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

// Lane tool (issue #14 / p2-s4). The editing itself is panel-based: type edits
// in the Properties panel, width-along-s in the 2D Editor's Lane Width tab; the
// tool exists for toolbar consistency, lane-granular viewport highlighting, and
// the one direct gesture it owns — Delete removes the selected lane. A click
// selects the picked LANE, empty space clears.

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class LaneProfileTool : public Tool {
  Q_OBJECT

public:
  LaneProfileTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;

  /// Delete/Backspace removes the primary lane through edit::remove_lane (ONE
  /// undo step). Shift+D cycles the primary lane's travel direction
  /// (Standard->Reversed->Both->Standard) through edit::set_lane_direction. A
  /// null/center/non-outermost lane can't be removed, and the center lane has
  /// no direction — the gesture then emits a status message rather than doing
  /// nothing silently.
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] QString instruction() const override;

private:
  /// Shift+D handler: cycles the selected lane's @direction via ONE command.
  [[nodiscard]] bool cycle_direction();

  Document& document_;
  SelectionModel& selection_;
};

} // namespace roadmaker::editor
