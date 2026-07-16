#include "tools/create_road_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/geometry/road_intersection.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "document/document.hpp"

namespace roadmaker::editor {

namespace {

// Overlay sizes in world meters, matching the Edit Nodes conventions.
constexpr double kSnapMarkerRadius = 1.0;  ///< snap hint diamond half-diagonal
constexpr double kSnapWhiskerLength = 5.0; ///< tangent hint along the locked heading
constexpr double kCurveSampleStep = 2.0;   ///< fitted-preview polyline step [m]

double wrap_angle(double angle) {
  return std::remainder(angle, 2.0 * std::numbers::pi);
}

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

void append_diamond(PreviewGeometry& geometry, double x, double y, double radius) {
  append_segment(geometry, x - radius, y, x, y + radius);
  append_segment(geometry, x, y + radius, x + radius, y);
  append_segment(geometry, x + radius, y, x, y - radius);
  append_segment(geometry, x, y - radius, x - radius, y);
}

void append_cross(PreviewGeometry& geometry, double x, double y, double radius) {
  append_segment(geometry, x - radius, y - radius, x + radius, y + radius);
  append_segment(geometry, x - radius, y + radius, x + radius, y - radius);
}

void append_polyline_of(PreviewGeometry& geometry, const ReferenceLine& line) {
  const double length = line.length();
  const int segments = std::max(1, static_cast<int>(std::ceil(length / kCurveSampleStep)));
  PathPoint previous = line.evaluate(0.0);
  for (int i = 1; i <= segments; ++i) {
    const PathPoint current = line.evaluate(length * i / segments);
    append_segment(geometry, previous.x, previous.y, current.x, current.y);
    previous = current;
  }
}

} // namespace

CreateRoadTool::CreateRoadTool(Document& document, QObject* parent)
    : Tool(parent), document_(document) {}

void CreateRoadTool::activate() {}

void CreateRoadTool::deactivate() {
  reset_session();
}

std::optional<edit::SnapResult> CreateRoadTool::snap(const Waypoint& cursor) const {
  return edit::snap_point(document_.network(), cursor, snap_options_);
}

EndpointHeadings CreateRoadTool::locked_headings() const {
  EndpointHeadings locked;
  if (!points_.empty()) {
    locked.start = points_.front().heading;
    if (points_.size() > 1 && points_.back().heading.has_value()) {
      // The snap heading points away from its source road; the created road
      // arrives there, so it closes onto the end with the reverse heading.
      locked.end = wrap_angle(*points_.back().heading + std::numbers::pi);
    }
  }
  return locked;
}

void CreateRoadTool::begin_at(double world_x, double world_y) {
  ToolEvent event;
  event.world_x = world_x;
  event.world_y = world_y;
  place_point(event);
  emit preview_changed();
}

void CreateRoadTool::place_point(const ToolEvent& event) {
  const Waypoint raw{.x = event.world_x, .y = event.world_y};
  const std::optional<edit::SnapResult> snapped = snap(raw);
  const Waypoint position = snapped.has_value() ? snapped->position : raw;

  if (!points_.empty()) {
    const Waypoint& last = points_.back().position;
    if (std::hypot(position.x - last.x, position.y - last.y) < tol::kLength) {
      emit status_message(tr("Waypoint rejected: coincides with the previous one"));
      return;
    }
  }

  // A side snap (near a road's body, not its end) tees the new road's end into
  // that road; an endpoint snap takes priority (that end welds/chains instead).
  std::optional<edit::SideSnap> side_snap;
  if (!snapped.has_value() || snapped->kind != edit::SnapKind::RoadEndpoint) {
    side_snap = edit::snap_to_road_side(document_.network(), raw, snap_options_);
  }

  const bool first_point = points_.empty();
  points_.push_back(PlacedPoint{
      .position = position,
      .heading = snapped.has_value() ? snapped->heading : std::nullopt,
      .snap_road = snapped.has_value() ? snapped->road : std::nullopt,
      .side_snap = side_snap,
  });

  // Extend-from-endpoint: the first point anchored on the SELECTED road's END
  // lengthens that road (same id) to the final point, rather than authoring a
  // new welded road. Only the END extends (the kernel refuses Start).
  if (first_point && selected_road_.has_value() && snapped.has_value() &&
      snapped->kind == edit::SnapKind::RoadEndpoint && snapped->road == *selected_road_) {
    if (const Road* road = document_.network().road(*selected_road_); road != nullptr) {
      const PathPoint end = road->plan_view.evaluate(road->plan_view.length());
      if (std::hypot(position.x - end.x, position.y - end.y) < tol::kLength * 1e3) {
        extend_end_ = RoadEnd{.road = *selected_road_, .contact = ContactPoint::End};
        emit status_message(tr("Extending the selected road — click ahead, then Enter"));
      }
    }
  }

  if (points_.size() == 1 && points_.front().heading.has_value()) {
    emit status_message(tr("Start locked to the road end — the new road chains tangentially"));
  } else {
    emit status_message(tr("%n waypoint(s) — Enter or double-click creates the road",
                           nullptr,
                           static_cast<int>(points_.size())));
  }
  emit preview_changed();
}

bool CreateRoadTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  place_point(event);
  return true;
}

bool CreateRoadTool::mouse_move(const ToolEvent& event) {
  const Waypoint raw{.x = event.world_x, .y = event.world_y};
  hover_snap_ = snap(raw);
  cursor_ = hover_snap_.has_value() ? hover_snap_->position : raw;
  emit preview_changed();
  return false; // hovering never consumes: camera nav and hover info stay live
}

bool CreateRoadTool::mouse_double_click(const ToolEvent& event) {
  static_cast<void>(event); // the pair's first press already placed the point
  commit();
  return true;
}

bool CreateRoadTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    commit();
    return true;
  }
  if (key == Qt::Key_Backspace) {
    if (points_.empty()) {
      return false;
    }
    points_.pop_back();
    emit status_message(tr("Removed the last waypoint"));
    emit preview_changed();
    return true;
  }
  if (key == Qt::Key_Escape) {
    if (points_.empty()) {
      return false;
    }
    reset_session();
    emit status_message(tr("Create Road cancelled"));
    return true;
  }
  return false;
}

void CreateRoadTool::commit() {
  if (points_.size() < 2) {
    emit status_message(tr("A road needs at least two waypoints"));
    return;
  }
  const RoadNetwork& network = document_.network();

  // Extend-from-endpoint: lengthen the selected road to the final point. One
  // command, same road id — not a new road.
  if (extend_end_.has_value()) {
    auto command = edit::extend_road(network, *extend_end_, points_.back().position);
    const Expected<void> pushed = document_.push_command(std::move(command));
    if (!pushed.has_value()) {
      emit status_message(
          tr("Cannot extend road: %1").arg(QString::fromStdString(pushed.error().message)));
      return;
    }
    reset_session();
    emit status_message(tr("Road extended"));
    return;
  }

  std::vector<Waypoint> waypoints;
  waypoints.reserve(points_.size());
  for (const PlacedPoint& point : points_) {
    waypoints.push_back(point.position);
  }
  const PlacedPoint& first = points_.front();
  const PlacedPoint& last = points_.back();

  // Which assembly fires, in priority order (02_editing_tools.md §2):
  //   (i)   first point on a road END  → create_linked_road (weld/chain);
  //   (ii)  first/last point on a road SIDE → create_teed_road (T-junction);
  //   (iii) the fitted line crosses a road interior → create_crossing_road (X);
  //   (iv)  otherwise → plain create_road.
  std::unique_ptr<edit::Command> command;
  if (first.snap_road.has_value() && network.road(*first.snap_road) != nullptr) {
    const auto& plan = network.road(*first.snap_road)->plan_view;
    const PathPoint start = plan.evaluate(0.0);
    const PathPoint end = plan.evaluate(plan.length());
    const double to_start = std::hypot(first.position.x - start.x, first.position.y - start.y);
    const double to_end = std::hypot(first.position.x - end.x, first.position.y - end.y);
    const RoadEnd source{.road = *first.snap_road,
                         .contact = to_start <= to_end ? ContactPoint::Start : ContactPoint::End};
    command = edit::create_linked_road(
        network, std::move(waypoints), profile_, {}, source, locked_headings());
  } else if (first.side_snap.has_value() || last.side_snap.has_value()) {
    const bool tee_first = first.side_snap.has_value();
    const edit::SideSnap& snap = tee_first ? *first.side_snap : *last.side_snap;
    const ContactPoint teed = tee_first ? ContactPoint::Start : ContactPoint::End;
    command = edit::create_teed_road(
        network, std::move(waypoints), profile_, {}, snap.road, snap.s, teed, locked_headings());
  } else {
    // Does the fitted reference line cross an existing road's interior?
    std::optional<BodyCrossing> crossing;
    if (const auto line = fit_clothoid_path(waypoints, locked_headings()); line.has_value()) {
      crossing = first_body_crossing(network, *line, RoadId{});
    }
    if (crossing.has_value()) {
      command = edit::create_crossing_road(
          network, std::move(waypoints), profile_, {}, crossing->road, locked_headings());
    } else {
      command = edit::create_road(std::move(waypoints), profile_, {}, locked_headings());
    }
  }
  const Expected<void> pushed = document_.push_command(std::move(command));
  if (!pushed.has_value()) {
    // Session kept: the points are the user's work; fix and retry.
    emit status_message(
        tr("Cannot create road: %1").arg(QString::fromStdString(pushed.error().message)));
    return;
  }
  reset_session();
  emit status_message(tr("Road created"));
}

void CreateRoadTool::reset_session() {
  points_.clear();
  hover_snap_.reset();
  cursor_.reset();
  extend_end_.reset();
  emit preview_changed();
}

PreviewGeometry CreateRoadTool::preview() const {
  PreviewGeometry geometry;

  for (const PlacedPoint& point : points_) {
    geometry.add_handle(point.position.x, point.position.y);
  }

  // Ghost polyline through the placed points and on to the cursor.
  for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
    append_segment(geometry,
                   points_[i].position.x,
                   points_[i].position.y,
                   points_[i + 1].position.x,
                   points_[i + 1].position.y);
  }
  if (!points_.empty() && cursor_.has_value()) {
    append_segment(
        geometry, points_.back().position.x, points_.back().position.y, cursor_->x, cursor_->y);
  }

  // Fitted-clothoid preview through the placed points plus the cursor as a
  // provisional last point — the same fit the commit will push (§2). A
  // failed fit silently leaves just the ghost polyline.
  std::vector<Waypoint> candidate;
  candidate.reserve(points_.size() + 1);
  for (const PlacedPoint& point : points_) {
    candidate.push_back(point.position);
  }
  if (cursor_.has_value() && !points_.empty() &&
      std::hypot(cursor_->x - points_.back().position.x, cursor_->y - points_.back().position.y) >=
          tol::kLength) {
    candidate.push_back(*cursor_);
  }
  if (candidate.size() >= 2) {
    EndpointHeadings locked;
    locked.start = points_.front().heading;
    if (candidate.size() > points_.size() && hover_snap_.has_value() &&
        hover_snap_->heading.has_value()) {
      locked.end = wrap_angle(*hover_snap_->heading + std::numbers::pi);
    } else if (candidate.size() == points_.size()) {
      locked.end = locked_headings().end;
    }
    if (const auto line = fit_clothoid_path(candidate, locked); line.has_value()) {
      append_polyline_of(geometry, *line);
      // Cross hint: an X where the fitted line would cross a road interior, so
      // the user sees the 4-way will fire before committing.
      if (!points_.front().side_snap.has_value() && !points_.front().snap_road.has_value()) {
        if (const auto crossing = first_body_crossing(document_.network(), *line, RoadId{});
            crossing.has_value()) {
          append_cross(geometry, crossing->point.x, crossing->point.y, kSnapMarkerRadius * 1.5);
        }
      }
    }
  }

  // Tee hint: a diamond at any endpoint (placed or hovered) that snapped onto a
  // road's side — the T-junction that will form there.
  for (const PlacedPoint& point : points_) {
    if (point.side_snap.has_value()) {
      append_diamond(
          geometry, point.side_snap->position.x, point.side_snap->position.y, kSnapMarkerRadius);
    }
  }
  if (hover_snap_.has_value() && cursor_.has_value() && !points_.empty()) {
    if (const auto side = edit::snap_to_road_side(document_.network(), *cursor_, snap_options_);
        side.has_value() && hover_snap_->kind != edit::SnapKind::RoadEndpoint) {
      append_diamond(geometry, side->position.x, side->position.y, kSnapMarkerRadius);
    }
  }

  // Snap hint: diamond at the snapped cursor, whisker along a continuation
  // heading when the snap carries one.
  if (hover_snap_.has_value()) {
    append_diamond(geometry, hover_snap_->position.x, hover_snap_->position.y, kSnapMarkerRadius);
    if (hover_snap_->heading.has_value()) {
      const double dx = std::cos(*hover_snap_->heading) * kSnapWhiskerLength;
      const double dy = std::sin(*hover_snap_->heading) * kSnapWhiskerLength;
      append_segment(geometry,
                     hover_snap_->position.x,
                     hover_snap_->position.y,
                     hover_snap_->position.x + dx,
                     hover_snap_->position.y + dy);
    }
  }

  return geometry;
}

QString CreateRoadTool::instruction() const {
  return tr("Click to place waypoints · Enter or double-click creates the road · Backspace removes "
            "the last · Esc cancels");
}

} // namespace roadmaker::editor
