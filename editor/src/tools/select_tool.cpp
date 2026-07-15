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
  if (move_mode_) {
    emit status_message(tr("Move tool — hover shows the 4-arrow cursor; drag a road or a prop to "
                           "move it, or click to select it and use the transform gizmo (Esc "
                           "cancels)"));
    return;
  }
  emit status_message(tr("Click to select — Shift adds, Ctrl toggles; drag a road body to move the "
                         "whole road, a node handle to bend it, or empty space for a rubber band"));
}

void SelectTool::deactivate() {
  abort_drag();
  abort_move();
  abort_object_move();
  hover_cursor_ = Qt::ArrowCursor;
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
  // A prop hit also carries its owning road; record it as a prop press so a
  // drag moves the PROP, not the road under it. A junction-floor pick carries no
  // road, so it never starts a body move (it only selects on release).
  const bool on_object = event.pick.has_value() && event.pick->object.is_valid();
  // A signal pick carries its owning road, but a signal is a leaf like a prop —
  // a press on it must not start a body-move of the road under it. It only
  // selects on release (signal drag-move is the gizmo's job, not the body drag).
  const bool on_signal = event.pick.has_value() && event.pick->signal.is_valid();
  const bool on_road =
      event.pick.has_value() && event.pick->road.is_valid() && !on_object && !on_signal;
  press_ =
      PressState{.world = cursor,
                 .on_geometry = on_road,
                 .road = on_road ? std::optional<RoadId>(event.pick->road) : std::nullopt,
                 .object = on_object ? std::optional<ObjectId>(event.pick->object) : std::nullopt,
                 .object_road = on_object ? event.pick->road : RoadId{}};
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

  if (object_move_.has_value()) {
    update_object_move(cursor);
    emit preview_changed();
    return true;
  }

  if (press_.has_value()) {
    const bool beyond_tolerance = std::abs(cursor.x - press_->world.x) > click_tolerance_ ||
                                  std::abs(cursor.y - press_->world.y) > click_tolerance_;
    // A drag on a prop moves the prop; on a road body moves the whole road
    // (auto-selecting it first); from empty space it spans a rubber band —
    // except the Move tool, which never bands.
    if (press_->object.has_value()) {
      if (beyond_tolerance) {
        begin_object_move(*press_->object, press_->object_road);
        if (object_move_.has_value()) {
          update_object_move(cursor);
          emit preview_changed();
        }
      }
      return true;
    }
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
    if (!move_mode_ && (band_current_.has_value() || beyond_tolerance)) {
      band_current_ = cursor;
      emit preview_changed();
    }
    return true;
  }

  // Plain hover (no gesture): in move mode show the 4-arrow over a movable
  // entity. Return false so the viewport's hover readout still runs.
  update_move_cursor(event.pick.has_value() &&
                     (event.pick->object.is_valid() || event.pick->road.is_valid()));
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

  if (object_move_.has_value()) {
    // A prop drag that never crossed the tolerance commits nothing (no-op).
    document_.commit_preview();
    object_move_.reset();
    hover_cursor_ = Qt::ArrowCursor;
    emit cursor_changed(Qt::ArrowCursor);
    emit status_message(tr("Prop moved — Ctrl+Z to undo"));
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
        selection_.select({.road = event.pick->road,
                           .lane = event.pick->lane,
                           .object = event.pick->object,
                           .signal = event.pick->signal,
                           .junction = event.pick->junction},
                          mode);
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

bool SelectTool::mouse_double_click(const ToolEvent& event) {
  if (drag_.has_value() || !event.pick.has_value()) {
    return false;
  }
  const RoadId road_id = event.pick->road;
  const Road* road = document_.network().road(road_id);
  if (road == nullptr) {
    return false;
  }
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
  // Committed; hand off to Edit Nodes to grab the fresh node for shaping.
  selection_.select({.road = road_id, .lane = LaneId{}}, SelectMode::Replace);
  emit status_message(tr("Bend point inserted — drag to shape it"));
  emit edit_nodes_requested(road_id, index);
  return true;
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
  if (object_move_.has_value()) {
    abort_object_move();
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
  std::vector<RoadId> doomed_roads;
  for (const RoadId road_id : selection_.selected_roads()) {
    if (document_.network().road(road_id) != nullptr) {
      doomed_roads.push_back(road_id);
    }
  }
  std::vector<ObjectId> doomed_objects;
  for (const ObjectId object_id : selection_.selected_objects()) {
    if (document_.network().object(object_id) != nullptr) {
      doomed_objects.push_back(object_id);
    }
  }
  std::vector<SignalId> doomed_signals;
  for (const SignalId signal_id : selection_.selected_signals()) {
    if (document_.network().signal(signal_id) != nullptr) {
      doomed_signals.push_back(signal_id);
    }
  }
  const std::size_t total = doomed_roads.size() + doomed_objects.size() + doomed_signals.size();
  if (total == 0) {
    return false;
  }

  // One QUndoStack macro = one undo step for the whole selection (02 §7).
  // Each delete_road carries its referential closure, so an earlier command
  // in the macro may have already taken a later selected road (or its props)
  // with it — those are skipped, not errors. Props go first: they are leaves,
  // and deleting an owning road would otherwise cascade them out from under a
  // still-pending delete_object.
  const bool macro = total > 1;
  if (macro) {
    document_.undo_stack()->beginMacro(tr("Delete %1 Items").arg(total));
  }
  for (const ObjectId object_id : doomed_objects) {
    if (document_.network().object(object_id) != nullptr) {
      (void)document_.push_command(edit::delete_object(document_.network(), object_id));
    }
  }
  for (const SignalId signal_id : doomed_signals) {
    if (document_.network().signal(signal_id) != nullptr) {
      (void)document_.push_command(edit::delete_signal(document_.network(), signal_id));
    }
  }
  int deleted_roads = 0;
  for (const RoadId road_id : doomed_roads) {
    if (document_.network().road(road_id) == nullptr) {
      continue;
    }
    if (document_.push_command(edit::delete_road(document_.network(), road_id))) {
      ++deleted_roads;
    }
  }
  if (macro) {
    document_.undo_stack()->endMacro();
  }
  const auto deleted_objects = doomed_objects.size();
  const auto deleted_signals = doomed_signals.size();
  const int kinds =
      (deleted_roads > 0 ? 1 : 0) + (deleted_objects > 0 ? 1 : 0) + (deleted_signals > 0 ? 1 : 0);
  if (kinds > 1) {
    emit status_message(tr("Deleted selection — Ctrl+Z restores"));
  } else if (deleted_signals > 0) {
    emit status_message(deleted_signals == 1
                            ? tr("Deleted 1 signal — Ctrl+Z restores")
                            : tr("Deleted %1 signals — Ctrl+Z restores").arg(deleted_signals));
  } else if (deleted_objects > 0) {
    emit status_message(deleted_objects == 1
                            ? tr("Deleted 1 object — Ctrl+Z restores")
                            : tr("Deleted %1 objects — Ctrl+Z restores").arg(deleted_objects));
  } else {
    emit status_message(deleted_roads == 1
                            ? tr("Deleted 1 road — Ctrl+Z restores")
                            : tr("Deleted %1 roads — Ctrl+Z restores").arg(deleted_roads));
  }
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

void SelectTool::begin_object_move(ObjectId object, RoadId road) {
  object_move_ = ObjectMoveState{.object = object, .road = road};
  // Auto-select the grabbed prop (its owning road comes along, matching a click
  // on the prop) so the properties panel and gizmo track it.
  selection_.select({.road = road, .lane = LaneId{}, .object = object, .junction = JunctionId{}},
                    SelectMode::Replace);
  press_.reset();
  emit cursor_changed(Qt::SizeAllCursor);
  emit status_message(tr("Moving prop — release to place, Esc cancels"));
}

void SelectTool::update_object_move(const Waypoint& cursor) {
  if (!object_move_.has_value()) {
    return;
  }
  const Road* road = document_.network().road(object_move_->road);
  if (road == nullptr || road->plan_view.empty()) {
    return;
  }
  // Props are road-relative: re-project the cursor onto the owning road and
  // preview move_object at the new station. One command commits on release.
  //
  // A cursor dragged clear of the road holds the prop at its last good station
  // instead of flinging it out into the grass: find_station bounds s to the
  // road's length but leaves t unbounded, and move_object validates s but not
  // t, so an unguarded frame out here would happily succeed. Same threshold as
  // the Library drop, so dragging and dropping agree on where the road ends.
  const std::optional<StationCoord> st =
      station_within(road->plan_view, cursor.x, cursor.y, kObjectSnapThreshold);
  if (!st.has_value()) {
    if (!object_move_->off_road) {
      object_move_->off_road = true;
      emit status_message(tr("Keep the prop on or beside its road — Esc cancels"));
    }
    return;
  }
  if (object_move_->off_road) {
    object_move_->off_road = false;
    emit status_message(tr("Moving prop — release to place, Esc cancels"));
  }
  const ObjectId object = object_move_->object;
  const double s = st->s;
  const double t = st->t;
  const Expected<void> moved =
      document_.preview_active()
          ? document_.update_preview([object, s, t](const RoadNetwork& base) {
              return edit::move_object(base, object, s, t);
            })
          : document_.begin_preview(edit::move_object(document_.network(), object, s, t));
  static_cast<void>(moved);
}

void SelectTool::abort_object_move() {
  if (!object_move_.has_value()) {
    return;
  }
  document_.cancel_preview();
  object_move_.reset();
  hover_cursor_ = Qt::ArrowCursor;
  emit cursor_changed(Qt::ArrowCursor);
  emit status_message(tr("Move cancelled"));
  emit preview_changed();
}

void SelectTool::update_move_cursor(bool over_movable) {
  if (!move_mode_) {
    return;
  }
  const Qt::CursorShape shape = over_movable ? Qt::SizeAllCursor : Qt::ArrowCursor;
  if (shape != hover_cursor_) {
    hover_cursor_ = shape;
    emit cursor_changed(shape);
  }
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
      geometry.add_handle(node.x, node.y);
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
      geometry.add_handle(move_->snap->position.x,
                          move_->snap->position.y,
                          0.0,
                          HandleKind::Node,
                          HandleState::Hovered);
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
