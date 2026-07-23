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

// Kernel tests for edit::set_stopline_distance / flip_stopline / reset_stopline
// (p4-s3, #318).
//
// The three commands are pure junction value edits, so every case runs through
// the §8 oracle: apply changes the document, revert restores it BYTE-identically
// and the turn set is never touched (junctions_are_current). Solvability is
// validated through junction_stoplines(), the same query the mesher reads, so a
// stale or non-arm road end is a clean error rather than a crash.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_stoplines;
using roadmaker::JunctionId;
using roadmaker::JunctionStopLineInfo;
using roadmaker::kStopLineDefaultDistance;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::StopLine;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::flip_stopline;
using roadmaker::edit::reset_stopline;
using roadmaker::edit::set_stopline_distance;
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

/// The roomy four-way shared with the corner and query suites.
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

RoadEnd first_arm(const RoadNetwork& network, JunctionId junction) {
  return junction_stoplines(network, junction).front().arm;
}

std::optional<JunctionStopLineInfo>
solved(const RoadNetwork& network, JunctionId junction, const RoadEnd& arm) {
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  const auto entry = std::ranges::find_if(
      lines, [&](const JunctionStopLineInfo& info) { return info.arm == arm; });
  if (entry == lines.end()) {
    return std::nullopt;
  }
  return *entry;
}

const StopLine* record_for(const RoadNetwork& network, JunctionId junction, const RoadEnd& arm) {
  const Junction* record = network.junction(junction);
  const auto entry = std::ranges::find_if(record->stoplines,
                                          [&](const StopLine& line) { return line.arm == arm; });
  return entry == record->stoplines.end() ? nullptr : &*entry;
}

} // namespace

// --- set_stopline_distance ---------------------------------------------------

TEST(StopLineOperations, SetDistanceRoundTripsByteIdentical) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  auto command = set_stopline_distance(network, junction, first_arm(network, junction), 7.5);
  expect_command_round_trip(network, *command);
}

TEST(StopLineOperations, SetDistanceCreatesTheRecordAndIsVisibleInTheQuery) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_EQ(record_for(network, junction, arm), nullptr) << "the arm starts fully derived";

  auto command = set_stopline_distance(network, junction, arm, 7.5);
  ASSERT_TRUE(command->apply(network).has_value());

  const StopLine* record = record_for(network, junction, arm);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->distance.has_value());
  EXPECT_DOUBLE_EQ(*record->distance, 7.5);

  const std::optional<JunctionStopLineInfo> info = solved(network, junction, arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->distance_authored);
  EXPECT_DOUBLE_EQ(info->distance, 7.5);
}

TEST(StopLineOperations, SetDistanceUpdatesAnExistingRecordInPlace) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 7.5)->apply(network).has_value());

  auto command = set_stopline_distance(network, junction, arm, 2.0);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(network.junction(junction)->stoplines.size(), 1U) << "no duplicate record";
  EXPECT_DOUBLE_EQ(*record_for(network, junction, arm)->distance, 2.0);
}

TEST(StopLineOperations, SetDistanceRecordsTheCrosswalkLink) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);

  auto command = set_stopline_distance(network, junction, arm, 3.0, std::string("7"));
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(record_for(network, junction, arm)->crosswalk_odr_id, "7");

  // A later distance edit that passes no link leaves the provenance alone.
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 4.5)->apply(network).has_value());
  EXPECT_EQ(record_for(network, junction, arm)->crosswalk_odr_id, "7");
}

TEST(StopLineOperations, SetDistanceStoresTheValueUnclamped) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 5000.0)->apply(network).has_value());

  EXPECT_DOUBLE_EQ(*record_for(network, junction, arm)->distance, 5000.0)
      << "authored-like: stored as given, clamped only when solved";
  EXPECT_DOUBLE_EQ(solved(network, junction, arm)->distance,
                   solved(network, junction, arm)->max_distance);
}

TEST(StopLineOperations, SetDistanceDirtiesTheArmRoadAndKeepsTheTurnSet) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  auto command = set_stopline_distance(network, junction, arm, 6.0);

  const roadmaker::edit::DirtySet& dirty = command->dirty();
  EXPECT_EQ(dirty.roads, std::vector<RoadId>{arm.road});
  EXPECT_EQ(dirty.junctions, std::vector<JunctionId>{junction});
  EXPECT_TRUE(dirty.junctions_are_current) << "a stop line never changes the turn set";
  EXPECT_FALSE(dirty.topology);
}

TEST(StopLineOperations, SetDistanceRejectsBadInput) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  const std::string pristine = snapshot_xodr(network);

  for (const double bad :
       {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    auto command = set_stopline_distance(network, junction, arm, bad);
    ASSERT_NE(command, nullptr);
    EXPECT_FALSE(command->apply(network).has_value()) << "distance " << bad << " must be rejected";
    // A failed apply leaves the network untouched.
    expect_network_matches(network, pristine);
  }
}

TEST(StopLineOperations, SetDistanceRejectsAStaleJunction) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_TRUE(network.erase_junction(junction));

  auto command = set_stopline_distance(network, junction, arm, 3.0);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
}

TEST(StopLineOperations, SetDistanceRejectsARoadEndThatIsNotAnArm) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId stranger = author(network, {Waypoint{200.0, 200.0}, Waypoint{260.0, 200.0}}, "s");
  const std::string pristine = snapshot_xodr(network);

  auto command = set_stopline_distance(network, junction, end_of(stranger), 3.0);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, pristine);
}

// --- flip_stopline -----------------------------------------------------------

TEST(StopLineOperations, FlipRoundTripsByteIdentical) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  auto command = flip_stopline(network, junction, first_arm(network, junction));
  expect_command_round_trip(network, *command);
}

TEST(StopLineOperations, FlipSwitchesTheSpannedDirection) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  const double incoming_t = solved(network, junction, arm)->t_center;

  ASSERT_TRUE(flip_stopline(network, junction, arm)->apply(network).has_value());
  const std::optional<JunctionStopLineInfo> info = solved(network, junction, arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->flipped);
  EXPECT_LT(info->t_center * incoming_t, 0.0) << "the band moved to the other side";
}

TEST(StopLineOperations, FlipTwiceNormalizesTheRecordAway) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  const std::string pristine = snapshot_xodr(network);

  ASSERT_TRUE(flip_stopline(network, junction, arm)->apply(network).has_value());
  EXPECT_NE(record_for(network, junction, arm), nullptr);

  ASSERT_TRUE(flip_stopline(network, junction, arm)->apply(network).has_value());
  EXPECT_EQ(record_for(network, junction, arm), nullptr)
      << "a record back at its defaults is erased, not kept empty";
  // Flip-twice must be byte-identical to never having flipped.
  expect_network_matches(network, pristine);
}

TEST(StopLineOperations, FlipKeepsARecordThatStillAuthorsSomething) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 6.0)->apply(network).has_value());

  ASSERT_TRUE(flip_stopline(network, junction, arm)->apply(network).has_value());
  ASSERT_TRUE(flip_stopline(network, junction, arm)->apply(network).has_value());

  const StopLine* record = record_for(network, junction, arm);
  ASSERT_NE(record, nullptr) << "the authored distance survives the round trip";
  EXPECT_DOUBLE_EQ(*record->distance, 6.0);
  EXPECT_FALSE(record->flipped);
}

TEST(StopLineOperations, FlipIntoADirectionWithNoLanesIsAnError) {
  LaneProfile one_way = LaneProfile::two_lane_default();
  one_way.left.clear();
  one_way.center_marking = false;

  RoadNetwork network;
  // author_clothoid_road takes a std::span — the waypoints must be materialized.
  const std::vector<Waypoint> west_points{Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}};
  auto west_road = roadmaker::author_clothoid_road(network, west_points, one_way, "", "1");
  ASSERT_TRUE(west_road.has_value());
  const RoadId west = *west_road;
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
  const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
  auto create = roadmaker::edit::create_junction(network, ends);
  ASSERT_NE(create, nullptr);
  ASSERT_TRUE(create->apply(network).has_value());
  JunctionId junction{};
  network.for_each_junction([&](JunctionId id, const Junction&) { junction = id; });

  const std::string pristine = snapshot_xodr(network);
  auto command = flip_stopline(network, junction, end_of(west));
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value())
      << "there are no outgoing lanes to span — an error, not a zero-width band";
  expect_network_matches(network, pristine);
}

// --- reset_stopline ----------------------------------------------------------

TEST(StopLineOperations, ResetRoundTripsByteIdentical) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 9.0)->apply(network).has_value());

  auto command = reset_stopline(network, junction, arm);
  expect_command_round_trip(network, *command);
}

TEST(StopLineOperations, ResetReturnsTheArmToTheDerivedDefault) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  const std::string pristine = snapshot_xodr(network);
  ASSERT_TRUE(set_stopline_distance(network, junction, arm, 9.0)->apply(network).has_value());

  ASSERT_TRUE(reset_stopline(network, junction, arm)->apply(network).has_value());
  EXPECT_EQ(record_for(network, junction, arm), nullptr);
  const std::optional<JunctionStopLineInfo> info = solved(network, junction, arm);
  ASSERT_TRUE(info.has_value()) << "the derived default is still there";
  EXPECT_FALSE(info->authored);
  EXPECT_DOUBLE_EQ(info->distance, kStopLineDefaultDistance);
  expect_network_matches(network, pristine);
}

TEST(StopLineOperations, ResetOnAnUnauthoredArmIsACleanError) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = first_arm(network, junction);
  const std::string pristine = snapshot_xodr(network);

  auto command = reset_stopline(network, junction, arm);
  ASSERT_NE(command, nullptr) << "a no-op factory must return an invalid command, never null";
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, pristine);
}

TEST(StopLineOperations, ResetLeavesTheOtherArmsAlone) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  ASSERT_GE(lines.size(), 2U);
  const RoadEnd first = lines[0].arm;
  const RoadEnd second = lines[1].arm;
  ASSERT_TRUE(set_stopline_distance(network, junction, first, 9.0)->apply(network).has_value());
  ASSERT_TRUE(set_stopline_distance(network, junction, second, 3.0)->apply(network).has_value());

  ASSERT_TRUE(reset_stopline(network, junction, first)->apply(network).has_value());
  EXPECT_EQ(record_for(network, junction, first), nullptr);
  ASSERT_NE(record_for(network, junction, second), nullptr);
  EXPECT_DOUBLE_EQ(*record_for(network, junction, second)->distance, 3.0);
}
