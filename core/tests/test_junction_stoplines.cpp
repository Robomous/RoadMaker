// Kernel tests for the stop-line query API (p4-s3, #318).
//
// junction_stoplines() is the single geometry source the mesher, the writer's
// object materialization, the editor tool/panel and the bindings all read, so
// these cases pin the derivation (one line per arm, set back
// kStopLineDefaultDistance, spanning the approach lanes), the authored
// overrides layered over it, and the two ways a line disappears: no driving
// lanes in the line's direction, and a legacy/foreign signalLines object
// already painting one.

#include "roadmaker/mesh/junction_stoplines.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
    EXPECT_NEAR(std::hypot(info.left[0] - info.right[0], info.left[1] - info.right[1]),
                info.span,
                1e-9);
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
  EXPECT_NEAR(info->max_distance,
              network.road(arm.road)->plan_view.length() - kStopLineThickness,
              1e-9);
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
  const RoadId stranger = author(network, {Waypoint{200.0, 200.0}, Waypoint{260.0, 200.0}}, "stranger");

  // A record naming a road that is not an arm of this junction lies dormant:
  // it neither adds a line nor disturbs the four derived ones.
  author_record(network,
                junction,
                StopLine{.arm = RoadEnd{.road = stranger, .contact = ContactPoint::End},
                         .distance = 12.0});

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
