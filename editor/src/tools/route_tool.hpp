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

#pragma once

// The Route tool (p8-s3, issue #247): give a scenario actor a lane-anchored
// path. Click an actor to make it the target, then click driving lanes to lay
// waypoints; drag a laid waypoint to move it, Backspace removes the last one.
// Headless by construction, like every tool — ToolEvent in, commands +
// PreviewGeometry out.
//
// EVERY MUTATION GOES THROUGH Document::push_scenario_command, one command per
// gesture: the first two clicks are ONE assign_route (the schema's own
// two-waypoint minimum, so no intermediate one-waypoint document ever exists),
// each further click ONE insert_route_waypoint, a drag ONE set_route_waypoint
// on release. The tool never touches the scenario directly.
//
// THE PREVIEW DRAWS THE RESOLVED ROUTE, not the clicks: solid legs are what
// `osc::resolve_route` solved against the live network, dashed lines are the
// waypoint sequence wherever resolution is incomplete — so a route that no
// longer drives shows exactly which stretch is broken instead of looking fine.

#include <optional>
#include <string>

#include "document/actor_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::osc {
struct Route;
} // namespace roadmaker::osc

namespace roadmaker::editor {

class Document;
class SelectionModel;

class RouteTool : public Tool {
  Q_OBJECT

public:
  RouteTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The actor whose route the next click edits; empty = none chosen yet.
  [[nodiscard]] const std::string& target() const { return target_; }

private:
  /// LMB held: a click that will lay a waypoint, a grab of an existing one
  /// (`waypoint` >= 0), or a press on an actor (`actor` non-empty).
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    std::string actor;
    int waypoint = -1;
    bool dragging = false;
  };

  /// The target's inline `<Route>`, or nullptr when it has none (or the target
  /// is gone). Resolved fresh on every use — the scenario may have changed
  /// under the tool through undo or another panel.
  [[nodiscard]] const osc::Route* current_route() const;

  /// The waypoint of the current route within grab range of (x, y), or -1.
  [[nodiscard]] int waypoint_at(double x, double y) const;

  Document& document_;
  SelectionModel& selection_;

  std::string target_;

  /// The first waypoint of a route that does not exist yet. Held in the tool,
  /// NOT in the document: `assign_route` refuses fewer than two waypoints (a
  /// one-waypoint `<Route>` is invalid in every revision), so the document
  /// never holds the intermediate state — the second click commits both as one
  /// command, and Esc discards this with nothing to undo.
  std::optional<LaneAnchor> pending_;

  std::optional<LaneAnchor> hovered_;
  std::string hover_hint_;

  /// Raw cursor position from the last move — what waypoint_at() hit-tests
  /// with in preview(), since a LaneAnchor has already snapped away from it.
  double cursor_x_ = 0.0;
  double cursor_y_ = 0.0;

  std::optional<PressState> press_;
};

} // namespace roadmaker::editor
