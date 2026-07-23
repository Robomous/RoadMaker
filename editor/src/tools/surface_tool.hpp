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

#include "roadmaker/road/surface.hpp"

#include <QObject>
#include <cstddef>
#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// Reshapes a ground surface's boundary as a node graph (p5-s1, issue #231,
/// GW-2 step 6). Node handles and tangent handles ride the selected surface;
/// dragging one runs a preview session and commits ONE `set_surface_boundary`
/// on release, which is also what detaches a derived surface to Authored
/// (decision D3).
///
/// Follows EditNodesTool: it operates on the SELECTION rather than picking a
/// target of its own, so a Scene-tree click and a viewport click agree. The
/// difference is that a derived surface has no stored nodes — the tool works
/// against `surface_boundary_nodes()`, which seeds them from the mesher's own
/// region polygon, so handles appear on a surface nobody has ever edited.
class SurfaceTool : public Tool {
  Q_OBJECT

public:
  SurfaceTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around node and tangent handles.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] bool dragging() const { return drag_.has_value(); }

  [[nodiscard]] PreviewGeometry preview() const override;
  [[nodiscard]] QString instruction() const override;

  /// The effective boundary of the selected surface — its authored nodes, or a
  /// derived surface's seed set. Empty when nothing editable is selected.
  /// Exposed for the Attributes pane and the tests.
  [[nodiscard]] std::vector<SurfaceNode> nodes() const;

  /// The surface the handles belong to; invalid when the selection has none.
  [[nodiscard]] SurfaceId target() const;

  /// Pushes `revert_surface_to_derived` on the selected surface. No-op (with a
  /// status message) when it is already derived. Behind the context-menu entry
  /// and the Attributes-pane button.
  void revert_to_derived();

private:
  /// What a drag is moving: the node itself, or one of its tangent handles.
  enum class Grip { Node, TangentIn, TangentOut };

  struct DragState {
    SurfaceId surface;
    std::size_t index = 0;
    Grip grip = Grip::Node;
    std::vector<SurfaceNode> base; ///< the whole loop as it was at grab time
    bool moved = false;
  };

  /// Hit-test order matches the draw order: tangent handles sit on top of the
  /// nodes they belong to, and midpoint markers are last.
  [[nodiscard]] std::optional<DragState> pick_grip(double x, double y) const;
  [[nodiscard]] std::optional<std::size_t> pick_midpoint(double x, double y) const;

  /// Applies the cursor to `drag_`, producing the edited loop.
  [[nodiscard]] std::vector<SurfaceNode> dragged_nodes(double x, double y) const;

  void insert_node(std::size_t before_index);
  void delete_active_node();

  /// Pushes one command, reporting a rejection through the status bar.
  bool push(std::vector<SurfaceNode> nodes, const QString& success);

  Document& document_;
  SelectionModel& selection_;

  /// Screen-independent grab radius [m]. Matches EditNodesTool's default.
  double pick_radius_ = 2.0;

  std::optional<DragState> drag_;
  std::optional<std::size_t> active_; ///< last touched node, for Delete
};

} // namespace roadmaker::editor
