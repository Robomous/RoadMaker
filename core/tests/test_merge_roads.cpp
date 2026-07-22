// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Kernel tests for edit::check_mergeable / merge_roads (M3a). Merge welds a's
// END to b's START into one road (a's id kept, b erased); split→merge is
// geometry-identical (the section boundary survives), undo is byte-identical,
// far-end links re-point, and each precondition refuses with its message.

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

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::Road;
using roadmaker::RoadId;
using roadmaker::RoadLink;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::expect_same_geometry;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId author(RoadNetwork& network,
              std::vector<Waypoint> waypoints,
              const char* odr_id,
              LaneProfile profile = LaneProfile::two_lane_default()) {
  auto road = roadmaker::author_clothoid_road(network, waypoints, profile, "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

void apply(RoadNetwork& network, std::unique_ptr<Command> command) {
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("apply: " + applied.error().message);
  }
}

std::string message(const RoadNetwork& network, RoadId a, RoadId b) {
  auto ok = roadmaker::edit::check_mergeable(network, a, b);
  return ok.has_value() ? std::string() : ok.error().message;
}

} // namespace

TEST(MergeRoads, TwoAdjacentRoadsMergeAndRoundTrip) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");
  const RoadId b =
      author(network, {Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "2");
  ASSERT_TRUE(roadmaker::edit::check_mergeable(network, a, b).has_value())
      << message(network, a, b);

  apply(network, roadmaker::edit::merge_roads(network, a, b));

  // b is gone, a spans both, and the merged road is a straight 100 m line.
  EXPECT_EQ(network.road_count(), 1U);
  ASSERT_NE(network.road(a), nullptr);
  EXPECT_EQ(network.road(b), nullptr);
  EXPECT_NEAR(network.road(a)->plan_view.length(), 100.0, roadmaker::tol::kRoundTripPosition);
  const auto end = network.road(a)->plan_view.evaluate(100.0);
  EXPECT_NEAR(end.x, 100.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(end.y, 0.0, roadmaker::tol::kRoundTripPosition);
}

TEST(MergeRoads, ApplyRevertReapplyIsByteIdentical) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");
  const RoadId b =
      author(network, {Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "2");
  auto command = roadmaker::edit::merge_roads(network, a, b);
  const std::string before = snapshot_xodr(network);

  ASSERT_TRUE(command->apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command->revert(network).has_value());
  expect_network_matches(network, before);
  ASSERT_TRUE(command->apply(network).has_value());
  expect_network_matches(network, after);
  ASSERT_TRUE(command->revert(network).has_value());
  expect_network_matches(network, before);
}

TEST(MergeRoads, SplitThenMergeIsGeometryIdentical) {
  RoadNetwork network;
  const RoadId road = author(network,
                             {Waypoint{.x = 0.0, .y = 0.0},
                              Waypoint{.x = 40.0, .y = 15.0},
                              Waypoint{.x = 100.0, .y = 0.0}},
                             "1");
  const Road original = *network.road(road);

  apply(network, roadmaker::edit::split_road(network, road, 50.0));
  ASSERT_EQ(network.road_count(), 2U);
  // The tail is the road that isn't the original.
  RoadId tail;
  network.for_each_road([&](RoadId id, const Road&) {
    if (id != road) {
      tail = id;
    }
  });
  ASSERT_TRUE(tail.is_valid());

  apply(network, roadmaker::edit::merge_roads(network, road, tail));
  ASSERT_EQ(network.road_count(), 1U);
  expect_same_geometry(original, *network.road(road));
}

TEST(MergeRoads, FarEndLinkAndNeighborRepoint) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 150.0, .y = 0.0}}, "1");
  // Split twice → head + mid + tail, each linked to the next.
  apply(network, roadmaker::edit::split_road(network, road, 50.0));
  RoadId mid;
  network.for_each_road([&](RoadId id, const Road&) {
    if (id != road) {
      mid = id;
    }
  });
  apply(network, roadmaker::edit::split_road(network, mid, 50.0)); // mid now [50,100]
  RoadId tail;
  network.for_each_road([&](RoadId id, const Road&) {
    if (id != road && id != mid) {
      tail = id;
    }
  });
  ASSERT_TRUE(mid.is_valid() && tail.is_valid());

  // Merge head + mid: the merged road inherits mid's successor (tail), and
  // tail's predecessor re-points from mid onto head.
  apply(network, roadmaker::edit::merge_roads(network, road, mid));
  EXPECT_EQ(network.road(mid), nullptr);
  ASSERT_TRUE(network.road(road)->successor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(road)->successor->target), tail);
  ASSERT_TRUE(network.road(tail)->predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(tail)->predecessor->target), road);
}

TEST(MergeRoads, PreconditionsRefuseWithMessages) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");
  // Self and stale.
  EXPECT_NE(message(network, a, a).find("itself"), std::string::npos);
  EXPECT_NE(message(network, a, RoadId{}).find("stale"), std::string::npos);

  // Position gap: a distant road.
  const RoadId far =
      author(network, {Waypoint{.x = 200.0, .y = 0.0}, Waypoint{.x = 250.0, .y = 0.0}}, "2");
  EXPECT_NE(message(network, a, far).find("apart"), std::string::npos);

  // Wrong orientation: b's END meets a's end (End-to-End).
  const RoadId ee =
      author(network, {Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "3");
  EXPECT_NE(message(network, a, ee).find("reverse one first"), std::string::npos);

  // Seam mismatch: different lane profile (lane count) at the seam.
  const RoadId wide = author(network,
                             {Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                             "4",
                             LaneProfile::highway());
  EXPECT_FALSE(message(network, a, wide).empty());
}

TEST(MergeRoads, RefusesJunctionRoad) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");
  const RoadId b =
      author(network, {Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "2");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(a)->successor = RoadLink{.target = junction, .contact = ContactPoint::Start};
  EXPECT_NE(message(network, a, b).find("junction"), std::string::npos);
}
