// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Create Road tool (issue #13): headless ToolEvent sequences drive waypoint
// placement, snapping, Backspace/Esc, and the Enter/double-click commit; the
// tests assert on the undo stack, the serialized network, and preview
// geometry — the M2 tool test seam (docs/design/m2/01_editing_framework.md
// §4).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "tools/create_road_tool.hpp"

using roadmaker::LaneProfile;
using roadmaker::PathPoint;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::CreateRoadTool;
using roadmaker::editor::Document;
using roadmaker::editor::ToolEvent;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

void click(CreateRoadTool& tool, double x, double y) {
  ASSERT_TRUE(tool.mouse_press(at(x, y, Qt::LeftButton)));
}

TEST(CreateRoadTool, ClicksThenEnterCreateOneRoadCommand) {
  Document document;
  CreateRoadTool tool(document);
  tool.activate();

  click(tool, 0.0, 0.0);
  click(tool, 60.0, 10.0);
  click(tool, 120.0, 0.0);
  EXPECT_EQ(tool.waypoint_count(), 3U);
  EXPECT_EQ(document.network().road_count(), 0U); // nothing until commit

  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(document.network().road_count(), 1U);
  EXPECT_EQ(document.undo_stack()->count(), 1); // exactly ONE command
  EXPECT_EQ(tool.waypoint_count(), 0U);         // session cleared for the next road

  const RoadId created = document.network().find_road("1");
  ASSERT_TRUE(created.is_valid());
  EXPECT_EQ(document.network().road(created)->name, "Road 1"); // auto-named
  ASSERT_TRUE(document.network().road(created)->authoring_waypoints.has_value());
  EXPECT_EQ(document.network().road(created)->authoring_waypoints->size(), 3U);

  document.undo_stack()->undo();
  EXPECT_EQ(document.network().road_count(), 0U);
}

TEST(CreateRoadTool, DoubleClickCommitsLikeEnter) {
  Document document;
  CreateRoadTool tool(document);

  click(tool, 0.0, 0.0);
  click(tool, 80.0, 0.0);
  // Qt delivers the pair's first press before the double-click event, so
  // the final point is already placed when the commit gesture arrives.
  ASSERT_TRUE(tool.mouse_double_click(at(80.0, 0.0, Qt::LeftButton)));
  EXPECT_EQ(document.network().road_count(), 1U);
  EXPECT_EQ(document.undo_stack()->count(), 1);
}

TEST(CreateRoadTool, EnterWithTooFewWaypointsPushesNothing) {
  Document document;
  CreateRoadTool tool(document);

  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  click(tool, 0.0, 0.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(document.network().road_count(), 0U);
  EXPECT_EQ(document.undo_stack()->count(), 0);
  EXPECT_EQ(tool.waypoint_count(), 1U); // the point survives for a retry
}

TEST(CreateRoadTool, EscCancelsAndLeavesTheNetworkByteIdentical) {
  Document document;
  const std::string before = xodr(document);
  CreateRoadTool tool(document);

  click(tool, 0.0, 0.0);
  click(tool, 50.0, 5.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.waypoint_count(), 0U);
  EXPECT_EQ(document.undo_stack()->count(), 0);
  EXPECT_EQ(xodr(document), before);

  // With no session, Esc is not consumed (lets the viewport handle it).
  EXPECT_FALSE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
}

TEST(CreateRoadTool, BackspaceRemovesTheLastWaypointOnly) {
  Document document;
  CreateRoadTool tool(document);

  click(tool, 0.0, 0.0);
  click(tool, 40.0, 0.0);
  click(tool, 80.0, 0.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier));
  EXPECT_EQ(tool.waypoint_count(), 2U);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  const RoadId created = document.network().find_road("1");
  ASSERT_TRUE(created.is_valid());
  EXPECT_EQ(document.network().road(created)->authoring_waypoints->size(), 2U);

  EXPECT_FALSE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier)); // empty session
}

TEST(CreateRoadTool, DuplicateConsecutiveClickIsRejected) {
  Document document;
  CreateRoadTool tool(document);

  click(tool, 10.0, 10.0);
  click(tool, 10.0, 10.0); // same spot: rejected at click time (§2)
  EXPECT_EQ(tool.waypoint_count(), 1U);
}

// The §2 tangent-snap chain: the first click lands on an existing road's
// end, so the new road starts there with its heading locked — the created
// road's start heading equals evaluate(end).hdg of the snapped road within
// tol::kAngle.
TEST(CreateRoadTool, TangentSnapChainsG1FromAnExistingRoadEnd) {
  Document document;
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                              Waypoint{.x = 50.0, .y = 12.0},
                                                              Waypoint{.x = 100.0, .y = 0.0}},
                                                             LaneProfile::two_lane_rural(),
                                                             "First"))
                  .has_value());
  const RoadId first = document.network().find_road("1");
  const auto& first_line = document.network().road(first)->plan_view;
  const PathPoint end = first_line.evaluate(first_line.length());

  CreateRoadTool tool(document);
  tool.set_snap_options({.radius = 2.0});
  click(tool, end.x + 0.5, end.y - 0.5); // within snap radius of the end
  click(tool, end.x + 70.0, end.y + 20.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  const RoadId chained = document.network().find_road("2");
  ASSERT_TRUE(chained.is_valid());
  const PathPoint start = document.network().road(chained)->plan_view.evaluate(0.0);
  EXPECT_NEAR(start.x, end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(start.y, end.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(
      std::remainder(start.hdg - end.hdg, 2.0 * std::numbers::pi), 0.0, roadmaker::tol::kAngle);
}

// Closing the LAST point onto a road end locks the arrival heading: the new
// road's end tangent is anti-parallel to the snapped road's continuation.
TEST(CreateRoadTool, ClosingOntoARoadEndLocksTheArrivalHeading) {
  Document document;
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::create_road(
                      {Waypoint{.x = 200.0, .y = 0.0}, Waypoint{.x = 300.0, .y = 0.0}},
                      LaneProfile::two_lane_rural(),
                      "Target"))
                  .has_value());

  CreateRoadTool tool(document);
  tool.set_snap_options({.radius = 2.0});
  click(tool, 100.0, 60.0);
  click(tool, 199.6, -0.4); // snaps onto Target's start at (200,0)
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  const RoadId created = document.network().find_road("2");
  ASSERT_TRUE(created.is_valid());
  const auto& line = document.network().road(created)->plan_view;
  const PathPoint arrive = line.evaluate(line.length());
  EXPECT_NEAR(arrive.x, 200.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(arrive.y, 0.0, roadmaker::tol::kRoundTripPosition);
  // Continuation at Target's start points away (pi); arriving reverses it.
  EXPECT_NEAR(
      std::remainder(arrive.hdg - 0.0, 2.0 * std::numbers::pi), 0.0, roadmaker::tol::kAngle);
}

TEST(CreateRoadTool, FailedCommandKeepsTheSessionAndPushesNothing) {
  Document document;
  const std::string before = xodr(document);
  CreateRoadTool tool(document);
  tool.set_profile(LaneProfile{}); // no lanes: create_road applies with an error

  click(tool, 0.0, 0.0);
  click(tool, 50.0, 0.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(document.network().road_count(), 0U);
  EXPECT_EQ(document.undo_stack()->count(), 0);
  EXPECT_EQ(tool.waypoint_count(), 2U); // the user's points survive
  EXPECT_EQ(xodr(document), before);
}

TEST(CreateRoadTool, PreviewShowsGhostFitAndSnapHint) {
  Document document;
  CreateRoadTool tool(document);
  EXPECT_TRUE(tool.preview().empty());

  click(tool, 0.0, 0.0);
  EXPECT_EQ(tool.preview().handles.size(), 1U); // one placed point

  // Hovering after one point: ghost segment to the cursor.
  EXPECT_FALSE(tool.mouse_move(at(30.0, 5.0))); // hover never consumes
  const auto one_point = tool.preview();
  EXPECT_GE(one_point.line_positions.size(), 6U);

  // A second point plus a hover yields the fitted-clothoid polyline: many
  // more segments than the 2-segment ghost alone.
  click(tool, 60.0, 10.0);
  EXPECT_FALSE(tool.mouse_move(at(120.0, 0.0)));
  const auto fitted = tool.preview();
  EXPECT_GT(fitted.line_positions.size(), 4U * 6U);

  // Deactivation clears the session.
  tool.deactivate();
  EXPECT_TRUE(tool.preview().empty());
  EXPECT_EQ(tool.waypoint_count(), 0U);
}

TEST(CreateRoadTool, TemplateProfileIsAppliedOnCommit) {
  Document document;
  CreateRoadTool tool(document);
  tool.set_profile(LaneProfile::highway());

  click(tool, 0.0, 0.0);
  click(tool, 90.0, 0.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Enter, Qt::NoModifier)); // keypad Enter commits too

  const RoadId created = document.network().find_road("1");
  ASSERT_TRUE(created.is_valid());
  const auto* road = document.network().road(created);
  ASSERT_EQ(road->sections.size(), 1U);
  // highway(): 3 left + center + 3 right.
  EXPECT_EQ(document.network().lane_section(road->sections.front())->lanes.size(), 7U);
}

// --- tee / cross / extend on commit -----------------------------------------

namespace {

/// A straight target road on y=0 from x=-100 to x=100 (id "1").
RoadId make_target(Document& document) {
  const bool pushed = document
                          .push_command(roadmaker::edit::create_road(
                              {Waypoint{.x = -100.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                              LaneProfile::two_lane_rural(),
                              "Target"))
                          .has_value();
  EXPECT_TRUE(pushed);
  return document.network().find_road("1");
}

} // namespace

TEST(CreateRoadTool, DrawingAcrossARoadFormsACrossJunctionOnCommit) {
  Document document;
  make_target(document);
  ASSERT_EQ(document.network().junction_count(), 0U);

  CreateRoadTool tool(document);
  tool.activate();
  // A vertical road crossing the target's interior at the origin.
  click(tool, 0.0, -60.0);
  click(tool, 0.0, 60.0);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(document.network().junction_count(), 1U); // the 4-way formed
  EXPECT_EQ(document.undo_stack()->count(), 2);       // target + cross, one macro each
}

TEST(CreateRoadTool, EndpointSideSnapTeesOnCommit) {
  Document document;
  make_target(document);

  CreateRoadTool tool(document);
  tool.set_snap_options({.radius = 2.0});
  tool.activate();
  // Draw down toward the target; the last point lands on its side (within the
  // snap radius of y=0) so the end tees in.
  click(tool, 0.0, 50.0);
  click(tool, 0.0, 1.5);
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(document.network().junction_count(), 1U); // a T-junction formed
  EXPECT_EQ(document.undo_stack()->count(), 2);
}

TEST(CreateRoadTool, TeeAndCrossAreEachOneUndoMacro) {
  // Cross.
  {
    Document document;
    make_target(document);
    const std::string before = xodr(document);
    CreateRoadTool tool(document);
    tool.activate();
    click(tool, 0.0, -60.0);
    click(tool, 0.0, 60.0);
    ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
    ASSERT_EQ(document.network().junction_count(), 1U);
    document.undo_stack()->undo(); // ONE undo restores pre-commit exactly
    EXPECT_EQ(xodr(document), before);
  }
  // Tee.
  {
    Document document;
    make_target(document);
    const std::string before = xodr(document);
    CreateRoadTool tool(document);
    tool.set_snap_options({.radius = 2.0});
    tool.activate();
    click(tool, 0.0, 50.0);
    click(tool, 0.0, 1.5);
    ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
    ASSERT_EQ(document.network().junction_count(), 1U);
    document.undo_stack()->undo();
    EXPECT_EQ(xodr(document), before);
  }
}

TEST(CreateRoadTool, ExtendEndpointClickAddsCurvatureContinuousExtension) {
  Document document;
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                              Waypoint{.x = 60.0, .y = 8.0},
                                                              Waypoint{.x = 120.0, .y = 0.0}},
                                                             LaneProfile::two_lane_rural(),
                                                             "Bend"))
                  .has_value());
  const RoadId road = document.network().find_road("1");
  ASSERT_TRUE(road.is_valid());
  const auto& plan = document.network().road(road)->plan_view;
  const double join_s = plan.length();
  const PathPoint end = plan.evaluate(join_s);

  CreateRoadTool tool(document);
  tool.set_snap_options({.radius = 2.0});
  tool.set_selected_road(road); // the SelectionModel would wire this in the app
  tool.activate();
  // First click snaps to the road END (arms extend); second is the target.
  click(tool, end.x + 0.3, end.y - 0.2);
  click(tool, end.x + 40.0 * std::cos(end.hdg), end.y + 40.0 * std::sin(end.hdg));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(document.network().road_count(), 1U); // same road, no new one
  EXPECT_EQ(document.undo_stack()->count(), 2);   // create + extend
  const auto& extended = document.network().road(road)->plan_view;
  EXPECT_GT(extended.length(), join_s + 1.0); // it grew
  const PathPoint below = extended.evaluate(join_s - 1e-3);
  const PathPoint above = extended.evaluate(join_s + 1e-3);
  EXPECT_LT(std::abs(above.curvature - below.curvature), roadmaker::tol::kWeldCurvature);
  EXPECT_LT(std::abs(std::remainder(above.hdg - below.hdg, 2.0 * std::numbers::pi)),
            roadmaker::tol::kWeldHeading);
}

TEST(CreateRoadTool, ExtendStartEndpointClickExtendsBackward) {
  Document document;
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                              Waypoint{.x = 60.0, .y = 8.0},
                                                              Waypoint{.x = 120.0, .y = 0.0}},
                                                             LaneProfile::two_lane_rural(),
                                                             "Bend"))
                  .has_value());
  const RoadId road = document.network().find_road("1");
  ASSERT_TRUE(road.is_valid());
  const auto& plan = document.network().road(road)->plan_view;
  const double old_length = plan.length();
  const PathPoint start = plan.evaluate(0.0);

  CreateRoadTool tool(document);
  tool.set_snap_options({.radius = 2.0});
  tool.set_selected_road(road); // the SelectionModel would wire this in the app
  tool.activate();
  // First click snaps to the road START (the NEARER endpoint arms extension);
  // the second click aims BEHIND the start, opposite its tangent.
  click(tool, start.x + 0.3, start.y - 0.2);
  click(tool, start.x - 40.0 * std::cos(start.hdg), start.y - 40.0 * std::sin(start.hdg));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(document.network().road_count(), 1U); // same road, no new one
  EXPECT_EQ(document.undo_stack()->count(), 2);   // create + extend
  const auto& extended = document.network().road(road)->plan_view;
  EXPECT_GT(extended.length(), old_length + 1.0); // it grew at the front
  const double join_s = extended.length() - old_length;
  const PathPoint below = extended.evaluate(join_s - 1e-3);
  const PathPoint above = extended.evaluate(join_s + 1e-3);
  EXPECT_LT(std::abs(above.curvature - below.curvature), roadmaker::tol::kWeldCurvature);
  EXPECT_LT(std::abs(std::remainder(above.hdg - below.hdg, 2.0 * std::numbers::pi)),
            roadmaker::tol::kWeldHeading);
}

} // namespace
