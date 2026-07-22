// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Kernel tests for edit::insert_node_at (M3a bend points). Inserting a node
// pins the heading at every node from the current curve, so the re-fit
// reproduces the road's shape (line/arc/spiral within tol) and only the
// covering record splits — unlike insert_waypoint, which drifts the shape.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::LaneProfile;
using roadmaker::Road;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::expect_same_geometry;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

void expect_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
  ASSERT_TRUE(command.apply(network).has_value());
  expect_network_matches(network, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
}

} // namespace

TEST(InsertNode, PreservesShapeAndAddsOneNode) {
  RoadNetwork network;
  // A curved authored road (so authoring_waypoints are recorded — the case
  // insert_waypoint would reflow).
  const RoadId road = author(network,
                             {Waypoint{.x = 0.0, .y = 0.0},
                              Waypoint{.x = 40.0, .y = 20.0},
                              Waypoint{.x = 100.0, .y = 0.0}},
                             "1");
  const Road original = *network.road(road);
  const std::size_t nodes_before = original.authoring_waypoints->size();

  auto command = roadmaker::edit::insert_node_at(network, road, 25.0);
  expect_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const Road& edited = *network.road(road);
  // One more node, and the plan-view shape is preserved within round-trip tol.
  ASSERT_TRUE(edited.authoring_waypoints.has_value());
  EXPECT_EQ(edited.authoring_waypoints->size(), nodes_before + 1);
  expect_same_geometry(original, edited);
}

TEST(InsertNode, PreservesForeignRoadShape) {
  RoadNetwork network;
  const RoadId road = author(network,
                             {Waypoint{.x = 0.0, .y = 0.0},
                              Waypoint{.x = 50.0, .y = 15.0},
                              Waypoint{.x = 120.0, .y = -5.0}},
                             "1");
  // Drop the recorded waypoints to emulate a foreign-loaded road.
  network.road(road)->authoring_waypoints.reset();
  const Road original = *network.road(road);

  auto command = roadmaker::edit::insert_node_at(network, road, 70.0);
  ASSERT_TRUE(command->apply(network).has_value());
  expect_same_geometry(original, *network.road(road));
}

TEST(InsertNode, RejectsNodeTooCloseToExisting) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const std::string before = snapshot_xodr(network);

  // 1 m from the start node (< kMinNodeSpacingM = 2 m).
  auto command = roadmaker::edit::insert_node_at(network, road, 1.0);
  const auto applied = command->apply(network);
  ASSERT_FALSE(applied.has_value());
  EXPECT_NE(applied.error().message.find("within 2 m"), std::string::npos)
      << applied.error().message;
  expect_network_matches(network, before);
}

TEST(InsertNode, RejectsStationOutsideRoad) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  EXPECT_FALSE(roadmaker::edit::insert_node_at(network, road, -5.0)->apply(network).has_value());
  EXPECT_FALSE(roadmaker::edit::insert_node_at(network, road, 500.0)->apply(network).has_value());
}

TEST(InsertNode, RejectsStaleRoad) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  ASSERT_TRUE(roadmaker::edit::delete_road(network, road)->apply(network).has_value());
  EXPECT_FALSE(roadmaker::edit::insert_node_at(network, road, 50.0)->apply(network).has_value());
}
