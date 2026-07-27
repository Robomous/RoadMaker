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

// cascade-s2 (#462): a junction follows every move gesture, not just a node
// drag.
//
// The contract these tests hold to is docs/domain/connection_contract.md
// §junction regeneration on move. In one sentence: a moved ARM regenerates its
// junction, a junction whose every arm moves is carried RIGIDLY, foreign and
// locked junctions are left alone, a CONNECTING road is refused, and a move that
// leaves a junction unbuildable is refused with the network untouched.
//
// edit::verify_junction_welds is the oracle throughout — #403 gave it the
// elevation and grade dimensions, so a junction that regenerates into a vertical
// step now fails the check instead of passing it.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::Maneuver;
using roadmaker::Road;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadLink;
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

/// A real three-arm junction: west, east and south roads meeting at the origin,
/// each pointing INTO the junction so every arm's contact is its End.
struct TJunction {
  RoadId west;
  RoadId east;
  RoadId south;
  std::array<RoadEnd, 3> ends;
  JunctionId junction;
};

TJunction make_junction(RoadNetwork& network) {
  TJunction t{};
  t.west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  t.east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  t.south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  t.ends = {RoadEnd{.road = t.west, .contact = ContactPoint::End},
            RoadEnd{.road = t.east, .contact = ContactPoint::End},
            RoadEnd{.road = t.south, .contact = ContactPoint::End}};
  auto create = roadmaker::edit::create_junction(network, t.ends);
  if (!create->apply(network).has_value()) {
    throw std::runtime_error("create_junction failed");
  }
  network.for_each_junction([&t](JunctionId id, const Junction&) { t.junction = id; });
  return t;
}

std::vector<RoadId> arms_of(const TJunction& t) {
  return {t.west, t.east, t.south};
}

/// The connecting roads a junction owns, in arena order.
std::vector<RoadId> connecting_roads(const RoadNetwork& network, JunctionId junction) {
  std::vector<RoadId> roads;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.junction == junction) {
      roads.push_back(id);
    }
  });
  return roads;
}

/// Every geometry record of every named road, so a rigid transform can be
/// checked record by record rather than through the file bytes.
std::vector<std::vector<roadmaker::GeometryRecord>> records_of(const RoadNetwork& network,
                                                               std::span<const RoadId> roads) {
  std::vector<std::vector<roadmaker::GeometryRecord>> all;
  all.reserve(roads.size());
  for (const RoadId id : roads) {
    const auto records = network.road(id)->plan_view.records();
    all.emplace_back(records.begin(), records.end());
  }
  return all;
}

void expect_welds_hold(const RoadNetwork& network, JunctionId junction) {
  const auto welds = roadmaker::edit::verify_junction_welds(network, junction);
  ASSERT_TRUE(welds.has_value()) << welds.error().message;
  EXPECT_FALSE(welds->breaches) << "pos=" << welds->max_position_gap
                                << " hdg=" << welds->max_heading_gap
                                << " z=" << welds->max_elevation_gap
                                << " grade=" << welds->max_grade_gap;
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

} // namespace

// --- the rigid case ----------------------------------------------------------

// Translating a junction together with EVERY one of its arms is a rigid body
// motion. It cannot invalidate anything, which is precisely why the old blanket
// refusal was indiscriminate rather than merely conservative.
TEST(JunctionCascade, TranslatingAJunctionWithEveryArmIsRigid) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> before_connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(before_connecting.empty());
  const std::size_t before_connections = network.junction(t.junction)->connections.size();

  const std::vector<RoadId> arms = arms_of(t);
  auto command = roadmaker::edit::translate_roads(network, arms, 25.0, -10.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_welds_hold(network, t.junction);
  EXPECT_EQ(network.junction(t.junction)->connections.size(), before_connections);
  // Carried, not replanned: the very same connecting roads, in the same order.
  EXPECT_EQ(connecting_roads(network, t.junction), before_connecting);
  // The stage decided about this junction — the editor must not decide again.
  EXPECT_TRUE(command->dirty().junctions_are_current);
  // And the connecting roads actually moved, rather than being left behind.
  for (const RoadId road : before_connecting) {
    const auto& dirty_roads = command->dirty().roads;
    EXPECT_NE(std::ranges::find(dirty_roads, road), dirty_roads.end())
        << "a carried connecting road must be re-meshed";
  }
}

// The acceptance in its strictest form: the file after a rigid move is the file
// before it, TRANSLATED — every connecting-road geometry record shifted by
// exactly (dx, dy) with its heading, length and shape untouched. A regeneration
// would replan each turn from scratch and could not promise that.
//
// Byte-identity is deliberately NOT the oracle here. Translating out and back
// does not reproduce the same doubles — (-8.361136669640441 + 30) - 30 is
// -8.36113666964044 — so a byte compare would be testing IEEE cancellation
// rather than the guarantee. The undo path, which restores from value snapshots
// and IS byte-identical, is covered by UndoIsByteIdentical... below.
TEST(JunctionCascade, ARigidMoveIsTheSameJunctionTranslated) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(connecting.empty());
  const std::vector<std::vector<roadmaker::GeometryRecord>> before =
      records_of(network, connecting);

  constexpr double kDx = 30.0;
  constexpr double kDy = 12.0;
  const std::vector<RoadId> arms = arms_of(t);
  ASSERT_TRUE(
      roadmaker::edit::translate_roads(network, arms, kDx, kDy)->apply(network).has_value());

  const std::vector<std::vector<roadmaker::GeometryRecord>> after = records_of(network, connecting);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    ASSERT_EQ(after[i].size(), before[i].size()) << "connecting road " << i << " was replanned";
    for (std::size_t r = 0; r < before[i].size(); ++r) {
      EXPECT_NEAR(after[i][r].x, before[i][r].x + kDx, 1e-9);
      EXPECT_NEAR(after[i][r].y, before[i][r].y + kDy, 1e-9);
      // Untouched, not merely close: a translation changes position and nothing
      // else, so these must reproduce exactly.
      EXPECT_DOUBLE_EQ(after[i][r].hdg, before[i][r].hdg);
      EXPECT_DOUBLE_EQ(after[i][r].length, before[i][r].length);
    }
  }
}

// The rotation half of the same guarantee: every connecting-road record turns
// about the pivot and its heading advances by the angle, while lengths and
// shapes are untouched — the rotation is rigid, so arcs and spirals need no
// coefficient edit.
TEST(JunctionCascade, ARigidRotationIsTheSameJunctionTurned) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(connecting.empty());
  const std::vector<std::vector<roadmaker::GeometryRecord>> before =
      records_of(network, connecting);

  constexpr double kAngle = std::numbers::pi / 5.0;
  const std::vector<RoadId> arms = arms_of(t);
  auto command = roadmaker::edit::rotate_roads(network, arms, kAngle, 0.0, 0.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_welds_hold(network, t.junction);
  EXPECT_EQ(connecting_roads(network, t.junction), connecting);
  EXPECT_TRUE(command->dirty().junctions_are_current);

  const std::vector<std::vector<roadmaker::GeometryRecord>> after = records_of(network, connecting);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    ASSERT_EQ(after[i].size(), before[i].size()) << "connecting road " << i << " was replanned";
    for (std::size_t r = 0; r < before[i].size(); ++r) {
      const double cos_a = std::cos(kAngle);
      const double sin_a = std::sin(kAngle);
      EXPECT_NEAR(after[i][r].x, (cos_a * before[i][r].x) - (sin_a * before[i][r].y), 1e-9);
      EXPECT_NEAR(after[i][r].y, (sin_a * before[i][r].x) + (cos_a * before[i][r].y), 1e-9);
      EXPECT_NEAR(
          std::remainder(after[i][r].hdg - before[i][r].hdg - kAngle, 2.0 * std::numbers::pi),
          0.0,
          1e-9);
      EXPECT_DOUBLE_EQ(after[i][r].length, before[i][r].length);
    }
  }
}

// THE WORLD-COORDINATE TRAP. A JunctionCorner stores radii and setbacks, a
// StopLine a setback and a flip, a SurfaceSpan a flag and a sort index — all
// frame-independent, all carried for free. Maneuver::control_points are plain
// world x/y, so a hand-shaped turn is the one authored record that stays behind
// unless the transform moves it explicitly.
TEST(JunctionCascade, ARigidMoveCarriesLockedManeuverControlPoints) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(connecting.empty());

  // Hand-shape one turn: an interior control point, stored in world space.
  const std::array<Waypoint, 1> path{Waypoint{.x = 1.0, .y = -1.0}};
  auto shape = roadmaker::edit::set_maneuver_path(network, t.junction, connecting.front(), path);
  ASSERT_TRUE(shape->apply(network).has_value()) << "set_maneuver_path";
  const auto authored =
      std::ranges::find_if(network.junction(t.junction)->maneuvers,
                           [&](const Maneuver& m) { return m.road == connecting.front(); });
  ASSERT_NE(authored, network.junction(t.junction)->maneuvers.end());
  ASSERT_EQ(authored->control_points.size(), 1U);
  // set_maneuver_path locks the maneuver, which is what makes this the case that
  // distinguishes CARRYING a junction from REGENERATING it: a regeneration
  // honours the lock (ManeuverPolicy::Respect) and keeps this connecting road's
  // plan view verbatim, so the road would stay behind while its arms walked off.
  ASSERT_TRUE(authored->locked);
  const auto path_start = network.road(connecting.front())->plan_view.evaluate(0.0);

  const std::vector<RoadId> arms = arms_of(t);
  ASSERT_TRUE(
      roadmaker::edit::translate_roads(network, arms, 100.0, 50.0)->apply(network).has_value());

  const auto moved =
      std::ranges::find_if(network.junction(t.junction)->maneuvers,
                           [&](const Maneuver& m) { return m.road == connecting.front(); });
  ASSERT_NE(moved, network.junction(t.junction)->maneuvers.end());
  ASSERT_EQ(moved->control_points.size(), 1U);
  EXPECT_DOUBLE_EQ(moved->control_points.front().x, 101.0);
  EXPECT_DOUBLE_EQ(moved->control_points.front().y, 49.0);
  // The hand-shaped road itself moved with them, rather than being frozen in
  // place by its own lock.
  const auto path_after = network.road(connecting.front())->plan_view.evaluate(0.0);
  EXPECT_NEAR(path_after.x, path_start.x + 100.0, 1e-9);
  EXPECT_NEAR(path_after.y, path_start.y + 50.0, 1e-9);
}

// --- the partial case --------------------------------------------------------

// One arm moves, the junction regenerates from the new arm poses — exactly what
// a node drag has always done, now reachable through the gesture that used to
// refuse outright.
TEST(JunctionCascade, TranslatingASubsetOfArmsRegeneratesTheJunction) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> before_connecting = connecting_roads(network, t.junction);
  const std::size_t before_connections = network.junction(t.junction)->connections.size();
  const std::vector<std::vector<roadmaker::GeometryRecord>> before =
      records_of(network, before_connecting);

  const std::array<RoadId, 1> one{t.south};
  auto command = roadmaker::edit::translate_roads(network, one, 3.0, -4.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_welds_hold(network, t.junction);
  // The turn SET did not change, so #263's keyed matching must keep every
  // connecting road's identity — held references and the undo stack depend on it.
  EXPECT_EQ(network.junction(t.junction)->connections.size(), before_connections);
  EXPECT_EQ(connecting_roads(network, t.junction), before_connecting);
  EXPECT_TRUE(command->dirty().junctions_are_current);

  // And the geometry DID change: this is a regeneration, not a no-op. Only the
  // turns touching the moved arm move — a west→east through-turn is unaffected
  // by the south arm shifting — so the assertion is that SOME turn was replanned,
  // not that all of them were.
  const std::vector<std::vector<roadmaker::GeometryRecord>> after =
      records_of(network, before_connecting);
  ASSERT_EQ(after.size(), before.size());
  bool any_replanned = false;
  for (std::size_t i = 0; i < before.size() && !any_replanned; ++i) {
    any_replanned =
        after[i].size() != before[i].size() ||
        std::ranges::any_of(std::views::iota(std::size_t{0}, before[i].size()), [&](std::size_t r) {
          return after[i][r].x != before[i][r].x || after[i][r].y != before[i][r].y ||
                 after[i][r].hdg != before[i][r].hdg;
        });
  }
  EXPECT_TRUE(any_replanned) << "the junction did not follow the moved arm";
}

TEST(JunctionCascade, RotatingAnArmRegeneratesTheJunction) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> before_connecting = connecting_roads(network, t.junction);

  // Rotate the south arm about its own free end, so the junction mouth swings
  // but stays well within reach.
  auto command =
      roadmaker::edit::rotate_road(network, t.south, std::numbers::pi / 24.0, 0.0, -40.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_welds_hold(network, t.junction);
  EXPECT_EQ(connecting_roads(network, t.junction), before_connecting);
  EXPECT_TRUE(command->dirty().junctions_are_current);
}

TEST(JunctionCascade, UndoIsByteIdenticalIncludingJunctionGeometry) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::array<RoadId, 1> one{t.south};
  auto command = roadmaker::edit::translate_roads(network, one, 2.0, -3.0);
  expect_command_round_trip(network, *command);
}

// --- refusals ----------------------------------------------------------------

// The documented failure policy: refuse, say why, and leave the network
// untouched. Not dissolve — that would destroy hand-authored maneuvers, corners
// and stop lines to satisfy a gesture the user can simply undo — and not
// skip-and-report, which is the stale junction this sprint exists to remove.
TEST(JunctionCascade, AMoveThatCannotRegenerateIsRefusedAndLeavesTheNetworkUntouched) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::string before = snapshot_xodr(network);

  // Well past JunctionGenOptions::max_end_distance_m (50 m).
  const std::array<RoadId, 1> one{t.south};
  auto command = roadmaker::edit::translate_roads(network, one, 0.0, -400.0);
  const auto applied = command->apply(network);
  ASSERT_FALSE(applied.has_value());
  // "road ends are 406.0 m apart" on its own is not actionable — the sentence
  // has to name the junction the user must go and look at.
  const std::string named = fmt::format("junction {}", network.junction(t.junction)->odr_id);
  EXPECT_NE(applied.error().message.find(named), std::string::npos) << applied.error().message;
  expect_network_matches(network, before);
}

// A connecting road's pose is generated from the arms: moving it by hand means
// nothing, because the next regeneration overwrites it. This is the ONE junction
// refusal cascade-s2 keeps.
TEST(JunctionCascade, AConnectingRoadIsStillRefused) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  const std::vector<RoadId> connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(connecting.empty());
  const std::string before = snapshot_xodr(network);

  const auto expect_refused = [&](std::unique_ptr<Command> command) {
    const auto applied = command->apply(network);
    ASSERT_FALSE(applied.has_value());
    EXPECT_NE(applied.error().message.find("connecting road"), std::string::npos)
        << applied.error().message;
    expect_network_matches(network, before);
  };
  expect_refused(roadmaker::edit::translate_road(network, connecting.front(), 1.0, 1.0));
  expect_refused(roadmaker::edit::rotate_road(network, connecting.front(), 0.2, 0.0, 0.0));
}

// --- the junctions left alone ------------------------------------------------

// A junction read from someone else's file has no recorded arms, so there is
// nothing to regenerate FROM. The move must not fail because of it.
TEST(JunctionCascade, AForeignJunctionsArmMovesWithoutRegenerating) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{40.0, 0.0}}, "1");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->successor = RoadLink{.target = junction, .contact = ContactPoint::Start};
  ASSERT_TRUE(network.junction(junction)->arms.empty());

  auto command = roadmaker::edit::translate_road(network, road, 5.0, 5.0);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.road(road)->plan_view.evaluate(0.0).x, 5.0);
}

// #319: the user asked for the hand-tuned result to survive edits to the arms.
// The lock binds the automatic loops, and stage [2] is one of them.
TEST(JunctionCascade, ALockedJunctionKeepsItsGeometryWhenAnArmMoves) {
  RoadNetwork network;
  const TJunction t = make_junction(network);
  ASSERT_TRUE(
      roadmaker::edit::set_junction_locked(network, t.junction, true)->apply(network).has_value());
  const std::vector<RoadId> connecting = connecting_roads(network, t.junction);
  ASSERT_FALSE(connecting.empty());
  // EVERY connecting road, not just the first: the turns that do not touch the
  // moved arm would not move under a regeneration either, so checking one of
  // those proves nothing at all.
  const std::vector<std::vector<roadmaker::GeometryRecord>> before =
      records_of(network, connecting);

  const std::array<RoadId, 1> one{t.south};
  ASSERT_TRUE(
      roadmaker::edit::translate_roads(network, one, 2.0, -2.0)->apply(network).has_value());

  const std::vector<std::vector<roadmaker::GeometryRecord>> after = records_of(network, connecting);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    ASSERT_EQ(after[i].size(), before[i].size()) << "connecting road " << i;
    for (std::size_t r = 0; r < before[i].size(); ++r) {
      EXPECT_DOUBLE_EQ(after[i][r].x, before[i][r].x)
          << "a locked junction must keep its hand-tuned connections";
      EXPECT_DOUBLE_EQ(after[i][r].y, before[i][r].y);
      EXPECT_DOUBLE_EQ(after[i][r].hdg, before[i][r].hdg);
      EXPECT_DOUBLE_EQ(after[i][r].length, before[i][r].length);
    }
  }
}

// --- one test per gesture ----------------------------------------------------

// The point of the funnel: four different gestures, one rule. If any of these
// regresses, some gesture has grown its own opinion about junctions again —
// which is exactly the state cascade-s2 found the kernel in.
TEST(JunctionCascade, NoGestureLeavesTheJunctionStale) {
  using Gesture = std::function<std::unique_ptr<Command>(RoadNetwork&, const TJunction&)>;
  std::vector<std::pair<const char*, Gesture>> gestures;
  gestures.emplace_back("node drag", [](RoadNetwork& net, const TJunction& t) {
    // The far (free) node of the south arm, so the junction mouth moves with it.
    return roadmaker::edit::move_waypoint(net, t.south, 0, Waypoint{.x = 4.0, .y = -44.0});
  });
  gestures.emplace_back("translate", [](RoadNetwork& net, const TJunction& t) {
    const std::array<RoadId, 1> one{t.south};
    return roadmaker::edit::translate_roads(net, one, 2.0, -3.0);
  });
  gestures.emplace_back("rotate", [](RoadNetwork& net, const TJunction& t) {
    return roadmaker::edit::rotate_road(net, t.south, std::numbers::pi / 30.0, 0.0, -40.0);
  });
  // #403 shipped this one: set_elevation_profile — what both the Z gizmo and the
  // Profile panel push — dirties junctions_touching. cascade-s2 does not re-land
  // it; it guards it, because an elevation edit that stopped dirtying the
  // junction would put a silent vertical step back at every contact.
  gestures.emplace_back("elevation", [](RoadNetwork& net, const TJunction& t) {
    const std::vector<roadmaker::edit::ElevationPoint> profile{
        roadmaker::edit::ElevationPoint{.s = 0.0, .z = 3.0, .grade = 0.0},
        roadmaker::edit::ElevationPoint{.s = net.road(t.south)->length, .z = 0.0, .grade = 0.0}};
    return roadmaker::edit::set_elevation_profile(net, t.south, profile);
  });

  for (const auto& [name, gesture] : gestures) {
    RoadNetwork network;
    const TJunction t = make_junction(network);
    auto command = gesture(network, t);
    ASSERT_TRUE(command->apply(network).has_value()) << name;

    // Either the command regenerated the junction itself, or it declared the
    // junction dirty so its consumer will. Silence is the failure mode.
    const roadmaker::edit::DirtySet dirty = command->dirty();
    const bool named = std::ranges::find(dirty.junctions, t.junction) != dirty.junctions.end();
    EXPECT_TRUE(named || dirty.junctions_are_current)
        << name << ": the junction was left neither regenerated nor marked dirty";
    if (dirty.junctions_are_current) {
      expect_welds_hold(network, t.junction);
    }
  }
}
