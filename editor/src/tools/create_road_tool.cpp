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

#include "tools/create_road_tool.hpp"

#include "roadmaker/edit/assembly.hpp"
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
  if (points_.empty()) {
    return locked;
  }
  // Lock a fit heading ONLY on a genuine road-END snap (chaining intent). A bare
  // tangent-continuation hit (cursor near a road's forward extension ray) also
  // carries a heading, but locking it silently forces a fixed-heading Hermite
  // that loops into a teardrop when the heading opposes the chord (#352). Its
  // position is still snapped; only the heading lock is withheld.
  const PlacedPoint& first = points_.front();
  if (first.kind == edit::SnapKind::RoadEndpoint) {
    locked.start = first.heading;
  }
  const PlacedPoint& last = points_.back();
  if (points_.size() > 1 && last.kind == edit::SnapKind::RoadEndpoint && last.heading.has_value()) {
    // The snap heading points away from its source road; the created road
    // arrives there, so it closes onto the end with the reverse heading.
    locked.end = wrap_angle(*last.heading + std::numbers::pi);
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
      .kind = snapped.has_value() ? snapped->kind : edit::SnapKind::Grid,
      .snap_road = snapped.has_value() ? snapped->road : std::nullopt,
      .side_snap = side_snap,
  });

  // Extend-from-endpoint: the first point anchored on the SELECTED road's
  // NEARER endpoint lengthens that road (same id) to the final point, rather
  // than authoring a new welded road. Either end extends now (#275) — the
  // kernel prepends/reverses for a Start, appends for an End.
  if (first_point && selected_road_.has_value() && snapped.has_value() &&
      snapped->kind == edit::SnapKind::RoadEndpoint && snapped->road == *selected_road_) {
    if (const Road* road = document_.network().road(*selected_road_); road != nullptr) {
      const PathPoint start = road->plan_view.evaluate(0.0);
      const PathPoint end = road->plan_view.evaluate(road->plan_view.length());
      const double to_start = std::hypot(position.x - start.x, position.y - start.y);
      const double to_end = std::hypot(position.x - end.x, position.y - end.y);
      if (std::min(to_start, to_end) < tol::kLength * 1e3) {
        extend_end_ =
            RoadEnd{.road = *selected_road_,
                    .contact = to_start <= to_end ? ContactPoint::Start : ContactPoint::End};
        emit status_message(tr("Extending the selected road — click, then Enter"));
      }
    }
  }

  if (points_.size() == 1 && points_.front().kind == edit::SnapKind::RoadEndpoint) {
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

  // Enumerate EVERY junction the stroke forms and commit them as ONE undoable
  // command (#354): a genuine road-END first point welds, a first/last point on
  // a road SIDE tees, and every road the fitted line crosses forms its own X.
  //   Start: end snap → weld (chain); else first-point side snap → tee.
  //   End:   last-point side snap → tee.
  //   Body:  each crossed road (body_crossings), in order along the stroke.
  edit::assembly::RoadInteractions interactions;
  if (first.kind == edit::SnapKind::RoadEndpoint && first.snap_road.has_value() &&
      network.road(*first.snap_road) != nullptr) {
    const auto& plan = network.road(*first.snap_road)->plan_view;
    const PathPoint start = plan.evaluate(0.0);
    const PathPoint end = plan.evaluate(plan.length());
    const double to_start = std::hypot(first.position.x - start.x, first.position.y - start.y);
    const double to_end = std::hypot(first.position.x - end.x, first.position.y - end.y);
    interactions.start_link =
        RoadEnd{.road = *first.snap_road,
                .contact = to_start <= to_end ? ContactPoint::Start : ContactPoint::End};
  } else if (first.side_snap.has_value()) {
    interactions.start_tee = std::pair{first.side_snap->road, first.side_snap->s};
  }
  if (last.side_snap.has_value()) {
    interactions.end_tee = std::pair{last.side_snap->road, last.side_snap->s};
  }
  if (const auto line = fit_clothoid_path(waypoints, locked_headings()); line.has_value()) {
    for (const BodyCrossing& crossing : body_crossings(network, *line, RoadId{})) {
      // A road teed/welded at an endpoint is not also crossed (the tee would put
      // it in a junction before the cross runs); drop any such overlap.
      const bool is_tee_target =
          (interactions.start_link.has_value() && interactions.start_link->road == crossing.road) ||
          (interactions.start_tee.has_value() && interactions.start_tee->first == crossing.road) ||
          (interactions.end_tee.has_value() && interactions.end_tee->first == crossing.road);
      if (!is_tee_target) {
        interactions.crossings.push_back(crossing.road);
      }
    }
  }

  const bool has_interaction = interactions.start_link.has_value() ||
                               interactions.start_tee.has_value() ||
                               interactions.end_tee.has_value() || !interactions.crossings.empty();
  std::unique_ptr<edit::Command> command =
      has_interaction ? edit::assembly::create_road_with_interactions(network,
                                                                      std::move(waypoints),
                                                                      profile_,
                                                                      {},
                                                                      locked_headings(),
                                                                      std::move(interactions))
                      : edit::create_road(std::move(waypoints), profile_, {}, locked_headings());
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
    // Mirror commit's locking (locked_headings): only a genuine road-END snap
    // locks a heading, so the ghost fit of a tangent-ray first point is straight
    // — no teardrop preview (#352).
    EndpointHeadings locked;
    if (points_.front().kind == edit::SnapKind::RoadEndpoint) {
      locked.start = points_.front().heading;
    }
    if (candidate.size() > points_.size() && hover_snap_.has_value() &&
        hover_snap_->kind == edit::SnapKind::RoadEndpoint && hover_snap_->heading.has_value()) {
      locked.end = wrap_angle(*hover_snap_->heading + std::numbers::pi);
    } else if (candidate.size() == points_.size()) {
      locked.end = locked_headings().end;
    }
    if (const auto line = fit_clothoid_path(candidate, locked); line.has_value()) {
      append_polyline_of(geometry, *line);
      // Cross hints: an X at EVERY road interior the fitted line crosses, so the
      // user sees all the 4-ways that will fire before committing (#354). A
      // tangent-ray first point is a plain/crossing road (not a chain), so it
      // still shows the hints; only a genuine endpoint chain suppresses them.
      if (!points_.front().side_snap.has_value() &&
          points_.front().kind != edit::SnapKind::RoadEndpoint) {
        for (const BodyCrossing& crossing : body_crossings(document_.network(), *line, RoadId{})) {
          append_cross(geometry, crossing.point.x, crossing.point.y, kSnapMarkerRadius * 1.5);
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
