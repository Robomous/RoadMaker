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

#include "tools/route_tool.hpp"

#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/route.hpp"

#include <QString>
#include <cmath>
#include <string>
#include <utility>
#include <variant>

#include "document/document.hpp"
#include "document/route_preview.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

/// A drag has to move this far before it stops being a click [m] — the same
/// threshold the Actor tool uses, for the same tremor.
constexpr double kDragThreshold = 0.25;

/// How near the cursor must be to a laid waypoint for a press to grab it [m].
/// Matches kActorGrabRadius: the two knobs live on the same lanes at the same
/// zoom, and two different grab feels would read as one of them being broken.
constexpr double kWaypointGrabRadius = 3.0;

/// A route name no other route in the scenario carries. The writer refuses a
/// duplicate (`naming.unique_element_names_on_same_level`), so uniqueness here
/// is what keeps a second actor's route from making the document unsavable.
std::string unique_route_name(const osc::Scenario& scenario, const std::string& base) {
  const auto taken = [&scenario](const std::string& name) {
    for (const osc::AssignedRoute& assigned : osc::assigned_routes(scenario)) {
      if (assigned.route->name == name) {
        return true;
      }
    }
    return false;
  };
  if (!taken(base)) {
    return base;
  }
  for (int counter = 2;; ++counter) {
    std::string candidate = base + std::to_string(counter);
    if (!taken(candidate)) {
      return candidate;
    }
  }
}

osc::RouteWaypoint waypoint_from(const LaneAnchor& anchor) {
  return osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                            .position = osc::Position{to_lane_position(anchor)},
                            .preserved = {}};
}

} // namespace

RouteTool::RouteTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void RouteTool::activate() {
  // An actor already selected IS the answer to "whose route?" — re-asking with
  // a click would make the Scenario panel selection meaningless here.
  const std::vector<std::string> selected = selection_.selected_actors();
  if (!selected.empty()) {
    target_ = selected.front();
  }
}

void RouteTool::deactivate() {
  target_.clear();
  pending_.reset();
  hovered_.reset();
  hover_hint_.clear();
  press_.reset();
  emit preview_changed();
}

const osc::Route* RouteTool::current_route() const {
  if (target_.empty()) {
    return nullptr;
  }
  for (const osc::AssignedRoute& assigned : osc::assigned_routes(document_.scenario())) {
    if (assigned.entity_ref == target_) {
      return assigned.route;
    }
  }
  return nullptr;
}

int RouteTool::waypoint_at(double x, double y) const {
  const osc::Route* route = current_route();
  if (route == nullptr) {
    return -1;
  }
  int best = -1;
  double best_distance = kWaypointGrabRadius;
  const std::vector<std::optional<ActorPose>> poses =
      route_waypoint_poses(document_.network(), *route);
  for (std::size_t index = 0; index < poses.size(); ++index) {
    if (!poses[index].has_value()) {
      continue; // not drawn, so not grabbable — the actor hit test's own rule
    }
    const double distance =
        std::hypot(poses[index]->position[0] - x, poses[index]->position[1] - y);
    if (distance < best_distance) {
      best_distance = distance;
      best = static_cast<int>(index);
    }
  }
  return best;
}

bool RouteTool::mouse_press(const ToolEvent& event) {
  if ((event.buttons & Qt::LeftButton) == 0) {
    return false;
  }

  PressState state{.world_x = event.world_x, .world_y = event.world_y};

  // An actor under the cursor wins over a waypoint: retargeting is the rarer
  // gesture, so it must not be shadowed by a waypoint that happens to sit on
  // the actor's own anchor. Same shared hit test as the Actor/Select tools.
  if (const std::optional<std::string> hit =
          actor_at(document_.scenario(), document_.network(), event.world_x, event.world_y)) {
    state.actor = *hit;
  } else {
    state.waypoint = waypoint_at(event.world_x, event.world_y);
  }

  press_ = state;
  return true;
}

bool RouteTool::mouse_move(const ToolEvent& event) {
  cursor_x_ = event.world_x;
  cursor_y_ = event.world_y;
  hovered_ = nearest_lane_anchor(document_.network(), event.world_x, event.world_y);
  hover_hint_ = hovered_.has_value()
                    ? std::string{}
                    : actor_drop_hint(document_.network(), event.world_x, event.world_y);

  if (press_.has_value() && press_->waypoint >= 0) {
    const double moved =
        std::hypot(event.world_x - press_->world_x, event.world_y - press_->world_y);
    if (moved > kDragThreshold) {
      press_->dragging = true;
    }
  }

  emit preview_changed();
  return true;
}

bool RouteTool::mouse_release(const ToolEvent& event) {
  if (!press_.has_value()) {
    return false;
  }
  const PressState state = *press_;
  press_.reset();

  // A press on an actor retargets the tool; the pending first waypoint (if
  // any) belonged to the previous target and is discarded with it.
  if (!state.actor.empty()) {
    target_ = state.actor;
    pending_.reset();
    selection_.select(SelectionEntry{.actor = state.actor});
    emit preview_changed();
    return true;
  }

  const std::optional<LaneAnchor> anchor =
      nearest_lane_anchor(document_.network(), event.world_x, event.world_y);

  // A grabbed waypoint that actually travelled commits ONE set_route_waypoint.
  if (state.dragging && state.waypoint >= 0) {
    if (!anchor.has_value()) {
      emit toast_requested(QString::fromStdString(
                               actor_drop_hint(document_.network(), event.world_x, event.world_y)),
                           ToastSeverity::Warning);
      emit preview_changed();
      return true;
    }
    const auto moved = document_.push_scenario_command(
        osc::edit::set_route_waypoint(document_.scenario(),
                                      target_,
                                      static_cast<std::size_t>(state.waypoint),
                                      osc::Position{to_lane_position(*anchor)}));
    if (!moved.has_value()) {
      emit toast_requested(QString::fromStdString(moved.error().message), ToastSeverity::Error);
    }
    emit preview_changed();
    return true;
  }
  if (state.waypoint >= 0) {
    // A click on a waypoint that never became a drag: nothing to do — the knob
    // is already where the user left it.
    emit preview_changed();
    return true;
  }

  if (target_.empty()) {
    emit toast_requested(tr("Click an actor first — the route needs someone to drive it."),
                         ToastSeverity::Warning);
    emit preview_changed();
    return true;
  }
  if (!anchor.has_value()) {
    // Same refusal-with-a-reason the Actor tool gives: a waypoint off every
    // lane is a waypoint the resolver could only report as unresolvable.
    emit toast_requested(
        QString::fromStdString(actor_drop_hint(document_.network(), event.world_x, event.world_y)),
        ToastSeverity::Warning);
    emit preview_changed();
    return true;
  }

  if (const osc::Route* route = current_route()) {
    // ONE command appends the waypoint at the end.
    const auto appended = document_.push_scenario_command(osc::edit::insert_route_waypoint(
        document_.scenario(), target_, route->waypoints.size(), waypoint_from(*anchor)));
    if (!appended.has_value()) {
      emit toast_requested(QString::fromStdString(appended.error().message), ToastSeverity::Error);
    }
    emit preview_changed();
    return true;
  }

  if (!pending_.has_value()) {
    // The schema needs two waypoints, so the first click commits nothing: the
    // document never holds a one-waypoint route it could not save.
    pending_ = anchor;
    emit preview_changed();
    return true;
  }

  osc::Route route;
  route.name = unique_route_name(document_.scenario(), target_ + "Route");
  route.waypoints.push_back(waypoint_from(*pending_));
  route.waypoints.push_back(waypoint_from(*anchor));
  const auto assigned = document_.push_scenario_command(
      osc::edit::assign_route(document_.scenario(), target_, std::move(route)));
  if (!assigned.has_value()) {
    emit toast_requested(QString::fromStdString(assigned.error().message), ToastSeverity::Error);
  } else {
    pending_.reset();
  }
  emit preview_changed();
  return true;
}

bool RouteTool::key_press(int key, Qt::KeyboardModifiers /*modifiers*/) {
  if (key == Qt::Key_Escape) {
    // First Esc drops the uncommitted waypoint, a second drops the target —
    // stepping out of the gesture in the order it was stepped into.
    if (press_.has_value() || pending_.has_value()) {
      press_.reset();
      pending_.reset();
    } else {
      target_.clear();
    }
    emit preview_changed();
    return true;
  }
  if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
    const osc::Route* route = current_route();
    if (route == nullptr || route->waypoints.empty()) {
      return false;
    }
    // The kernel refuses removing below two ("delete the whole route
    // instead"); the refusal surfaces as a toast rather than being pre-checked
    // here — one rule, owned by the command layer.
    const auto removed = document_.push_scenario_command(osc::edit::remove_route_waypoint(
        document_.scenario(), target_, route->waypoints.size() - 1));
    if (!removed.has_value()) {
      emit toast_requested(QString::fromStdString(removed.error().message), ToastSeverity::Warning);
    }
    emit preview_changed();
    return true;
  }
  return false;
}

PreviewGeometry RouteTool::preview() const {
  PreviewGeometry geometry;
  const auto append_polyline = [](const std::vector<std::array<double, 3>>& path,
                                  std::vector<double>& out) {
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      out.insert(
          out.end(),
          {path[i][0], path[i][1], path[i][2], path[i + 1][0], path[i + 1][1], path[i + 1][2]});
    }
  };

  if (const osc::Route* route = current_route()) {
    // ★ THE RESOLVED ROUTE, not the clicks. Solid legs are what resolve_route
    // solved against the live network right now — after a road move they
    // follow it, after a lane deletion the solved stretches still draw and the
    // broken one visibly does not (GW-6 steps 7 and 8, in the viewport).
    const osc::ResolvedRoute resolved = osc::resolve_route(document_.network(), *route);
    for (const osc::RouteLeg& leg : resolved.legs) {
      append_polyline(route_leg_polyline(document_.network(), leg), geometry.line_positions);
    }

    const std::vector<std::optional<ActorPose>> poses =
        route_waypoint_poses(document_.network(), *route);
    if (!resolved.complete) {
      // The waypoint SEQUENCE, dashed, wherever resolution fell short — intent
      // where there is no solved path, so the gap is visible instead of the
      // route just looking shorter than it is.
      std::vector<std::array<double, 3>> sequence;
      for (const std::optional<ActorPose>& pose : poses) {
        if (pose.has_value()) {
          sequence.push_back(pose->position);
        }
      }
      append_polyline(sequence, geometry.dashed_line_positions);
    }

    const int hovered_waypoint = press_.has_value() && press_->waypoint >= 0
                                     ? press_->waypoint
                                     : waypoint_at(cursor_x_, cursor_y_);
    for (std::size_t index = 0; index < poses.size(); ++index) {
      if (!poses[index].has_value()) {
        continue;
      }
      const bool grabbed =
          press_.has_value() && press_->dragging && press_->waypoint == static_cast<int>(index);
      const bool hot = grabbed || hovered_waypoint == static_cast<int>(index);
      geometry.add_handle(poses[index]->position[0],
                          poses[index]->position[1],
                          poses[index]->position[2],
                          HandleKind::Node,
                          grabbed ? HandleState::Grabbed
                          : hot   ? HandleState::Hovered
                                  : HandleState::Idle);
    }
  }

  // The uncommitted first waypoint, plus its dashed reach to the cursor: the
  // route-to-be, drawn as guidance rather than as a solved path.
  if (pending_.has_value()) {
    if (const std::optional<ActorPose> pose = actor_world_pose(document_.network(), *pending_)) {
      geometry.add_handle(pose->position[0], pose->position[1], pose->position[2]);
      if (hovered_.has_value()) {
        if (const std::optional<ActorPose> hover =
                actor_world_pose(document_.network(), *hovered_)) {
          geometry.dashed_line_positions.insert(geometry.dashed_line_positions.end(),
                                                {pose->position[0],
                                                 pose->position[1],
                                                 pose->position[2],
                                                 hover->position[0],
                                                 hover->position[1],
                                                 hover->position[2]});
        }
      }
    }
  }

  // The "a waypoint would land here" ghost — the same projection the commit
  // uses, as a Midpoint knob so it reads as "insert" rather than as a node.
  if (!target_.empty() && hovered_.has_value() && !(press_.has_value() && press_->dragging)) {
    if (const std::optional<ActorPose> pose = actor_world_pose(document_.network(), *hovered_)) {
      geometry.add_handle(
          pose->position[0], pose->position[1], pose->position[2], HandleKind::Midpoint);
    }
  }

  return geometry;
}

QString RouteTool::instruction() const {
  if (!hover_hint_.empty()) {
    return QString::fromStdString(hover_hint_);
  }
  if (target_.empty()) {
    return tr("Click an actor to choose whose route to author.");
  }
  if (pending_.has_value()) {
    return tr("Click the next lane to finish %1's route (Esc discards the first waypoint).")
        .arg(QString::fromStdString(target_));
  }
  if (current_route() == nullptr) {
    return tr("Click a driving lane to lay %1's first waypoint.")
        .arg(QString::fromStdString(target_));
  }
  return tr("Click a lane to append a waypoint; drag a waypoint to move it; Backspace removes "
            "the last one.");
}

} // namespace roadmaker::editor
