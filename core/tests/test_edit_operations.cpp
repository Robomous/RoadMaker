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

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneDirection;
using roadmaker::LaneId;
using roadmaker::LaneMaterial;
using roadmaker::LaneProfile;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMark;
using roadmaker::RoadMarkType;
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

RoadId author_default(RoadNetwork& network, const char* odr_id, double y_offset = 0.0) {
  return author(network,
                {Waypoint{.x = 0.0, .y = y_offset},
                 Waypoint{.x = 60.0, .y = y_offset + 8.0},
                 Waypoint{.x = 120.0, .y = y_offset}},
                odr_id);
}

/// The §8 round-trip oracle for every factory: apply changes the document,
/// revert restores it byte-identically, re-apply reproduces the applied
/// state byte-identically, and a final revert leaves it pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  {
    SCOPED_TRACE("first apply");
    ASSERT_TRUE(command.apply(network).has_value());
  }
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after); // a command that changes nothing is a bug
  {
    SCOPED_TRACE("revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
  {
    SCOPED_TRACE("re-apply (idempotence)");
    ASSERT_TRUE(command.apply(network).has_value());
    expect_network_matches(network, after);
  }
  {
    SCOPED_TRACE("final revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
}

/// A failed apply must leave the serialized network untouched.
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

} // namespace

// --- document / lane value edits ---------------------------------------------

TEST(EditOperations, RenameRoadRoundTripsAndRenames) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  auto command = roadmaker::edit::rename_road(network, road, "Main Street");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->name, "Main Street");
  EXPECT_FALSE(command->dirty().topology);
}

TEST(EditOperations, SetLaneTypeWidthAndMarkRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();

  auto set_type = roadmaker::edit::set_lane_type(network, outer_right, LaneType::Parking);
  expect_command_round_trip(network, *set_type);

  auto set_width = roadmaker::edit::set_lane_width(network, outer_right, 2.75);
  expect_command_round_trip(network, *set_width);

  auto set_mark = roadmaker::edit::set_road_mark(
      network, outer_right, RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid, .width = 0.25});
  expect_command_round_trip(network, *set_mark);

  ASSERT_TRUE(set_width->apply(network).has_value());
  EXPECT_NEAR(network.lane(outer_right)->widths.at(0).a, 2.75, 1e-12);
}

TEST(EditOperations, LaneEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  LaneId center;
  for (const LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      center = lane_id;
    }
  }

  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, center, 3.0));
  const LaneId outer = network.lane_section(section)->lanes.back();
  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, outer, 0.0));
  expect_command_rejected(network,
                          roadmaker::edit::set_lane_type(network, LaneId{}, LaneType::Driving));
}

TEST(EditOperations, SetLaneDirectionRoundTripsAndSets) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();
  ASSERT_NE(network.lane(outer_right)->odr_id, 0);

  // Reversed is not the fresh default (Standard), so the round-trip oracle —
  // which rejects a no-op command — has a real change to observe.
  auto set_dir = roadmaker::edit::set_lane_direction(network, outer_right, LaneDirection::Reversed);
  expect_command_round_trip(network, *set_dir);

  ASSERT_TRUE(set_dir->apply(network).has_value());
  EXPECT_EQ(network.lane(outer_right)->direction, LaneDirection::Reversed);
  EXPECT_FALSE(set_dir->dirty().topology);
  EXPECT_TRUE(set_dir->dirty().junctions.empty()); // direction never regens junctions
}

TEST(EditOperations, SetLaneDirectionRejectsCenterLaneAndStaleId) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  LaneId center;
  for (const LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      center = lane_id;
    }
  }
  ASSERT_TRUE(center.is_valid());

  expect_command_rejected(
      network, roadmaker::edit::set_lane_direction(network, center, LaneDirection::Both));
  expect_command_rejected(
      network, roadmaker::edit::set_lane_direction(network, LaneId{}, LaneDirection::Reversed));
}

TEST(EditOperations, SetLaneMaterialRoundTripsAssignReplaceClear) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();
  ASSERT_NE(network.lane(outer_right)->odr_id, 0);

  // Assign one record onto a fresh (material-less) lane — a real change.
  auto assign = roadmaker::edit::set_lane_material(
      network,
      outer_right,
      {LaneMaterial{.s_offset = 0.0, .friction = 0.9, .surface = "rm:asphalt"}});
  expect_command_round_trip(network, *assign);
  ASSERT_TRUE(assign->apply(network).has_value());
  ASSERT_EQ(network.lane(outer_right)->materials.size(), 1U);
  EXPECT_EQ(*network.lane(outer_right)->materials.front().surface, "rm:asphalt");
  EXPECT_FALSE(assign->dirty().topology);
  EXPECT_TRUE(assign->dirty().junctions.empty());

  // Replace with a two-record profile — the lane already carries one record.
  auto replace = roadmaker::edit::set_lane_material(
      network,
      outer_right,
      {LaneMaterial{.s_offset = 0.0, .friction = 0.9, .surface = "rm:asphalt"},
       LaneMaterial{.s_offset = 40.0, .friction = 0.7, .surface = "rm:asphalt_worn"}});
  expect_command_round_trip(network, *replace);
  ASSERT_TRUE(replace->apply(network).has_value());
  ASSERT_EQ(network.lane(outer_right)->materials.size(), 2U);

  // Clear — empty vector removes every record.
  auto clear = roadmaker::edit::set_lane_material(network, outer_right, {});
  expect_command_round_trip(network, *clear);
}

TEST(EditOperations, SetLaneMaterialRejects) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();
  LaneId center;
  for (const LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      center = lane_id;
    }
  }
  ASSERT_TRUE(center.is_valid());

  // Center lane may not carry material (center_lane_no_material).
  expect_command_rejected(network,
                          roadmaker::edit::set_lane_material(
                              network, center, {LaneMaterial{.s_offset = 0.0, .friction = 0.9}}));
  // Descending sOffset (elem_asc_order).
  expect_command_rejected(
      network,
      roadmaker::edit::set_lane_material(network,
                                         outer_right,
                                         {LaneMaterial{.s_offset = 40.0, .friction = 0.8},
                                          LaneMaterial{.s_offset = 10.0, .friction = 0.8}}));
  // Negative friction (t_grEqZero).
  expect_command_rejected(
      network,
      roadmaker::edit::set_lane_material(
          network, outer_right, {LaneMaterial{.s_offset = 0.0, .friction = -0.1}}));
  // Record beyond the section end.
  expect_command_rejected(
      network,
      roadmaker::edit::set_lane_material(
          network, outer_right, {LaneMaterial{.s_offset = 1.0e6, .friction = 0.8}}));
  // Stale lane id.
  expect_command_rejected(
      network,
      roadmaker::edit::set_lane_material(network, LaneId{}, {LaneMaterial{.friction = 0.8}}));
}

// --- waypoint edits ------------------------------------------------------------

TEST(EditOperations, MoveWaypointRefitsAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->length;

  auto command = roadmaker::edit::move_waypoint(network, road, 1, Waypoint{.x = 60.0, .y = 30.0});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NE(network.road(road)->length, old_length);
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_NEAR(network.road(road)->authoring_waypoints->at(1).y, 30.0, 1e-12);
  // The fitted path still passes through the moved waypoint.
  const auto& records = network.road(road)->plan_view.records();
  EXPECT_NEAR(records.at(1).x, 60.0, 1e-9);
  EXPECT_NEAR(records.at(1).y, 30.0, 1e-9);
}

TEST(EditOperations, InsertAndDeleteWaypointRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto insert = roadmaker::edit::insert_waypoint(network, road, 1, Waypoint{.x = 30.0, .y = 20.0});
  expect_command_round_trip(network, *insert);

  auto erase = roadmaker::edit::delete_waypoint(network, road, 1);
  expect_command_round_trip(network, *erase);
}

TEST(EditOperations, WaypointEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");

  expect_command_rejected(network, roadmaker::edit::move_waypoint(network, road, 5, {}));
  // 2 waypoints is the minimum — deleting one must fail.
  expect_command_rejected(network, roadmaker::edit::delete_waypoint(network, road, 0));
  // Moving onto the neighbor creates coincident waypoints.
  expect_command_rejected(
      network, roadmaker::edit::move_waypoint(network, road, 0, Waypoint{.x = 50.0, .y = 0.0}));
}

TEST(EditOperations, FirstEditOfForeignRoadDerivesWaypoints) {
  // Build a road without authoring waypoints (as loaded from a foreign file).
  RoadNetwork network;
  const RoadId road = network.create_road("foreign", "1");
  network.road(road)->plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 40.0, .shape = roadmaker::LineGeom{}});
  network.road(road)->plan_view.append({.x = 40.0,
                                        .y = 0.0,
                                        .hdg = 0.0,
                                        .length = 30.0,
                                        .shape = roadmaker::ArcGeom{.curvature = 0.01}});
  network.road(road)->length = network.road(road)->plan_view.length();
  const LaneSectionId section = network.add_lane_section(road, 0.0);
  network.add_lane(section, 0, LaneType::None);
  network.add_lane(section, -1, LaneType::Driving);
  network.lane(network.lane_section(section)->lanes.back())->widths.push_back({.a = 3.5});

  auto command = roadmaker::edit::move_waypoint(network, road, 2, Waypoint{.x = 75.0, .y = 8.0});
  ASSERT_TRUE(command->apply(network).has_value());
  // Derived waypoints: 2 record starts + endpoint.
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_EQ(network.road(road)->authoring_waypoints->size(), 3U);
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_FALSE(network.road(road)->authoring_waypoints.has_value());
}

TEST(EditOperations, InsertWaypointOnCurvePreservesEndpoints) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const roadmaker::PathPoint start = network.road(road)->plan_view.evaluate(0.0);
  const roadmaker::PathPoint end =
      network.road(road)->plan_view.evaluate(network.road(road)->length);

  // Insert ON the fitted curve, midway through the first segment — the Edit
  // Nodes midpoint-marker flow (02 §3). Stations come from the public helper.
  const auto stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(stations.has_value());
  ASSERT_EQ(stations->size(), 3U);
  const double mid_s = (stations->at(0) + stations->at(1)) / 2.0;
  const roadmaker::PathPoint mid = network.road(road)->plan_view.evaluate(mid_s);

  auto command =
      roadmaker::edit::insert_waypoint(network, road, 1, Waypoint{.x = mid.x, .y = mid.y});
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(roadmaker::edit::effective_waypoints(*network.road(road)).size(), 4U);

  const roadmaker::ReferenceLine& line = network.road(road)->plan_view;
  EXPECT_NEAR(line.evaluate(0.0).x, start.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(0.0).y, start.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(line.length()).x, end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(line.length()).y, end.y, roadmaker::tol::kRoundTripPosition);

  // The re-fit interpolates the inserted node at its (new) station.
  const auto refit_stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(refit_stations.has_value());
  ASSERT_EQ(refit_stations->size(), 4U);
  EXPECT_NEAR(line.evaluate(refit_stations->at(1)).x, mid.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(refit_stations->at(1)).y, mid.y, roadmaker::tol::kRoundTripPosition);
}

TEST(EditOperations, ForeignRoadEditGoldenCurvedRoad) {
  // The issue #10 golden: load the line→spiral→arc→spiral fixture (no
  // rm:waypoints), edit a node, save — derived-waypoint re-fit and the
  // written file stay within tolerance of the original geometry.
  auto loaded = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "curved_road.xodr");
  ASSERT_TRUE(loaded.has_value());
  RoadNetwork& network = loaded->network;
  const RoadId road = network.find_road("1");
  ASSERT_TRUE(network.road(road) != nullptr);
  ASSERT_FALSE(network.road(road)->authoring_waypoints.has_value());

  // Dense sample of the original curve, the deviation oracle below. The
  // polyline chord error (~2.5e-5 m sagitta at this density on the R=20 arc)
  // stays below the tolerance the oracle asserts.
  const roadmaker::ReferenceLine original = network.road(road)->plan_view;
  const double original_length = original.length();
  constexpr int kGoldenSamples = 1600;
  constexpr int kSamples = 400;
  std::vector<roadmaker::PathPoint> golden;
  golden.reserve(kGoldenSamples + 1);
  for (int i = 0; i <= kGoldenSamples; ++i) {
    golden.push_back(original.evaluate(original_length * i / kGoldenSamples));
  }

  // Derived nodes: 4 record starts + endpoint. Insert an on-curve node in
  // the middle of the arc segment, then apply — the first edit derives
  // waypoints and re-fits the whole chain.
  const std::vector<Waypoint> nodes = roadmaker::edit::effective_waypoints(*network.road(road));
  ASSERT_EQ(nodes.size(), 5U);
  const auto stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(stations.has_value());
  const double arc_mid_s = (stations->at(2) + stations->at(3)) / 2.0;
  const roadmaker::PathPoint arc_mid = original.evaluate(arc_mid_s);
  auto command =
      roadmaker::edit::insert_waypoint(network, road, 3, Waypoint{.x = arc_mid.x, .y = arc_mid.y});
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.road(road)->authoring_waypoints->size(), 6U);

  // Every pre-edit node (all of them points of the original curve) is still
  // interpolated, endpoints included.
  const roadmaker::ReferenceLine& refit = network.road(road)->plan_view;
  const auto refit_stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(refit_stations.has_value());
  for (std::size_t i = 0; i < network.road(road)->authoring_waypoints->size(); ++i) {
    SCOPED_TRACE("node " + std::to_string(i));
    const Waypoint& node = network.road(road)->authoring_waypoints->at(i);
    const roadmaker::PathPoint fitted = refit.evaluate(refit_stations->at(i));
    EXPECT_NEAR(fitted.x, node.x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(fitted.y, node.y, roadmaker::tol::kRoundTripPosition);
  }

  // Shape fidelity, the 01 §2.5 promise: the derivation re-fit interpolates
  // the chain's headings (G1 Hermite), so this pure line/spiral/arc/spiral
  // chain — edited with an on-curve node — is reproduced within rm::tol.
  constexpr double kGoldenShapeTolerance = roadmaker::tol::kRoundTripPosition;
  const double refit_length = refit.length();
  EXPECT_NEAR(refit_length, original_length, roadmaker::tol::kRoundTripPosition);
  const auto distance_to_golden = [&golden](const roadmaker::PathPoint& p) {
    double nearest = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < golden.size(); ++i) {
      const double ax = golden[i].x;
      const double ay = golden[i].y;
      const double bx = golden[i + 1].x - ax;
      const double by = golden[i + 1].y - ay;
      const double t =
          std::clamp(((p.x - ax) * bx + (p.y - ay) * by) / (bx * bx + by * by), 0.0, 1.0);
      nearest = std::min(nearest, std::hypot(p.x - (ax + t * bx), p.y - (ay + t * by)));
    }
    return nearest;
  };
  double max_deviation = 0.0;
  for (int i = 0; i <= kSamples; ++i) {
    max_deviation =
        std::max(max_deviation, distance_to_golden(refit.evaluate(refit_length * i / kSamples)));
  }
  EXPECT_LT(max_deviation, kGoldenShapeTolerance);

  // Save → reload: the written file (now carrying rm:waypoints) parses back
  // to the same geometry.
  const std::string written = snapshot_xodr(network);
  auto reparsed = roadmaker::parse_xodr(written);
  ASSERT_TRUE(reparsed.has_value());
  const RoadId road2 = reparsed->network.find_road("1");
  ASSERT_TRUE(reparsed->network.road(road2) != nullptr);
  ASSERT_TRUE(reparsed->network.road(road2)->authoring_waypoints.has_value());
  EXPECT_EQ(reparsed->network.road(road2)->authoring_waypoints->size(), 6U);
  const roadmaker::ReferenceLine& reread = reparsed->network.road(road2)->plan_view;
  ASSERT_NEAR(reread.length(), refit_length, roadmaker::tol::kRoundTripPosition);
  for (int i = 0; i <= kSamples; ++i) {
    const double s = refit_length * i / kSamples;
    SCOPED_TRACE("station " + std::to_string(s));
    EXPECT_NEAR(reread.evaluate(s).x, refit.evaluate(s).x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(reread.evaluate(s).y, refit.evaluate(s).y, roadmaker::tol::kRoundTripPosition);
  }
}

// --- elevation ------------------------------------------------------------------

TEST(EditOperations, SetNodeElevationFitsCubicThroughNodes) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto raise = roadmaker::edit::set_node_elevation(network, road, 1, 4.0);
  expect_command_round_trip(network, *raise);

  ASSERT_TRUE(raise->apply(network).has_value());
  const roadmaker::Road& value = *network.road(road);
  ASSERT_EQ(value.elevation.size(), 2U); // two cubic segments through 3 nodes

  // The fitted profile reproduces the node heights (0, 4, 0) exactly at the
  // node stations, and its records ascend in s
  // (asam.net:xodr:1.4.0:road.elevation.elem_asc_order).
  const auto stations = roadmaker::edit::waypoint_stations(value);
  ASSERT_TRUE(stations.has_value());
  ASSERT_EQ(stations->size(), 3U);
  const std::array<double, 3> expected{0.0, 4.0, 0.0};
  for (std::size_t i = 0; i < stations->size(); ++i) {
    EXPECT_NEAR(roadmaker::eval_profile(value.elevation, (*stations)[i]),
                expected[i],
                roadmaker::tol::kLength);
  }
  EXPECT_TRUE(std::ranges::is_sorted(
      value.elevation,
      [](const roadmaker::Poly3& a, const roadmaker::Poly3& b) { return a.s < b.s; }));

  // Lowering the node back to zero drops the profile entirely (the OpenDRIVE
  // default elevation is zero, so a flat road carries no <elevation>).
  auto lower = roadmaker::edit::set_node_elevation(network, road, 1, 0.0);
  ASSERT_TRUE(lower->apply(network).has_value());
  EXPECT_TRUE(network.road(road)->elevation.empty());

  // A stale road id is refused without touching the network.
  expect_command_rejected(network, roadmaker::edit::set_node_elevation(network, RoadId{}, 0, 1.0));
  // An out-of-range waypoint index is refused too.
  expect_command_rejected(network, roadmaker::edit::set_node_elevation(network, road, 99, 1.0));
}

// --- topology: create / delete / split roads --------------------------------------

TEST(EditOperations, CreateRoadRoundTripsWithStableIds) {
  RoadNetwork network;
  author_default(network, "1");

  auto command =
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 80.0, .y = 60.0}},
                                   LaneProfile::two_lane_default(),
                                   "Second");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId created = network.find_road("2"); // auto-assigned next free id
  ASSERT_TRUE(created.is_valid());
  EXPECT_TRUE(command->dirty().topology);
  ASSERT_EQ(command->dirty().roads.size(), 1U);
  EXPECT_EQ(command->dirty().roads[0], created);
}

TEST(EditOperations, CreateRoadRejectsDegenerateWaypoints) {
  RoadNetwork network;
  author_default(network, "1");
  expect_command_rejected(
      network,
      roadmaker::edit::create_road({Waypoint{.x = 1.0, .y = 1.0}, Waypoint{.x = 1.0, .y = 1.0}},
                                   LaneProfile::two_lane_default(),
                                   ""));
}

TEST(EditOperations, CreateRoadAutoNamesFromTheAssignedOdrId) {
  RoadNetwork network;
  author_default(network, "1");

  auto command =
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 80.0, .y = 60.0}},
                                   LaneProfile::two_lane_rural(),
                                   "");
  ASSERT_TRUE(command->apply(network).has_value());
  const RoadId created = network.find_road("2");
  ASSERT_TRUE(created.is_valid());
  EXPECT_EQ(network.road(created)->name, "Road 2");
}

// The Create Road tangent-snap chain (02 §2): locking the new road's start
// heading to the snapped road's continuation heading joins the two G1.
TEST(EditOperations, CreateRoadLockedStartHeadingChainsG1) {
  RoadNetwork network;
  const RoadId first = author_default(network, "1");
  const roadmaker::Road& source = *network.road(first);
  const auto end = source.plan_view.evaluate(source.plan_view.length());

  auto command = roadmaker::edit::create_road(
      {Waypoint{.x = end.x, .y = end.y}, Waypoint{.x = end.x + 70.0, .y = end.y - 25.0}},
      LaneProfile::two_lane_rural(),
      "Chained",
      {.start = end.hdg});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const RoadId chained = network.find_road("2");
  ASSERT_TRUE(chained.is_valid());
  const auto start = network.road(chained)->plan_view.evaluate(0.0);
  EXPECT_NEAR(start.x, end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(start.y, end.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(
      std::remainder(start.hdg - end.hdg, 2.0 * std::numbers::pi), 0.0, roadmaker::tol::kAngle);
}

TEST(EditOperations, DeleteRoadDetachesAndUndoResurrectsOriginalIds) {
  RoadNetwork network;
  const RoadId incoming = author_default(network, "1");
  const RoadId doomed = author_default(network, "2", 40.0);
  const RoadId neighbor = author_default(network, "3", 80.0);
  network.road(neighbor)->predecessor =
      roadmaker::RoadLink{.target = doomed, .contact = ContactPoint::End};

  const JunctionId junction = network.create_junction("100", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = doomed,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });

  auto command = roadmaker::edit::delete_road(network, doomed);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(doomed), nullptr);
  EXPECT_TRUE(network.junction(junction)->connections.empty());
  EXPECT_FALSE(network.road(neighbor)->predecessor.has_value());

  ASSERT_TRUE(command->revert(network).has_value());
  // The acceptance criterion: ids held elsewhere are valid again.
  ASSERT_EQ(network.junction(junction)->connections.size(), 1U);
  const JunctionConnection& connection = network.junction(junction)->connections[0];
  ASSERT_NE(network.road(connection.connecting_road), nullptr);
  EXPECT_EQ(network.road(connection.connecting_road)->odr_id, "2");
  ASSERT_TRUE(network.road(neighbor)->predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(neighbor)->predecessor->target), doomed);
}

/// §7 closure setup shared by the two closure tests: `incoming` feeds
/// `connecting` (a proper junction-internal road) which exits onto
/// `outgoing`.
struct JunctionClosureFixture {
  RoadId incoming;
  RoadId connecting;
  RoadId outgoing;
  JunctionId junction;
};

JunctionClosureFixture make_junction_closure(RoadNetwork& network) {
  JunctionClosureFixture fixture;
  fixture.incoming = author_default(network, "1");
  fixture.connecting = author_default(network, "2", 40.0);
  fixture.outgoing = author_default(network, "3", 80.0);
  fixture.junction = network.create_junction("100", "X");

  network.road(fixture.incoming)->successor =
      roadmaker::RoadLink{.target = fixture.junction, .contact = ContactPoint::Start};
  network.road(fixture.outgoing)->predecessor =
      roadmaker::RoadLink{.target = fixture.junction, .contact = ContactPoint::Start};
  roadmaker::Road& connecting = *network.road(fixture.connecting);
  connecting.junction = fixture.junction;
  connecting.predecessor =
      roadmaker::RoadLink{.target = fixture.incoming, .contact = ContactPoint::End};
  connecting.successor =
      roadmaker::RoadLink{.target = fixture.outgoing, .contact = ContactPoint::Start};
  network.junction(fixture.junction)
      ->connections.push_back(JunctionConnection{
          .incoming_road = fixture.incoming,
          .connecting_road = fixture.connecting,
          .contact_point = ContactPoint::Start,
          .lane_links = {{-1, -1}},
      });
  return fixture;
}

TEST(EditOperations, SetNodeElevationMarksTouchingJunctionDirty) {
  RoadNetwork network;
  const JunctionClosureFixture fixture = make_junction_closure(network);

  // Editing the grade of a junction-incoming road must re-blend the junction
  // surface (03 §4), so the touched junction rides along in the dirty set.
  auto command = roadmaker::edit::set_node_elevation(network, fixture.incoming, 2, 3.0);
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_NE(std::ranges::find(dirty.roads, fixture.incoming), dirty.roads.end());
  EXPECT_NE(std::ranges::find(dirty.junctions, fixture.junction), dirty.junctions.end());
}

TEST(EditOperations, DeleteIncomingRoadDeletesItsConnectingRoads) {
  RoadNetwork network;
  const JunctionClosureFixture fixture = make_junction_closure(network);

  auto command = roadmaker::edit::delete_road(network, fixture.incoming);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  // The connection referencing the incoming road is gone, and its connecting
  // road went with it — the §7 closure.
  EXPECT_EQ(network.road(fixture.incoming), nullptr);
  EXPECT_EQ(network.road(fixture.connecting), nullptr);
  ASSERT_NE(network.junction(fixture.junction), nullptr);
  EXPECT_TRUE(network.junction(fixture.junction)->connections.empty());
  // The outgoing road survives; its link into the surviving junction stays.
  ASSERT_NE(network.road(fixture.outgoing), nullptr);
  EXPECT_TRUE(network.road(fixture.outgoing)->predecessor.has_value());

  // Both doomed roads are in the dirty set, plus the junction they touched.
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(dirty.topology);
  EXPECT_NE(std::ranges::find(dirty.roads, fixture.connecting), dirty.roads.end());
  EXPECT_NE(std::ranges::find(dirty.junctions, fixture.junction), dirty.junctions.end());

  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.road(fixture.connecting), nullptr);
  EXPECT_EQ(network.road(fixture.connecting)->odr_id, "2");
  ASSERT_EQ(network.junction(fixture.junction)->connections.size(), 1U);
  EXPECT_EQ(network.junction(fixture.junction)->connections[0].connecting_road, fixture.connecting);
}

TEST(EditOperations, DeleteJunctionDeletesConnectingRoadsAndClearsLinks) {
  RoadNetwork network;
  const JunctionClosureFixture fixture = make_junction_closure(network);

  auto command = roadmaker::edit::delete_junction(network, fixture.junction);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  // The junction takes its connecting road along; the incoming and outgoing
  // roads survive with their links into the junction cleared.
  EXPECT_EQ(network.junction(fixture.junction), nullptr);
  EXPECT_EQ(network.road(fixture.connecting), nullptr);
  ASSERT_NE(network.road(fixture.incoming), nullptr);
  EXPECT_FALSE(network.road(fixture.incoming)->successor.has_value());
  ASSERT_NE(network.road(fixture.outgoing), nullptr);
  EXPECT_FALSE(network.road(fixture.outgoing)->predecessor.has_value());

  ASSERT_TRUE(command->revert(network).has_value());
  // Every removed object and link is back under its original id.
  ASSERT_NE(network.junction(fixture.junction), nullptr);
  ASSERT_NE(network.road(fixture.connecting), nullptr);
  EXPECT_EQ(network.road(fixture.connecting)->junction, fixture.junction);
  ASSERT_TRUE(network.road(fixture.incoming)->successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(fixture.incoming)->successor->target),
            fixture.junction);
}

TEST(EditOperations, SplitRoadPreservesGeometryAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;
  const double split_s = length * 0.4;
  const roadmaker::PathPoint at_split = network.road(road)->plan_view.evaluate(split_s);

  auto command = roadmaker::edit::split_road(network, road, split_s);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId tail = network.find_road("2");
  ASSERT_TRUE(tail.is_valid());

  const roadmaker::Road& head_road = *network.road(road);
  const roadmaker::Road& tail_road = *network.road(tail);
  EXPECT_NEAR(head_road.length, split_s, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(head_road.length + tail_road.length, length, roadmaker::tol::kRoundTripPosition);

  // The seam is G1: tail starts exactly where the head ends.
  const roadmaker::PathPoint tail_start = tail_road.plan_view.evaluate(0.0);
  EXPECT_NEAR(tail_start.x, at_split.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.y, at_split.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.hdg, at_split.hdg, roadmaker::tol::kRoundTripHeading);

  // Linked head->tail with identity lane links on the seam.
  ASSERT_TRUE(head_road.successor.has_value());
  EXPECT_EQ(std::get<RoadId>(head_road.successor->target), tail);
  ASSERT_TRUE(tail_road.predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(tail_road.predecessor->target), road);
  ASSERT_EQ(tail_road.sections.size(), 1U);
  EXPECT_EQ(network.lane_section(tail_road.sections[0])->lanes.size(),
            network.lane_section(head_road.sections[0])->lanes.size());
}

TEST(EditOperations, SplitRoadRejectsBadStationsAndJunctionRoads) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;

  expect_command_rejected(network, roadmaker::edit::split_road(network, road, 0.0));
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length));

  // Junction-LINKED ends are splittable since the hardening sprint (#92 —
  // the successor-side junction remaps onto the tail); only a junction's
  // CONNECTING road still refuses, as does a junction referencing the road
  // without any link on it (foreign data with no arm to say which end moved).
  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->junction = junction;
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length * 0.5));
  network.road(road)->junction = {};

  network.junction(junction)->connections.push_back(
      JunctionConnection{.incoming_road = road, .connecting_road = road});
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length * 0.5));
}

// --- junctions ---------------------------------------------------------------------

namespace {

/// Three straight two-lane arms whose ends meet near the origin — the golden
/// T-junction of 02 §6 (west, east and south arms, all contacting at End).
struct TJunction {
  RoadId west;
  RoadId east;
  RoadId south;
  std::array<RoadEnd, 3> ends;
};

TJunction make_t_junction(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  return TJunction{.west = west,
                   .east = east,
                   .south = south,
                   .ends = {RoadEnd{.road = west, .contact = ContactPoint::End},
                            RoadEnd{.road = east, .contact = ContactPoint::End},
                            RoadEnd{.road = south, .contact = ContactPoint::End}}};
}

/// Every driving lane on `road` is negative (right) on a road authored with
/// two_lane_default, so an End-contact arm's single incoming lane is -1.
int connections_with_incoming(const RoadNetwork& network, JunctionId junction, RoadId incoming) {
  int count = 0;
  for (const JunctionConnection& connection : network.junction(junction)->connections) {
    if (connection.incoming_road == incoming) {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST(EditOperations, CreateJunctionGeneratesTJunctionAndRoundTrips) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);

  auto command = roadmaker::edit::create_junction(network, t.ends);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  ASSERT_TRUE(network.junction(junction) != nullptr);

  // 3 two-way single-lane arms → 6 ordered turns → 6 connecting roads.
  const auto& connections = network.junction(junction)->connections;
  EXPECT_EQ(connections.size(), 6U);
  // Each arm is the incoming road for exactly two turns (to the other two).
  EXPECT_EQ(connections_with_incoming(network, junction, t.west), 2);
  EXPECT_EQ(connections_with_incoming(network, junction, t.east), 2);
  EXPECT_EQ(connections_with_incoming(network, junction, t.south), 2);

  // Arms are linked to the junction; arms record for regeneration.
  EXPECT_EQ(std::get<JunctionId>(network.road(t.west)->successor->target), junction);
  EXPECT_EQ(network.junction(junction)->arms.size(), 3U);

  for (const JunctionConnection& connection : connections) {
    // One laneLink per connection, incoming lane -1 → connecting lane -1.
    ASSERT_EQ(connection.lane_links.size(), 1U);
    EXPECT_EQ(connection.lane_links.front().first, -1);
    EXPECT_EQ(connection.lane_links.front().second, -1);
    EXPECT_EQ(connection.contact_point, ContactPoint::Start);

    const roadmaker::Road* connecting = network.road(connection.connecting_road);
    ASSERT_NE(connecting, nullptr);
    EXPECT_EQ(connecting->junction, junction);
    EXPECT_EQ(std::get<RoadId>(connecting->predecessor->target), connection.incoming_road);
    // The single connecting lane links incoming (-1) to outgoing (+1).
    const LaneSectionId section = connecting->sections.front();
    for (const LaneId lane_id : network.lane_section(section)->lanes) {
      const roadmaker::Lane& lane = *network.lane(lane_id);
      if (lane.odr_id == -1) {
        EXPECT_EQ(lane.type, LaneType::Driving);
        EXPECT_EQ(lane.predecessor, -1);
        EXPECT_EQ(lane.successor, 1);
        ASSERT_FALSE(lane.widths.empty());
        EXPECT_NEAR(lane.widths.front().a, 3.5, 1e-9); // source width
      }
    }
  }
}

// Regression for issue #89 (soak seed 1): the delete_road closure pruned
// junction CONNECTIONS referencing the doomed road but left its RoadEnd in
// the recorded arms — a dangling id that regeneration re-plans from and
// rm:arms persists into saved files. Arms are part of the closure.
TEST(EditOperations, DeleteRoadPrunesJunctionArmsAndUndoRestoresThem) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  ASSERT_EQ(network.junction(junction)->arms.size(), 3U);

  auto command = roadmaker::edit::delete_road(network, t.west);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const roadmaker::Junction& after = *network.junction(junction);
  EXPECT_EQ(after.arms.size(), 2U);
  for (const RoadEnd& arm : after.arms) {
    EXPECT_NE(arm.road, t.west);
    EXPECT_NE(network.road(arm.road), nullptr);
  }
  for (const JunctionConnection& connection : after.connections) {
    EXPECT_NE(connection.incoming_road, t.west);
    EXPECT_NE(network.road(connection.incoming_road), nullptr);
    EXPECT_NE(network.road(connection.connecting_road), nullptr);
  }

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.junction(junction)->arms.size(), 3U);
}

// Regression for issue #90 (soak round-trip invariant): a junction the
// delete_road closure leaves with ONE recorded arm must still round-trip
// byte-identically — the reader rejects rm:arms below 2 arms, so the writer
// must not emit a degenerate list only the first generation carries.
TEST(EditOperations, JunctionBelowTwoArmsRoundTripsByteIdentically) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  const std::array<RoadEnd, 2> two_ends = {t.ends[0], t.ends[1]};
  ASSERT_TRUE(roadmaker::edit::create_junction(network, two_ends)->apply(network).has_value());
  ASSERT_TRUE(roadmaker::edit::delete_road(network, t.west)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  ASSERT_EQ(network.junction(junction)->arms.size(), 1U);

  const auto first = roadmaker::write_xodr(network, "regression");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->find("rm:arms"), std::string::npos)
      << "a degenerate arm list must not be persisted";
  auto parsed = roadmaker::parse_xodr(*first, "<regression>");
  ASSERT_TRUE(parsed.has_value());
  const auto second = roadmaker::write_xodr(parsed->network, "regression");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
}

TEST(EditOperations, CreateJunctionOutputValidatesCleanly) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());

  using roadmaker::XodrVersion;
  for (const XodrVersion version : {XodrVersion::v1_8_1, XodrVersion::v1_9_0}) {
    const auto findings =
        roadmaker::validate_network(network, roadmaker::WriterOptions{.target_version = version});
    // No structural errors. A generated junction now closes its <boundary>
    // (every adjacent arm pair is bridged, and any gap is filled by an auxiliary
    // boundary road, #62), so the boundary-omitted warning
    // (junctions.boundary.close_gap_with_new_roads) no longer fires — the loop
    // guards that no unexpected diagnostic slipped in.
    EXPECT_EQ(roadmaker::count_errors(findings), 0U);
    for (const auto& finding : findings) {
      EXPECT_EQ(finding.rule_id, roadmaker::rules::kJunctionBoundaryCloseGap)
          << "version index " << static_cast<int>(version) << " unexpected diagnostic";
    }
  }
}

// ---- attach_t_junction (hardening sprint #92) --------------------------------

/// A 120 m straight main road along +x and a side road ending 10 m south of
/// its midpoint — the canonical tee.
struct TAttachFixture {
  RoadId main_road;
  RoadId side;
};

TAttachFixture make_t_attach(RoadNetwork& network) {
  return TAttachFixture{
      .main_road = author(network, {Waypoint{-60.0, 0.0}, Waypoint{60.0, 0.0}}, "1"),
      .side = author(network, {Waypoint{0.0, -50.0}, Waypoint{0.0, -10.0}}, "2"),
  };
}

TEST(EditOperations, AttachTJunctionBuildsThreeArmJunctionAndRoundTrips) {
  RoadNetwork network;
  const TAttachFixture t = make_t_attach(network);

  auto command = roadmaker::edit::attach_t_junction(
      network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 60.0);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  const roadmaker::Junction& built = *network.junction(junction);
  ASSERT_EQ(built.arms.size(), 3U);
  // 3 two-way single-lane arms -> 6 ordered turns, like the endpoint T.
  EXPECT_EQ(built.connections.size(), 6U);

  // The main road was shortened to [0, s-gap) and a tail road carries the
  // far half; the deleted middle stub is the junction area.
  const roadmaker::Road& head = *network.road(t.main_road);
  EXPECT_LT(head.length, 60.0);
  ASSERT_TRUE(head.successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(head.successor->target), junction);
  bool tail_found = false;
  for (const RoadEnd& arm : built.arms) {
    ASSERT_NE(network.road(arm.road), nullptr);
    if (arm.road != t.main_road && arm.road != t.side) {
      tail_found = true;
      const roadmaker::Road& tail = *network.road(arm.road);
      EXPECT_EQ(arm.contact, ContactPoint::Start);
      ASSERT_TRUE(tail.predecessor.has_value());
      EXPECT_EQ(std::get<JunctionId>(tail.predecessor->target), junction);
    }
  }
  EXPECT_TRUE(tail_found);
  // head + side + tail + 6 connecting roads.
  EXPECT_EQ(network.road_count(), 9U);
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(EditOperations, AttachTJunctionUndoRestoresTheExactPreSplitNetwork) {
  RoadNetwork network;
  const TAttachFixture t = make_t_attach(network);
  const auto before = roadmaker::write_xodr(network, "t");
  ASSERT_TRUE(before.has_value());

  auto command = roadmaker::edit::attach_t_junction(
      network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 60.0);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_TRUE(command->revert(network).has_value());

  const auto after = roadmaker::write_xodr(network, "t");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);

  // Redo resurrects the identical junction under the same ids.
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_TRUE(network.find_junction("1").is_valid());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(EditOperations, AttachTJunctionRejectsBadInput) {
  RoadNetwork network;
  const TAttachFixture t = make_t_attach(network);

  // Self-attach.
  EXPECT_FALSE(
      roadmaker::edit::attach_t_junction(
          network, RoadEnd{.road = t.main_road, .contact = ContactPoint::End}, t.main_road, 60.0)
          ->apply(network)
          .has_value());
  // Too close to the target's end for the junction area.
  EXPECT_FALSE(roadmaker::edit::attach_t_junction(
                   network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 2.0)
                   ->apply(network)
                   .has_value());
  // All rejections left the network untouched.
  EXPECT_EQ(network.road_count(), 2U);
  EXPECT_EQ(network.junction_count(), 0U);
}

TEST(EditOperations, AttachTJunctionRefusesAnOccupiedAttachSlot) {
  RoadNetwork network;
  const TAttachFixture t = make_t_attach(network);
  ASSERT_TRUE(roadmaker::edit::attach_t_junction(
                  network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 60.0)
                  ->apply(network)
                  .has_value());
  // The side road's End now links into junction 1 — a second tee from the
  // same end must refuse without touching the network.
  const auto snapshot = roadmaker::write_xodr(network, "t");
  EXPECT_FALSE(
      roadmaker::edit::attach_t_junction(
          network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 30.0)
          ->apply(network)
          .has_value());
  EXPECT_EQ(*roadmaker::write_xodr(network, "t"), *snapshot);
}

TEST(EditOperations, SecondTeeOnTheHeadHalfRemapsTheFirstJunction) {
  RoadNetwork network;
  const TAttachFixture t = make_t_attach(network);
  ASSERT_TRUE(roadmaker::edit::attach_t_junction(
                  network, RoadEnd{.road = t.side, .contact = ContactPoint::End}, t.main_road, 60.0)
                  ->apply(network)
                  .has_value());
  const JunctionId first = network.find_junction("1");
  ASSERT_TRUE(first.is_valid());

  // Tee a second side road into the HEAD half — its End links into junction
  // 1, so the split inside the second attach must remap junction 1's arm,
  // connections, and connecting-road links onto the new far piece (#92
  // lifts the M2 "no split near junctions" restriction for exactly this).
  const roadmaker::Road& head = *network.road(t.main_road);
  const double s2 = head.length / 2.0; // safely inside the head half
  const double head_x0 = -60.0;        // head starts where the main road did
  const RoadId side2 =
      author(network, {Waypoint{head_x0 + s2, -50.0}, Waypoint{head_x0 + s2, -10.0}}, "50");
  auto second = roadmaker::edit::attach_t_junction(
      network, RoadEnd{.road = side2, .contact = ContactPoint::End}, t.main_road, s2);
  ASSERT_TRUE(second->apply(network).has_value());

  // Junction 1 must no longer reference the (re-split) head road anywhere.
  const roadmaker::Junction& first_junction = *network.junction(first);
  for (const RoadEnd& arm : first_junction.arms) {
    EXPECT_NE(arm.road, t.main_road);
    ASSERT_NE(network.road(arm.road), nullptr);
  }
  for (const JunctionConnection& connection : first_junction.connections) {
    EXPECT_NE(connection.incoming_road, t.main_road);
    ASSERT_NE(network.road(connection.incoming_road), nullptr);
    ASSERT_NE(network.road(connection.connecting_road), nullptr);
  }
  EXPECT_EQ(network.junction_count(), 2U);
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);

  // The whole double-tee undoes back to the single-tee network exactly.
  const auto with_one = [&] {
    RoadNetwork fresh;
    const TAttachFixture f = make_t_attach(fresh);
    (void)f;
    return fresh;
  };
  (void)with_one;
  ASSERT_TRUE(second->revert(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

// ---- elevation profile command (hardening sprint WS-C) -----------------------

TEST(EditOperations, SetElevationProfileHonorsGradesAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");

  auto command = roadmaker::edit::set_elevation_profile(
      network,
      road,
      {roadmaker::edit::ElevationPoint{.s = 0.0, .z = 0.0, .grade = 0.0},
       roadmaker::edit::ElevationPoint{.s = 60.0, .z = 5.0},
       roadmaker::edit::ElevationPoint{.s = 120.0, .z = 0.0, .grade = 0.0}});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const roadmaker::Road& after = *network.road(road);
  ASSERT_FALSE(after.elevation.empty());
  EXPECT_NEAR(roadmaker::eval_profile(after.elevation, 60.0), 5.0, 1e-9);
  EXPECT_NEAR(after.elevation.front().eval_derivative(0.0), 0.0, 1e-9); // locked grade
  EXPECT_NEAR(after.elevation.back().eval_derivative(120.0), 0.0, 1e-9);

  // Read-back for the profile panel: nodes at every record start + road end.
  const auto points = roadmaker::edit::elevation_profile_points(after);
  ASSERT_GE(points.size(), 3U);
  EXPECT_NEAR(points.front().z, 0.0, 1e-9);
  EXPECT_NEAR(points.back().s, 120.0, 1e-6);

  // Flattening writes NO profile (the OpenDRIVE default).
  ASSERT_TRUE(roadmaker::edit::set_elevation_profile(
                  network, road, {roadmaker::edit::ElevationPoint{.s = 0.0, .z = 0.0}})
                  ->apply(network)
                  .has_value());
  EXPECT_TRUE(network.road(road)->elevation.empty());
}

TEST(EditOperations, SetElevationProfileRejectsBadNodes) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{100.0, 0.0}}, "1");
  // Outside the road / duplicate stations / empty.
  EXPECT_FALSE(roadmaker::edit::set_elevation_profile(
                   network, road, {roadmaker::edit::ElevationPoint{.s = 150.0, .z = 1.0}})
                   ->apply(network)
                   .has_value());
  EXPECT_FALSE(
      roadmaker::edit::set_elevation_profile(network,
                                             road,
                                             {roadmaker::edit::ElevationPoint{.s = 10.0, .z = 1.0},
                                              roadmaker::edit::ElevationPoint{.s = 10.0, .z = 2.0}})
          ->apply(network)
          .has_value());
  EXPECT_FALSE(
      roadmaker::edit::set_elevation_profile(network, road, {})->apply(network).has_value());
  EXPECT_TRUE(network.road(road)->elevation.empty());
}

TEST(EditOperations, SteepGradeYieldsAnAdvisoryWarning) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{100.0, 0.0}}, "1");
  ASSERT_TRUE(roadmaker::edit::set_elevation_profile(
                  network,
                  road,
                  {roadmaker::edit::ElevationPoint{.s = 0.0, .z = 0.0, .grade = 0.0},
                   roadmaker::edit::ElevationPoint{.s = 50.0, .z = 10.0}, // ~20 % avg
                   roadmaker::edit::ElevationPoint{.s = 100.0, .z = 0.0, .grade = 0.0}})
                  ->apply(network)
                  .has_value());
  bool warned = false;
  for (const auto& finding : roadmaker::validate_network(network)) {
    if (finding.severity == roadmaker::Severity::Warning &&
        finding.message.find("grade") != std::string::npos) {
      warned = true;
      EXPECT_TRUE(finding.rule_id.empty()) << "advisory must not spoof an ASAM rule id";
    }
  }
  EXPECT_TRUE(warned);

  // Configurable, and non-positive disables.
  roadmaker::WriterOptions permissive;
  permissive.max_grade_warning = 0.5;
  for (const auto& finding : roadmaker::validate_network(network, permissive)) {
    EXPECT_EQ(finding.message.find("grade"), std::string::npos);
  }
}

// ---- interactive fit loop guard (hardening sprint #93, maintainer CRASH-1) ---

TEST(EditOperations, RunawayLoopFitsAreRefusedAtTheCommandLayer) {
  // A sharp turn-back makes the G1 spline balloon (~5.4x the waypoint span
  // here); unbounded, mesh/marking work grows until the editor dies.
  const std::vector<Waypoint> turn_back = {
      Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 95.0, .y = 3.0}};
  RoadNetwork network;
  EXPECT_FALSE(roadmaker::edit::create_road(turn_back, LaneProfile::two_lane_default(), "")
                   ->apply(network)
                   .has_value());
  EXPECT_EQ(network.road_count(), 0U);

  // Same guard on the waypoint-edit re-fit: dragging the last node of a
  // straight road back onto its middle must refuse, not balloon.
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{100.0, 0.0}}, "1");
  ASSERT_TRUE(roadmaker::edit::insert_waypoint(network, road, 2, Waypoint{200.0, 0.0})
                  ->apply(network)
                  .has_value());
  const auto snapshot = roadmaker::write_xodr(network, "guard");
  EXPECT_FALSE(roadmaker::edit::move_waypoint(network, road, 2, Waypoint{95.0, 3.0})
                   ->apply(network)
                   .has_value());
  EXPECT_EQ(*roadmaker::write_xodr(network, "guard"), *snapshot);
}

TEST(EditOperations, RegenerateJunctionIsByteEqualWhenNothingChanged) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  const std::string before = snapshot_xodr(network);

  auto regen = roadmaker::edit::regenerate_junction(network, junction);
  ASSERT_TRUE(regen->apply(network).has_value());
  EXPECT_EQ(before, snapshot_xodr(network)); // deterministic re-run reproduces the document

  // And it must undo cleanly. expect_command_round_trip cannot be used here —
  // it asserts a command changes something — but reverting a no-op is exactly
  // where the P2 creator restructure could have corrupted the junction while
  // still reproducing the document on apply.
  ASSERT_TRUE(regen->revert(network).has_value());
  EXPECT_EQ(before, snapshot_xodr(network));
}

// --- regeneration: turn-set changes (P2 #263) -------------------------------

LaneId lane_with_odr_id(const RoadNetwork& network, RoadId road, int odr_id) {
  for (const LaneId lane_id : network.lane_section(network.road(road)->sections.front())->lanes) {
    if (network.lane(lane_id)->odr_id == odr_id) {
      return lane_id;
    }
  }
  return LaneId{};
}

void retype_lane(RoadNetwork& network, RoadId road, int odr_id, roadmaker::LaneType type) {
  const LaneId lane = lane_with_odr_id(network, road, odr_id);
  ASSERT_TRUE(lane.is_valid());
  ASSERT_TRUE(roadmaker::edit::set_lane_type(network, lane, type)->apply(network).has_value());
}

void add_outermost_lane(RoadNetwork& network, RoadId road, int side) {
  ASSERT_TRUE(roadmaker::edit::add_lane(
                  network, network.road(road)->sections.front(), side, roadmaker::LaneType::Driving)
                  ->apply(network)
                  .has_value());
}

/// Adds one turn to the west→east movement, and only that one.
///
/// plan_junction pairs `min(incoming, outgoing)` lanes per ordered arm pair, so
/// widening one arm alone changes nothing — the other side of the movement
/// still bottlenecks it at one lane. On an End-contact arm the incoming lanes
/// are the negative ones and the outgoing lanes the positive ones, so west
/// gains a driving lane on the right and east one on the left.
void widen_west_to_east(RoadNetwork& network, const TJunction& t) {
  add_outermost_lane(network, t.west, -1);
  add_outermost_lane(network, t.east, 1);
}

TEST(EditOperations, RegenerateJunctionGrowsTheTurnSetWhenAnArmGainsADrivingLane) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  const std::size_t before = network.junction(junction)->connections.size();
  ASSERT_EQ(before, 6U); // 3 two-way single-lane arms: every ordered pair

  widen_west_to_east(network, t);

  // The case that used to be refused outright ("delete and recreate the
  // junction"), which is what GW-2 step 12's turn lane needs.
  auto regen = roadmaker::edit::regenerate_junction(network, junction);
  expect_command_round_trip(network, *regen);
  ASSERT_TRUE(regen->apply(network).has_value());

  // Exactly one turn appears: west→east gains a second lane pairing, while
  // west→south still bottlenecks on south's single outgoing lane.
  EXPECT_EQ(network.junction(junction)->connections.size(), before + 1);
  EXPECT_EQ(connections_with_incoming(network, junction, t.west), 3);

  // The junction is not merely bigger — it is welded and exportable.
  const auto welds = roadmaker::edit::verify_junction_welds(network, junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
  const auto written = roadmaker::write_xodr(network, "grown");
  ASSERT_TRUE(written.has_value());
  const auto reloaded = roadmaker::parse_xodr(*written, "grown");
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(roadmaker::count_errors(reloaded->diagnostics), 0U);
}

TEST(EditOperations, RegenerateJunctionShrinksTheTurnSetAndUndoResurrectsTheExactIds) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  widen_west_to_east(network, t);
  ASSERT_TRUE(roadmaker::edit::regenerate_junction(network, junction)->apply(network).has_value());

  const std::size_t grown = network.junction(junction)->connections.size();
  std::vector<RoadId> ids_before;
  for (const JunctionConnection& connection : network.junction(junction)->connections) {
    ids_before.push_back(connection.connecting_road);
  }

  // Take the west lane back out of service: the turn it carried has to go, and
  // its connecting road with it.
  retype_lane(network, t.west, -3, roadmaker::LaneType::Shoulder);

  auto regen = roadmaker::edit::regenerate_junction(network, junction);
  const std::string before_regen = snapshot_xodr(network);
  ASSERT_TRUE(regen->apply(network).has_value());
  EXPECT_LT(network.junction(junction)->connections.size(), grown);

  // A connecting road actually went away — not merely dropped from the table
  // but erased from the arena. Without this a regen that unlinks the road but
  // leaks it still passes the count and undo checks.
  const auto still_alive =
      std::ranges::count_if(ids_before, [&](RoadId id) { return network.road(id) != nullptr; });
  EXPECT_LT(static_cast<std::size_t>(still_alive), ids_before.size())
      << "a dropped connecting road must be erased, not orphaned";

  // Undo restores the table AND resurrects the erased connecting roads under
  // their original ids — erase_exact reserves the slot precisely so this holds.
  ASSERT_TRUE(regen->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before_regen);
  ASSERT_EQ(network.junction(junction)->connections.size(), grown);
  for (std::size_t i = 0; i < ids_before.size(); ++i) {
    EXPECT_EQ(network.junction(junction)->connections[i].connecting_road, ids_before[i]);
    EXPECT_NE(network.road(ids_before[i]), nullptr) << "original connecting id resurrected";
  }
}

/// The connecting road serving the `from`→`to` movement, or an invalid id.
/// The outgoing road is not stored on the connection — it lives only on the
/// connecting road's successor link.
RoadId
connecting_road_for(const RoadNetwork& network, JunctionId junction, RoadId from, RoadId to) {
  for (const JunctionConnection& connection : network.junction(junction)->connections) {
    if (connection.incoming_road != from) {
      continue;
    }
    const roadmaker::Road* road = network.road(connection.connecting_road);
    if (road == nullptr || !road->successor.has_value()) {
      continue;
    }
    if (const RoadId* target = std::get_if<RoadId>(&road->successor->target);
        target != nullptr && *target == to) {
      return connection.connecting_road;
    }
  }
  return RoadId{};
}

TEST(EditOperations, RegenerateJunctionKeepsTheIdsOfTurnsAGrowthDoesNotTouch) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  // east→south uses east's INCOMING lanes and south's OUTGOING lanes; south→west
  // uses south's incoming and west's outgoing. widen_west_to_east touches
  // neither set, so keyed matching must reuse both connecting roads rather than
  // churn every id whenever the turn set changes at all. (Movements arriving at
  // east DO legitimately re-target: a new outgoing lane changes which lane the
  // discipline rule picks.)
  const RoadId east_south = connecting_road_for(network, junction, t.east, t.south);
  const RoadId south_west = connecting_road_for(network, junction, t.south, t.west);
  ASSERT_TRUE(east_south.is_valid());
  ASSERT_TRUE(south_west.is_valid());

  widen_west_to_east(network, t);
  ASSERT_TRUE(roadmaker::edit::regenerate_junction(network, junction)->apply(network).has_value());

  EXPECT_EQ(connecting_road_for(network, junction, t.east, t.south), east_south);
  EXPECT_EQ(connecting_road_for(network, junction, t.south, t.west), south_west);
  EXPECT_NE(network.road(east_south), nullptr);
  EXPECT_NE(network.road(south_west), nullptr);
}

TEST(EditOperations, RegenerateJunctionInPlaceOnlyStillRefusesAChangedTurnSet) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  widen_west_to_east(network, t);

  // The per-frame preview path (move_waypoint_following_junctions) asks for
  // this: creating connecting roads there would reserve arena slots on every
  // discarded drag frame.
  expect_command_rejected(network,
                          roadmaker::edit::regenerate_junction(
                              network, junction, {}, roadmaker::edit::TurnSetPolicy::InPlaceOnly));
}

TEST(EditOperations, AddLaneNamesTheJunctionsItsRoadFeeds) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  // Without this the editor's regeneration loop iterates an empty list, so the
  // junction stays stale no matter what the loop's skip condition says.
  auto add = roadmaker::edit::add_lane(
      network, network.road(t.west)->sections.front(), -1, roadmaker::LaneType::Driving);
  const roadmaker::edit::DirtySet dirty = add->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions[0], junction);
  EXPECT_FALSE(dirty.junctions_are_current) << "add_lane does not regenerate; the editor does";
}

TEST(EditOperations, InsertLaneRemapsTheJunctionLaneLinksThatNamedTheShiftedLane) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  // Every generated connection off an End-contact arm links its incoming lane
  // -1. Inserting a lane at -1 pushes that lane to -2, so the lane_links naming
  // it must follow — otherwise, even before the editor regenerates, the
  // junction references a lane that has moved.
  const auto west_links_name = [&](int odr) {
    for (const JunctionConnection& connection : network.junction(junction)->connections) {
      if (connection.incoming_road == t.west) {
        for (const auto& [from, to] : connection.lane_links) {
          if (from == odr) {
            return true;
          }
        }
      }
    }
    return false;
  };
  ASSERT_TRUE(west_links_name(-1));

  auto insert = roadmaker::edit::insert_lane(
      network, network.road(t.west)->sections.front(), -1, roadmaker::LaneType::Driving);
  expect_command_round_trip(network, *insert);
  ASSERT_TRUE(insert->apply(network).has_value());

  EXPECT_TRUE(west_links_name(-2)) << "the lane_link followed the renumbering";
  EXPECT_FALSE(west_links_name(-1)) << "nothing still names the old id (now the new lane)";
}

TEST(EditOperations, RegenerateJunctionTracksMovedIncomingEnd) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  // Move the west arm's junction end; the connecting roads must follow it.
  auto move = roadmaker::edit::move_waypoint(network, t.west, 1, Waypoint{.x = -8.0, .y = -1.0});
  ASSERT_TRUE(move->apply(network).has_value());
  ASSERT_TRUE(!roadmaker::junctions_touching(network, t.west).empty());

  auto regen = roadmaker::edit::regenerate_junction(network, junction);
  expect_command_round_trip(network, *regen);
  ASSERT_TRUE(regen->apply(network).has_value());

  for (const JunctionConnection& connection : network.junction(junction)->connections) {
    if (connection.incoming_road != t.west) {
      continue;
    }
    const roadmaker::Road* connecting = network.road(connection.connecting_road);
    const auto start = connecting->plan_view.evaluate(0.0);
    EXPECT_NEAR(start.x, -8.0, 1e-6);
    EXPECT_NEAR(start.y, -1.0, 1e-6);
  }
}

TEST(EditOperations, RegenerateJunctionRejectsForeignJunction) {
  RoadNetwork network;
  const JunctionId junction = network.create_junction("100", "X"); // no recorded arms
  expect_command_rejected(network, roadmaker::edit::regenerate_junction(network, junction));
}

TEST(EditOperations, CreateJunctionRejectsBadEnds) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);

  const std::array<RoadEnd, 1> too_few{RoadEnd{.road = a, .contact = ContactPoint::End}};
  expect_command_rejected(network, roadmaker::edit::create_junction(network, too_few));

  const std::array<RoadEnd, 2> duplicate{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = a, .contact = ContactPoint::End},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, duplicate));

  network.road(b)->predecessor = roadmaker::RoadLink{.target = a, .contact = ContactPoint::End};
  const std::array<RoadEnd, 2> occupied{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, occupied));

  // Ends farther apart than the 50 m limit are a hard error (a End (120,0),
  // b Start (0,40) are ~126 m apart).
  const std::array<RoadEnd, 2> too_far{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  network.road(b)->predecessor.reset();
  expect_command_rejected(network, roadmaker::edit::create_junction(network, too_far));
}

TEST(EditOperations, PreviewJunctionReportsCountAndDroppedTurns) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  const auto preview = roadmaker::edit::preview_junction(network, t.ends);
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->connection_count, 6);
  EXPECT_TRUE(preview->dropped_turns.empty());

  // A tight loop budget (fit must be shorter than 0.5× the end distance) is
  // unsatisfiable for every turn, so all six are dropped with a note.
  const auto dropped = roadmaker::edit::preview_junction(
      network, t.ends, roadmaker::edit::JunctionGenOptions{.max_loop_factor = 0.5});
  ASSERT_TRUE(dropped.has_value());
  EXPECT_EQ(dropped->connection_count, 0);
  EXPECT_EQ(dropped->dropped_turns.size(), 6U);
}

TEST(EditOperations, DeleteJunctionDetachesRoadsAndRoundTrips) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(roadmaker::edit::create_junction(network, t.ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  auto command = roadmaker::edit::delete_junction(network, junction);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.junction(junction), nullptr);
  EXPECT_FALSE(network.road(t.west)->successor.has_value());
  EXPECT_FALSE(network.road(t.east)->successor.has_value());
  EXPECT_FALSE(network.road(t.south)->successor.has_value());
  // The six connecting roads went with the junction (only the 3 arms remain).
  EXPECT_EQ(network.road_count(), 3U);
}

// --- lanes: add / remove --------------------------------------------------------

TEST(EditOperations, AddLaneAppendsOutermostAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const std::size_t lanes_before = network.lane_section(section)->lanes.size();

  auto command = roadmaker::edit::add_lane(network, section, -1, LaneType::Biking);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.lane_section(section)->lanes.size(), lanes_before + 1);
  const LaneId added = network.lane_section(section)->lanes.back(); // outermost right
  EXPECT_EQ(network.lane(added)->odr_id, -3);                       // default profile has -1, -2
  EXPECT_EQ(network.lane(added)->type, LaneType::Biking);
  // Width copied from the previous outermost (shoulder, 1.0 m).
  ASSERT_FALSE(network.lane(added)->widths.empty());
  EXPECT_NEAR(network.lane(added)->widths[0].a, 1.0, 1e-12);
}

TEST(EditOperations, RemoveLaneOutermostOnlyAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back(); // -2 shoulder
  const LaneId inner_right = *std::prev(network.lane_section(section)->lanes.end(), 2); // -1

  expect_command_rejected(network, roadmaker::edit::remove_lane(network, inner_right));

  auto command = roadmaker::edit::remove_lane(network, outer_right);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.lane(outer_right), nullptr);
  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.lane(outer_right), nullptr); // original id resurrected
  EXPECT_EQ(network.lane(outer_right)->type, LaneType::Shoulder);
}

/// The issue #14 integrity criterion: junction lane_links referencing the
/// removed lane (on either side of a pair) are dropped with the lane and
/// restored exactly on undo.
TEST(EditOperations, RemoveLaneDropsJunctionLaneLinksAndRestoresThem) {
  RoadNetwork network;
  const RoadId incoming = author_default(network, "1");
  const RoadId connecting = author_default(network, "2", 40.0);
  const JunctionId junction = network.create_junction("100", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = connecting,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}, {-2, -2}},
  });

  const auto outer_right = [&](RoadId road_id) {
    const LaneSectionId section = network.road(road_id)->sections[0];
    return network.lane_section(section)->lanes.back(); // -2 shoulder
  };

  // Removing the INCOMING road's -2 drops the pair by its first element.
  auto from_incoming = roadmaker::edit::remove_lane(network, outer_right(incoming));
  expect_command_round_trip(network, *from_incoming);
  ASSERT_TRUE(from_incoming->apply(network).has_value());
  {
    const auto& links = network.junction(junction)->connections[0].lane_links;
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(links[0], (std::pair<int, int>{-1, -1}));
  }
  const roadmaker::edit::DirtySet dirty = from_incoming->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions[0], junction);
  ASSERT_TRUE(from_incoming->revert(network).has_value());
  EXPECT_EQ(network.junction(junction)->connections[0].lane_links.size(), 2U);

  // Removing the CONNECTING road's -2 drops the pair by its second element.
  auto from_connecting = roadmaker::edit::remove_lane(network, outer_right(connecting));
  ASSERT_TRUE(from_connecting->apply(network).has_value());
  {
    const auto& links = network.junction(junction)->connections[0].lane_links;
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(links[0], (std::pair<int, int>{-1, -1}));
  }
  ASSERT_TRUE(from_connecting->revert(network).has_value());
  EXPECT_EQ(network.junction(junction)->connections[0].lane_links.size(), 2U);
}

/// Spec 02 §4: multiple <roadMark> records per lane stay supported in data —
/// the M2 edit touches the first (sOffset 0) record only, and the surviving
/// tail keeps ascending sOffset order (…road_mark.elem_asc_order).
TEST(EditOperations, SetRoadMarkEditsTheFirstRecordOnly) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();
  network.lane(outer_right)->road_marks = {
      RoadMark{.s_offset = 0.0, .type = RoadMarkType::Broken, .width = 0.12},
      RoadMark{.s_offset = 40.0, .type = RoadMarkType::Solid, .width = 0.12},
  };

  auto command = roadmaker::edit::set_road_mark(
      network, outer_right, RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid, .width = 0.25});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const std::vector<RoadMark>& marks = network.lane(outer_right)->road_marks;
  ASSERT_EQ(marks.size(), 2U);
  EXPECT_EQ(marks[0].type, RoadMarkType::Solid);
  EXPECT_NEAR(marks[0].width, 0.25, 1e-12);
  EXPECT_EQ(marks[1].type, RoadMarkType::Solid); // tail untouched
  EXPECT_NEAR(marks[1].s_offset, 40.0, 1e-12);

  // An sOffset at or past the next record would break ascending order.
  expect_command_rejected(
      network,
      roadmaker::edit::set_road_mark(
          network, outer_right, RoadMark{.s_offset = 40.0, .type = RoadMarkType::None}));
}

// --- integration with the stack ---------------------------------------------------

TEST(EditOperations, EditStackDrivesTopologyCommands) {
  RoadNetwork network;
  author_default(network, "1");
  const std::string pristine = snapshot_xodr(network);

  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack
                  .push(network,
                        roadmaker::edit::create_road(
                            {Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 70.0, .y = 55.0}},
                            LaneProfile::two_lane_default(),
                            "Branch"))
                  .has_value());
  const RoadId created = network.find_road("2");
  ASSERT_TRUE(created.is_valid());
  ASSERT_TRUE(
      stack.push(network, roadmaker::edit::rename_road(network, created, "Renamed")).has_value());
  const std::string edited = snapshot_xodr(network);

  ASSERT_TRUE(stack.undo(network).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());
  expect_network_matches(network, pristine);

  ASSERT_TRUE(stack.redo(network).has_value());
  ASSERT_TRUE(stack.redo(network).has_value());
  expect_network_matches(network, edited);
  EXPECT_EQ(network.road(created)->name, "Renamed"); // same generational id after redo
}

// --- extend_road: keep drawing off a road's END -----------------------------

namespace {

/// A point `distance` m ahead of the road's END along its end tangent — always
/// reachable by a forward clothoid (nearly straight).
Waypoint ahead_of_end(const RoadNetwork& network, RoadId road, double distance) {
  const auto& plan = network.road(road)->plan_view;
  const roadmaker::PathPoint end = plan.evaluate(plan.length());
  return Waypoint{.x = end.x + (distance * std::cos(end.hdg)),
                  .y = end.y + (distance * std::sin(end.hdg))};
}

/// A point `distance` m BEHIND the road's START, opposite the start tangent —
/// the reachable side for a backward (START-contact) extension.
Waypoint behind_start(const RoadNetwork& network, RoadId road, double distance) {
  const auto& plan = network.road(road)->plan_view;
  const roadmaker::PathPoint start = plan.evaluate(0.0);
  return Waypoint{.x = start.x - (distance * std::cos(start.hdg)),
                  .y = start.y - (distance * std::sin(start.hdg))};
}

} // namespace

TEST(EditOperations, ExtendRoadHasCurvatureContinuityAtJoin) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1"); // an S-bend: curved at the end
  const double join_s = network.road(road)->plan_view.length();
  const Waypoint to = ahead_of_end(network, road, 40.0);

  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::End}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& plan = network.road(road)->plan_view;
  constexpr double kEps = 1e-3;
  const roadmaker::PathPoint before = plan.evaluate(join_s - kEps);
  const roadmaker::PathPoint after = plan.evaluate(join_s + kEps);
  EXPECT_LT(std::abs(after.curvature - before.curvature), roadmaker::tol::kWeldCurvature);
  EXPECT_LT(std::abs(std::remainder(after.hdg - before.hdg, 2.0 * std::numbers::pi)),
            roadmaker::tol::kWeldHeading);
}

TEST(EditOperations, ExtendRoadMatchesGrade) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double join_s = network.road(road)->plan_view.length();
  // Give the road a sloped profile so the end grade is non-zero.
  {
    std::vector<roadmaker::edit::ElevationPoint> points{{.s = 0.0, .z = 0.0},
                                                        {.s = join_s, .z = 6.0}};
    auto elev = roadmaker::edit::set_elevation_profile(network, road, std::move(points));
    ASSERT_TRUE(elev->apply(network).has_value());
  }
  const Waypoint to = ahead_of_end(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::End}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<roadmaker::Poly3>& elevation = network.road(road)->elevation;
  ASSERT_FALSE(elevation.empty());
  constexpr double kEps = 0.05;
  const double z_before = roadmaker::eval_profile(elevation, join_s - kEps);
  const double z_after = roadmaker::eval_profile(elevation, join_s + kEps);
  EXPECT_LT(std::abs(z_after - z_before), 1e-2); // z continuous at the join

  const double grade_before = (roadmaker::eval_profile(elevation, join_s - kEps) -
                               roadmaker::eval_profile(elevation, join_s - 3.0 * kEps)) /
                              (2.0 * kEps);
  const double grade_after = (roadmaker::eval_profile(elevation, join_s + 3.0 * kEps) -
                              roadmaker::eval_profile(elevation, join_s + kEps)) /
                             (2.0 * kEps);
  EXPECT_LT(std::abs(grade_after - grade_before), 1e-2); // dz/ds continuous
  EXPECT_GT(grade_after, 1e-3);                          // the slope really is carried past the end
}

TEST(EditOperations, ExtendRoadUndoIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const Waypoint to = ahead_of_end(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::End}, to);
  expect_command_round_trip(network, *command);
}

TEST(EditOperations, ExtendRoadRoundTripsThroughXodr) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const Waypoint to = ahead_of_end(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::End}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::string written = snapshot_xodr(network);
  auto reparsed = roadmaker::parse_xodr(written);
  ASSERT_TRUE(reparsed.has_value());
  const RoadId reread = reparsed->network.find_road("1");
  ASSERT_TRUE(reparsed->network.road(reread) != nullptr);
  roadmaker::test::expect_same_geometry(*network.road(road), *reparsed->network.road(reread));
}

// --- extend_road: keep drawing off a road's START --------------------------

TEST(EditOperations, ExtendRoadStartHasCurvatureContinuityAtJoin) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1"); // an S-bend: curved at both ends
  const double old_length = network.road(road)->plan_view.length();
  const Waypoint to = behind_start(network, road, 40.0);

  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& plan = network.road(road)->plan_view;
  const double join_s = plan.length() - old_length; // the old start now sits here
  EXPECT_GT(join_s, 1.0);
  constexpr double kEps = 1e-3;
  const roadmaker::PathPoint below = plan.evaluate(join_s - kEps);
  const roadmaker::PathPoint above = plan.evaluate(join_s + kEps);
  EXPECT_LT(std::abs(above.curvature - below.curvature), roadmaker::tol::kWeldCurvature);
  EXPECT_LT(std::abs(std::remainder(above.hdg - below.hdg, 2.0 * std::numbers::pi)),
            roadmaker::tol::kWeldHeading);
}

TEST(EditOperations, ExtendRoadStartMatchesGrade) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->plan_view.length();
  // A sloped profile so the start grade is non-zero and must carry backward.
  {
    std::vector<roadmaker::edit::ElevationPoint> points{{.s = 0.0, .z = 2.0},
                                                        {.s = old_length, .z = 8.0}};
    auto elev = roadmaker::edit::set_elevation_profile(network, road, std::move(points));
    ASSERT_TRUE(elev->apply(network).has_value());
  }
  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<roadmaker::Poly3>& elevation = network.road(road)->elevation;
  ASSERT_FALSE(elevation.empty());
  const double join_s = network.road(road)->plan_view.length() - old_length;
  constexpr double kEps = 0.05;
  const double z_before = roadmaker::eval_profile(elevation, join_s - kEps);
  const double z_after = roadmaker::eval_profile(elevation, join_s + kEps);
  EXPECT_LT(std::abs(z_after - z_before), 1e-2); // z continuous at the join

  const double grade_before = (roadmaker::eval_profile(elevation, join_s - kEps) -
                               roadmaker::eval_profile(elevation, join_s - 3.0 * kEps)) /
                              (2.0 * kEps);
  const double grade_after = (roadmaker::eval_profile(elevation, join_s + 3.0 * kEps) -
                              roadmaker::eval_profile(elevation, join_s + kEps)) /
                             (2.0 * kEps);
  EXPECT_LT(std::abs(grade_after - grade_before), 1e-2); // dz/ds continuous
  EXPECT_GT(grade_after, 1e-3);                          // the slope carries past the start
}

TEST(EditOperations, ExtendRoadStartRebasesEverything) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  // A second lane section so the interior-boundary shift is exercised.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 50.0)->apply(network).has_value());
  ASSERT_GE(network.road(road)->sections.size(), 2U);
  // A superelevation and a lane offset so the shift-invariance is exercised.
  {
    roadmaker::Road* mutable_road = network.road(road);
    mutable_road->superelevation = {roadmaker::Poly3{.s = 0.0, .a = 0.02, .b = 0.001},
                                    roadmaker::Poly3{.s = 20.0, .a = 0.04, .b = -0.001}};
    mutable_road->lane_offset = {roadmaker::Poly3{.s = 0.0, .a = 0.5, .b = 0.0}};
  }
  const double old_length = network.road(road)->plan_view.length();

  std::vector<double> old_s0;
  for (const LaneSectionId sid : network.road(road)->sections) {
    old_s0.push_back(network.lane_section(sid)->s0);
  }
  // Lane widths (section-local) must survive the shift byte-for-byte.
  const LaneSectionId first_section = network.road(road)->sections.front();
  std::vector<std::vector<roadmaker::Poly3>> old_widths;
  for (const LaneId lane : network.lane_section(first_section)->lanes) {
    old_widths.push_back(network.lane(lane)->widths);
  }
  const std::array<double, 3> probes{5.0, 20.0, old_length - 5.0};
  std::array<double, 3> super_before{};
  std::array<double, 3> offset_before{};
  for (std::size_t i = 0; i < probes.size(); ++i) {
    super_before[i] = roadmaker::eval_profile(network.road(road)->superelevation, probes[i]);
    offset_before[i] = roadmaker::eval_profile(network.road(road)->lane_offset, probes[i]);
  }
  const std::size_t old_waypoints = network.road(road)->authoring_waypoints->size();
  const Waypoint to = behind_start(network, road, 40.0);

  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const roadmaker::Road* result = network.road(road);
  const double l_ext = result->plan_view.length() - old_length;
  EXPECT_GT(l_ext, 1.0);

  // The first section stays anchored at s0 = 0 (it spans the new head); every
  // interior boundary slides forward by exactly L_ext.
  ASSERT_EQ(result->sections.size(), old_s0.size());
  EXPECT_NEAR(network.lane_section(result->sections.front())->s0, 0.0, 1e-9);
  for (std::size_t i = 1; i < result->sections.size(); ++i) {
    EXPECT_NEAR(network.lane_section(result->sections[i])->s0, old_s0[i] + l_ext, 1e-9);
  }
  // Lane widths unchanged.
  const std::vector<LaneId>& lanes = network.lane_section(result->sections.front())->lanes;
  ASSERT_EQ(lanes.size(), old_widths.size());
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    EXPECT_EQ(network.lane(lanes[i])->widths, old_widths[i]);
  }
  // Waypoint prepended; the authored point is now the first.
  ASSERT_EQ(result->authoring_waypoints->size(), old_waypoints + 1);
  EXPECT_EQ(result->authoring_waypoints->front(), to);
  // Superelevation / lane_offset VALUES at the old stations survive at s + L_ext.
  for (std::size_t i = 0; i < probes.size(); ++i) {
    EXPECT_NEAR(
        roadmaker::eval_profile(result->superelevation, probes[i] + l_ext), super_before[i], 1e-9);
    EXPECT_NEAR(
        roadmaker::eval_profile(result->lane_offset, probes[i] + l_ext), offset_before[i], 1e-9);
  }
}

TEST(EditOperations, ExtendRoadStartShiftsObjectsAndSignals) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->plan_view.length();
  const double place_s = 30.0;
  const roadmaker::ObjectId object = network.add_object(
      road, roadmaker::Object{.road = road, .odr_id = "o1", .s = place_s, .t = 1.5});
  const roadmaker::SignalId signal = network.add_signal(
      road, roadmaker::Signal{.road = road, .odr_id = "s1", .s = place_s, .t = -1.5});
  const roadmaker::PathPoint world_before = network.road(road)->plan_view.evaluate(place_s);

  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const double l_ext = network.road(road)->plan_view.length() - old_length;
  EXPECT_NEAR(network.object(object)->s, place_s + l_ext, 1e-9);
  EXPECT_NEAR(network.signal(signal)->s, place_s + l_ext, 1e-9);
  // The origin resolves to the SAME world point (s shifted with the geometry).
  const roadmaker::PathPoint world_after =
      network.road(road)->plan_view.evaluate(network.object(object)->s);
  EXPECT_NEAR(world_after.x, world_before.x, 1e-3);
  EXPECT_NEAR(world_after.y, world_before.y, 1e-3);
}

TEST(EditOperations, ExtendRoadStartShiftsObjectOutlineAndRepeat) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->plan_view.length();
  // A crosswalk-style object whose <outline> uses absolute-s <cornerRoad>
  // corners, plus a local-coordinate outline that must NOT move.
  roadmaker::Object outlined{.road = road, .odr_id = "o1", .s = 30.0, .t = 0.0};
  outlined.outlines.push_back(
      roadmaker::ObjectOutline{.road_coords = true,
                               .corners = {roadmaker::OutlineCorner{.a = 28.0, .b = -2.0},
                                           roadmaker::OutlineCorner{.a = 32.0, .b = -2.0},
                                           roadmaker::OutlineCorner{.a = 32.0, .b = 2.0},
                                           roadmaker::OutlineCorner{.a = 28.0, .b = 2.0}}});
  outlined.outlines.push_back(
      roadmaker::ObjectOutline{.road_coords = false,
                               .outer = false,
                               .corners = {roadmaker::OutlineCorner{.a = -1.0, .b = -1.0},
                                           roadmaker::OutlineCorner{.a = 1.0, .b = 1.0}}});
  const roadmaker::ObjectId outlined_id = network.add_object(road, std::move(outlined));
  // A tree line whose <repeat> starts at its own absolute s.
  roadmaker::Object repeated{.road = road, .odr_id = "o2", .s = 10.0, .t = 4.0};
  repeated.repeats.push_back(roadmaker::ObjectRepeat{.s = 12.0, .length = 40.0, .distance = 8.0});
  const roadmaker::ObjectId repeated_id = network.add_object(road, std::move(repeated));

  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const double l_ext = network.road(road)->plan_view.length() - old_length;
  EXPECT_GT(l_ext, 1.0);
  const roadmaker::Object* result = network.object(outlined_id);
  EXPECT_NEAR(result->s, 30.0 + l_ext, 1e-9);
  const std::array<double, 4> corner_s{28.0, 32.0, 32.0, 28.0};
  ASSERT_EQ(result->outlines[0].corners.size(), corner_s.size());
  for (std::size_t i = 0; i < corner_s.size(); ++i) {
    EXPECT_NEAR(result->outlines[0].corners[i].a, corner_s[i] + l_ext, 1e-9);
  }
  // The local (u/v) outline is relative to the origin and must be untouched.
  EXPECT_NEAR(result->outlines[1].corners[0].a, -1.0, 1e-12);
  EXPECT_NEAR(result->outlines[1].corners[1].a, 1.0, 1e-12);
  EXPECT_NEAR(network.object(repeated_id)->s, 10.0 + l_ext, 1e-9);
  EXPECT_NEAR(network.object(repeated_id)->repeats[0].s, 12.0 + l_ext, 1e-9);
  EXPECT_NEAR(network.object(repeated_id)->repeats[0].length, 40.0, 1e-12);
}

TEST(EditOperations, ExtendRoadEndLeavesObjectOutlineAndRepeatAlone) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  roadmaker::Object outlined{.road = road, .odr_id = "o1", .s = 30.0, .t = 0.0};
  outlined.outlines.push_back(
      roadmaker::ObjectOutline{.road_coords = true,
                               .corners = {roadmaker::OutlineCorner{.a = 28.0, .b = -2.0},
                                           roadmaker::OutlineCorner{.a = 32.0, .b = 2.0}}});
  outlined.repeats.push_back(roadmaker::ObjectRepeat{.s = 12.0, .length = 40.0, .distance = 8.0});
  const roadmaker::ObjectId object = network.add_object(road, std::move(outlined));

  const Waypoint to = ahead_of_end(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::End}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const roadmaker::Object* result = network.object(object);
  EXPECT_NEAR(result->s, 30.0, 1e-12);
  EXPECT_NEAR(result->outlines[0].corners[0].a, 28.0, 1e-12);
  EXPECT_NEAR(result->outlines[0].corners[1].a, 32.0, 1e-12);
  EXPECT_NEAR(result->repeats[0].s, 12.0, 1e-12);
}

TEST(EditOperations, ExtendRoadStartUndoIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  expect_command_round_trip(network, *command);
}

TEST(EditOperations, ExtendRoadStartWithOutlinedObjectUndoIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  roadmaker::Object outlined{.road = road, .odr_id = "o1", .s = 30.0, .t = 0.0};
  outlined.outlines.push_back(
      roadmaker::ObjectOutline{.road_coords = true,
                               .corners = {roadmaker::OutlineCorner{.a = 28.0, .b = -2.0},
                                           roadmaker::OutlineCorner{.a = 32.0, .b = 2.0}}});
  outlined.repeats.push_back(roadmaker::ObjectRepeat{.s = 12.0, .length = 40.0, .distance = 8.0});
  network.add_object(road, std::move(outlined));
  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  expect_command_round_trip(network, *command);
}

TEST(EditOperations, ExtendRoadStartRoundTripsThroughXodr) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::string written = snapshot_xodr(network);
  auto reparsed = roadmaker::parse_xodr(written);
  ASSERT_TRUE(reparsed.has_value());
  const RoadId reread = reparsed->network.find_road("1");
  ASSERT_TRUE(reparsed->network.road(reread) != nullptr);
  roadmaker::test::expect_same_geometry(*network.road(road), *reparsed->network.road(reread));
}

TEST(EditOperations, ExtendRoadStartRejectsLinkedStart) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  // Weld a second road onto the START so the predecessor link is occupied.
  const RoadId neighbor =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = -60.0, .y = -8.0}}, "2");
  (void)neighbor;
  network.road(road)->predecessor =
      roadmaker::RoadLink{.target = network.find_road("2"), .contact = ContactPoint::Start};

  const Waypoint to = behind_start(network, road, 40.0);
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  expect_command_rejected(network, std::move(command));
}

TEST(EditOperations, ExtendRoadStartRejectsPointBehind) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  // A target just AHEAD of the start along the road's own +s tangent is "behind"
  // the backward fit and cannot be reached (it loops past max_loop_factor).
  const roadmaker::PathPoint start = network.road(road)->plan_view.evaluate(0.0);
  const Waypoint to{.x = start.x + (5.0 * std::cos(start.hdg)),
                    .y = start.y + (5.0 * std::sin(start.hdg))};
  auto command = roadmaker::edit::extend_road(
      network, RoadEnd{.road = road, .contact = ContactPoint::Start}, to);
  expect_command_rejected(network, std::move(command));
}
