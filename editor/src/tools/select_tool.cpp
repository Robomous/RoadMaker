#include "tools/select_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

SelectMode mode_from(Qt::KeyboardModifiers modifiers) {
  if ((modifiers & Qt::ControlModifier) != 0) { // Cmd on macOS via Qt's mapping
    return SelectMode::Toggle;
  }
  if ((modifiers & Qt::ShiftModifier) != 0) {
    return SelectMode::Add;
  }
  return SelectMode::Replace;
}

/// Selected roads, deduplicated (road-level and lane entries of the same
/// road collapse to one), in selection order.
std::vector<RoadId> selected_roads(const SelectionModel& selection) {
  std::vector<RoadId> roads;
  for (const SelectionEntry& entry : selection.entries()) {
    if (std::ranges::find(roads, entry.road) == roads.end()) {
      roads.push_back(entry.road);
    }
  }
  return roads;
}

} // namespace

SelectTool::SelectTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void SelectTool::activate() {
  emit status_message(tr("Click to select — Shift adds, Ctrl toggles; drag from empty space for a "
                         "rubber band; drag a node handle of a selected road to move it"));
}

void SelectTool::deactivate() {
  abort_drag();
  if (press_.has_value() || band_current_.has_value()) {
    press_.reset();
    band_current_.reset();
    emit preview_changed();
  }
}

std::optional<SelectTool::DragState> SelectTool::pick_selected_node(const Waypoint& cursor) const {
  std::optional<DragState> best;
  double best_dist = pick_radius_;
  for (const RoadId road_id : selected_roads(selection_)) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr || !road->authoring_waypoints) {
      continue;
    }
    for (std::size_t i = 0; i < road->authoring_waypoints->size(); ++i) {
      const Waypoint& node = (*road->authoring_waypoints)[i];
      const double dist = std::hypot(cursor.x - node.x, cursor.y - node.y);
      if (dist <= best_dist) {
        best_dist = dist;
        best = DragState{.road = road_id, .index = i, .original = node, .current = node};
      }
    }
  }
  return best;
}

bool SelectTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || drag_.has_value() || press_.has_value()) {
    return false;
  }
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};
  // Node handles pick with priority over lane patches (02 §1) — but only on
  // selected roads, where handles are visible.
  if (auto grab = pick_selected_node(cursor)) {
    drag_ = std::move(grab);
    emit status_message(selection_.entries().size() > 1
                            ? tr("Moving the grabbed node — multi-node move comes later in M2. "
                                 "Esc cancels")
                            : tr("Moving node — Esc cancels"));
    emit preview_changed();
    return true;
  }
  press_ = PressState{.world = cursor, .on_geometry = event.pick.has_value()};
  return true;
}

bool SelectTool::mouse_move(const ToolEvent& event) {
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};

  if (drag_.has_value()) {
    edit::SnapOptions options = snap_options_;
    options.exclude_road = drag_->road;
    drag_->snap = edit::snap_point(document_.network(), cursor, options);
    const Waypoint target =
        drag_->snap ? Waypoint{.x = drag_->snap->position.x, .y = drag_->snap->position.y} : cursor;

    const RoadId road = drag_->road;
    const std::size_t index = drag_->index;
    const Expected<void> moved = document_.preview_active()
                                     ? document_.update_preview([&](const RoadNetwork& base) {
                                         return edit::move_waypoint(base, road, index, target);
                                       })
                                     : document_.begin_preview(edit::move_waypoint(
                                           document_.network(), road, index, target));
    if (moved.has_value()) {
      drag_->current = target;
    }
    // On failure the session (if any) stays at its last good state; the drag
    // keeps running so a later move can recover.
    emit preview_changed();
    return true;
  }

  if (press_.has_value()) {
    const bool beyond_tolerance = std::abs(cursor.x - press_->world.x) > click_tolerance_ ||
                                  std::abs(cursor.y - press_->world.y) > click_tolerance_;
    // Rubber bands span from empty space only; a drag that started on
    // geometry is consumed but inert (no road-body move in M2).
    if (!press_->on_geometry && (band_current_.has_value() || beyond_tolerance)) {
      band_current_ = cursor;
      emit preview_changed();
    }
    return true;
  }

  return false;
}

bool SelectTool::mouse_release(const ToolEvent& event) {
  if (drag_.has_value()) {
    // A grab that never previewed pushes nothing; commit_preview is a no-op
    // without an active session.
    document_.commit_preview();
    drag_.reset();
    emit status_message(tr("Node moved"));
    emit preview_changed();
    return true;
  }

  if (press_.has_value()) {
    if (band_current_.has_value()) {
      apply_band_selection(*band_current_, event.modifiers);
    } else {
      // Zero-area band (never left the click tolerance) degenerates to a
      // click-pick; a miss clears unless a modifier asks to keep.
      const SelectMode mode = mode_from(event.modifiers);
      if (event.pick.has_value()) {
        selection_.select({.road = event.pick->road, .lane = event.pick->lane}, mode);
      } else if (mode == SelectMode::Replace) {
        selection_.clear();
      }
    }
    press_.reset();
    band_current_.reset();
    emit preview_changed();
    return true;
  }

  return false;
}

bool SelectTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape) {
    return false;
  }
  if (drag_.has_value()) {
    abort_drag();
    return true;
  }
  if (press_.has_value()) {
    press_.reset();
    band_current_.reset();
    emit preview_changed();
    return true;
  }
  return false;
}

void SelectTool::apply_band_selection(const Waypoint& current, Qt::KeyboardModifiers modifiers) {
  const auto [x_lo, x_hi] = std::minmax(press_->world.x, current.x);
  const auto [y_lo, y_hi] = std::minmax(press_->world.y, current.y);

  std::vector<SelectionEntry> hits;
  for (const RoadMesh& road : document_.mesh().roads) {
    const RoadAabb aabb = compute_road_aabb(road);
    const bool overlaps =
        aabb.lo[0] <= x_hi && aabb.hi[0] >= x_lo && aabb.lo[1] <= y_hi && aabb.hi[1] >= y_lo;
    if (overlaps) {
      hits.push_back(SelectionEntry{.road = road.road, .lane = LaneId{}});
    }
  }
  selection_.select_many(hits, mode_from(modifiers));
}

void SelectTool::abort_drag() {
  if (!drag_.has_value()) {
    return;
  }
  document_.cancel_preview();
  drag_.reset();
  emit status_message(tr("Move cancelled"));
  emit preview_changed();
}

PreviewGeometry SelectTool::preview() const {
  PreviewGeometry geometry;

  // Node handles on every selected road; during a drag the network already
  // holds the live-previewed waypoints, so the moving handle tracks.
  for (const RoadId road_id : selected_roads(selection_)) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr || !road->authoring_waypoints) {
      continue;
    }
    for (const Waypoint& node : *road->authoring_waypoints) {
      geometry.point_positions.insert(geometry.point_positions.end(), {node.x, node.y, 0.0});
    }
  }

  if (drag_.has_value()) {
    if (drag_->snap) {
      geometry.point_positions.insert(geometry.point_positions.end(),
                                      {drag_->snap->position.x, drag_->snap->position.y, 0.0});
    }
    geometry.line_positions.insert(
        geometry.line_positions.end(),
        {drag_->original.x, drag_->original.y, 0.0, drag_->current.x, drag_->current.y, 0.0});
  }

  if (press_.has_value() && band_current_.has_value()) {
    const double x0 = press_->world.x;
    const double y0 = press_->world.y;
    const double x1 = band_current_->x;
    const double y1 = band_current_->y;
    geometry.line_positions.insert(geometry.line_positions.end(),
                                   {x0, y0, 0.0, x1, y0, 0.0,   // bottom
                                    x1, y0, 0.0, x1, y1, 0.0,   // right
                                    x1, y1, 0.0, x0, y1, 0.0,   // top
                                    x0, y1, 0.0, x0, y0, 0.0}); // left
  }

  return geometry;
}

} // namespace roadmaker::editor
