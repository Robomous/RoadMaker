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

// The Route tool's interaction (p8-s3, issue #247), driven headlessly through
// ToolEvent — the same contract every tool test uses.
//
// The gesture grammar being pinned:
//   - the FIRST click commits NOTHING (the schema needs two waypoints, so the
//     document never holds a one-waypoint route it could not save);
//   - the second click is ONE assign_route — one Ctrl+Z removes the route;
//   - each further click is ONE appended waypoint, a drag ONE move;
//   - Backspace removes the last waypoint and is REFUSED below two, because a
//     deletion that made the scenario unsavable would look like it succeeded;
//   - breaking the network under a route surfaces in Document diagnostics —
//     GW-6 step 8 in the editor, not just in the kernel.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/route.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <string>
#include <variant>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/route_tool.hpp"

namespace roadmaker::editor {
namespace {

/// A straight two-lane road along +x, 100 m. Lane -1 sits at y = -1.75.
void author_straight_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

/// An actor named Ego on lane -1 at s = 50 — the target every route test uses.
void place_ego(Document& document) {
  const Road* road = nullptr;
  document.network().for_each_road([&](RoadId, const Road& value) { road = &value; });
  ASSERT_NE(road, nullptr);
  osc::LanePosition position;
  position.road_id = road->odr_id;
  position.lane_id = "-1";
  position.s = 50.0;
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), osc::make_actor(osc::ActorKind::Car, "Ego"), position))
                  .has_value());
}

ToolEvent at(double x, double y) {
  return ToolEvent{.world_x = x, .world_y = y, .buttons = Qt::LeftButton};
}

void click(RouteTool& tool, double x, double y) {
  (void)tool.mouse_move(at(x, y));
  (void)tool.mouse_press(at(x, y));
  (void)tool.mouse_release(at(x, y));
}

const osc::Route* ego_route(const Document& document) {
  for (const osc::AssignedRoute& assigned : osc::assigned_routes(document.scenario())) {
    if (assigned.entity_ref == "Ego") {
      return assigned.route;
    }
  }
  return nullptr;
}

double waypoint_s(const osc::Route& route, std::size_t index) {
  const auto* lane = std::get_if<osc::LanePosition>(&route.waypoints.at(index).position);
  return lane == nullptr ? -1.0 : lane->s;
}

} // namespace

TEST(RouteTool, TheFirstClickCommitsNothingAndTheSecondAssignsOneRoute) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);

  click(tool, 50.0, -1.75); // on Ego: chooses the target
  EXPECT_EQ(tool.target(), "Ego");

  const int before = document.undo_stack()->count();
  click(tool, 60.0, -1.75);
  EXPECT_EQ(document.undo_stack()->count(), before) << "a one-waypoint route was committed";
  EXPECT_EQ(ego_route(document), nullptr);

  click(tool, 80.0, -1.75);
  EXPECT_EQ(document.undo_stack()->count(), before + 1);
  const osc::Route* route = ego_route(document);
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->waypoints.size(), 2U);
  EXPECT_NEAR(waypoint_s(*route, 0), 60.0, 1e-6);
  EXPECT_NEAR(waypoint_s(*route, 1), 80.0, 1e-6);
  EXPECT_FALSE(route->name.empty());

  // ★ ONE Ctrl+Z removes the whole two-click gesture.
  document.undo_stack()->undo();
  EXPECT_EQ(ego_route(document), nullptr);
}

TEST(RouteTool, AThirdClickAppendsExactlyOneWaypointAsOneEntry) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75);
  click(tool, 80.0, -1.75);

  const int before = document.undo_stack()->count();
  click(tool, 90.0, -1.75);
  EXPECT_EQ(document.undo_stack()->count(), before + 1);
  const osc::Route* route = ego_route(document);
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->waypoints.size(), 3U);
  EXPECT_NEAR(waypoint_s(*route, 2), 90.0, 1e-6);
}

TEST(RouteTool, AClickWithNoTargetRefusesWithAToast) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);
  QSignalSpy toasts(&tool, &Tool::toast_requested);
  ASSERT_TRUE(toasts.isValid());

  click(tool, 20.0, -1.75); // a lane, but nobody to route
  EXPECT_EQ(ego_route(document), nullptr);
  ASSERT_EQ(toasts.count(), 1);
  EXPECT_FALSE(toasts.at(0).at(0).toString().isEmpty());
}

TEST(RouteTool, AClickOffEveryLaneRefusesWithTheDropHint) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);
  QSignalSpy toasts(&tool, &Tool::toast_requested);
  ASSERT_TRUE(toasts.isValid());

  click(tool, 50.0, -1.75); // target Ego
  click(tool, 5000.0, 5000.0);

  EXPECT_EQ(ego_route(document), nullptr);
  ASSERT_EQ(toasts.count(), 1);
  EXPECT_FALSE(toasts.at(0).at(0).toString().isEmpty());
}

TEST(RouteTool, ActivateAdoptsTheSelectedActorAsTheTarget) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  selection.select(SelectionEntry{.actor = "Ego"});

  RouteTool tool(document, selection);
  tool.activate();
  EXPECT_EQ(tool.target(), "Ego");
}

TEST(RouteTool, BackspaceRemovesTheLastWaypointAndRefusesBelowTwo) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);
  QSignalSpy toasts(&tool, &Tool::toast_requested);
  ASSERT_TRUE(toasts.isValid());

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75);
  click(tool, 80.0, -1.75);
  click(tool, 90.0, -1.75); // three waypoints

  (void)tool.key_press(Qt::Key_Backspace, Qt::NoModifier);
  {
    const osc::Route* route = ego_route(document);
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->waypoints.size(), 2U);
  }
  EXPECT_EQ(toasts.count(), 0);

  // ★ Two left: removing another would make the document unsavable, so the
  // command layer refuses and the tool surfaces WHY instead of shrinking the
  // route to something invalid.
  (void)tool.key_press(Qt::Key_Backspace, Qt::NoModifier);
  {
    const osc::Route* route = ego_route(document);
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->waypoints.size(), 2U);
  }
  ASSERT_EQ(toasts.count(), 1);
  EXPECT_FALSE(toasts.at(0).at(0).toString().isEmpty());
}

TEST(RouteTool, DraggingAWaypointCommitsOneMoveOnRelease) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75);
  click(tool, 80.0, -1.75);

  const int before = document.undo_stack()->count();
  (void)tool.mouse_move(at(60.0, -1.75));
  (void)tool.mouse_press(at(60.0, -1.75)); // grabs waypoint 0 (10 m from Ego, out of its radius)
  (void)tool.mouse_move(at(70.0, -1.75));
  (void)tool.mouse_release(at(70.0, -1.75));

  EXPECT_EQ(document.undo_stack()->count(), before + 1) << "a drag must be ONE command";
  const osc::Route* route = ego_route(document);
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->waypoints.size(), 2U);
  EXPECT_NEAR(waypoint_s(*route, 0), 70.0, 1e-6);
  EXPECT_NEAR(waypoint_s(*route, 1), 80.0, 1e-6) << "the drag disturbed a waypoint it never held";
}

TEST(RouteTool, EscapeDropsThePendingWaypointThenTheTarget) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75); // pending, uncommitted

  (void)tool.key_press(Qt::Key_Escape, Qt::NoModifier);
  EXPECT_EQ(tool.target(), "Ego") << "the first Esc must only drop the pending waypoint";

  // The pending waypoint is gone: the next two clicks build a fresh pair.
  click(tool, 30.0, -1.75);
  click(tool, 40.0, -1.75);
  const osc::Route* route = ego_route(document);
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->waypoints.size(), 2U);
  EXPECT_NEAR(waypoint_s(*route, 0), 30.0, 1e-6);

  (void)tool.key_press(Qt::Key_Escape, Qt::NoModifier);
  EXPECT_TRUE(tool.target().empty()) << "a second Esc steps out of the tool's target";
}

TEST(RouteTool, ThePreviewDrawsTheResolvedRouteAndItsHandles) {
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75);
  click(tool, 80.0, -1.75);

  const PreviewGeometry geometry = tool.preview();
  EXPECT_FALSE(geometry.line_positions.empty()) << "a resolved route must draw solid legs";
  EXPECT_TRUE(geometry.dashed_line_positions.empty())
      << "a complete route has no gap to draw dashed";
  // Two waypoint knobs; the hover ghost may add a Midpoint on top.
  int nodes = 0;
  for (const Handle& handle : geometry.handles) {
    nodes += handle.kind == HandleKind::Node ? 1 : 0;
  }
  EXPECT_EQ(nodes, 2);
}

TEST(RouteTool, BreakingTheNetworkUnderARouteSurfacesInTheDiagnosticsTable) {
  // ★ GW-6 STEP 8, IN THE EDITOR: the kernel test proves resolve_route
  // reports; this proves the Document actually re-runs it on a topology edit
  // and the findings reach the same table the panel shows.
  Document document;
  author_straight_road(document);
  place_ego(document);
  SelectionModel selection(document);
  RouteTool tool(document, selection);
  DiagnosticsModel model(document, nullptr);

  click(tool, 50.0, -1.75);
  click(tool, 60.0, -1.75);
  click(tool, 80.0, -1.75);
  EXPECT_TRUE(document.scenario_diagnostics().empty()) << "an intact route has nothing to report";

  RoadId road;
  document.network().for_each_road([&](RoadId id, const Road&) { road = id; });
  QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
  ASSERT_TRUE(resets.isValid());
  ASSERT_TRUE(document.push_command(edit::delete_road(document.network(), road)).has_value());

  ASSERT_FALSE(document.scenario_diagnostics().empty())
      << "deleting the routed road must be REPORTED, not absorbed";
  EXPECT_GE(resets.count(), 1) << "the diagnostics table never learned of the finding";
  EXPECT_GE(model.rowCount(), static_cast<int>(document.scenario_diagnostics().size()))
      << "the model does not present the route findings";

  // The route itself is untouched — the network broke, not the document.
  const osc::Route* route = ego_route(document);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->waypoints.size(), 2U);
}

} // namespace roadmaker::editor
