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

// Kernel tests for edit::set_surface_span_included / set_surface_span_sort_index
// (p4-s5, #320).
//
// Both are pure junction value edits, so every case runs through the §8 oracle:
// apply changes the document, revert restores it BYTE-identically, and the turn
// set is never touched (junctions_are_current — only the floor re-meshes).
// Solvability is validated through junction_surface_spans(), the same query the
// mesher reads, so a stale junction, a foreign road or a span (virtual)
// junction is a clean error rather than a crash.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_surface_spans;
using roadmaker::JunctionId;
using roadmaker::JunctionSurfaceSpanInfo;
using roadmaker::kMaxSurfaceSpanSortIndex;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::SpanArm;
using roadmaker::SurfaceSpan;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::set_surface_span_included;
using roadmaker::edit::set_surface_span_sort_index;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// The §8 command oracle: apply changes the doc, revert restores it
/// byte-identically, re-apply reproduces, final revert is pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after); // a command that changes nothing is a bug
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

/// The roomy four-way shared with the corner, stop-line and query suites.
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

RoadId first_span(const RoadNetwork& network, JunctionId junction) {
  return junction_surface_spans(network, junction).front().road;
}

const SurfaceSpan* record_for(const RoadNetwork& network, JunctionId junction, RoadId road) {
  const Junction* entry = network.junction(junction);
  const auto found = std::ranges::find_if(
      entry->surface_spans, [&](const SurfaceSpan& record) { return record.road == road; });
  return found == entry->surface_spans.end() ? nullptr : &*found;
}

} // namespace

TEST(SurfaceSpanOperations, ExcludingSamplesRoundTrips) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  auto command = set_surface_span_included(network, junction, span, false);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const SurfaceSpan* record = record_for(network, junction, span);
  ASSERT_NE(record, nullptr);
  EXPECT_FALSE(record->included);
  EXPECT_EQ(record->sort_index, 0);
}

TEST(SurfaceSpanOperations, SortIndexRoundTrips) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  auto command = set_surface_span_sort_index(network, junction, span, 3);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const SurfaceSpan* record = record_for(network, junction, span);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->sort_index, 3);
  EXPECT_TRUE(record->included);
}

TEST(SurfaceSpanOperations, TheEditIsFloorOnly) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  auto command = set_surface_span_sort_index(network, junction, span, 1);
  ASSERT_NE(command, nullptr);
  // Only the floor re-meshes: the turn set is untouched, so the editor must
  // NOT regenerate the connecting roads behind this edit.
  EXPECT_TRUE(command->dirty().roads.empty());
  ASSERT_EQ(command->dirty().junctions.size(), 1U);
  EXPECT_EQ(command->dirty().junctions.front(), junction);
  EXPECT_TRUE(command->dirty().junctions_are_current);
}

TEST(SurfaceSpanOperations, ToggleTwiceIsByteIdenticalToNeverTouchingIt) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);
  const std::string pristine = snapshot_xodr(network);

  auto off = set_surface_span_included(network, junction, span, false);
  ASSERT_TRUE(off->apply(network).has_value());
  auto on = set_surface_span_included(network, junction, span, true);
  ASSERT_NE(on, nullptr);
  ASSERT_TRUE(on->apply(network).has_value());

  // Erase-at-default: the record is GONE, not merely back at its defaults.
  EXPECT_EQ(record_for(network, junction, span), nullptr);
  expect_network_matches(network, pristine);
}

TEST(SurfaceSpanOperations, RaiseThenLowerIsByteIdenticalToNeverTouchingIt) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);
  const std::string pristine = snapshot_xodr(network);

  auto raise = set_surface_span_sort_index(network, junction, span, 1);
  ASSERT_TRUE(raise->apply(network).has_value());
  auto lower = set_surface_span_sort_index(network, junction, span, 0);
  ASSERT_NE(lower, nullptr);
  ASSERT_TRUE(lower->apply(network).has_value());

  EXPECT_EQ(record_for(network, junction, span), nullptr);
  expect_network_matches(network, pristine);
}

TEST(SurfaceSpanOperations, TheOtherFieldSurvivesAnEraseAtDefault) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  ASSERT_TRUE(
      set_surface_span_included(network, junction, span, false)->apply(network).has_value());
  ASSERT_TRUE(set_surface_span_sort_index(network, junction, span, 2)->apply(network).has_value());
  // Returning ONE field to its default must not delete the other's authoring.
  ASSERT_TRUE(set_surface_span_sort_index(network, junction, span, 0)->apply(network).has_value());

  const SurfaceSpan* record = record_for(network, junction, span);
  ASSERT_NE(record, nullptr);
  EXPECT_FALSE(record->included);
  EXPECT_EQ(record->sort_index, 0);
}

TEST(SurfaceSpanOperations, NoOpsAreRejectedAgainstTheEffectiveValue) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  // No record at all: the effective values are the defaults, so setting them
  // again is a no-op — the record's ABSENCE is not an excuse to create one.
  EXPECT_FALSE(
      set_surface_span_included(network, junction, span, true)->apply(network).has_value());
  EXPECT_FALSE(set_surface_span_sort_index(network, junction, span, 0)->apply(network).has_value());
  EXPECT_TRUE(network.junction(junction)->surface_spans.empty());

  ASSERT_TRUE(set_surface_span_sort_index(network, junction, span, 4)->apply(network).has_value());
  EXPECT_FALSE(set_surface_span_sort_index(network, junction, span, 4)->apply(network).has_value());
}

TEST(SurfaceSpanOperations, OutOfRangeSortIndexIsRefused) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  EXPECT_FALSE(set_surface_span_sort_index(network, junction, span, kMaxSurfaceSpanSortIndex + 1)
                   ->apply(network)
                   .has_value());
  EXPECT_FALSE(set_surface_span_sort_index(network, junction, span, -kMaxSurfaceSpanSortIndex - 1)
                   ->apply(network)
                   .has_value());
  EXPECT_TRUE(set_surface_span_sort_index(network, junction, span, kMaxSurfaceSpanSortIndex)
                  ->apply(network)
                  .has_value());
}

TEST(SurfaceSpanOperations, StaleAndForeignTargetsAreCleanErrors) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId span = first_span(network, junction);

  // An ARM road is not a connecting road, so it is not a span of the floor.
  const RoadId arm = network.junction(junction)->arms.front().road;
  EXPECT_FALSE(
      set_surface_span_included(network, junction, arm, false)->apply(network).has_value());

  EXPECT_FALSE(
      set_surface_span_included(network, JunctionId{}, span, false)->apply(network).has_value());
  EXPECT_FALSE(
      set_surface_span_sort_index(network, junction, RoadId{}, 1)->apply(network).has_value());
}

TEST(SurfaceSpanOperations, ASpanJunctionHasNoSpansToControl) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;

  // A virtual junction has no floor at all, so the same solvability test that
  // guards a foreign road refuses it — no special case needed.
  EXPECT_TRUE(junction_surface_spans(network, junction).empty());
  EXPECT_FALSE(
      set_surface_span_included(network, junction, road, false)->apply(network).has_value());
  EXPECT_TRUE(network.junction(junction)->surface_spans.empty());
}
