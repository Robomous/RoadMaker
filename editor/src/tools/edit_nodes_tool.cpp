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

#include "tools/edit_nodes_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

// Tangent whiskers are still world-meter line overlays (they scale with the
// road, unlike the node knobs, which the viewport now draws screen-constant).
constexpr double kTangentHalfMax = 6.0; ///< tangent whisker half-length cap
constexpr double kTangentSegmentShare = 0.25;

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

bool has_param_poly3(const Road& road) {
  return std::ranges::any_of(road.plan_view.records(), [](const GeometryRecord& record) {
    return std::holds_alternative<ParamPoly3Geom>(record.shape);
  });
}

} // namespace

EditNodesTool::EditNodesTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void EditNodesTool::activate() {}

void EditNodesTool::deactivate() {
  if (drag_.has_value()) {
    document_.cancel_preview();
    drag_.reset();
  }
  if (active_.has_value()) {
    active_.reset();
  }
  emit preview_changed();
}

std::optional<NodeDragState> EditNodesTool::pick_node(const Waypoint& cursor) const {
  // Shared node hit-test (also behind the context menu). Handles show on
  // selected roads only, so only those are candidates.
  if (const auto hit = pick_waypoint(
          document_.network(), selection_.selected_roads(), cursor.x, cursor.y, pick_radius_)) {
    return NodeDragState{.road = hit->road,
                         .index = hit->index,
                         .original = hit->position,
                         .current = hit->position};
  }
  return std::nullopt;
}

std::optional<EditNodesTool::MarkerHit> EditNodesTool::pick_midpoint(const Waypoint& cursor) const {
  std::optional<MarkerHit> best;
  double best_dist = pick_radius_;
  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr) {
      continue;
    }
    const auto stations = edit::waypoint_stations(*road);
    if (!stations.has_value()) {
      continue; // out-of-sync rm:waypoints metadata — no markers offered
    }
    for (std::size_t i = 0; i + 1 < stations->size(); ++i) {
      const PathPoint mid = road->plan_view.evaluate((stations->at(i) + stations->at(i + 1)) / 2.0);
      const double dist = std::hypot(cursor.x - mid.x, cursor.y - mid.y);
      if (dist <= best_dist) {
        best_dist = dist;
        best = MarkerHit{.road = road_id,
                         .insert_index = i + 1,
                         .station = (stations->at(i) + stations->at(i + 1)) / 2.0,
                         .position = Waypoint{.x = mid.x, .y = mid.y}};
      }
    }
  }
  return best;
}

void EditNodesTool::notify_derivation(const Road& road) {
  if (road.authoring_waypoints.has_value()) {
    return;
  }
  // One-time by construction: the edit records waypoints, so the road never
  // takes this path again (01 §2.5).
  emit status_message(has_param_poly3(road)
                          ? tr("First edit re-fits this road's paramPoly3 geometry as clothoids "
                               "(the shape changes slightly)")
                          : tr("First edit derives editing nodes from the imported geometry"));
}

bool EditNodesTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || drag_.has_value()) {
    return false;
  }
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};

  // Node handles pick first, then midpoint markers, then lane patches.
  if (auto grab = pick_node(cursor)) {
    if (const Road* road = document_.network().road(grab->road)) {
      notify_derivation(*road);
    }
    active_ = {grab->road, grab->index};
    drag_ = std::move(grab);
    emit preview_changed();
    return true;
  }

  if (const auto marker = pick_midpoint(cursor)) {
    if (const Road* road = document_.network().road(marker->road)) {
      notify_derivation(*road);
    }
    // insert_node_at pins headings from the current curve, so the shape is
    // preserved (unlike insert_waypoint, which reflowed authored roads).
    const Expected<void> inserted = document_.push_command(
        edit::insert_node_at(document_.network(), marker->road, marker->station));
    if (!inserted.has_value()) {
      emit status_message(
          tr("Cannot insert node: %1").arg(QString::fromStdString(inserted.error().message)));
      return true;
    }
    // Grab the fresh node so the same gesture can keep dragging it (the drag
    // is then a second undo entry, per §3's one-command-per-gesture).
    active_ = {marker->road, marker->insert_index};
    drag_ = NodeDragState{.road = marker->road,
                          .index = marker->insert_index,
                          .original = marker->position,
                          .current = marker->position};
    emit status_message(tr("Node inserted — drag to place it"));
    emit preview_changed();
    return true;
  }

  if (event.pick.has_value()) {
    selection_.select({.road = event.pick->road, .lane = event.pick->lane});
    active_.reset();
    emit preview_changed();
    return true;
  }

  selection_.clear();
  active_.reset();
  emit preview_changed();
  return true;
}

bool EditNodesTool::mouse_move(const ToolEvent& event) {
  if (!drag_.has_value()) {
    // Not dragging: track the node under the cursor for the hover handle
    // state, but don't consume the event (the viewport keeps its road hover).
    update_hover({.x = event.world_x, .y = event.world_y});
    return false;
  }
  update_node_drag(document_, *drag_, snap_options_, {.x = event.world_x, .y = event.world_y});
  emit preview_changed();
  return true;
}

void EditNodesTool::update_hover(const Waypoint& cursor) {
  std::optional<std::pair<RoadId, std::size_t>> found;
  if (const auto hit = pick_node(cursor)) {
    found = {hit->road, hit->index};
  }
  if (found != hovered_) {
    hovered_ = found;
    emit preview_changed();
  }
}

HandleState EditNodesTool::node_state(RoadId road, std::size_t index) const {
  const std::pair<RoadId, std::size_t> node{road, index};
  if (drag_.has_value() && std::pair{drag_->road, drag_->index} == node) {
    return HandleState::Grabbed;
  }
  if (active_ == node || hovered_ == node) {
    return HandleState::Hovered;
  }
  return HandleState::Idle;
}

bool EditNodesTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!drag_.has_value()) {
    return false;
  }
  // A grab that never previewed pushes nothing (commit is a no-op then). The
  // drag regenerated the touched junctions per frame (#156); don't repeat it.
  const bool moved = document_.preview_active();
  document_.commit_preview(/*already_regenerated=*/true);
  drag_.reset();
  if (moved) {
    emit status_message(tr("Node moved"));
  }
  emit preview_changed();
  return true;
}

bool EditNodesTool::mouse_double_click(const ToolEvent& event) {
  if (drag_.has_value() || !event.pick.has_value()) {
    return false;
  }
  const RoadId road_id = event.pick->road;
  const Road* road = document_.network().road(road_id);
  if (road == nullptr) {
    return false;
  }
  notify_derivation(*road);
  const StationCoord coord = find_station(road->plan_view, event.world_x, event.world_y);
  const auto stations = edit::waypoint_stations(*road);
  if (!stations.has_value()) {
    return false;
  }
  std::size_t index = 0;
  while (index < stations->size() && (*stations)[index] < coord.s) {
    ++index;
  }
  const Expected<void> inserted =
      document_.push_command(edit::insert_node_at(document_.network(), road_id, coord.s));
  if (!inserted.has_value()) {
    emit status_message(
        tr("Cannot insert bend: %1").arg(QString::fromStdString(inserted.error().message)));
    return true;
  }
  adopt_node(road_id, index);
  emit status_message(tr("Bend point inserted — drag to shape it"));
  return true;
}

void EditNodesTool::adopt_node(RoadId road, std::size_t index) {
  selection_.select({.road = road, .lane = LaneId{}}); // handles must be visible
  active_ = {road, index};
  if (const Road* road_ptr = document_.network().road(road)) {
    const std::vector<Waypoint> nodes = edit::effective_waypoints(*road_ptr);
    if (index < nodes.size()) {
      drag_ = NodeDragState{
          .road = road, .index = index, .original = nodes[index], .current = nodes[index]};
    }
  }
  emit preview_changed();
}

bool EditNodesTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    if (drag_.has_value()) {
      document_.cancel_preview();
      drag_.reset();
      emit status_message(tr("Move cancelled"));
      emit preview_changed();
      return true;
    }
    if (active_.has_value()) {
      active_.reset();
      emit preview_changed();
      return true;
    }
    return false;
  }
  if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && active_.has_value() &&
      !drag_.has_value()) {
    delete_active_node();
    return true;
  }
  return false;
}

void EditNodesTool::delete_active_node() {
  if (const Road* road = document_.network().road(active_->first)) {
    notify_derivation(*road);
  }
  const Expected<void> removed = document_.push_command(
      edit::delete_waypoint(document_.network(), active_->first, active_->second));
  if (!removed.has_value()) {
    emit status_message(
        tr("Cannot delete node: %1").arg(QString::fromStdString(removed.error().message)));
    return;
  }
  active_.reset();
  emit status_message(tr("Node deleted"));
  emit preview_changed();
}

PreviewGeometry EditNodesTool::preview() const {
  PreviewGeometry geometry;

  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr) {
      continue;
    }
    const std::vector<Waypoint> nodes = edit::effective_waypoints(*road);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      geometry.add_handle(nodes[i].x, nodes[i].y, 0.0, HandleKind::Node, node_state(road_id, i));
    }

    const auto stations = edit::waypoint_stations(*road);
    if (!stations.has_value()) {
      continue; // out-of-sync metadata: handles only, no tangents or markers
    }

    // Display-only tangent whiskers (02 §3): the fitted heading at each
    // node, scaled to a share of the shorter adjacent segment.
    for (std::size_t i = 0; i < stations->size(); ++i) {
      const double prev_len =
          i > 0 ? stations->at(i) - stations->at(i - 1) : stations->at(i + 1) - stations->at(i);
      const double next_len =
          i + 1 < stations->size() ? stations->at(i + 1) - stations->at(i) : prev_len;
      const double half =
          std::min(kTangentHalfMax, kTangentSegmentShare * std::min(prev_len, next_len));
      const PathPoint at = road->plan_view.evaluate(stations->at(i));
      const double dx = std::cos(at.hdg) * half;
      const double dy = std::sin(at.hdg) * half;
      append_segment(geometry, at.x - dx, at.y - dy, at.x + dx, at.y + dy);
    }

    // Midpoint insert markers, one per segment (the active-node emphasis is
    // now carried by the node handle's Hovered/Grabbed state above).
    for (std::size_t i = 0; i + 1 < stations->size(); ++i) {
      const PathPoint mid = road->plan_view.evaluate((stations->at(i) + stations->at(i + 1)) / 2.0);
      geometry.add_handle(mid.x, mid.y, 0.0, HandleKind::Midpoint, HandleState::Idle);
    }
  }

  if (drag_.has_value()) {
    append_node_drag_overlay(*drag_, geometry);
  }

  return geometry;
}

QString EditNodesTool::instruction() const {
  return tr("Drag a node to move it · click a midpoint marker to insert one · click a node then "
            "Delete removes it · Esc cancels");
}

} // namespace roadmaker::editor
