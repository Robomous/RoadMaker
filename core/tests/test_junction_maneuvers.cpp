// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Kernel tests for the maneuver query and the six maneuver commands (p4-s6,
// #227).
//
// A maneuver is ONE connecting road's path through a junction. The turn type is
// DERIVED from the arm-face headings — ASAM OpenDRIVE has no carrier for it
// (§12.2 Table 56) — and only stored when overridden, so most of what is tested
// here is derivation plus the sparse-record idiom: authors-nothing ⇒ erase, and
// every command validated through junction_maneuvers(), the same query the tool
// and the panel read.
//
// Every command here runs through the §8 oracle: `Junction::maneuvers` is
// persisted as `<userData code="rm:maneuver">`, so a lock, a turn-type override
// and an endpoint slide all reach write_xodr() exactly like a geometry change
// does. Byte-equality of the serialized document is therefore the whole test of
// apply→revert — including the authors-nothing rule, since a record the writer
// drops must leave bytes identical to the ones before the edit.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_maneuvers;
using roadmaker::JunctionId;
using roadmaker::JunctionManeuverInfo;
using roadmaker::kMaxManeuverControlPoints;
using roadmaker::LaneProfile;
using roadmaker::Maneuver;
using roadmaker::maneuver_turn_type;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::TurnType;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::TurnSetPolicy;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// The §8 command oracle: apply changes the document, revert restores it
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

/// The ROOMY four-way shared with the corner, stop-line and surface-span
/// suites: arms stop 20 m short of the centre, so no turn is clamped by the
/// tight-junction geometry.
struct Cross {
  JunctionId junction;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;
};

Cross make_cross(RoadNetwork& network) {
  Cross cross;
  cross.west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
  cross.east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  cross.south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  cross.north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
  const std::vector<RoadEnd> ends{
      end_of(cross.west), end_of(cross.east), end_of(cross.south), end_of(cross.north)};
  auto command = roadmaker::edit::create_junction(network, ends);
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("create_junction: " + applied.error().message);
  }
  network.for_each_junction([&](JunctionId id, const Junction&) { cross.junction = id; });
  return cross;
}

/// The maneuver leaving `from` and entering `to`.
JunctionManeuverInfo
maneuver_between(const RoadNetwork& network, JunctionId junction, RoadEnd from, RoadEnd to) {
  for (const JunctionManeuverInfo& info : junction_maneuvers(network, junction)) {
    if (info.from == from && info.to == to) {
      return info;
    }
  }
  throw std::runtime_error("maneuver_between: no such maneuver");
}

const Maneuver* record_for(const RoadNetwork& network, JunctionId junction, RoadId road) {
  const Junction* entry = network.junction(junction);
  const auto found = std::ranges::find_if(
      entry->maneuvers, [&](const Maneuver& record) { return record.road == road; });
  return found == entry->maneuvers.end() ? nullptr : &*found;
}

void apply_or_throw(RoadNetwork& network, std::unique_ptr<Command> command) {
  if (command == nullptr) {
    throw std::runtime_error("null command");
  }
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("apply: " + applied.error().message);
  }
}

} // namespace

// --- the query ---------------------------------------------------------------

TEST(JunctionManeuvers, DerivesTurnTypesFromArmFaces) {
  RoadNetwork network;
  const Cross cross = make_cross(network);

  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, cross.junction);
  // Four arms, one driving lane each way, every ordered pair but the U-turns.
  ASSERT_EQ(maneuvers.size(), 12U);

  const auto straight =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  EXPECT_EQ(straight.computed, TurnType::Straight);
  EXPECT_EQ(straight.effective, TurnType::Straight);
  EXPECT_FALSE(straight.overridden);
  EXPECT_FALSE(straight.authored);
  EXPECT_FALSE(straight.locked);
  EXPECT_FALSE(straight.is_uturn_explicit);
  EXPECT_EQ(straight.from_lane, -1);
  EXPECT_EQ(straight.to_lane, 1);
  EXPECT_FALSE(straight.road_odr_id.empty());
  EXPECT_GE(straight.path.size(), 2U);

  // Heading east, north is a left turn and south a right turn.
  EXPECT_EQ(
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.north)).computed,
      TurnType::Left);
  EXPECT_EQ(
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.south)).computed,
      TurnType::Right);

  int straights = 0;
  int lefts = 0;
  int rights = 0;
  for (const JunctionManeuverInfo& info : maneuvers) {
    straights += info.computed == TurnType::Straight ? 1 : 0;
    lefts += info.computed == TurnType::Left ? 1 : 0;
    rights += info.computed == TurnType::Right ? 1 : 0;
  }
  EXPECT_EQ(straights, 4);
  EXPECT_EQ(lefts, 4);
  EXPECT_EQ(rights, 4);
}

TEST(JunctionManeuvers, ClassifiesThresholdEdges) {
  EXPECT_EQ(maneuver_turn_type(0.0, false), TurnType::Straight);
  EXPECT_EQ(maneuver_turn_type(30.0 * kDeg, false), TurnType::Straight);
  EXPECT_EQ(maneuver_turn_type(-30.0 * kDeg, false), TurnType::Straight);
  EXPECT_EQ(maneuver_turn_type(30.5 * kDeg, false), TurnType::Left);
  EXPECT_EQ(maneuver_turn_type(-30.5 * kDeg, false), TurnType::Right);
  EXPECT_EQ(maneuver_turn_type(149.5 * kDeg, false), TurnType::Left);
  EXPECT_EQ(maneuver_turn_type(-149.5 * kDeg, false), TurnType::Right);
  EXPECT_EQ(maneuver_turn_type(150.0 * kDeg, false), TurnType::UTurn);
  EXPECT_EQ(maneuver_turn_type(-150.0 * kDeg, false), TurnType::UTurn);
  // Same arm is a U-turn whatever the deflection.
  EXPECT_EQ(maneuver_turn_type(0.0, true), TurnType::UTurn);
  // The angle is normalized, so a wrapped deflection classifies identically.
  EXPECT_EQ(maneuver_turn_type((360.0 + 90.0) * kDeg, false), TurnType::Left);
}

TEST(JunctionManeuvers, SlideSegmentsSpanTheAnchorLane) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));

  // The incoming anchor is lane -1: right of the reference line, so its span
  // runs from -width to the inner boundary at 0.
  EXPECT_DOUBLE_EQ(info.start_slide.max_offset, 0.0);
  EXPECT_LT(info.start_slide.min_offset, -1.0);
  EXPECT_DOUBLE_EQ(info.start_offset, 0.0);
  EXPECT_NEAR(info.start_slide.anchor[0], info.start_slide.max_point[0], 1e-9);
  EXPECT_NEAR(info.start_slide.anchor[1], info.start_slide.max_point[1], 1e-9);
  const double start_span =
      std::hypot(info.start_slide.max_point[0] - info.start_slide.min_point[0],
                 info.start_slide.max_point[1] - info.start_slide.min_point[1]);
  EXPECT_NEAR(start_span, -info.start_slide.min_offset, 1e-6);

  // The outgoing anchor is lane +1: left of the reference line, span [0, width].
  EXPECT_DOUBLE_EQ(info.end_slide.min_offset, 0.0);
  EXPECT_GT(info.end_slide.max_offset, 1.0);

  // The path starts and ends on the anchors.
  ASSERT_FALSE(info.path.empty());
  EXPECT_NEAR(info.path.front()[0], info.start_slide.anchor[0], 1e-6);
  EXPECT_NEAR(info.path.back()[1], info.end_slide.anchor[1], 1e-6);
}

TEST(JunctionManeuvers, ForeignJunctionExposesReadOnlyManeuvers) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  // A junction read from someone else's file has no arm list.
  network.junction(cross.junction)->arms.clear();

  EXPECT_EQ(junction_maneuvers(network, cross.junction).size(), 12U);
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;

  // Readable and lockable, but there is nothing to re-derive from.
  auto reset = roadmaker::edit::reset_maneuver(network, cross.junction, road);
  EXPECT_FALSE(reset->apply(network).has_value());
  auto rebuild = roadmaker::edit::rebuild_maneuvers(network, cross.junction);
  EXPECT_FALSE(rebuild->apply(network).has_value());
}

TEST(JunctionManeuvers, StaleJunctionYieldsNoManeuvers) {
  RoadNetwork network;
  EXPECT_TRUE(junction_maneuvers(network, JunctionId{}).empty());
}

// --- set_maneuver_locked -----------------------------------------------------

TEST(ManeuverOperations, LockRoundTripsAndErasesOnUnlock) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east)).road;
  const std::string derived = snapshot_xodr(network);

  auto lock = roadmaker::edit::set_maneuver_locked(network, cross.junction, road, true);
  ASSERT_NE(lock, nullptr);
  expect_command_round_trip(network, *lock);

  ASSERT_TRUE(lock->apply(network).has_value());
  const Maneuver* record = record_for(network, cross.junction, road);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->locked);

  // Unlocking leaves the record authoring nothing, so it is dropped entirely —
  // and the writer's drop rule makes lock-then-unlock byte-identical to never
  // having locked at all.
  auto unlock = roadmaker::edit::set_maneuver_locked(network, cross.junction, road, false);
  ASSERT_TRUE(unlock->apply(network).has_value());
  EXPECT_EQ(record_for(network, cross.junction, road), nullptr);
  expect_network_matches(network, derived);
}

TEST(ManeuverOperations, LockRejectsNoOpAndUnknownRoad) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;

  auto noop = roadmaker::edit::set_maneuver_locked(network, cross.junction, road, false);
  EXPECT_FALSE(noop->apply(network).has_value());

  auto foreign = roadmaker::edit::set_maneuver_locked(network, cross.junction, cross.west, true);
  EXPECT_FALSE(foreign->apply(network).has_value());

  auto stale = roadmaker::edit::set_maneuver_locked(network, JunctionId{}, road, true);
  EXPECT_FALSE(stale->apply(network).has_value());
}

// --- set_maneuver_turn_type --------------------------------------------------

TEST(ManeuverOperations, TurnTypeOverrideRoundTripsAndClears) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east)).road;
  const std::string derived = snapshot_xodr(network);

  auto override_left =
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, TurnType::Left);
  ASSERT_NE(override_left, nullptr);
  expect_command_round_trip(network, *override_left);
  ASSERT_TRUE(override_left->apply(network).has_value());

  const JunctionManeuverInfo overridden =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  EXPECT_EQ(overridden.computed, TurnType::Straight);
  EXPECT_EQ(overridden.effective, TurnType::Left);
  EXPECT_TRUE(overridden.overridden);
  EXPECT_TRUE(overridden.authored);

  // Setting the SAME value again is a no-op.
  auto again =
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, TurnType::Left);
  EXPECT_FALSE(again->apply(network).has_value());

  // Storing the computed type clears the override instead of pinning it, and
  // the record then authors nothing and is erased.
  auto to_computed =
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, TurnType::Straight);
  ASSERT_TRUE(to_computed->apply(network).has_value());
  EXPECT_EQ(record_for(network, cross.junction, road), nullptr);
  expect_network_matches(network, derived);
}

TEST(ManeuverOperations, ClearingAnAbsentOverrideIsAnError) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;

  auto clear = roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, std::nullopt);
  EXPECT_FALSE(clear->apply(network).has_value());
}

TEST(ManeuverOperations, ClearingAnOverrideKeepsTheLock) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;

  apply_or_throw(network,
                 roadmaker::edit::set_maneuver_locked(network, cross.junction, road, true));
  apply_or_throw(
      network,
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, TurnType::UTurn));
  apply_or_throw(
      network,
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, std::nullopt));

  const Maneuver* record = record_for(network, cross.junction, road);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->locked);
  EXPECT_FALSE(record->turn_type.has_value());
}

// --- set_maneuver_path -------------------------------------------------------

namespace {

/// A control point pushed sideways off the straight west→east maneuver.
Waypoint bulge_point(const JunctionManeuverInfo& info) {
  return Waypoint{.x = 0.5 * (info.start_slide.anchor[0] + info.end_slide.anchor[0]),
                  .y = (0.5 * (info.start_slide.anchor[1] + info.end_slide.anchor[1])) + 4.0};
}

} // namespace

TEST(ManeuverOperations, ReshapeMovesTheRoadAndLocksIt) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  const double derived_length = network.road(info.road)->length;
  const std::vector<Waypoint> points{bulge_point(info)};

  auto command = roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, points);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  // The road really moved, and length + width + elevation were rewritten
  // together (a stale length would export invalid OpenDRIVE).
  const roadmaker::Road* road = network.road(info.road);
  EXPECT_GT(road->length, derived_length);
  EXPECT_DOUBLE_EQ(road->length, road->plan_view.length());

  // The lock is implicit and lands in the SAME undo step.
  const Maneuver* record = record_for(network, cross.junction, info.road);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->locked);
  ASSERT_EQ(record->control_points.size(), 1U);
  EXPECT_DOUBLE_EQ(record->control_points.front().y, points.front().y);

  const JunctionManeuverInfo after =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  EXPECT_TRUE(after.locked);
  EXPECT_TRUE(after.authored);
  EXPECT_EQ(after.control_points.size(), 1U);
}

TEST(ManeuverOperations, ReshapeSlidesEndpointsWithinTheAnchorLane) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));

  const double inside = 0.5 * info.start_slide.min_offset;
  auto command = roadmaker::edit::set_maneuver_path(
      network, cross.junction, info.road, {}, inside, std::nullopt);
  ASSERT_TRUE(command->apply(network).has_value());
  const JunctionManeuverInfo moved =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  EXPECT_DOUBLE_EQ(moved.start_offset, inside);
  EXPECT_NEAR(moved.path.front()[1], info.start_slide.anchor[1] + inside, 1e-6);
}

TEST(ManeuverOperations, ReshapeValidatesItsInput) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));

  const std::vector<Waypoint> too_many(kMaxManeuverControlPoints + 1, Waypoint{0.0, 1.0});
  EXPECT_FALSE(roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, too_many)
                   ->apply(network)
                   .has_value());

  const std::vector<Waypoint> nonfinite{
      Waypoint{.x = std::numeric_limits<double>::quiet_NaN(), .y = 0.0}};
  EXPECT_FALSE(roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, nonfinite)
                   ->apply(network)
                   .has_value());

  // Outside the anchor lane's span.
  EXPECT_FALSE(roadmaker::edit::set_maneuver_path(
                   network, cross.junction, info.road, {}, info.start_slide.min_offset - 1.0)
                   ->apply(network)
                   .has_value());
  EXPECT_FALSE(roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, {}, 5.0)
                   ->apply(network)
                   .has_value());

  // Not a maneuver of this junction.
  EXPECT_FALSE(roadmaker::edit::set_maneuver_path(network, cross.junction, cross.north, {})
                   ->apply(network)
                   .has_value());
}

TEST(ManeuverOperations, ReshapeRejectsANoOp) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  const std::vector<Waypoint> points{bulge_point(info)};

  apply_or_throw(network,
                 roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, points));
  auto again = roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, points);
  EXPECT_FALSE(again->apply(network).has_value());
}

// --- reset_maneuver ----------------------------------------------------------

TEST(ManeuverOperations, ResetRestoresTheDerivedPath) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  const std::string derived = snapshot_xodr(network);

  const std::vector<Waypoint> bulge{bulge_point(info)};
  apply_or_throw(network,
                 roadmaker::edit::set_maneuver_path(network, cross.junction, info.road, bulge));

  auto reset = roadmaker::edit::reset_maneuver(network, cross.junction, info.road);
  ASSERT_NE(reset, nullptr);
  expect_command_round_trip(network, *reset);
  ASSERT_TRUE(reset->apply(network).has_value());

  EXPECT_EQ(record_for(network, cross.junction, info.road), nullptr);
  expect_network_matches(network, derived);
}

TEST(ManeuverOperations, ResetNeedsSomethingAuthored) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;

  EXPECT_FALSE(
      roadmaker::edit::reset_maneuver(network, cross.junction, road)->apply(network).has_value());
}

// --- U-turns -----------------------------------------------------------------

TEST(ManeuverOperations, AddUTurnCreatesTheTurnThePlannerRefuses) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const std::size_t before = network.junction(cross.junction)->connections.size();

  auto command = roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.west));
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(network.junction(cross.junction)->connections.size(), before + 1);
  const JunctionManeuverInfo uturn =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.west));
  EXPECT_TRUE(uturn.is_uturn_explicit);
  EXPECT_EQ(uturn.computed, TurnType::UTurn);
  EXPECT_TRUE(uturn.locked);
  EXPECT_TRUE(uturn.authored);

  // A second one is refused; so is a road end that is not an arm.
  EXPECT_FALSE(roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.west))
                   ->apply(network)
                   .has_value());
  const RoadId stray = author(network, {Waypoint{200.0, 200.0}, Waypoint{240.0, 200.0}}, "901");
  EXPECT_FALSE(roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(stray))
                   ->apply(network)
                   .has_value());
}

TEST(ManeuverOperations, DeletingAUTurnRoadCleansTableAndRecord) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  apply_or_throw(network,
                 roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.west)));
  const RoadId uturn =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.west)).road;

  auto command = roadmaker::edit::delete_road(network, uturn);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  const Junction* junction = network.junction(cross.junction);
  EXPECT_TRUE(std::ranges::none_of(junction->connections,
                                   [&](const roadmaker::JunctionConnection& connection) {
                                     return connection.connecting_road == uturn;
                                   }));
  EXPECT_TRUE(junction->maneuvers.empty());
}

// --- the regeneration guard --------------------------------------------------

TEST(ManeuverRegeneration, LockedGeometrySurvivesRegeneration) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo locked_info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  const JunctionManeuverInfo free_info =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.north));
  const std::vector<Waypoint> bulge{bulge_point(locked_info)};
  apply_or_throw(
      network,
      roadmaker::edit::set_maneuver_path(network, cross.junction, locked_info.road, bulge));
  const double locked_length = network.road(locked_info.road)->length;
  const double free_length = network.road(free_info.road)->length;

  apply_or_throw(network, roadmaker::edit::regenerate_junction(network, cross.junction));

  EXPECT_DOUBLE_EQ(network.road(locked_info.road)->length, locked_length);
  EXPECT_DOUBLE_EQ(network.road(free_info.road)->length, free_length);
  EXPECT_TRUE(record_for(network, cross.junction, locked_info.road)->locked);
}

TEST(ManeuverRegeneration, UnclaimedLockedRoadIsKept) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  apply_or_throw(network,
                 roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.west)));
  const RoadId uturn =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.west)).road;

  apply_or_throw(network, roadmaker::edit::regenerate_junction(network, cross.junction));

  // The planner never emits a same-arm turn, so the U-turn is unclaimed — and
  // kept only because its maneuver is locked.
  ASSERT_NE(network.road(uturn), nullptr);
  const Junction* junction = network.junction(cross.junction);
  EXPECT_EQ(junction->connections.size(), 13U);
  EXPECT_TRUE(std::ranges::any_of(junction->connections,
                                  [&](const roadmaker::JunctionConnection& connection) {
                                    return connection.connecting_road == uturn;
                                  }));
  // ...and it is still the LAST entry, so repeated regenerations are stable.
  EXPECT_EQ(junction->connections.back().connecting_road, uturn);
}

TEST(ManeuverRegeneration, InPlaceOnlySurvivesAKeptUTurn) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  apply_or_throw(network,
                 roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.west)));

  // The preview path: the connection COUNT no longer matches the plan's turn
  // count, but no turn appears and none is dropped, so it must still succeed.
  auto command =
      roadmaker::edit::regenerate_junction(network, cross.junction, {}, TurnSetPolicy::InPlaceOnly);
  EXPECT_TRUE(command->apply(network).has_value());
}

TEST(ManeuverRegeneration, RebuildClearsGeometryButKeepsOverrides) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  const JunctionManeuverInfo shaped =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.east));
  const JunctionManeuverInfo labelled =
      maneuver_between(network, cross.junction, end_of(cross.west), end_of(cross.north));
  const double derived_length = network.road(shaped.road)->length;

  const std::vector<Waypoint> bulge{bulge_point(shaped)};
  apply_or_throw(network,
                 roadmaker::edit::set_maneuver_path(network, cross.junction, shaped.road, bulge));
  apply_or_throw(network,
                 roadmaker::edit::set_maneuver_turn_type(
                     network, cross.junction, labelled.road, TurnType::UTurn));
  apply_or_throw(network,
                 roadmaker::edit::add_uturn_maneuver(network, cross.junction, end_of(cross.south)));
  const RoadId uturn =
      maneuver_between(network, cross.junction, end_of(cross.south), end_of(cross.south)).road;

  auto rebuild = roadmaker::edit::rebuild_maneuvers(network, cross.junction);
  ASSERT_NE(rebuild, nullptr);
  expect_command_round_trip(network, *rebuild);
  ASSERT_TRUE(rebuild->apply(network).has_value());

  // Geometry is derived again and the geometric record is gone...
  EXPECT_DOUBLE_EQ(network.road(shaped.road)->length, derived_length);
  EXPECT_EQ(record_for(network, cross.junction, shaped.road), nullptr);
  // ...the semantic override survives...
  const Maneuver* kept = record_for(network, cross.junction, labelled.road);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->turn_type, TurnType::UTurn);
  EXPECT_FALSE(kept->locked);
  // ...and the explicit U-turn, which no plan contains, is dropped with its
  // record (no phantom rm:maneuver entry left behind).
  EXPECT_EQ(network.road(uturn), nullptr);
  EXPECT_EQ(network.junction(cross.junction)->maneuvers.size(), 1U);
}

TEST(ManeuverRegeneration, RebuildNeedsSomethingToRebuild) {
  RoadNetwork network;
  const Cross cross = make_cross(network);
  EXPECT_FALSE(
      roadmaker::edit::rebuild_maneuvers(network, cross.junction)->apply(network).has_value());

  // A lone turn-type override survives a rebuild, so it is not a reason to run
  // one.
  const RoadId road = junction_maneuvers(network, cross.junction).front().road;
  apply_or_throw(
      network,
      roadmaker::edit::set_maneuver_turn_type(network, cross.junction, road, TurnType::UTurn));
  EXPECT_FALSE(
      roadmaker::edit::rebuild_maneuvers(network, cross.junction)->apply(network).has_value());
}
