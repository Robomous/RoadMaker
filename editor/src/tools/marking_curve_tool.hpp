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

// Marking Curve tool (p3-s4, issue #223). Multi-click places points along a
// road; the first click anchors the curve to the road under it, and every point
// projects onto that road's reference line. A live preview shows the CLAMPED
// snapped band; Enter or double-click commits ONE edit::add_object, Backspace
// removes the last point, Esc cancels. Nothing enters the network until commit
// (no preview session — like Create Road). The asset is a crosswalk (a striped
// band) or a plain marking (a solid line). Headless by construction: ToolEvent
// in, commands + PreviewGeometry out.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/id.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "document/library_manifest.hpp"
#include "render/material_catalog.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class MarkingCurveTool : public Tool {
  Q_OBJECT

public:
  MarkingCurveTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The crosswalk/marking asset the commit authors. MainWindow wires this to
  /// the merged Library's default compatible asset. An incompatible/unset item
  /// makes the first click toast rather than start a curve.
  void set_params_provider(std::function<LibraryItem()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Placed points (handles), the ghost polyline to the cursor, and the CLAMPED
  /// snapped centreline the commit will author.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] std::size_t point_count() const { return points_.size(); }

  [[nodiscard]] QString instruction() const override;

private:
  [[nodiscard]] LibraryItem current_item() const;
  void place_point(double world_x, double world_y);
  void commit();
  void reset_session();

  Document& document_;
  SelectionModel& selection_;
  MaterialCatalog materials_;
  std::function<LibraryItem()> params_provider_;
  std::vector<Waypoint> points_;
  std::optional<RoadId> anchor_;
  std::optional<Waypoint> cursor_;
};

} // namespace roadmaker::editor
