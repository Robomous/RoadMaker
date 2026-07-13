#include "tools/select_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
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

SelectMode mode_from(Qt::KeyboardModifiers modifiers) {
  if ((modifiers & Qt::ControlModifier) != 0) { // Cmd on macOS via Qt's mapping
    return SelectMode::Toggle;
  }
  if ((modifiers & Qt::ShiftModifier) != 0) {
    return SelectMode::Add;
  }
  return SelectMode::Replace;
}

} // namespace

SelectTool::SelectTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void SelectTool::activate() {
  emit status_message(tr("Click to select — Shift adds, Ctrl toggles; drag a road body to move the "
                         "whole road, a node handle to bend it, or empty space for a rubber band"));
}

void SelectTool::deactivate() {
  abort_drag();
  abort_move();
  if (press_.has_value() || band_current_.has_value()) {
    press_.reset();
    band_current_.reset();
    emit preview_changed();
  }
}

std::optional<NodeDragState> SelectTool::pick_selected_node(const Waypoint& cursor) const {
  std::optional<NodeDragState> best;
  double best_dist = pick_radius_;
  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr || !road->authoring_waypoints) {
      continue;
    }
    for (std::size_t i = 0; i < road->authoring_waypoints->size(); ++i) {
      const Waypoint& node = (*road->authoring_waypoints)[i];
      const double dist = std::hypot(cursor.x - node.x, cursor.y - node.y);
      if (dist <= best_dist) {
        best_dist = dist;
        best = NodeDragState{.road = road_id, .index = i, .original = node, .current = node};
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
  press_ = PressState{.world = cursor,
                      .on_geometry = event.pick.has_value(),
                      .road = event.pick.has_value() ? std::optional<RoadId>(event.pick->road)
                                                     : std::nullopt};
  return true;
}

bool SelectTool::mouse_move(const ToolEvent& event) {
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};

  if (drag_.has_value()) {
    update_node_drag(document_, *drag_, snap_options_, cursor);
    emit preview_changed();
    return true;
  }

  if (move_.has_value()) {
    update_move_drag(cursor);
    emit preview_changed();
    return true;
  }

  if (press_.has_value()) {
    const bool beyond_tolerance = std::abs(cursor.x - press_->world.x) > click_tolerance_ ||
                                  std::abs(cursor.y - press_->world.y) > click_tolerance_;
    // A drag that started on a road body moves the whole road (auto-selecting
    // it first); one from empty space spans a rubber band.
    if (press_->on_geometry) {
      if (beyond_tolerance) {
        begin_move_drag(event.modifiers);
        if (move_.has_value()) {
          update_move_drag(cursor);
          emit preview_changed();
        }
      }
      return true;
    }
    if (band_current_.has_value() || beyond_tolerance) {
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

  if (move_.has_value()) {
    const std::size_t count = move_->roads.size();
    // A move that never crossed a fit / preview commits nothing (no-op).
    document_.commit_preview();
    move_.reset();
    emit cursor_changed(Qt::ArrowCursor);
    emit status_message(count == 1 ? tr("Road moved — Ctrl+Z to undo")
                                   : tr("%1 roads moved — Ctrl+Z to undo").arg(count));
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
  if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
    // Mid-gesture the keys are inert — Esc is the way out of a drag/band.
    return !drag_.has_value() && !press_.has_value() && delete_selection();
  }
  if (key != Qt::Key_Escape) {
    return false;
  }
  if (drag_.has_value()) {
    abort_drag();
    return true;
  }
  if (move_.has_value()) {
    abort_move();
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

bool SelectTool::delete_selection() {
  std::vector<RoadId> doomed;
  for (const RoadId road_id : selection_.selected_roads()) {
    if (document_.network().road(road_id) != nullptr) {
      doomed.push_back(road_id);
    }
  }
  if (doomed.empty()) {
    return false;
  }

  // One QUndoStack macro = one undo step for the whole selection (02 §7).
  // Each delete_road carries its referential closure, so an earlier command
  // in the macro may have already taken a later selected road with it —
  // those are skipped, not errors.
  const bool macro = doomed.size() > 1;
  if (macro) {
    document_.undo_stack()->beginMacro(tr("Delete %1 Roads").arg(doomed.size()));
  }
  int deleted = 0;
  for (const RoadId road_id : doomed) {
    if (document_.network().road(road_id) == nullptr) {
      continue;
    }
    if (document_.push_command(edit::delete_road(document_.network(), road_id))) {
      ++deleted;
    }
  }
  if (macro) {
    document_.undo_stack()->endMacro();
  }
  emit status_message(deleted == 1 ? tr("Deleted 1 road — Ctrl+Z restores")
                                   : tr("Deleted %1 roads — Ctrl+Z restores").arg(deleted));
  emit preview_changed(); // the deleted roads' node handles vanish
  return true;
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

void SelectTool::begin_move_drag(Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (!press_.has_value() || !press_->road.has_value()) {
    return;
  }
  const RoadId pressed = *press_->road;
  const RoadNetwork& network = document_.network();

  // Move set: the whole selection when the pressed road is part of it, else the
  // pressed road alone (auto-selected below, only once the refusals pass).
  const std::vector<RoadId> selected = selection_.selected_roads();
  const bool pressed_selected = std::ranges::find(selected, pressed) != selected.end();
  std::vector<RoadId> roads =
      pressed_selected && !selected.empty() ? selected : std::vector<RoadId>{pressed};

  // Junction roads have generated poses — refuse with a toast, touch nothing.
  for (const RoadId road_id : roads) {
    const std::vector<JunctionId> touched = junctions_touching(network, road_id);
    if (!touched.empty()) {
      const Road* road = network.road(road_id);
      const Junction* junction = network.junction(touched.front());
      emit status_message(
          tr("Road %1 belongs to Junction %2 — junction roads can't be moved. Delete the "
             "junction or move its free end nodes instead.")
              .arg(road != nullptr ? QString::fromStdString(road->odr_id) : QString())
              .arg(junction != nullptr ? QString::fromStdString(junction->odr_id) : QString()));
      press_.reset();
      return;
    }
  }

  // A link leaving the moved set breaks — confirm BEFORE begin_preview (a modal
  // opened mid-drag would swallow the mouse-release).
  const auto in_set = [&roads](RoadId id) { return std::ranges::find(roads, id) != roads.end(); };
  const auto leaves_set = [&](const std::optional<RoadLink>& link) {
    if (!link.has_value()) {
      return false;
    }
    const auto* target = std::get_if<RoadId>(&link->target);
    return target != nullptr && !in_set(*target);
  };
  bool breaks_link = false;
  for (const RoadId road_id : roads) {
    const Road* road = network.road(road_id);
    if (road != nullptr && (leaves_set(road->predecessor) || leaves_set(road->successor))) {
      breaks_link = true;
      break;
    }
  }
  if (breaks_link && confirm_link_break_ && !confirm_link_break_()) {
    press_.reset();
    return;
  }

  if (!pressed_selected) {
    selection_.select({.road = pressed, .lane = LaneId{}}, SelectMode::Replace);
  }

  const std::size_t count = roads.size();
  move_ =
      MoveDragState{.roads = std::move(roads), .press = press_->world, .current = press_->world};
  press_.reset();
  emit cursor_changed(Qt::SizeAllCursor);
  emit status_message(count == 1
                          ? tr("Moving road — release to place, Esc cancels")
                          : tr("Moving %1 roads — release to place, Esc cancels").arg(count));
}

void SelectTool::update_move_drag(const Waypoint& cursor) {
  if (!move_.has_value()) {
    return;
  }
  double dx = cursor.x - move_->press.x;
  double dy = cursor.y - move_->press.y;
  move_->snap.reset();

  // Single-road drags snap the nearer translated endpoint to another road's
  // endpoint; multi-road drags don't snap in v1 (documented follow-up — a
  // multi-road exclude span is a kernel/snap enhancement).
  if (move_->roads.size() == 1) {
    const Road* road = document_.network().road(move_->roads.front());
    if (road != nullptr && !road->plan_view.empty()) {
      edit::SnapOptions options = snap_options_;
      options.exclude_road = move_->roads.front();
      options.endpoints = true;
      options.tangent = false;
      const PathPoint start = road->plan_view.evaluate(0.0);
      const PathPoint end = road->plan_view.evaluate(road->plan_view.length());
      double best = options.radius;
      double adj_x = 0.0;
      double adj_y = 0.0;
      for (const PathPoint& endpoint : {start, end}) {
        const Waypoint candidate{.x = endpoint.x + dx, .y = endpoint.y + dy};
        const auto snap = edit::snap_point(document_.network(), candidate, options);
        if (!snap.has_value()) {
          continue;
        }
        const double distance =
            std::hypot(snap->position.x - candidate.x, snap->position.y - candidate.y);
        if (distance <= best) {
          best = distance;
          adj_x = snap->position.x - candidate.x;
          adj_y = snap->position.y - candidate.y;
          move_->snap = snap;
        }
      }
      dx += adj_x;
      dy += adj_y;
    }
  }

  const std::vector<RoadId> roads = move_->roads;
  const Expected<void> moved =
      document_.preview_active()
          ? document_.update_preview([&roads, dx, dy](const RoadNetwork& base) {
              return edit::translate_roads(base, roads, dx, dy);
            })
          : document_.begin_preview(edit::translate_roads(document_.network(), roads, dx, dy));
  static_cast<void>(moved);
  move_->current = Waypoint{.x = move_->press.x + dx, .y = move_->press.y + dy};
}

void SelectTool::abort_move() {
  if (!move_.has_value()) {
    return;
  }
  document_.cancel_preview();
  move_.reset();
  emit cursor_changed(Qt::ArrowCursor);
  emit status_message(tr("Move cancelled"));
  emit preview_changed();
}

PreviewGeometry SelectTool::preview() const {
  PreviewGeometry geometry;

  // Node handles on every selected road; during a drag the network already
  // holds the live-previewed waypoints, so the moving handle tracks.
  for (const RoadId road_id : selection_.selected_roads()) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr || !road->authoring_waypoints) {
      continue;
    }
    for (const Waypoint& node : *road->authoring_waypoints) {
      geometry.point_positions.insert(geometry.point_positions.end(), {node.x, node.y, 0.0});
    }
  }

  if (drag_.has_value()) {
    append_node_drag_overlay(*drag_, geometry);
  }

  if (move_.has_value()) {
    // Press→current tether, plus the engaged endpoint-snap marker.
    geometry.line_positions.insert(
        geometry.line_positions.end(),
        {move_->press.x, move_->press.y, 0.0, move_->current.x, move_->current.y, 0.0});
    if (move_->snap.has_value()) {
      geometry.point_positions.insert(geometry.point_positions.end(),
                                      {move_->snap->position.x, move_->snap->position.y, 0.0});
    }
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
