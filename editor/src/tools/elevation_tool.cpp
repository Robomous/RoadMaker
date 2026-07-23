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

#include "tools/elevation_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <cmath>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

ElevationTool::ElevationTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void ElevationTool::activate() {}

void ElevationTool::deactivate() {
  set_active(std::nullopt);
}

std::optional<std::pair<RoadId, std::size_t>>
ElevationTool::pick_node(const Waypoint& cursor) const {
  std::optional<std::pair<RoadId, std::size_t>> best;
  double best_dist = pick_radius_;
  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr) {
      continue;
    }
    const std::vector<Waypoint> nodes = edit::effective_waypoints(*road);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const double dist = std::hypot(cursor.x - nodes[i].x, cursor.y - nodes[i].y);
      if (dist <= best_dist) {
        best_dist = dist;
        best = std::pair{road_id, i};
      }
    }
  }
  return best;
}

void ElevationTool::set_active(std::optional<std::pair<RoadId, std::size_t>> node) {
  if (node == active_) {
    return;
  }
  active_ = node;
  if (active_.has_value()) {
    // Surface the node's current height in the status bar (the overlay has no
    // text primitive yet — the numeric value lives in the Properties spin box).
    if (const Road* road = document_.network().road(active_->first)) {
      const auto stations = edit::waypoint_stations(*road);
      if (stations.has_value() && active_->second < stations->size()) {
        const double z = eval_profile(road->elevation, (*stations)[active_->second]);
        emit status_message(
            tr("Node %1 selected — elevation %2 m").arg(active_->second).arg(z, 0, 'f', 3));
      }
    }
  }
  emit active_node_changed();
  emit preview_changed();
}

bool ElevationTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};

  if (const auto node = pick_node(cursor)) {
    // Make the node's road the primary selection so the panel targets it.
    selection_.select({.road = node->first, .lane = LaneId{}});
    set_active(node);
    return true;
  }

  if (event.pick.has_value()) {
    selection_.select({.road = event.pick->road, .lane = LaneId{}});
    set_active(std::nullopt);
    return true;
  }

  selection_.clear();
  set_active(std::nullopt);
  return true; // LMB belongs to the tool even on a miss (M2 button map)
}

bool ElevationTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape && active_.has_value()) {
    set_active(std::nullopt);
    return true;
  }
  return false;
}

PreviewGeometry ElevationTool::preview() const {
  PreviewGeometry geometry;
  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr) {
      continue;
    }
    const std::vector<Waypoint> nodes = edit::effective_waypoints(*road);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const bool is_active =
          active_.has_value() && active_->first == road_id && active_->second == i;
      geometry.add_handle(nodes[i].x,
                          nodes[i].y,
                          0.0,
                          HandleKind::Node,
                          is_active ? HandleState::Hovered : HandleState::Idle);
    }
  }
  return geometry;
}

QString ElevationTool::instruction() const {
  return tr("Click a road node, then set its height in the Properties panel or the 2D Editor · the "
            "grade re-fits smoothly");
}

} // namespace roadmaker::editor
