// Kernel tests for edit::set_corner_radius / set_corner_extents (p4-s1, #225).
// Corners are named by their adjacent arm pair; the commands are pure junction
// value edits, so undo is byte-identical and the turn set is never touched
// (junctions_are_current). Adjacency is validated through the same solver the
// mesher uses, so a non-adjacent or stale pair is a clean error, not a crash.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_corners;
using roadmaker::JunctionCornerInfo;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::set_corner_extents;
using roadmaker::edit::set_corner_radius;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// The §8 command oracle: apply changes the doc, revert restores it
/// byte-identically, re-apply reproduces, final revert is pristine.
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

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

/// A four-arm crossing with a 20 m arm gap, junction already materialized.
/// The gap matters: with the mesher's tight default spacing every corner's
/// `max_radius` is ~2.5 m, so an authored 9 m would come back clamped and the
/// tests below could not tell "honored" from "clamped". Roomy arms leave the
/// authored value the binding constraint (the clamp itself is covered by
/// AnAbsurdRadiusIsStoredAndClampedOnlyWhenSolved).
JunctionId make_cross(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
  const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
  auto command = roadmaker::edit::create_junction(network, ends);
  if (command == nullptr) {
    throw std::runtime_error("create_junction: null command");
  }
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("create_junction: " + applied.error().message);
  }
  JunctionId found{};
  network.for_each_junction([&](JunctionId id, const Junction&) { found = id; });
  return found;
}

} // namespace

TEST(CornerOperations, FourArmCrossingExposesFourCorners) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  EXPECT_EQ(junction_corners(network, junction).size(), 4U);
}

TEST(CornerOperations, SetRadiusRoundTripsByteIdentical) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 10.0);
  expect_command_round_trip(network, *command);
}

TEST(CornerOperations, SetRadiusStoresTheAuthoredValueOnTheNamedCorner) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_GE(corners.size(), 2U);

  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 9.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const Junction& after = *network.junction(junction);
  ASSERT_EQ(after.corners.size(), 1U);
  EXPECT_EQ(after.corners[0].arm_a, corners[0].arm_a);
  EXPECT_EQ(after.corners[0].arm_b, corners[0].arm_b);
  ASSERT_TRUE(after.corners[0].radius.has_value());
  EXPECT_DOUBLE_EQ(*after.corners[0].radius, 9.0);

  // Only the named corner moved; the others keep their derived radii.
  const std::vector<JunctionCornerInfo> solved = junction_corners(network, junction);
  ASSERT_EQ(solved.size(), corners.size());
  EXPECT_NEAR(solved[0].radius, 9.0, 1e-9);
  EXPECT_TRUE(solved[0].radius_authored);
  for (std::size_t i = 1; i < solved.size(); ++i) {
    EXPECT_FALSE(solved[i].radius_authored);
    EXPECT_NEAR(solved[i].radius, corners[i].radius, 1e-9);
  }
}

TEST(CornerOperations, SetRadiusReportsJunctionsCurrentSoTheEditorSkipsRegeneration) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 8.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_EQ(dirty.junctions, std::vector<JunctionId>{junction});
  EXPECT_TRUE(dirty.junctions_are_current);
  EXPECT_FALSE(dirty.topology);
  EXPECT_TRUE(dirty.roads.empty());
}

TEST(CornerOperations, NonPositiveRadiusRemovesTheOverride) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  auto set = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 12.0);
  ASSERT_TRUE(set->apply(network).has_value());
  ASSERT_EQ(network.junction(junction)->corners.size(), 1U);

  auto clear = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 0.0);
  expect_command_round_trip(network, *clear);
  ASSERT_TRUE(clear->apply(network).has_value());
  EXPECT_TRUE(network.junction(junction)->corners.empty());

  // Back to the derived radius, exactly.
  EXPECT_NEAR(junction_corners(network, junction)[0].radius, corners[0].radius, 1e-9);
}

TEST(CornerOperations, ClearingACornerThatHasNoOverrideIsRejected) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  const std::string before = snapshot_xodr(network);
  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, -1.0);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

TEST(CornerOperations, SetExtentsRoundTripsAndSupersedesARadius) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());
  const JunctionCornerInfo& corner = corners[0];

  // Both legs stay under their geometric bound so the authored values, not the
  // clamp, are what the solve returns — while still being asymmetric, which is
  // the whole point of per-side extents.
  const double authored_a = corner.extent_a * 0.6;
  const double authored_b = corner.extent_b * 0.9;
  ASSERT_LT(authored_a, corner.max_extent_a);
  ASSERT_LT(authored_b, corner.max_extent_b);
  ASSERT_NE(authored_a, authored_b);

  auto extents =
      set_corner_extents(network, junction, corner.arm_a, corner.arm_b, authored_a, authored_b);
  expect_command_round_trip(network, *extents);
  ASSERT_TRUE(extents->apply(network).has_value());

  const std::vector<JunctionCornerInfo> solved = junction_corners(network, junction);
  EXPECT_TRUE(solved[0].extents_authored);
  EXPECT_NEAR(solved[0].extent_a, authored_a, 1e-9);
  EXPECT_NEAR(solved[0].extent_b, authored_b, 1e-9);

  // A radius is symmetric by definition, so authoring one clears the extents.
  auto radius = set_corner_radius(network, junction, corner.arm_a, corner.arm_b, 6.0);
  ASSERT_TRUE(radius->apply(network).has_value());
  const Junction& after = *network.junction(junction);
  ASSERT_EQ(after.corners.size(), 1U);
  EXPECT_FALSE(after.corners[0].extent_a.has_value());
  EXPECT_FALSE(after.corners[0].extent_b.has_value());
}

TEST(CornerOperations, NonPositiveExtentsAreRejected) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  const std::string before = snapshot_xodr(network);
  auto command =
      set_corner_extents(network, junction, corners[0].arm_a, corners[0].arm_b, 0.0, 4.0);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

TEST(CornerOperations, NonAdjacentPairIsACleanError) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_GE(corners.size(), 3U);

  // Corner 0's first arm paired with corner 2's second arm: both are real arms
  // of this junction, but they are not neighbors, so there is no corner there.
  const std::string before = snapshot_xodr(network);
  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[2].arm_b, 7.0);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

TEST(CornerOperations, ReversedPairIsACleanError) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  // The pair is ORDERED (a's right edge meets b's left edge); swapping names a
  // different, non-existent corner.
  auto command = set_corner_radius(network, junction, corners[0].arm_b, corners[0].arm_a, 7.0);
  EXPECT_FALSE(command->apply(network).has_value());
}

TEST(CornerOperations, StaleJunctionIdIsACleanError) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());
  const RoadEnd arm_a = corners[0].arm_a;
  const RoadEnd arm_b = corners[0].arm_b;

  auto command = set_corner_radius(network, JunctionId{}, arm_a, arm_b, 7.0);
  EXPECT_FALSE(command->apply(network).has_value());
}

TEST(CornerOperations, AnAbsurdRadiusIsStoredAndClampedOnlyWhenSolved) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_FALSE(corners.empty());

  // Authoring is not clamped — a later arm move can make room — but the solve
  // never exceeds what the faces leave, so the mesh can never fail.
  auto command = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 1.0e6);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(*network.junction(junction)->corners[0].radius, 1.0e6);

  const std::vector<JunctionCornerInfo> solved = junction_corners(network, junction);
  EXPECT_LE(solved[0].radius, solved[0].max_radius + 1e-9);
}

TEST(CornerOperations, TwoCornersCanBeAuthoredIndependently) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionCornerInfo> corners = junction_corners(network, junction);
  ASSERT_GE(corners.size(), 2U);

  auto first = set_corner_radius(network, junction, corners[0].arm_a, corners[0].arm_b, 5.0);
  ASSERT_TRUE(first->apply(network).has_value());
  auto second = set_corner_radius(network, junction, corners[1].arm_a, corners[1].arm_b, 11.0);
  expect_command_round_trip(network, *second);
  ASSERT_TRUE(second->apply(network).has_value());

  EXPECT_EQ(network.junction(junction)->corners.size(), 2U);
  const std::vector<JunctionCornerInfo> solved = junction_corners(network, junction);
  EXPECT_NEAR(solved[0].radius, 5.0, 1e-9);
  EXPECT_NEAR(solved[1].radius, 11.0, 1e-9);
}
