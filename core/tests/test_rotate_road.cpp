// Kernel tests for edit::rotate_road (A3 transform gizmo). A rigid rotation
// about a world pivot: plan-view start positions and authoring waypoints rotate,
// each record's heading gains the angle, lengths/lanes/elevation are untouched,
// undo is byte-identical, a link to a non-rotating road breaks on both sides,
// and a junction road refuses (named diagnostic).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
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
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId author_line(RoadNetwork& network, const char* odr_id, double y = 0.0) {
  const std::vector<Waypoint> waypoints{
      Waypoint{.x = 0.0, .y = y}, Waypoint{.x = 40.0, .y = y + 5.0}, Waypoint{.x = 80.0, .y = y}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

double norm_angle(double a) {
  return std::atan2(std::sin(a), std::cos(a));
}

} // namespace

TEST(RotateRoad, RotatesGeometryAndWaypointsAboutPivotRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const Road original = *network.road(road);
  const double length = original.plan_view.length();
  ASSERT_TRUE(original.authoring_waypoints.has_value());
  const std::vector<Waypoint> waypoints_before = *original.authoring_waypoints;

  constexpr double kAngle = std::numbers::pi / 2.0; // +90° CCW
  constexpr double kPx = 10.0;
  constexpr double kPy = 20.0;
  auto command = roadmaker::edit::rotate_road(network, road, kAngle, kPx, kPy);

  // §8 round-trip oracle: apply → revert (pristine) → apply → revert (pristine).
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

  ASSERT_TRUE(command->apply(network).has_value());
  const Road& rotated = *network.road(road);

  // Arc length is preserved; every sampled pose is the rigid rotation of the
  // original pose about the pivot, heading advanced by the angle.
  EXPECT_NEAR(rotated.plan_view.length(), length, roadmaker::tol::kLength);
  const double c = std::cos(kAngle);
  const double s = std::sin(kAngle);
  for (int i = 0; i <= 50; ++i) {
    const double st = length * i / 50.0;
    const PathPoint b = original.plan_view.evaluate(st);
    const PathPoint a = rotated.plan_view.evaluate(st);
    const double ex = kPx + (c * (b.x - kPx)) - (s * (b.y - kPy));
    const double ey = kPy + (s * (b.x - kPx)) + (c * (b.y - kPy));
    EXPECT_NEAR(a.x, ex, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(a.y, ey, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(norm_angle(a.hdg - (b.hdg + kAngle)), 0.0, roadmaker::tol::kRoundTripHeading);
  }

  // Waypoints rotate about the same pivot.
  ASSERT_TRUE(rotated.authoring_waypoints.has_value());
  ASSERT_EQ(rotated.authoring_waypoints->size(), waypoints_before.size());
  for (std::size_t i = 0; i < waypoints_before.size(); ++i) {
    const double ex =
        kPx + (c * (waypoints_before[i].x - kPx)) - (s * (waypoints_before[i].y - kPy));
    const double ey =
        kPy + (s * (waypoints_before[i].x - kPx)) + (c * (waypoints_before[i].y - kPy));
    EXPECT_NEAR((*rotated.authoring_waypoints)[i].x, ex, 1e-9);
    EXPECT_NEAR((*rotated.authoring_waypoints)[i].y, ey, 1e-9);
  }

  // A lone free road touches no junctions and no topology.
  EXPECT_FALSE(command->dirty().topology);
  EXPECT_TRUE(command->dirty().junctions.empty());
}

TEST(RotateRoad, ZeroAngleLeavesGeometryUnchangedAndUndoIsPristine) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const Road original = *network.road(road);
  const double length = original.plan_view.length();
  const std::string before = snapshot_xodr(network);

  auto command = roadmaker::edit::rotate_road(network, road, 0.0, 3.0, 4.0);
  ASSERT_TRUE(command->apply(network).has_value());

  // A zero rotation reproduces the geometry within tolerance (rebuilding the
  // reference line re-normalizes headings, so it is not byte-identical forward —
  // but the M2 invariant that matters, undo, IS byte-identical, checked below).
  const Road& after = *network.road(road);
  for (int i = 0; i <= 20; ++i) {
    const double st = length * i / 20.0;
    const PathPoint b = original.plan_view.evaluate(st);
    const PathPoint a = after.plan_view.evaluate(st);
    EXPECT_NEAR(a.x, b.x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(a.y, b.y, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(norm_angle(a.hdg - b.hdg), 0.0, roadmaker::tol::kRoundTripHeading);
  }
  ASSERT_TRUE(command->revert(network).has_value());
  expect_network_matches(network, before);
}

TEST(RotateRoad, LinkToANonRotatingRoadBreaksOnBothSides) {
  RoadNetwork network;
  const RoadId a = author_line(network, "1");
  const RoadId b = author_line(network, "2", 100.0);
  network.road(a)->successor = RoadLink{.target = b, .contact = ContactPoint::Start};
  network.road(b)->predecessor = RoadLink{.target = a, .contact = ContactPoint::End};

  // Rotate only A: the A↔B links no longer meet and clear on both sides.
  auto command = roadmaker::edit::rotate_road(network, a, std::numbers::pi / 6.0, 0.0, 0.0);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_FALSE(network.road(a)->successor.has_value());
  EXPECT_FALSE(network.road(b)->predecessor.has_value());
  const auto& dirty_roads = command->dirty().roads;
  EXPECT_NE(std::ranges::find(dirty_roads, b), dirty_roads.end());
}

TEST(RotateRoad, RefusesRoadLinkedToJunction) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->successor = RoadLink{.target = junction, .contact = ContactPoint::Start};

  auto command = roadmaker::edit::rotate_road(network, road, 0.5, 0.0, 0.0);
  const auto applied = command->apply(network);
  ASSERT_FALSE(applied.has_value());
  EXPECT_NE(applied.error().message.find("junction 100"), std::string::npos)
      << applied.error().message;
}

TEST(RotateRoad, RejectsStaleRoad) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  auto del = roadmaker::edit::delete_road(network, road);
  ASSERT_TRUE(del->apply(network).has_value());
  const std::string before = snapshot_xodr(network);
  auto command = roadmaker::edit::rotate_road(network, road, 0.5, 0.0, 0.0);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}
