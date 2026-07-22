// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Kernel tests for edit::translate_roads / translate_road (M3a topology UX).
// The move shifts plan-view x/y and authoring waypoints only; headings,
// lengths, s, lanes, elevation and marks are untouched, undo is byte-identical,
// links leaving the moved set break on both sides, and junction roads refuse.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::PathPoint;
using roadmaker::Road;
using roadmaker::RoadId;
using roadmaker::RoadLink;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
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

RoadId author_line(RoadNetwork& network, const char* odr_id, double y = 0.0) {
  return author(
      network,
      {Waypoint{.x = 0.0, .y = y}, Waypoint{.x = 40.0, .y = y + 5.0}, Waypoint{.x = 80.0, .y = y}},
      odr_id);
}

// The §8 round-trip oracle: apply changes the doc, revert restores it
// byte-identically, re-apply reproduces, final revert is pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
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

void expect_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

} // namespace

TEST(TranslateRoad, ShiftsGeometryAndWaypointsRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");

  // Sample the original pose and waypoints.
  const Road original = *network.road(road);
  const double length = original.plan_view.length();
  ASSERT_TRUE(original.authoring_waypoints.has_value());
  const std::vector<Waypoint> waypoints_before = *original.authoring_waypoints;

  constexpr double kDx = 12.5;
  constexpr double kDy = -7.25;
  auto command = roadmaker::edit::translate_road(network, road, kDx, kDy);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const Road& moved = *network.road(road);

  // Every station shifts by exactly (dx, dy); heading and length unchanged.
  EXPECT_NEAR(moved.plan_view.length(), length, roadmaker::tol::kLength);
  for (int i = 0; i <= 50; ++i) {
    const double s = length * i / 50.0;
    const PathPoint before = original.plan_view.evaluate(s);
    const PathPoint after = moved.plan_view.evaluate(s);
    EXPECT_NEAR(after.x, before.x + kDx, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(after.y, before.y + kDy, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(after.hdg, before.hdg, roadmaker::tol::kRoundTripHeading);
  }

  // Waypoints shift by the same delta.
  ASSERT_TRUE(moved.authoring_waypoints.has_value());
  ASSERT_EQ(moved.authoring_waypoints->size(), waypoints_before.size());
  for (std::size_t i = 0; i < waypoints_before.size(); ++i) {
    EXPECT_NEAR((*moved.authoring_waypoints)[i].x, waypoints_before[i].x + kDx, 1e-9);
    EXPECT_NEAR((*moved.authoring_waypoints)[i].y, waypoints_before[i].y + kDy, 1e-9);
  }

  // Moving a lone free road touches no junctions and no topology.
  EXPECT_FALSE(command->dirty().topology);
  EXPECT_TRUE(command->dirty().junctions.empty());
}

TEST(TranslateRoad, LinkBetweenTwoMovedRoadsSurvives) {
  RoadNetwork network;
  const RoadId a = author_line(network, "1");
  const RoadId b = author_line(network, "2", 100.0);
  network.road(a)->successor = RoadLink{.target = b, .contact = ContactPoint::Start};
  network.road(b)->predecessor = RoadLink{.target = a, .contact = ContactPoint::End};

  const std::array<RoadId, 2> both{a, b};
  auto command = roadmaker::edit::translate_roads(network, both, 5.0, 5.0);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  // Both roads moved together, so the link between them is preserved.
  ASSERT_TRUE(network.road(a)->successor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(a)->successor->target), b);
  ASSERT_TRUE(network.road(b)->predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(b)->predecessor->target), a);
}

TEST(TranslateRoad, LinkLeavingTheSetBreaksOnBothSides) {
  RoadNetwork network;
  const RoadId a = author_line(network, "1");
  const RoadId b = author_line(network, "2", 100.0);
  network.road(a)->successor = RoadLink{.target = b, .contact = ContactPoint::Start};
  network.road(b)->predecessor = RoadLink{.target = a, .contact = ContactPoint::End};

  // Move only A: the A→B / B→A links no longer meet and are cleared on both.
  auto command = roadmaker::edit::translate_road(network, a, 5.0, 5.0);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_FALSE(network.road(a)->successor.has_value());
  EXPECT_FALSE(network.road(b)->predecessor.has_value());
  // The unmoved neighbor is listed dirty so its (now link-free) state re-meshes.
  const auto& dirty_roads = command->dirty().roads;
  EXPECT_NE(std::ranges::find(dirty_roads, b), dirty_roads.end());
}

TEST(TranslateRoad, RefusesConnectingRoad) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->junction = junction; // now a connecting road

  expect_rejected(network, roadmaker::edit::translate_road(network, road, 1.0, 1.0));
}

TEST(TranslateRoad, RefusesRoadLinkedToJunction) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->successor = RoadLink{.target = junction, .contact = ContactPoint::Start};

  auto command = roadmaker::edit::translate_road(network, road, 1.0, 1.0);
  const auto applied = command->apply(network);
  ASSERT_FALSE(applied.has_value());
  // Diagnostic names the road and the junction.
  EXPECT_NE(applied.error().message.find("junction 100"), std::string::npos)
      << applied.error().message;
}

TEST(TranslateRoad, RejectsEmptyAndStale) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");

  expect_rejected(network, roadmaker::edit::translate_roads(network, {}, 1.0, 1.0));

  auto del = roadmaker::edit::delete_road(network, road);
  ASSERT_TRUE(del->apply(network).has_value());
  expect_rejected(network, roadmaker::edit::translate_road(network, road, 1.0, 1.0));
}

TEST(TranslateRoad, DeduplicatesRepeatedIds) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const Road original = *network.road(road);

  // The same id twice must shift the road once, not twice.
  const std::array<RoadId, 2> dupes{road, road};
  auto command = roadmaker::edit::translate_roads(network, dupes, 10.0, 0.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const PathPoint before = original.plan_view.evaluate(0.0);
  const PathPoint after = network.road(road)->plan_view.evaluate(0.0);
  EXPECT_NEAR(after.x, before.x + 10.0, roadmaker::tol::kRoundTripPosition);
}
