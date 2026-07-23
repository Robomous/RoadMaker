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

// Kernel tests for the stop-line query API (p4-s3, #318).
//
// junction_stoplines() is the single geometry source the mesher, the writer's
// object materialization, the editor tool/panel and the bindings all read, so
// these cases pin the derivation (one line per arm, set back
// kStopLineDefaultDistance, spanning the approach lanes), the authored
// overrides layered over it, and the two ways a line disappears: no driving
// lanes in the line's direction, and a legacy/foreign signalLines object
// already painting one.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_stoplines;
using roadmaker::JunctionId;
using roadmaker::JunctionStopLineInfo;
using roadmaker::kStopLineDefaultDistance;
using roadmaker::kStopLineThickness;
using roadmaker::LaneProfile;
using roadmaker::Object;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::StopLine;
using roadmaker::Waypoint;

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

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

/// The roomy four-way of test_corner_operations: arms stop 20 m short of the
/// centre, so each is 60 m long and an authored setback stays well clear of the
/// clamp (which AuthoredDistanceIsClampedToTheRoad covers deliberately).
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

/// Mutates the junction record in place — the query API is pure, so the tests
/// author records directly rather than going through the commands (those have
/// their own suite in test_stopline_operations.cpp).
void author_record(RoadNetwork& network, JunctionId junction, StopLine record) {
  network.junction(junction)->stoplines.push_back(std::move(record));
}

/// Returns the solved line for `arm` by VALUE: the callers query and search in
/// one expression, and a pointer into that temporary vector would dangle.
std::optional<JunctionStopLineInfo> find_arm(const std::vector<JunctionStopLineInfo>& lines,
                                             const RoadEnd& arm) {
  const auto entry = std::ranges::find_if(
      lines, [&](const JunctionStopLineInfo& info) { return info.arm == arm; });
  if (entry == lines.end()) {
    return std::nullopt;
  }
  return *entry;
}

} // namespace

TEST(JunctionStopLines, FourWayExposesFourDefaults) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  ASSERT_EQ(lines.size(), 4U) << "one derived line per arm, with nothing authored";
  for (const JunctionStopLineInfo& info : lines) {
    EXPECT_FALSE(info.authored);
    EXPECT_FALSE(info.distance_authored);
    EXPECT_FALSE(info.flipped);
    EXPECT_DOUBLE_EQ(info.distance, kStopLineDefaultDistance);
    EXPECT_DOUBLE_EQ(info.thickness, kStopLineThickness);
    EXPECT_TRUE(info.crosswalk_odr_id.empty());
    // Two-lane default: the approach side is one lane, comfortably over 2 m.
    EXPECT_GT(info.span, 2.0);
    // Every arm meets the junction at its End, so the band sits a thickness
    // half-step inside the setback measured back from the far end.
    EXPECT_NEAR(info.s_center,
                info.max_distance + kStopLineThickness - kStopLineDefaultDistance -
                    (kStopLineThickness / 2.0),
                1e-9);
    // The endpoints straddle the centre laterally and are span apart.
    EXPECT_NEAR(
        std::hypot(info.left[0] - info.right[0], info.left[1] - info.right[1]), info.span, 1e-9);
  }
}

TEST(JunctionStopLines, DeterministicOrderMatchesConnectionOrder) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);

  const std::vector<JunctionStopLineInfo> a = junction_stoplines(network, junction);
  const std::vector<JunctionStopLineInfo> b = junction_stoplines(network, junction);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].arm, b[i].arm) << "the query must be deterministic at index " << i;
  }

  // ... and that order is the junction's distinct incoming roads, in
  // connection order — what the writer's deterministic object ids rely on.
  std::vector<RoadId> expected;
  for (const auto& connection : network.junction(junction)->connections) {
    if (std::ranges::find(expected, connection.incoming_road) == expected.end()) {
      expected.push_back(connection.incoming_road);
    }
  }
  ASSERT_EQ(a.size(), expected.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].arm.road, expected[i]);
  }
}

TEST(JunctionStopLines, AuthoredDistanceOverridesTheDefault) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = junction_stoplines(network, junction).front().arm;
  author_record(network, junction, StopLine{.arm = arm, .distance = 9.5});

  const std::optional<JunctionStopLineInfo> info =
      find_arm(junction_stoplines(network, junction), arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->authored);
  EXPECT_TRUE(info->distance_authored);
  EXPECT_DOUBLE_EQ(info->distance, 9.5);

  // The other three arms stay derived — the record is per-arm, not junction-wide.
  for (const JunctionStopLineInfo& other : junction_stoplines(network, junction)) {
    if (!(other.arm == arm)) {
      EXPECT_FALSE(other.authored);
      EXPECT_DOUBLE_EQ(other.distance, kStopLineDefaultDistance);
    }
  }
}

TEST(JunctionStopLines, AuthoredDistanceIsClampedToTheRoad) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = junction_stoplines(network, junction).front().arm;
  author_record(network, junction, StopLine{.arm = arm, .distance = 5000.0});

  const std::optional<JunctionStopLineInfo> info =
      find_arm(junction_stoplines(network, junction), arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_DOUBLE_EQ(info->distance, info->max_distance)
      << "an absurd setback is stored unclamped and clamped only when solved";
  EXPECT_GT(info->max_distance, 0.0);
  EXPECT_NEAR(
      info->max_distance, network.road(arm.road)->plan_view.length() - kStopLineThickness, 1e-9);
  // Clamped or not, the band lands whole on the road.
  EXPECT_GE(info->s_center - (kStopLineThickness / 2.0), -1e-9);
  EXPECT_LE(info->s_center + (kStopLineThickness / 2.0),
            network.road(arm.road)->plan_view.length() + 1e-9);
}

TEST(JunctionStopLines, FlippedSpansTheOutgoingLanes) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = junction_stoplines(network, junction).front().arm;
  const double incoming_t = junction_stoplines(network, junction).front().t_center;
  author_record(network, junction, StopLine{.arm = arm, .flipped = true});

  const std::optional<JunctionStopLineInfo> info =
      find_arm(junction_stoplines(network, junction), arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->flipped);
  EXPECT_TRUE(info->authored);
  EXPECT_FALSE(info->distance_authored) << "flipping alone must not fabricate a setback";
  EXPECT_DOUBLE_EQ(info->distance, kStopLineDefaultDistance);
  // The outgoing lanes sit on the other side of the reference line.
  EXPECT_LT(info->t_center * incoming_t, 0.0);
}

TEST(JunctionStopLines, CrosswalkLinkIsCarriedThrough) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = junction_stoplines(network, junction).front().arm;
  author_record(network, junction, StopLine{.arm = arm, .crosswalk_odr_id = "7"});

  const std::optional<JunctionStopLineInfo> info =
      find_arm(junction_stoplines(network, junction), arm);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->crosswalk_odr_id, "7");
  EXPECT_TRUE(info->authored) << "a bare crosswalk link still counts as an authored record";
}

TEST(JunctionStopLines, ArmWithNoLanesInTheDirectionHasNoLine) {
  // A one-way arm entering the junction carries only right-hand (driving +s)
  // lanes, so its OUTGOING side is empty. Flipping its line leaves nothing to
  // span, and the line is dropped rather than drawn zero-width.
  LaneProfile one_way = LaneProfile::two_lane_default();
  one_way.left.clear();
  one_way.center_marking = false;

  RoadNetwork network;
  const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1", one_way);
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
  const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
  auto command = roadmaker::edit::create_junction(network, ends);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  JunctionId junction{};
  network.for_each_junction([&](JunctionId id, const Junction&) { junction = id; });

  const RoadEnd one_way_arm = end_of(west);
  ASSERT_TRUE(find_arm(junction_stoplines(network, junction), one_way_arm).has_value())
      << "unflipped, the one-way arm's approach lanes still carry a line";
  const std::size_t derived = junction_stoplines(network, junction).size();

  author_record(network, junction, StopLine{.arm = one_way_arm, .flipped = true});
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  EXPECT_FALSE(find_arm(lines, one_way_arm).has_value());
  EXPECT_EQ(lines.size(), derived - 1U);
}

TEST(JunctionStopLines, DormantRecordIsIgnored) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadId stranger =
      author(network, {Waypoint{200.0, 200.0}, Waypoint{260.0, 200.0}}, "stranger");

  // A record naming a road that is not an arm of this junction lies dormant:
  // it neither adds a line nor disturbs the four derived ones.
  author_record(
      network,
      junction,
      StopLine{.arm = RoadEnd{.road = stranger, .contact = ContactPoint::End}, .distance = 12.0});

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  EXPECT_EQ(lines.size(), 4U);
  for (const JunctionStopLineInfo& info : lines) {
    EXPECT_FALSE(info.authored);
  }
}

TEST(JunctionStopLines, StaleJunctionYieldsNothing) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  ASSERT_TRUE(network.erase_junction(junction));
  EXPECT_TRUE(junction_stoplines(network, junction).empty());
}

TEST(JunctionStopLines, LegacySignalLinesObjectSuppressesTheDefault) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const JunctionStopLineInfo seed = junction_stoplines(network, junction).front();

  // A foreign/legacy file paints its own stop line as a plain signalLines
  // object. It keeps rendering through the mesher's object branch, so the
  // derived twin must stand down rather than double-draw.
  Object legacy;
  legacy.road = seed.arm.road;
  legacy.odr_id = "900";
  legacy.type_str = "roadMark";
  legacy.subtype = "signalLines";
  legacy.s = seed.s_center;
  legacy.t = seed.t_center;
  legacy.length = kStopLineThickness;
  legacy.width = seed.span;
  ASSERT_TRUE(network.add_object(seed.arm.road, std::move(legacy)).is_valid());

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  EXPECT_EQ(lines.size(), 3U);
  EXPECT_FALSE(find_arm(lines, seed.arm).has_value());
}

TEST(JunctionStopLines, LegacyObjectOnTheFarHalfDoesNotSuppress) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const JunctionStopLineInfo seed = junction_stoplines(network, junction).front();

  // Suppression is deliberately local to the junction-facing half: a stop line
  // for the OTHER end of the same road says nothing about this junction's arm.
  Object far_away;
  far_away.road = seed.arm.road;
  far_away.odr_id = "901";
  far_away.type_str = "roadMark";
  far_away.subtype = "signalLines";
  far_away.s = 1.0; // the arm faces the junction at its End
  far_away.t = seed.t_center;
  far_away.length = kStopLineThickness;
  far_away.width = seed.span;
  ASSERT_TRUE(network.add_object(seed.arm.road, std::move(far_away)).is_valid());

  EXPECT_EQ(junction_stoplines(network, junction).size(), 4U);
}

// --- span (virtual) junction faces (p4-s4, #319) -----------------------------
//
// A span junction has no arms and no connections at all, so it derives a
// different set: TWO faces per SpanArm, keyed by the pseudo road ends
// {road, Start} (the s_start face) and {road, End} (the s_end face). The band
// sits OUTSIDE the span — upstream of s_start, downstream of s_end — because
// that is where traffic approaching the crossing has to stop.

namespace {

using roadmaker::SpanArm;

/// One uninterrupted 120 m road "1" with a mid-road span junction "9" over
/// [s_start, s_end] — the crosswalk case (§12.7): the road is never cut.
JunctionId make_span(RoadNetwork& network, double s_start, double s_end, RoadId* road_out) {
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  if (road_out != nullptr) {
    *road_out = road;
  }
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = s_start, .s_end = s_end});
  network.junction(junction)->locked = true;
  return junction;
}

RoadEnd face_of(RoadId road, ContactPoint face) {
  return RoadEnd{.road = road, .contact = face};
}

} // namespace

TEST(SpanJunctionStopLines, OneSpanExposesTwoFaces) {
  RoadNetwork network;
  RoadId road{};
  const JunctionId junction = make_span(network, 50.0, 56.5, &road);

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  ASSERT_EQ(lines.size(), 2U) << "two faces per span, with nothing authored";
  const double half = kStopLineThickness / 2.0;

  const std::optional<JunctionStopLineInfo> start =
      find_arm(lines, face_of(road, ContactPoint::Start));
  ASSERT_TRUE(start.has_value());
  EXPECT_TRUE(start->span_face);
  EXPECT_FALSE(start->authored);
  EXPECT_DOUBLE_EQ(start->distance, kStopLineDefaultDistance);
  EXPECT_NEAR(start->s_center, 50.0 - kStopLineDefaultDistance - half, 1e-9);
  EXPECT_NEAR(start->max_distance, 50.0 - kStopLineThickness, 1e-9);

  const std::optional<JunctionStopLineInfo> end = find_arm(lines, face_of(road, ContactPoint::End));
  ASSERT_TRUE(end.has_value());
  EXPECT_TRUE(end->span_face);
  EXPECT_NEAR(end->s_center, 56.5 + kStopLineDefaultDistance + half, 1e-9);
  EXPECT_NEAR(end->max_distance, 120.0 - 56.5 - kStopLineThickness, 1e-9);

  // Each face spans the lanes APPROACHING the span, which sit on opposite
  // sides of the reference line — the road is two-way.
  EXPECT_LT(start->t_center * end->t_center, 0.0);
  for (const JunctionStopLineInfo& info : lines) {
    EXPECT_GT(info.span, 2.0);
    EXPECT_NEAR(
        std::hypot(info.left[0] - info.right[0], info.left[1] - info.right[1]), info.span, 1e-9);
  }
}

TEST(SpanJunctionStopLines, ParallelRoadsExposeFourFaces) {
  RoadNetwork network;
  const RoadId north = author(network, {Waypoint{0.0, 6.0}, Waypoint{120.0, 6.0}}, "1");
  const RoadId south = author(network, {Waypoint{0.0, -6.0}, Waypoint{120.0, -6.0}}, "2");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans = {SpanArm{.road = north, .s_start = 40.0, .s_end = 44.0},
                                       SpanArm{.road = south, .s_start = 41.5, .s_end = 45.5}};

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  ASSERT_EQ(lines.size(), 4U) << "two faces per span road";
  for (const RoadId road : {north, south}) {
    for (const ContactPoint face : {ContactPoint::Start, ContactPoint::End}) {
      EXPECT_TRUE(find_arm(lines, face_of(road, face)).has_value());
    }
  }
  // Each road's own span edges drive its faces: the two roads are offset by
  // 1.5 m, so the s stations are too.
  const double a = find_arm(lines, face_of(north, ContactPoint::Start))->s_center;
  const double b = find_arm(lines, face_of(south, ContactPoint::Start))->s_center;
  EXPECT_NEAR(b - a, 1.5, 1e-9);
}

TEST(SpanJunctionStopLines, AFaceIsClampedAgainstTheNearRoadEnd) {
  RoadNetwork network;
  RoadId road{};
  // The span starts 0.05 m into the road — closer than half a band thickness,
  // so there is no room at all for the upstream face's setback.
  const JunctionId junction = make_span(network, 0.05, 6.5, &road);

  const std::optional<JunctionStopLineInfo> start =
      find_arm(junction_stoplines(network, junction), face_of(road, ContactPoint::Start));
  ASSERT_TRUE(start.has_value());
  EXPECT_DOUBLE_EQ(start->max_distance, 0.0);
  EXPECT_DOUBLE_EQ(start->distance, 0.0);
  // Clamped or not, the band lands whole on the road.
  EXPECT_GE(start->s_center - (kStopLineThickness / 2.0), -1e-9);
  EXPECT_LE(start->s_center + (kStopLineThickness / 2.0), 120.0 + 1e-9);
}

TEST(SpanJunctionStopLines, ASpanOutsideAShortenedRoadIsClampedNotDropped) {
  RoadNetwork network;
  RoadId road{};
  // s_end past the end of the road: the span outlives the edit that shortened
  // it, exactly as a dormant record does, and the faces clamp.
  const JunctionId junction = make_span(network, 118.0, 500.0, &road);

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  const std::optional<JunctionStopLineInfo> end = find_arm(lines, face_of(road, ContactPoint::End));
  ASSERT_TRUE(end.has_value());
  EXPECT_DOUBLE_EQ(end->max_distance, 0.0);
  EXPECT_NEAR(end->s_center, 120.0 - (kStopLineThickness / 2.0), 1e-9);
}

TEST(SpanJunctionStopLines, AuthoredDistanceAndFlipApplyPerFace) {
  RoadNetwork network;
  RoadId road{};
  const JunctionId junction = make_span(network, 50.0, 56.5, &road);
  const RoadEnd start = face_of(road, ContactPoint::Start);
  const double derived_t = find_arm(junction_stoplines(network, junction), start)->t_center;

  author_record(network, junction, StopLine{.arm = start, .distance = 9.0, .flipped = true});

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  const std::optional<JunctionStopLineInfo> info = find_arm(lines, start);
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->authored);
  EXPECT_TRUE(info->distance_authored);
  EXPECT_DOUBLE_EQ(info->distance, 9.0);
  EXPECT_NEAR(info->s_center, 50.0 - 9.0 - (kStopLineThickness / 2.0), 1e-9);
  // Flipping moves the band to the lanes LEAVING the span, on the other side.
  EXPECT_LT(info->t_center * derived_t, 0.0);

  // The other face is untouched: the record is per face, not per span.
  const std::optional<JunctionStopLineInfo> other =
      find_arm(lines, face_of(road, ContactPoint::End));
  ASSERT_TRUE(other.has_value());
  EXPECT_FALSE(other->authored);
  EXPECT_DOUBLE_EQ(other->distance, kStopLineDefaultDistance);
}

TEST(SpanJunctionStopLines, AOneWayRoadGuardsOnlyTheEdgeItsTrafficApproaches) {
  LaneProfile one_way = LaneProfile::two_lane_default();
  one_way.left.clear();
  one_way.center_marking = false;

  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1", one_way);
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});

  // Only the right-hand (+s) lanes exist, so only the upstream face has an
  // approach to guard; the downstream one is dropped rather than drawn
  // zero-width.
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines.front().arm, face_of(road, ContactPoint::Start));
}

TEST(SpanJunctionStopLines, ADormantRecordOnAStaleRoadIsIgnored) {
  RoadNetwork network;
  RoadId road{};
  const JunctionId junction = make_span(network, 50.0, 56.5, &road);
  const RoadId stranger =
      author(network, {Waypoint{200.0, 200.0}, Waypoint{260.0, 200.0}}, "stranger");
  author_record(
      network,
      junction,
      StopLine{.arm = RoadEnd{.road = stranger, .contact = ContactPoint::End}, .distance = 12.0});

  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  EXPECT_EQ(lines.size(), 2U);
  for (const JunctionStopLineInfo& info : lines) {
    EXPECT_FALSE(info.authored);
  }
}

TEST(SpanJunctionStopLines, AStaleSpanRoadYieldsNoFaces) {
  RoadNetwork network;
  RoadId road{};
  const JunctionId junction = make_span(network, 50.0, 56.5, &road);
  ASSERT_TRUE(network.erase_road(road));
  EXPECT_TRUE(junction_stoplines(network, junction).empty());
}

// --- mesher (WP4) ------------------------------------------------------------
//
// Stop lines are the only marking with no Object behind them: the mesher
// derives them from the junction at each road end via the same
// junction_stoplines() the writer uses, so the viewport and the export cannot
// drift.

namespace {

std::size_t stopline_submeshes(const RoadNetwork& network) {
  const roadmaker::NetworkMesh scene = roadmaker::build_network_mesh(network);
  std::size_t n = 0;
  for (const roadmaker::RoadMesh& road : scene.roads) {
    for (const roadmaker::SubMesh& marking : road.markings) {
      if (marking.name.find("stopline") != std::string::npos) {
        ++n;
      }
    }
  }
  return n;
}

/// Centroid of every stop-line submesh vertex on `road_odr_id`, or nullopt when
/// that road paints none.
std::optional<std::array<double, 3>> stopline_centroid(const RoadNetwork& network, RoadId road_id) {
  const roadmaker::NetworkMesh scene = roadmaker::build_network_mesh(network);
  std::array<double, 3> sum{};
  std::size_t count = 0;
  for (const roadmaker::RoadMesh& road : scene.roads) {
    if (!(road.road == road_id)) {
      continue;
    }
    for (const roadmaker::SubMesh& marking : road.markings) {
      if (marking.name.find("stopline") == std::string::npos) {
        continue;
      }
      for (std::size_t i = 0; i + 2 < marking.positions.size(); i += 3) {
        sum[0] += marking.positions[i];
        sum[1] += marking.positions[i + 1];
        sum[2] += marking.positions[i + 2];
        ++count;
      }
    }
  }
  if (count == 0) {
    return std::nullopt;
  }
  const double n = static_cast<double>(count);
  return std::array<double, 3>{sum[0] / n, sum[1] / n, sum[2] / n};
}

} // namespace

TEST(JunctionStopLineMesh, FourWayPaintsFourLinesWithNoAuthoring) {
  RoadNetwork network;
  make_cross(network);
  EXPECT_EQ(stopline_submeshes(network), 4U) << "the defaults mesh without anyone authoring them";
}

TEST(JunctionStopLineMesh, ChangingTheDistanceMovesTheQuad) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const RoadEnd arm = junction_stoplines(network, junction).front().arm;

  const std::optional<std::array<double, 3>> before = stopline_centroid(network, arm.road);
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(network, junction, arm, 12.0)
                  ->apply(network)
                  .has_value());
  const std::optional<std::array<double, 3>> after = stopline_centroid(network, arm.road);
  ASSERT_TRUE(after.has_value());

  // The arm runs along a world axis, so a bigger setback moves the band a
  // matching distance back from the junction.
  const double moved = std::hypot((*after)[0] - (*before)[0], (*after)[1] - (*before)[1]);
  EXPECT_NEAR(moved, 12.0 - kStopLineDefaultDistance, 1e-6);
}

TEST(JunctionStopLineMesh, ALegacyObjectMeshesOnceNotTwice) {
  RoadNetwork network;
  const JunctionId junction = make_cross(network);
  const JunctionStopLineInfo seed = junction_stoplines(network, junction).front();

  Object legacy;
  legacy.road = seed.arm.road;
  legacy.odr_id = "900";
  legacy.type_str = "roadMark";
  legacy.subtype = "signalLines";
  legacy.s = seed.s_center;
  legacy.t = seed.t_center;
  legacy.length = kStopLineThickness;
  legacy.width = seed.span;
  ASSERT_TRUE(network.add_object(seed.arm.road, std::move(legacy)).is_valid());

  // Three derived bands + the legacy object's own "stop line" submesh: the
  // suppressed arm is painted exactly once, by the object branch.
  EXPECT_EQ(stopline_submeshes(network), 3U);
  const roadmaker::NetworkMesh scene = roadmaker::build_network_mesh(network);
  std::size_t object_stop_lines = 0;
  for (const roadmaker::RoadMesh& road : scene.roads) {
    for (const roadmaker::SubMesh& marking : road.markings) {
      if (marking.name.find("stop line") != std::string::npos) {
        ++object_stop_lines;
      }
    }
  }
  EXPECT_EQ(object_stop_lines, 1U);
}

TEST(JunctionStopLineMesh, SpanJunctionFacesPaintOnTheUncutRoad) {
  RoadNetwork network;
  RoadId road{};
  const JunctionId junction = make_span(network, 50.0, 56.5, &road);

  // The road is never cut, so junction_at_end finds nothing on it: the mesher
  // has to reach the junction through its span list instead.
  EXPECT_EQ(stopline_submeshes(network), 2U);

  const std::optional<std::array<double, 3>> before = stopline_centroid(network, road);
  ASSERT_TRUE(before.has_value());
  // The two faces straddle the span, so their joint centroid sits at its middle.
  EXPECT_NEAR((*before)[0], (50.0 + 56.5) / 2.0, 1e-6);

  author_record(
      network,
      junction,
      StopLine{.arm = RoadEnd{.road = road, .contact = ContactPoint::Start}, .distance = 12.0});
  const std::optional<std::array<double, 3>> after = stopline_centroid(network, road);
  ASSERT_TRUE(after.has_value());
  // Only the upstream face moved, and it moved AWAY from the span, so the pair
  // centroid slides back by half the extra setback.
  EXPECT_NEAR((*after)[0] - (*before)[0], -(12.0 - kStopLineDefaultDistance) / 2.0, 1e-6);
}
