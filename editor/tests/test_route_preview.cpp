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

// The route → world projection (p8-s3, issue #247) — document/route_preview.
//
// What is pinned: the polyline runs along the LANE'S CENTRE (not the reference
// line), it is ordered in the leg's DIRECTION OF TRAVEL (which for a left-hand
// lane means descending s — the assumption a caller would silently get wrong),
// and a leg the network no longer carries yields nothing rather than a guess.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/route.hpp"

#include <gtest/gtest.h>

#include <string>

#include "document/document.hpp"
#include "document/route_preview.hpp"

namespace roadmaker::editor {
namespace {

/// A straight two-lane road along +x, 100 m. Lane -1 sits at negative y,
/// lane 1 mirrored above; the exact centre offset belongs to the default
/// profile, so the tests read it back through actor_world_pose rather than
/// hardcoding it.
void author_straight_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

RoadId only_road(const Document& document) {
  RoadId found;
  document.network().for_each_road([&](RoadId id, const Road&) { found = id; });
  return found;
}

osc::RouteWaypoint lane_waypoint(const std::string& road, const std::string& lane, double s) {
  osc::LanePosition position;
  position.road_id = road;
  position.lane_id = lane;
  position.s = s;
  return osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                            .position = osc::Position{position},
                            .preserved = {}};
}

} // namespace

TEST(RoutePreview, ALegPolylineRunsAlongTheLaneCentreInTravelOrder) {
  Document document;
  author_straight_road(document);

  const osc::RouteLeg leg{
      .road = only_road(document), .lane = {}, .lane_odr_id = -1, .s_start = 10.0, .s_end = 90.0};
  const std::vector<std::array<double, 3>> points = route_leg_polyline(document.network(), leg);

  // The polyline must sit exactly where an actor on the same lane would — ONE
  // projection, shared with actor_world_pose, is the module's whole contract.
  const Road* road = document.network().road(leg.road);
  ASSERT_NE(road, nullptr);
  osc::LanePosition on_lane;
  on_lane.road_id = road->odr_id;
  on_lane.lane_id = "-1";
  on_lane.s = 50.0;
  const std::optional<ActorPose> pose = actor_world_pose(document.network(), on_lane);
  ASSERT_TRUE(pose.has_value());
  EXPECT_LT(pose->position[1], 0.0) << "lane -1 must sit below the reference line";

  ASSERT_GE(points.size(), 2U);
  EXPECT_NEAR(points.front()[0], 10.0, 1e-6);
  EXPECT_NEAR(points.back()[0], 90.0, 1e-6);
  for (const std::array<double, 3>& point : points) {
    // The LANE's centre, not the road's reference line (which is y = 0).
    EXPECT_NEAR(point[1], pose->position[1], 1e-6);
  }
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    EXPECT_LT(points[i][0], points[i + 1][0]) << "a right-hand leg must ascend in s";
  }
}

TEST(RoutePreview, ALeftHandLegIsOrderedAgainstSBecauseThatIsItsTravel) {
  // ★ `s_end < s_start` is how a left-hand leg encodes its direction, and the
  // polyline must follow it — a caller that re-sorted by s would draw the
  // route driving backwards through the lane.
  Document document;
  author_straight_road(document);

  const osc::RouteLeg leg{
      .road = only_road(document), .lane = {}, .lane_odr_id = 1, .s_start = 90.0, .s_end = 10.0};
  const std::vector<std::array<double, 3>> points = route_leg_polyline(document.network(), leg);

  ASSERT_GE(points.size(), 2U);
  EXPECT_NEAR(points.front()[0], 90.0, 1e-6);
  EXPECT_NEAR(points.back()[0], 10.0, 1e-6);
  for (const std::array<double, 3>& point : points) {
    EXPECT_GT(point[1], 0.0) << "lane 1 sits above the reference line";
  }
}

TEST(RoutePreview, AVanishedRoadYieldsAnEmptyPolylineNotAGuess) {
  Document document;
  author_straight_road(document);

  const osc::RouteLeg leg{
      .road = RoadId{}, .lane = {}, .lane_odr_id = -1, .s_start = 0.0, .s_end = 50.0};
  EXPECT_TRUE(route_leg_polyline(document.network(), leg).empty());
}

TEST(RoutePreview, WaypointPosesStayIndexAlignedAndMarkTheUnresolvable) {
  // The index is what set_route_waypoint addresses, so a pose list that
  // silently skipped an unresolvable waypoint would make the tool edit the
  // wrong one.
  Document document;
  author_straight_road(document);

  const Road* road = document.network().road(only_road(document));
  ASSERT_NE(road, nullptr);

  osc::Route route;
  route.name = "EgoRoute";
  route.waypoints.push_back(lane_waypoint(road->odr_id, "-1", 20.0));
  osc::WorldPosition world;
  world.x = 5.0;
  world.y = 5.0;
  route.waypoints.push_back(
      osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                         .position = osc::Position{world},
                         .preserved = {}});
  route.waypoints.push_back(lane_waypoint(road->odr_id, "-1", 80.0));

  const std::vector<std::optional<ActorPose>> poses =
      route_waypoint_poses(document.network(), route);
  ASSERT_EQ(poses.size(), 3U);
  ASSERT_TRUE(poses[0].has_value());
  EXPECT_NEAR(poses[0]->position[0], 20.0, 1e-6);
  EXPECT_FALSE(poses[1].has_value()) << "a world position names no lane and must not project";
  ASSERT_TRUE(poses[2].has_value());
  EXPECT_NEAR(poses[2]->position[0], 80.0, 1e-6);
}

} // namespace roadmaker::editor
