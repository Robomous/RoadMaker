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

// Kernel tests for edit::translate_roads / translate_road (M3a topology UX).
// The move shifts plan-view x/y and authoring waypoints only; headings,
// lengths, s, lanes, elevation and marks are untouched, undo is byte-identical,
// links leaving the moved set break on both sides, and junction roads refuse.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/signal.hpp"
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
using roadmaker::NetworkMesh;
using roadmaker::Object;
using roadmaker::ObjectType;
using roadmaker::PathPoint;
using roadmaker::Road;
using roadmaker::RoadId;
using roadmaker::RoadLink;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
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

/// A tree declared at its bundled model's own size, so its mesh instance draws
/// at scale 1.0 (mirrors the helper in test_object_ops.cpp).
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

Signal make_sign(std::string odr_id, double s, double t) {
  Signal sign;
  sign.odr_id = std::move(odr_id);
  sign.dynamic = false;
  sign.type = "274";
  sign.subtype = "50";
  sign.country = "DE";
  sign.s = s;
  sign.t = t;
  return sign;
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

// #400: a prop stores no world pose — its instance transform is derived from
// the road frame at mesh time — so moving the road must re-derive it. The move
// itself changes no object datum, which is exactly why the mesh went stale: the
// re-derivation keys off the roads channel, not the objects one.
TEST(TranslateRoad, PlacedPropsAndSignsFollowTheMovedRoad) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  network.add_object(road, make_tree("1", 40.0, 4.0));
  network.add_signal(road, make_sign("2", 60.0, -4.0));

  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 1U);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  const std::array<double, 3> tree_before = mesh.objects.front().position;
  const double tree_heading_before = mesh.objects.front().heading;
  const std::array<double, 3> sign_before = mesh.signal_instances.front().position;

  constexpr double kDx = 12.5;
  constexpr double kDy = -7.25;
  auto command = roadmaker::edit::translate_road(network, road, kDx, kDy);
  ASSERT_TRUE(command->apply(network).has_value());

  // The object data is untouched by the move — this is a RENDER bug, not a data
  // one — so the objects channel stays empty and the roads channel carries it.
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(dirty.objects.empty());
  ASSERT_FALSE(dirty.roads.empty());

  roadmaker::remesh_roads(network, mesh, dirty.roads);
  roadmaker::remesh_object_instances(network, mesh, dirty.roads);

  ASSERT_EQ(mesh.objects.size(), 1U);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_NEAR(mesh.objects.front().position[0], tree_before[0] + kDx, 1e-9);
  EXPECT_NEAR(mesh.objects.front().position[1], tree_before[1] + kDy, 1e-9);
  EXPECT_NEAR(mesh.objects.front().position[2], tree_before[2], 1e-9);
  // A translation is a pure shift: the prop keeps its facing.
  EXPECT_NEAR(mesh.objects.front().heading, tree_heading_before, 1e-12);
  EXPECT_NEAR(mesh.signal_instances.front().position[0], sign_before[0] + kDx, 1e-9);
  EXPECT_NEAR(mesh.signal_instances.front().position[1], sign_before[1] + kDy, 1e-9);

  // Undo puts them back, through the same channel.
  ASSERT_TRUE(command->revert(network).has_value());
  roadmaker::remesh_roads(network, mesh, dirty.roads);
  roadmaker::remesh_object_instances(network, mesh, dirty.roads);
  EXPECT_NEAR(mesh.objects.front().position[0], tree_before[0], 1e-9);
  EXPECT_NEAR(mesh.objects.front().position[1], tree_before[1], 1e-9);
  EXPECT_NEAR(mesh.signal_instances.front().position[0], sign_before[0], 1e-9);

  // Incremental and from-scratch must agree — the parity that #400 broke.
  const NetworkMesh fresh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(fresh.objects.size(), mesh.objects.size());
  EXPECT_EQ(fresh.objects.front().position, mesh.objects.front().position);
  EXPECT_EQ(fresh.signal_instances.front().position, mesh.signal_instances.front().position);
}

// Re-deriving a road's placements APPENDS them, so a move would leave the
// channel ordered by most-recent edit instead of by road. That order is not
// cosmetic: the USD exporter names each prim after its index and the glTF
// exporter emits one node per instance in sequence, so a renumbered channel
// exports the same scene differently depending on its edit history. Moving the
// FIRST of two roads is the case a single-road test cannot see.
TEST(TranslateRoad, MovedRoadKeepsTheInstanceChannelInArenaOrder) {
  RoadNetwork network;
  const RoadId first = author_line(network, "1");
  const RoadId second = author_line(network, "2");
  network.add_object(first, make_tree("1", 20.0, 4.0));
  network.add_object(second, make_tree("2", 30.0, 4.0));
  network.add_signal(first, make_sign("3", 40.0, -4.0));
  network.add_signal(second, make_sign("4", 50.0, -4.0));

  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 2U);
  ASSERT_EQ(mesh.signal_instances.size(), 2U);

  auto command = roadmaker::edit::translate_road(network, first, 9.0, 3.0);
  ASSERT_TRUE(command->apply(network).has_value());
  const roadmaker::edit::DirtySet dirty = command->dirty();
  roadmaker::remesh_roads(network, mesh, dirty.roads);
  roadmaker::remesh_object_instances(network, mesh, dirty.roads);

  // Index for index against a from-scratch build — the guarantee the exporters
  // rely on (docs/architecture/kernel.md).
  const NetworkMesh fresh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), fresh.objects.size());
  for (std::size_t i = 0; i < fresh.objects.size(); ++i) {
    EXPECT_EQ(mesh.objects[i].object, fresh.objects[i].object) << "prop order drifted at " << i;
    EXPECT_EQ(mesh.objects[i].position, fresh.objects[i].position);
  }
  ASSERT_EQ(mesh.signal_instances.size(), fresh.signal_instances.size());
  for (std::size_t i = 0; i < fresh.signal_instances.size(); ++i) {
    EXPECT_EQ(mesh.signal_instances[i].signal, fresh.signal_instances[i].signal)
        << "signal order drifted at " << i;
    EXPECT_EQ(mesh.signal_instances[i].position, fresh.signal_instances[i].position);
  }
}

// A road that is gone takes its props with it: the instance channel is keyed by
// owning road, so re-deriving an erased road drops what it owned instead of
// leaving ghosts floating where the road used to be.
TEST(TranslateRoad, ErasedRoadLeavesNoGhostProps) {
  RoadNetwork network;
  const RoadId road = author_line(network, "1");
  network.add_object(road, make_tree("1", 40.0, 4.0));

  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 1U);

  auto command = roadmaker::edit::delete_road(network, road);
  ASSERT_TRUE(command->apply(network).has_value());
  const roadmaker::edit::DirtySet dirty = command->dirty();
  roadmaker::remesh_roads(network, mesh, dirty.roads);
  roadmaker::remesh_object_instances(network, mesh, dirty.roads);

  EXPECT_TRUE(mesh.objects.empty());
  EXPECT_TRUE(mesh.roads.empty());
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
