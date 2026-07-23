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

// Lane Form tool (p2-s5). Click a road body: an interior lane is formed at the
// picked position, starting at zero width at the click station and holding full
// width to the road end (edit::form_lane). Press begins a preview session,
// release commits it as ONE undo entry. Forming where the click does not land
// in the road's final lane section is refused by the kernel — the tool surfaces
// the guard message and leaves no state. Stays active for repeated forms; Esc
// cancels an uncommitted preview. Headless: ToolEvent in, commands out.

#include "roadmaker/road/road.hpp"

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class LaneFormTool : public Tool {
  Q_OBJECT

public:
  LaneFormTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] PreviewGeometry preview() const override;
  [[nodiscard]] QString instruction() const override;

private:
  void reset();

  Document& document_;
  SelectionModel& selection_;

  bool forming_ = false; ///< a preview session is live between press and release
  RoadId road_;          ///< the road being formed on (for the preview highlight)
  double s_start_ = 0.0; ///< the form station
};

} // namespace roadmaker::editor
