// Kernel tests for junction CONTROL — the lock that keeps a hand-tuned
// junction out of the automatic regeneration loops (p4-s4, issue #319).
//
// edit::set_junction_locked is a junction value edit in every case but one, so
// it runs through the §8 oracle: apply changes the document, revert restores it
// BYTE-identically. The exception is unlocking a junction whose arms no longer
// plan: there is no automatic state to hand back to, so the command performs
// the full §7 removal (junction + connecting roads) instead — and that path has
// to satisfy the same oracle, undo included.
//
// WP3/WP4 extend this file with the membership operations (add/remove arm,
// retarget, merge) and the span-creation op.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::SpanArm;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::set_junction_locked;
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

/// A failed apply must leave the serialized network untouched.
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
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
struct CrossFixture {
  RoadNetwork network;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;
  JunctionId junction;

  CrossFixture() {
    west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
    auto command = roadmaker::edit::create_junction(network, ends);
    if (command == nullptr) {
      throw std::runtime_error("create_junction: null command");
    }
    auto applied = command->apply(network);
    if (!applied.has_value()) {
      throw std::runtime_error("create_junction: " + applied.error().message);
    }
    network.for_each_junction([&](JunctionId id, const Junction&) { junction = id; });
  }

  /// Locks the junction outside the command under test (the fixture step for
  /// every unlock case).
  void lock() {
    auto command = set_junction_locked(network, junction, true);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->apply(network).has_value());
    ASSERT_TRUE(network.junction(junction)->locked);
  }
};

} // namespace

// --- locking ----------------------------------------------------------------

TEST(JunctionControl, LockingIsAValueEditThatRoundTrips) {
  CrossFixture fixture;
  ASSERT_FALSE(fixture.network.junction(fixture.junction)->locked);

  auto command = set_junction_locked(fixture.network, fixture.junction, true);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Lock Junction");
  // The turn set is untouched, so the editor must NOT regenerate on top of it.
  EXPECT_TRUE(command->dirty().junctions_are_current);
  ASSERT_EQ(command->dirty().junctions.size(), 1U);
  EXPECT_EQ(command->dirty().junctions.front(), fixture.junction);

  expect_command_round_trip(fixture.network, *command);

  // Round-trip ends reverted; apply once more to observe the flag itself.
  ASSERT_TRUE(command->apply(fixture.network).has_value());
  EXPECT_TRUE(fixture.network.junction(fixture.junction)->locked);
}

TEST(JunctionControl, LockingKeepsTheConnectingRoadsIntact) {
  CrossFixture fixture;
  const std::size_t connections = fixture.network.junction(fixture.junction)->connections.size();
  ASSERT_GT(connections, 0U);
  fixture.lock();
  EXPECT_EQ(fixture.network.junction(fixture.junction)->connections.size(), connections);
}

// --- unlocking --------------------------------------------------------------

TEST(JunctionControl, UnlockingADerivableJunctionIsAValueEditThatRoundTrips) {
  CrossFixture fixture;
  fixture.lock();

  auto command = set_junction_locked(fixture.network, fixture.junction, false);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Unlock Junction");
  // The junction rejoins the automatic loop, so the editor re-derives it inside
  // the same undo macro.
  EXPECT_FALSE(command->dirty().junctions_are_current);
  ASSERT_EQ(command->dirty().junctions.size(), 1U);
  EXPECT_EQ(command->dirty().junctions.front(), fixture.junction);

  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  EXPECT_FALSE(fixture.network.junction(fixture.junction)->locked);
}

TEST(JunctionControl, UnlockingAJunctionThatCannotBeDerivedRemovesIt) {
  CrossFixture fixture;
  fixture.lock();

  // Drag the west arm's far node out of reach: with the junction locked nothing
  // regenerates, so the arm list survives but no longer plans (the arm ends are
  // farther apart than the junction limit).
  auto move = roadmaker::edit::move_waypoint(
      fixture.network, fixture.west, 1, Waypoint{.x = -400.0, .y = 0.0});
  ASSERT_NE(move, nullptr);
  ASSERT_TRUE(move->apply(fixture.network).has_value());
  ASSERT_TRUE(fixture.network.junction(fixture.junction)->locked)
      << "a locked junction must not have been regenerated by the move";

  const std::size_t roads_before = [&] {
    std::size_t count = 0;
    fixture.network.for_each_road([&](RoadId, const roadmaker::Road&) { ++count; });
    return count;
  }();

  auto command = set_junction_locked(fixture.network, fixture.junction, false);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Unlock Junction");
  EXPECT_TRUE(command->dirty().topology) << "a removal is topology";

  // Same oracle as the value edits — undo must restore the junction AND every
  // connecting road byte-identically.
  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  EXPECT_EQ(fixture.network.junction(fixture.junction), nullptr) << "the junction is gone";
  std::size_t roads_after = 0;
  fixture.network.for_each_road([&](RoadId, const roadmaker::Road&) { ++roads_after; });
  EXPECT_LT(roads_after, roads_before) << "its connecting roads went with it";
}

// --- rejections -------------------------------------------------------------

TEST(JunctionControl, LockingALockedJunctionIsRejected) {
  CrossFixture fixture;
  fixture.lock();
  expect_command_rejected(fixture.network,
                          set_junction_locked(fixture.network, fixture.junction, true));
}

TEST(JunctionControl, UnlockingAnUnlockedJunctionIsRejected) {
  CrossFixture fixture;
  expect_command_rejected(fixture.network,
                          set_junction_locked(fixture.network, fixture.junction, false));
}

TEST(JunctionControl, AForeignJunctionHasNoAutomaticRegenerationToLock) {
  CrossFixture fixture;
  // No arms and no spans: exactly what a junction read from someone else's
  // file looks like.
  const JunctionId foreign = fixture.network.create_junction("99", "foreign");
  ASSERT_TRUE(fixture.network.junction(foreign)->arms.empty());
  ASSERT_TRUE(fixture.network.junction(foreign)->spans.empty());
  expect_command_rejected(fixture.network, set_junction_locked(fixture.network, foreign, true));
}

TEST(JunctionControl, ASpanJunctionCannotBeUnlocked) {
  RoadNetwork network;
  const RoadId road = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;

  expect_command_rejected(network, set_junction_locked(network, junction, false));
  // Locking it is a no-op instead — it is already locked by construction.
  expect_command_rejected(network, set_junction_locked(network, junction, true));
}

TEST(JunctionControl, AStaleJunctionIdIsRejected) {
  CrossFixture fixture;
  expect_command_rejected(fixture.network,
                          set_junction_locked(fixture.network, JunctionId{}, true));
}

// --- the regeneration gate ---------------------------------------------------

TEST(JunctionControl, DraggingAnArmOfALockedJunctionDoesNotFollow) {
  CrossFixture fixture;
  fixture.lock();
  const std::string before_junction = [&] {
    // Capture the connecting roads' geometry through the serialization, the
    // only byte-exact handle the oracle has.
    return snapshot_xodr(fixture.network);
  }();

  auto command = roadmaker::edit::move_waypoint_following_junctions(
      fixture.network, fixture.west, 0, Waypoint{.x = -80.0, .y = 14.0});
  ASSERT_NE(command, nullptr);
  // The locked junction is skipped, so this degrades to a plain move — same
  // name, and undo restores everything.
  EXPECT_EQ(command->name(), "Move Waypoint");
  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  EXPECT_NE(snapshot_xodr(fixture.network), before_junction) << "the arm itself did move";
  // The connecting roads did not follow: regenerating now would change them.
  auto regen = roadmaker::edit::regenerate_junction(fixture.network, fixture.junction);
  ASSERT_NE(regen, nullptr);
  const std::string stale = snapshot_xodr(fixture.network);
  ASSERT_TRUE(regen->apply(fixture.network).has_value())
      << "regenerate_junction must ignore the lock";
  EXPECT_NE(snapshot_xodr(fixture.network), stale);
}
