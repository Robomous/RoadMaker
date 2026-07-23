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

#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/road_intersection.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

namespace {

using roadmaker::count_errors;
using roadmaker::LaneProfile;
using roadmaker::RoadNetwork;
using roadmaker::validate_network;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::assembly::cross_onto_road;
using roadmaker::edit::assembly::cross_roads;
using roadmaker::edit::assembly::IntersectionParams;
using roadmaker::edit::assembly::Pose;
using roadmaker::edit::assembly::t_intersection;
using roadmaker::edit::assembly::tee_onto_road;
using roadmaker::edit::assembly::x_intersection;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

/// apply→revert byte-identical, re-apply reproduces, final revert pristine —
/// the M2 command oracle (see test_edit_operations.cpp).
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

TEST(Assembly, TIntersectionIsValidWithOneJunctionAndThreeArms) {
  RoadNetwork network;
  auto command = t_intersection(network, Pose{.x = 0.0, .y = 0.0, .heading = 0.0});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_GE(network.road_count(), 3U); // 3 arms + generated connecting roads
  // Undo removes exactly what it created.
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.road_count(), 0U);
  EXPECT_EQ(network.junction_count(), 0U);
}

TEST(Assembly, TIntersectionRoundTripsByteIdentical) {
  RoadNetwork network;
  auto command = t_intersection(network, Pose{.heading = 0.3});
  expect_round_trip(network, *command);
}

TEST(Assembly, XIntersectionIsValidWithOneJunctionAndFourArms) {
  RoadNetwork network;
  auto command = x_intersection(network, Pose{});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_GE(network.road_count(), 4U);
}

TEST(Assembly, XIntersectionRoundTripsByteIdentical) {
  RoadNetwork network;
  auto command = x_intersection(network, Pose{.x = 20.0, .y = -5.0, .heading = 1.1});
  expect_round_trip(network, *command);
}

TEST(Assembly, HonorsAnExplicitGapAndArmLength) {
  RoadNetwork network;
  IntersectionParams params;
  params.gap_m = 10.0;
  params.arm_length_m = 25.0;
  auto command = x_intersection(network, Pose{}, params);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(Assembly, RejectsNonPositiveArmLength) {
  RoadNetwork network;
  IntersectionParams params;
  params.arm_length_m = 0.0;
  auto command = t_intersection(network, Pose{}, params);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 0U); // failed apply leaves the network untouched
}

TEST(Assembly, RejectsEmptyLaneProfile) {
  RoadNetwork network;
  IntersectionParams params;
  params.profile = LaneProfile{}; // no lanes either side
  auto command = x_intersection(network, Pose{}, params);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 0U);
}

// --- assembly dropped ONTO an existing road (gate finding 1) ----------------

namespace {

roadmaker::RoadId straight_target(RoadNetwork& network, double length = 200.0) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = length, .y = 0.0}};
  auto road =
      roadmaker::author_clothoid_road(network, waypoints, LaneProfile::two_lane_rural(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("target author failed");
  }
  return *road;
}

roadmaker::JunctionId only_junction(const RoadNetwork& network) {
  roadmaker::JunctionId id;
  network.for_each_junction(
      [&](roadmaker::JunctionId jid, const roadmaker::Junction&) { id = jid; });
  return id;
}

} // namespace

TEST(Assembly, TeeOntoRoadAttachesAndWeldsClean) {
  RoadNetwork network;
  const roadmaker::RoadId target = straight_target(network);
  auto command = tee_onto_road(network, target, 100.0);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U); // attached, not a floating standalone
  const auto welds = roadmaker::edit::verify_junction_welds(network, only_junction(network));
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
  EXPECT_LE(welds->max_position_gap, roadmaker::tol::kWeldPosition);
}

TEST(Assembly, TeeOntoRoadRoundTripsByteIdentical) {
  RoadNetwork network;
  const roadmaker::RoadId target = straight_target(network);
  auto command = tee_onto_road(network, target, 100.0);
  expect_round_trip(network, *command);
}

TEST(Assembly, CrossOntoRoadMakesAFourWayThatWeldsClean) {
  RoadNetwork network;
  const roadmaker::RoadId target = straight_target(network);
  auto command = cross_onto_road(network, target, 100.0);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U);
  const auto welds = roadmaker::edit::verify_junction_welds(network, only_junction(network));
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}

TEST(Assembly, CrossOntoRoadRoundTripsByteIdentical) {
  RoadNetwork network;
  const roadmaker::RoadId target = straight_target(network);
  auto command = cross_onto_road(network, target, 100.0);
  expect_round_trip(network, *command);
}

TEST(Assembly, OntoRoadRefusesADropTooNearAnEnd) {
  RoadNetwork network;
  const roadmaker::RoadId target = straight_target(network);
  const std::string before = snapshot_xodr(network);
  auto tee = tee_onto_road(network, target, 1.0);
  EXPECT_FALSE(tee->apply(network).has_value());
  expect_network_matches(network, before);
  auto cross = cross_onto_road(network, target, 1.0);
  EXPECT_FALSE(cross->apply(network).has_value());
  expect_network_matches(network, before);
}

// --- cross_roads: 4-way where two EXISTING roads cross ----------------------

namespace {

/// A horizontal road on y=0 and a vertical road on x=0, crossing at the origin
/// (station 100 on each). Returns their ids {horizontal, vertical}.
std::pair<roadmaker::RoadId, roadmaker::RoadId> crossing_pair(RoadNetwork& network) {
  auto horizontal = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -100.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
      LaneProfile::two_lane_rural(),
      "",
      "1");
  auto vertical = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = 0.0, .y = -100.0}, Waypoint{.x = 0.0, .y = 100.0}},
      LaneProfile::two_lane_rural(),
      "",
      "2");
  if (!horizontal.has_value() || !vertical.has_value()) {
    throw std::runtime_error("crossing pair author failed");
  }
  return {*horizontal, *vertical};
}

} // namespace

TEST(Assembly, RoadIntersectionsFindsInteriorCrossing) {
  RoadNetwork network;
  const auto [horizontal, vertical] = crossing_pair(network);
  const auto crossings = roadmaker::road_intersections(network, horizontal, vertical);
  ASSERT_TRUE(crossings.has_value());
  ASSERT_EQ(crossings->size(), 1U);
  EXPECT_NEAR(crossings->front().s_a, 100.0, 1e-3);
  EXPECT_NEAR(crossings->front().s_b, 100.0, 1e-3);
  EXPECT_NEAR(crossings->front().point.x, 0.0, 1e-3);
  EXPECT_NEAR(crossings->front().point.y, 0.0, 1e-3);
}

TEST(Assembly, RoadIntersectionsReturnsNoneWhenDisjoint) {
  RoadNetwork network;
  auto a = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
      LaneProfile::two_lane_rural(),
      "",
      "1");
  auto b = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = 0.0, .y = 20.0}, Waypoint{.x = 100.0, .y = 20.0}},
      LaneProfile::two_lane_rural(),
      "",
      "2");
  ASSERT_TRUE(a.has_value() && b.has_value());
  const auto crossings = roadmaker::road_intersections(network, *a, *b);
  ASSERT_TRUE(crossings.has_value());
  EXPECT_TRUE(crossings->empty());
}

TEST(Assembly, CrossRoadsFormsFourWayJunctionAtCrossing) {
  RoadNetwork network;
  const auto [horizontal, vertical] = crossing_pair(network);
  auto command = cross_roads(network, horizontal, vertical);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  ASSERT_EQ(network.junction_count(), 1U);
  const roadmaker::JunctionId junction = only_junction(network);
  EXPECT_EQ(network.junction(junction)->arms.size(), 4U);        // four arms
  EXPECT_FALSE(network.junction(junction)->connections.empty()); // connecting lanes
  const auto welds = roadmaker::edit::verify_junction_welds(network, junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}

TEST(Assembly, CrossRoadsRoundTripsByteIdentical) {
  RoadNetwork network;
  const auto [horizontal, vertical] = crossing_pair(network);
  auto command = cross_roads(network, horizontal, vertical);
  expect_round_trip(network, *command);
}

TEST(Assembly, CrossRoadsRejectsRoadAlreadyInJunction) {
  RoadNetwork network;
  const auto [horizontal, vertical] = crossing_pair(network);
  auto first = cross_roads(network, horizontal, vertical);
  ASSERT_TRUE(first->apply(network).has_value());

  // A generated connecting road lives inside the junction (road.junction set).
  roadmaker::RoadId connecting;
  network.for_each_road([&](roadmaker::RoadId id, const roadmaker::Road& road) {
    if (road.junction.is_valid()) {
      connecting = id;
    }
  });
  ASSERT_TRUE(connecting.is_valid());

  auto fresh = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -50.0, .y = 50.0}, Waypoint{.x = 50.0, .y = 50.0}},
      LaneProfile::two_lane_rural(),
      "",
      ""); // auto id — the cross above already consumed several numeric ids
  ASSERT_TRUE(fresh.has_value());

  const std::string before = snapshot_xodr(network);
  auto command = cross_roads(network, connecting, *fresh);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

// --- create_road_with_interactions: many junctions per stroke (#354) --------

namespace {

using roadmaker::BodyCrossing;
using roadmaker::EndpointHeadings;
using roadmaker::edit::assembly::create_road_with_interactions;
using roadmaker::edit::assembly::RoadInteractions;

/// Two parallel horizontal roads at y = ±25 (x ∈ [−100, 100]) that a vertical
/// stroke through the origin crosses at both. Returns {lower, upper}.
std::pair<roadmaker::RoadId, roadmaker::RoadId> two_crossers(RoadNetwork& network) {
  auto lower = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -100.0, .y = -25.0}, Waypoint{.x = 100.0, .y = -25.0}},
      LaneProfile::two_lane_rural(),
      "",
      "1");
  auto upper = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -100.0, .y = 25.0}, Waypoint{.x = 100.0, .y = 25.0}},
      LaneProfile::two_lane_rural(),
      "",
      "2");
  if (!lower.has_value() || !upper.has_value()) {
    throw std::runtime_error("two_crossers author failed");
  }
  return {*lower, *upper};
}

RoadInteractions crossings_of(const RoadNetwork& network, const std::vector<Waypoint>& stroke) {
  RoadInteractions interactions;
  const auto line = roadmaker::fit_clothoid_path(stroke, EndpointHeadings{});
  for (const BodyCrossing& crossing :
       roadmaker::body_crossings(network, *line, roadmaker::RoadId{})) {
    interactions.crossings.push_back(crossing.road);
  }
  return interactions;
}

} // namespace

TEST(Assembly, CreateRoadWithInteractionsFormsBothCrossingsInOneCommand) {
  RoadNetwork network;
  two_crossers(network);
  const std::vector<Waypoint> stroke{Waypoint{.x = 0.0, .y = -70.0}, Waypoint{.x = 0.0, .y = 70.0}};
  RoadInteractions interactions = crossings_of(network, stroke);
  ASSERT_EQ(interactions.crossings.size(), 2U); // both roads detected, in order

  auto command = create_road_with_interactions(
      network, stroke, LaneProfile::two_lane_rural(), "", EndpointHeadings{}, interactions);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 2U); // an X at each crossed road
}

TEST(Assembly, CreateRoadWithInteractionsRoundTripsByteIdentical) {
  RoadNetwork network;
  two_crossers(network);
  const std::vector<Waypoint> stroke{Waypoint{.x = 0.0, .y = -70.0}, Waypoint{.x = 0.0, .y = 70.0}};
  auto command = create_road_with_interactions(network,
                                               stroke,
                                               LaneProfile::two_lane_rural(),
                                               "",
                                               EndpointHeadings{},
                                               crossings_of(network, stroke));
  expect_round_trip(network, *command); // one undo removes all of it
}

TEST(Assembly, CreateRoadWithInteractionsCombinesACrossAndAnEndTee) {
  RoadNetwork network;
  // A road to cross at y = 0 and a target to tee the far end into at y = 60.
  auto crossed = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -100.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
      LaneProfile::two_lane_rural(),
      "",
      "1");
  auto target = roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{.x = -100.0, .y = 60.0}, Waypoint{.x = 100.0, .y = 60.0}},
      LaneProfile::two_lane_rural(),
      "",
      "2");
  ASSERT_TRUE(crossed.has_value() && target.has_value());

  const std::vector<Waypoint> stroke{Waypoint{.x = 0.0, .y = -50.0}, Waypoint{.x = 0.0, .y = 50.0}};
  RoadInteractions interactions = crossings_of(network, stroke); // the X at y = 0
  ASSERT_EQ(interactions.crossings.size(), 1U);
  interactions.end_tee = std::pair{*target, 100.0}; // x = 0 → station 100 on the target

  auto command = create_road_with_interactions(
      network, stroke, LaneProfile::two_lane_rural(), "", EndpointHeadings{}, interactions);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 2U); // one X (cross) + one T (end tee)
}

} // namespace
