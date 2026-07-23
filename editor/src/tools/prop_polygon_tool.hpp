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

// Prop Polygon tool (p6-s5, issue #239). Click a closed region (three or more
// vertices) and the tool scatters the selected prop across it at a target
// density, anchoring each placed instance to the nearest road (a region interior
// sits off the reference line, so the reach is generous — kPolygonAnchorMaxT).
// A live preview shows the region outline plus one ghost per instance; the
// scatter is CACHED and only recomputed when the vertices, density, or seed
// change — never per mouse-move. `[`/`]` adjust density, `R` randomizes the seed
// (a tool-internal key, not a global shortcut), Enter or double-click bakes the
// props as ONE undo macro, Backspace pops the last vertex, Esc cancels.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/authoring.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class PropPolygonTool : public Tool {
  Q_OBJECT

public:
  PropPolygonTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The prop asset the scatter distributes. MainWindow wires this to the merged
  /// Library's default prop. An incompatible/unset item makes the first click
  /// toast rather than start a region.
  void set_params_provider(std::function<LibraryItem()> provider);

  /// Pins the scatter seed (test seam / deterministic replays). Invalidates the
  /// cached distribution so the next preview/bake reflects it.
  void set_seed(std::uint32_t seed);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The region outline (line_positions) plus one ghost handle per instance the
  /// bake will place (ghost == commit).
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] std::size_t vertex_count() const { return vertices_.size(); }

  [[nodiscard]] double density() const { return density_per_100m2_; }

  [[nodiscard]] QString instruction() const override;

private:
  [[nodiscard]] LibraryItem current_item() const;
  void adjust_density(double delta);
  void randomize();
  void invalidate();
  void ensure_distribution() const;
  void bake();
  void reset_session();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;

  std::vector<Waypoint> vertices_;
  std::optional<Waypoint> cursor_;
  double density_per_100m2_ = 4.0; ///< target props per 100 m²
  std::uint32_t seed_ = 0;

  // The scatter is expensive relative to a mouse-move, so it is cached and only
  // recomputed when the inputs (vertices/density/seed) change — `dirty_` guards
  // the lazy recompute inside the const preview()/bake() path.
  mutable std::optional<PropCurveDistribution> distribution_;
  mutable bool dirty_ = true;
};

} // namespace roadmaker::editor
