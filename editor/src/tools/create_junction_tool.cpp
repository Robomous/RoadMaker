#include "tools/create_junction_tool.hpp"

#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>

#include "document/document.hpp"

namespace roadmaker::editor {

CreateJunctionTool::CreateJunctionTool(Document& document, QObject* parent)
    : Tool(parent), document_(document) {}

void CreateJunctionTool::activate() {
  reset_session();
  emit status_message(tr("Create Junction — click 2+ road ends, Enter generates, Esc cancels"));
}

void CreateJunctionTool::deactivate() {
  reset_session();
}

std::optional<RoadEnd> CreateJunctionTool::snap_end(const Waypoint& cursor) const {
  // Endpoints only: the tool selects road ends, never tangent rays or grid.
  edit::SnapOptions options = snap_options_;
  options.endpoints = true;
  options.tangent = false;
  options.grid = std::nullopt;
  const auto snap = edit::snap_point(document_.network(), cursor, options);
  if (!snap.has_value() || snap->kind != edit::SnapKind::RoadEndpoint || !snap->road.has_value()) {
    return std::nullopt;
  }
  const Road* road = document_.network().road(*snap->road);
  if (road == nullptr) {
    return std::nullopt;
  }
  // Resolve which end the snap landed on by proximity to the two endpoints.
  const auto start = road->plan_view.evaluate(0.0);
  const auto end = road->plan_view.evaluate(road->plan_view.length());
  const double to_start = std::hypot(snap->position.x - start.x, snap->position.y - start.y);
  const double to_end = std::hypot(snap->position.x - end.x, snap->position.y - end.y);
  return RoadEnd{.road = *snap->road,
                 .contact = to_start <= to_end ? ContactPoint::Start : ContactPoint::End};
}

std::optional<edit::SideSnap> CreateJunctionTool::snap_side(const Waypoint& cursor) const {
  if (ends_.size() != 1) {
    return std::nullopt; // the tee needs exactly one attaching end
  }
  edit::SnapOptions options = snap_options_;
  // A body point of a road whose end is already selected is not a tee
  // target — that end IS the junction arm.
  options.exclude_road = ends_.front().road;
  return edit::snap_to_road_side(document_.network(), cursor, options);
}

void CreateJunctionTool::toggle(const RoadEnd& end) {
  const auto it = std::ranges::find(ends_, end);
  if (it != ends_.end()) {
    ends_.erase(it); // clicking a selected end again deselects it
  } else {
    ends_.push_back(end);
  }
}

bool CreateJunctionTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // LMB belongs to the tool even on a miss (M2 button map); a hit near a road
  // end toggles it into the selection. Endpoint snapping wins over the side
  // anchor inside its own radius — clicking an end must never read as a tee.
  const Waypoint cursor{event.world_x, event.world_y};
  if (const auto end = snap_end(cursor); end.has_value()) {
    side_anchor_.reset();
    toggle(*end);
    emit preview_changed();
    emit_count_status();
    return true;
  }
  if (const auto side = snap_side(cursor); side.has_value()) {
    side_anchor_ = side;
    emit preview_changed();
    const Road* target = document_.network().road(side->road);
    emit status_message(tr("Tee into %1 at s=%2 m — Enter attaches, Esc cancels")
                            .arg(QString::fromStdString(target->name))
                            .arg(side->s, 0, 'f', 1));
  }
  return true;
}

bool CreateJunctionTool::mouse_move(const ToolEvent& event) {
  hover_ = Waypoint{event.world_x, event.world_y};
  emit preview_changed();
  return false; // hover never consumes: camera/other handlers still see it
}

bool CreateJunctionTool::key_press(int key, Qt::KeyboardModifiers /*modifiers*/) {
  if (key == Qt::Key_Escape) {
    reset_session();
    emit preview_changed();
    emit status_message(tr("Create Junction — selection cleared"));
    return true;
  }
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    if (side_anchor_.has_value()) {
      generate_t_attach();
    } else {
      generate();
    }
    return true;
  }
  return false;
}

void CreateJunctionTool::generate_t_attach() {
  if (ends_.size() != 1 || !side_anchor_.has_value()) {
    emit status_message(tr("Tee needs exactly 1 selected road end plus a road-side anchor"));
    return;
  }
  // ONE undo step: attach_t_junction composes split + delete + junction.
  if (!document_.push_command(edit::attach_t_junction(
          document_.network(), ends_.front(), side_anchor_->road, side_anchor_->s))) {
    return; // push_command surfaced the failure in Diagnostics
  }
  reset_session();
  emit preview_changed();
  emit status_message(tr("T-junction attached"));
}

void CreateJunctionTool::generate() {
  if (ends_.size() < 2) {
    emit status_message(tr("Select at least 2 road ends before generating"));
    return;
  }
  // Preview first so the status bar can report dropped turns the generator
  // omits (nearly-parallel arms whose fit would loop).
  const auto preview = edit::preview_junction(document_.network(), ends_);
  if (!preview.has_value()) {
    emit status_message(
        tr("Cannot create junction: %1").arg(QString::fromStdString(preview.error().message)));
    return;
  }
  if (!document_.push_command(edit::create_junction(document_.network(), ends_))) {
    return; // push_command already logged and surfaced the failure
  }
  QString message = tr("Created junction with %1 connection(s)").arg(preview->connection_count);
  if (!preview->dropped_turns.empty()) {
    message += tr(" — %1 turn(s) dropped").arg(static_cast<int>(preview->dropped_turns.size()));
  }
  reset_session();
  emit preview_changed();
  emit status_message(message);
}

PreviewGeometry CreateJunctionTool::preview() const {
  PreviewGeometry geometry;
  const auto add_point = [&](double x, double y) {
    geometry.point_positions.insert(geometry.point_positions.end(), {x, y, 0.0});
  };
  for (const RoadEnd& end : ends_) {
    const Road* road = document_.network().road(end.road);
    if (road == nullptr) {
      continue;
    }
    const auto pose = road->plan_view.evaluate(
        end.contact == ContactPoint::Start ? 0.0 : road->plan_view.length());
    add_point(pose.x, pose.y);
  }
  if (side_anchor_.has_value()) {
    add_point(side_anchor_->position.x, side_anchor_->position.y);
  }
  if (hover_.has_value()) {
    if (const auto end = snap_end(*hover_); end.has_value()) {
      const Road* road = document_.network().road(end->road);
      const auto pose = road->plan_view.evaluate(
          end->contact == ContactPoint::Start ? 0.0 : road->plan_view.length());
      add_point(pose.x, pose.y);
    } else if (const auto side = snap_side(*hover_); side.has_value()) {
      // The side-snap indicator on the target's reference line.
      add_point(side->position.x, side->position.y);
    }
  }
  return geometry;
}

void CreateJunctionTool::reset_session() {
  ends_.clear();
  side_anchor_.reset();
  hover_.reset();
}

void CreateJunctionTool::emit_count_status() {
  emit status_message(tr("%1 road end(s) selected — Enter generates, Esc cancels")
                          .arg(static_cast<int>(ends_.size())));
}

} // namespace roadmaker::editor
