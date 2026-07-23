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

// Edit Nodes tool (issue #10, docs/design/m2/02_editing_tools.md §3). On the
// selected roads it shows node handles with display-only tangent whiskers
// and segment-midpoint markers: dragging a handle moves the waypoint (as
// Select/Move, preview session + one command), clicking a marker inserts a
// node ON the curve at that station (one command, immediately grabbed so the
// same gesture can keep dragging), and Delete/Backspace removes the active
// node. Node handles for foreign roads come from the §2.5 derivation
// (edit::effective_waypoints), with a one-time re-fit notice. Headless by
// construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>
#include <utility>

#include "tools/node_drag.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class EditNodesTool : public Tool {
  Q_OBJECT

public:
  EditNodesTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around node handles and midpoint markers; handles
  /// win over markers, markers over lane patches.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  /// Snap query options for node drags (the dragged road always excluded).
  void set_snap_options(edit::SnapOptions options) { snap_options_ = options; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Makes `index` the active node of `road` and grabs it for dragging — the
  /// "double-click then drag" bend handoff from Select (and this tool's own
  /// double-click). Selects the road so its handles are visible; the grab is
  /// preview-less until the next move, so a plain double-click just leaves the
  /// committed node active.
  void adopt_node(RoadId road, std::size_t index);

  /// Node handles (points) + tangent whiskers and midpoint-marker diamonds
  /// (lines) of every selected road, the active-node highlight square, and
  /// during a drag the snap hint + original→current tether.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] bool dragging() const { return drag_.has_value(); }

  /// The node a press activated (Delete's target); (road, waypoint index).
  [[nodiscard]] std::optional<std::pair<RoadId, std::size_t>> active_node() const {
    return active_;
  }

  [[nodiscard]] QString instruction() const override;

private:
  struct MarkerHit {
    RoadId road;
    std::size_t insert_index = 0; ///< new node's index (segment + 1)
    double station = 0.0;         ///< mid station of the segment, for insert_node_at
    Waypoint position;            ///< on the curve, at the segment's mid station
  };

  /// Nearest effective waypoint of a selected road within pick_radius_.
  [[nodiscard]] std::optional<NodeDragState> pick_node(const Waypoint& cursor) const;

  /// Nearest segment-midpoint marker of a selected road within pick_radius_.
  [[nodiscard]] std::optional<MarkerHit> pick_midpoint(const Waypoint& cursor) const;

  /// One-time §2.5 notice when an edit is about to derive waypoints.
  void notify_derivation(const Road& road);

  void delete_active_node();

  /// Tracks the node under the cursor (no drag) for the hover handle state,
  /// repainting only when it changes.
  void update_hover(const Waypoint& cursor);

  /// Handle state for waypoint `index` of `road`: Grabbed while dragging it,
  /// Hovered when it is the active (clicked) node or under the cursor, else
  /// Idle.
  [[nodiscard]] HandleState node_state(RoadId road, std::size_t index) const;

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;
  edit::SnapOptions snap_options_{};
  std::optional<NodeDragState> drag_;
  std::optional<std::pair<RoadId, std::size_t>> active_;
  std::optional<std::pair<RoadId, std::size_t>> hovered_;
};

} // namespace roadmaker::editor
