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

// Kernel tests for edit::rotate_road (A3 transform gizmo). A rigid rotation
// about a world pivot: plan-view start positions and authoring waypoints rotate,
// each record's heading gains the angle, lengths/lanes/elevation are untouched,
// undo is byte-identical, a link to a non-rotating road swings that road's
// contacting end round with it (cascade-s1), and a junction road refuses
// (named diagnostic).

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::NetworkMesh;
using roadmaker::Object;
using roadmaker::ObjectType;
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

/// A tree declared at its bundled model's own size (mirrors test_object_ops).
Object make_tree(std::string odr_id, double s, double t) {
  Object tree;
  tree.odr_id = std::move(odr_id);
  tree.name = "tree_pine"; // a bundled prop model
  tree.type = ObjectType::Tree;
  tree.s = s;
  tree.t = t;
  if (const roadmaker::props::PropModel* model = roadmaker::props::model(tree.name)) {
    tree.radius = model->radius;
    tree.height = model->height;
  }
  return tree;
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

// cascade-s1 (#461): rotate used to clear EVERY road-level link it had, on the
// reasoning that a road turning about a pivot meets nothing any more. It now
// swings its neighbours' contacting ends round with it. The fixture has to weld
// a genuinely coincident pair for that to be testable at all.
TEST(RotateRoad, ANonRotatingNeighbourIsSwungRoundNotCutLoose) {
  RoadNetwork network;
  auto author = [&](std::vector<Waypoint> waypoints, const char* odr_id) {
    auto road = roadmaker::author_clothoid_road(
        network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
    if (!road.has_value()) {
      throw std::runtime_error("author: " + road.error().message);
    }
    return *road;
  };
  const RoadId a = author({{0.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId b = author({{100.0, 0.0}, {200.0, 0.0}}, "2");
  ASSERT_TRUE(roadmaker::edit::close_gap(network,
                                         roadmaker::RoadEnd{a, ContactPoint::End},
                                         roadmaker::RoadEnd{b, ContactPoint::Start})
                  ->apply(network)
                  .has_value());

  // Rotate only A, about its own free end so the joint swings but stays close.
  auto command = roadmaker::edit::rotate_road(network, a, std::numbers::pi / 12.0, 0.0, 0.0);
  ASSERT_TRUE(command->apply(network).has_value());

  ASSERT_TRUE(network.road(a)->successor.has_value());
  ASSERT_TRUE(network.road(b)->predecessor.has_value());
  const auto weld =
      roadmaker::edit::verify_link_weld(network, roadmaker::RoadEnd{a, ContactPoint::End});
  ASSERT_TRUE(weld.has_value()) << weld.error().message;
  EXPECT_FALSE(weld->breaches);
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

// #400, rotation half: a prop rides the rigid rotation — position about the
// pivot, facing advanced by the angle — because its transform is derived from
// the road frame, not stored. Nothing in the object data changes.
TEST(RotateRoad, PlacedPropsRideTheRotation) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  network.add_object(road, make_tree("1", 40.0, 4.0));

  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 1U);
  const std::array<double, 3> before = mesh.objects.front().position;
  const double heading_before = mesh.objects.front().heading;

  constexpr double kAngle = std::numbers::pi / 3.0;
  constexpr double kPx = 10.0;
  constexpr double kPy = 20.0;
  auto command = roadmaker::edit::rotate_road(network, road, kAngle, kPx, kPy);
  ASSERT_TRUE(command->apply(network).has_value());

  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(dirty.objects.empty()) << "a rotation changes no object datum";
  roadmaker::remesh_roads(network, mesh, dirty.roads);
  roadmaker::remesh_object_instances(network, mesh, dirty.roads);

  const double c = std::cos(kAngle);
  const double s = std::sin(kAngle);
  const double ex = kPx + (c * (before[0] - kPx)) - (s * (before[1] - kPy));
  const double ey = kPy + (s * (before[0] - kPx)) + (c * (before[1] - kPy));
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_NEAR(mesh.objects.front().position[0], ex, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(mesh.objects.front().position[1], ey, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(norm_angle(mesh.objects.front().heading - (heading_before + kAngle)),
              0.0,
              roadmaker::tol::kRoundTripHeading);
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
