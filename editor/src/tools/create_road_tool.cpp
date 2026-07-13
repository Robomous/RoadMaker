#include "tools/create_road_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/reference_line.hpp"
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

void CreateRoadTool::activate() {
  emit status_message(tr("Click to place waypoints — Enter or double-click creates the road, "
                         "Backspace removes the last point, Esc cancels"));
}

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

  points_.push_back(PlacedPoint{
      .position = position,
      .heading = snapped.has_value() ? snapped->heading : std::nullopt,
  });
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
  std::vector<Waypoint> waypoints;
  waypoints.reserve(points_.size());
  for (const PlacedPoint& point : points_) {
    waypoints.push_back(point.position);
  }
  const Expected<void> pushed = document_.push_command(
      edit::create_road(std::move(waypoints), profile_, {}, locked_headings()));
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

} // namespace roadmaker::editor
