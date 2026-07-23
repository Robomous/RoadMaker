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

// Road styles (apply_road_style) — the p2-s8 kernel op (issue #219). A road
// style is a serializable cross-section applied to an EXISTING road: it
// replaces the lane profile and boundary marks and flattens the road to a
// single lane section, while preserving everything orthogonal to the cross
// section (reference-line geometry, elevation, superelevation, name, links, and
// placed objects/signals). These tests pin the preservation contract item by
// item and the byte-identical undo the command layer guarantees.

#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road_style.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::Lane;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSection;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::Object;
using roadmaker::Poly3;
using roadmaker::Road;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMarkColor;
using roadmaker::RoadMarkType;
using roadmaker::RoadNetwork;
using roadmaker::RoadStyle;
using roadmaker::Signal;
using roadmaker::StyleLane;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// A straight 100 m road on the default two-lane profile (lanes +1, -1, -2).
RoadId author_straight(RoadNetwork& network, const char* odr_id, double length = 100.0) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = length, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "Main St", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author_straight: " + road.error().message);
  }
  return *road;
}

/// A junction arm authored on the default two-lane profile.
RoadId author_arm(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, std::move(waypoints), LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author_arm: " + road.error().message);
  }
  return *road;
}

/// The §8 oracle: apply mutates, revert restores byte-identically, re-apply
/// reproduces the applied state, a final revert leaves it pristine.
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
  ASSERT_NE(command, nullptr);
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before); // a failed apply must not mutate
}

/// The lanes of a road's (first) section, keyed by odr id.
const Lane* lane_of(const RoadNetwork& network, RoadId road, int odr_id) {
  const LaneSection* section = network.lane_section(network.road(road)->sections.front());
  for (const LaneId id : section->lanes) {
    const Lane* lane = network.lane(id);
    if (lane->odr_id == odr_id) {
      return lane;
    }
  }
  return nullptr;
}

} // namespace

// --- preset contents (content-tested: a change here is a product change) ----

TEST(RoadStyle, UrbanTwoLaneContents) {
  const RoadStyle style = RoadStyle::urban_two_lane();
  ASSERT_EQ(style.left.size(), 2U);
  ASSERT_EQ(style.right.size(), 2U);

  // Inner same-direction lane: dashed white divider on its outer boundary.
  EXPECT_EQ(style.left[0].type, LaneType::Driving);
  EXPECT_DOUBLE_EQ(style.left[0].width.a, 3.5);
  ASSERT_TRUE(style.left[0].outer_mark.has_value());
  EXPECT_EQ(style.left[0].outer_mark->type, RoadMarkType::Broken);
  EXPECT_EQ(style.left[0].outer_mark->color, RoadMarkColor::White);

  // Outer lane: solid white edge line.
  ASSERT_TRUE(style.left[1].outer_mark.has_value());
  EXPECT_EQ(style.left[1].outer_mark->type, RoadMarkType::Solid);
  EXPECT_EQ(style.left[1].outer_mark->color, RoadMarkColor::White);

  ASSERT_TRUE(style.center_mark.has_value());
  EXPECT_EQ(style.center_mark->type, RoadMarkType::Solid);
  EXPECT_EQ(style.center_mark->color, RoadMarkColor::Yellow);
}

// --- structural ------------------------------------------------------------

TEST(RoadStyle, ApplyIsSingleSection) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::apply_road_style(network, road, RoadStyle::urban_two_lane());
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
}

TEST(RoadStyle, ApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::apply_road_style(network, road, RoadStyle::urban_two_lane());
  expect_command_round_trip(network, *command);
}

TEST(RoadStyle, ApplyReplacesTheLaneProfileAndMarks) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_TRUE(roadmaker::edit::apply_road_style(network, road, RoadStyle::urban_two_lane())
                  ->apply(network)
                  .has_value());

  // Urban two-lane: two 3.5 m driving lanes each side (odr +1,+2,-1,-2) + center.
  const LaneSection* section = network.lane_section(network.road(road)->sections.front());
  EXPECT_EQ(section->lanes.size(), 5U);
  for (const int odr : {1, 2, -1, -2}) {
    const Lane* lane = lane_of(network, road, odr);
    ASSERT_NE(lane, nullptr) << "missing lane " << odr;
    EXPECT_EQ(lane->type, LaneType::Driving);
    ASSERT_EQ(lane->widths.size(), 1U);
    EXPECT_DOUBLE_EQ(lane->widths.front().a, 3.5);
  }
  // Inner lane's outer boundary = the dashed white same-direction line.
  const Lane* inner = lane_of(network, road, 1);
  ASSERT_EQ(inner->road_marks.size(), 1U);
  EXPECT_EQ(inner->road_marks.front().type, RoadMarkType::Broken);
  EXPECT_EQ(inner->road_marks.front().color, RoadMarkColor::White);
  // Center line replaced (default was broken; urban is solid yellow).
  const Lane* center = lane_of(network, road, 0);
  ASSERT_EQ(center->road_marks.size(), 1U);
  EXPECT_EQ(center->road_marks.front().type, RoadMarkType::Solid);
  EXPECT_EQ(center->road_marks.front().color, RoadMarkColor::Yellow);
}

TEST(RoadStyle, ApplyFlattensAMultiSectionRoad) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // Split the road so it has several sections, then style it flat again.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 40.0)->apply(network).has_value());
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 70.0)->apply(network).has_value());
  ASSERT_EQ(network.road(road)->sections.size(), 3U);

  ASSERT_TRUE(roadmaker::edit::apply_road_style(network, road, RoadStyle::highway())
                  ->apply(network)
                  .has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
}

// --- preservation contract, item by item ------------------------------------

TEST(RoadStyle, ApplyPreservesGeometryElevationSuperelevationAndName) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "7");
  Road* mutable_road = network.road(road);
  const auto plan_view = mutable_road->plan_view;
  const double length = mutable_road->length;
  mutable_road->elevation = {Poly3{.s = 0.0, .a = 1.5, .b = 0.02}};
  mutable_road->superelevation = {Poly3{.s = 0.0, .a = 0.03}};
  const auto elevation = mutable_road->elevation;
  const auto superelevation = mutable_road->superelevation;

  ASSERT_TRUE(roadmaker::edit::apply_road_style(network, road, RoadStyle::urban_two_lane())
                  ->apply(network)
                  .has_value());

  const Road* styled = network.road(road);
  EXPECT_EQ(styled->name, "Main St");
  EXPECT_EQ(styled->odr_id, "7");
  EXPECT_DOUBLE_EQ(styled->length, length);
  EXPECT_EQ(styled->plan_view.length(), plan_view.length());
  EXPECT_EQ(styled->elevation, elevation);
  EXPECT_EQ(styled->superelevation, superelevation);
}

TEST(RoadStyle, ApplyPreservesPlacedObjectsAndSignals) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  const auto object = network.add_object(road, Object{.name = "tree", .s = 10.0, .t = 2.0});
  const auto signal = network.add_signal(road, Signal{.name = "stop", .s = 20.0, .t = -2.0});
  ASSERT_TRUE(object.is_valid());
  ASSERT_TRUE(signal.is_valid());

  ASSERT_TRUE(roadmaker::edit::apply_road_style(network, road, RoadStyle::urban_two_lane())
                  ->apply(network)
                  .has_value());

  ASSERT_NE(network.object(object), nullptr);
  EXPECT_EQ(network.object(object)->name, "tree");
  ASSERT_NE(network.signal(signal), nullptr);
  EXPECT_EQ(network.signal(signal)->name, "stop");
}

// --- junction integration ---------------------------------------------------

TEST(RoadStyle, ApplyOnJunctionArmMarksJunctionDirty) {
  RoadNetwork network;
  const RoadId west = author_arm(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author_arm(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author_arm(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::End},
                                    RoadEnd{.road = south, .contact = ContactPoint::End}};
  ASSERT_TRUE(roadmaker::edit::create_junction(network, ends)->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());

  auto command = roadmaker::edit::apply_road_style(network, west, RoadStyle::highway());
  const roadmaker::edit::DirtySet dirty = command->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions.front(), junction);
  EXPECT_FALSE(dirty.junctions_are_current); // the editor must regenerate the arm
  EXPECT_TRUE(command->apply(network).has_value());
}

// --- rejection paths --------------------------------------------------------

TEST(RoadStyle, RefusesAConnectingRoad) {
  RoadNetwork network;
  const RoadId target = author_straight(network, "1", 80.0);
  // Tee a side road onto it — that builds a junction with connecting roads.
  auto tee = roadmaker::edit::assembly::tee_onto_road(network, target, 40.0);
  ASSERT_TRUE(tee->apply(network).has_value());

  // Find a connecting road (one owned by a junction) and refuse to style it.
  RoadId connecting;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.junction.is_valid()) {
      connecting = id;
    }
  });
  ASSERT_TRUE(connecting.is_valid()) << "tee_onto_road produced no connecting road";
  expect_rejected(
      network, roadmaker::edit::apply_road_style(network, connecting, RoadStyle::urban_two_lane()));
}

TEST(RoadStyle, RefusesBadArguments) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // Stale road id.
  expect_rejected(network,
                  roadmaker::edit::apply_road_style(network, RoadId{}, RoadStyle::highway()));
  // A style with no lanes.
  expect_rejected(network, roadmaker::edit::apply_road_style(network, road, RoadStyle{}));
  // A non-positive lane width.
  RoadStyle bad;
  bad.right.push_back(StyleLane{.type = LaneType::Driving, .width = Poly3{.a = 0.0}});
  expect_rejected(network, roadmaker::edit::apply_road_style(network, road, bad));
}
