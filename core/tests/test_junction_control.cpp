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
// WP3 adds the MEMBERSHIP operations on top: add_junction_arm,
// remove_junction_arm and merge_junctions all run through the same retarget
// engine regeneration does, so they satisfy the same oracle — and they carry
// the p4-s1/p4-s3 dormancy contract (authored corners and stop lines are never
// stripped when an arm leaves, and travel with an absorbed junction).
// WP4 extends it further with the span-creation op.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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
using roadmaker::edit::add_junction_arm;
using roadmaker::edit::Command;
using roadmaker::edit::merge_junctions;
using roadmaker::edit::remove_junction_arm;
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
///
/// `arm_count` trims the junction to its first N arms so a membership test has
/// a spare road to add (3 ⇒ north is authored but is NOT an arm).
struct CrossFixture {
  RoadNetwork network;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;
  JunctionId junction;

  explicit CrossFixture(std::size_t arm_count = 4) {
    west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
    ends.resize(arm_count);
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

/// Two NEIGHBOURING junctions — every arm end of both lies well inside the
/// generator's 50 m proximity limit, which is what makes them mergeable.
///
///   left  (3 arms):  west end (-15,0), south end (0,-15), north end (0,15)
///   right (2 arms):  east end ( 15,0), ne end    (15,15)
struct MergeFixture {
  RoadNetwork network;
  RoadId west;
  RoadId south;
  RoadId north;
  RoadId east;
  RoadId north_east;
  JunctionId left;
  JunctionId right;

  MergeFixture() {
    west = author(network, {Waypoint{-90.0, 0.0}, Waypoint{-15.0, 0.0}}, "1");
    south = author(network, {Waypoint{0.0, -90.0}, Waypoint{0.0, -15.0}}, "2");
    north = author(network, {Waypoint{0.0, 90.0}, Waypoint{0.0, 15.0}}, "3");
    east = author(network, {Waypoint{90.0, 0.0}, Waypoint{15.0, 0.0}}, "4");
    north_east = author(network, {Waypoint{90.0, 90.0}, Waypoint{15.0, 15.0}}, "5");
    left = make(std::vector<RoadEnd>{end_of(west), end_of(south), end_of(north)});
    right = make(std::vector<RoadEnd>{end_of(east), end_of(north_east)});
  }

  JunctionId make(const std::vector<RoadEnd>& ends) {
    std::vector<JunctionId> before;
    network.for_each_junction([&](JunctionId id, const Junction&) { before.push_back(id); });
    auto command = roadmaker::edit::create_junction(network, ends);
    if (command == nullptr) {
      throw std::runtime_error("create_junction: null command");
    }
    auto applied = command->apply(network);
    if (!applied.has_value()) {
      throw std::runtime_error("create_junction: " + applied.error().message);
    }
    JunctionId made;
    network.for_each_junction([&](JunctionId id, const Junction&) {
      if (std::ranges::find(before, id) == before.end()) {
        made = id;
      }
    });
    return made;
  }
};

/// The connecting-road ids a junction's table currently names.
std::vector<RoadId> connecting_roads(const RoadNetwork& network, JunctionId junction) {
  std::vector<RoadId> ids;
  for (const auto& connection : network.junction(junction)->connections) {
    ids.push_back(connection.connecting_road);
  }
  return ids;
}

bool has_corner(const Junction& junction, const RoadEnd& arm_a, const RoadEnd& arm_b) {
  return std::ranges::any_of(junction.corners, [&](const roadmaker::JunctionCorner& corner) {
    return corner.arm_a == arm_a && corner.arm_b == arm_b;
  });
}

bool has_stopline(const Junction& junction, const RoadEnd& arm) {
  return std::ranges::any_of(junction.stoplines,
                             [&](const roadmaker::StopLine& line) { return line.arm == arm; });
}

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

// --- membership: add_junction_arm -------------------------------------------

TEST(JunctionControl, AddingAnArmRoundTrips) {
  CrossFixture fixture(3);
  fixture.lock();
  const RoadEnd arm = end_of(fixture.north);

  auto command = add_junction_arm(fixture.network, fixture.junction, arm);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Add Junction Arm");
  EXPECT_TRUE(command->dirty().topology) << "the arm list and the turn set both changed";
  EXPECT_TRUE(command->dirty().junctions_are_current)
      << "the retarget IS the generator — regenerating on top of it would re-plan";

  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.arms.size(), 4U);
  EXPECT_NE(std::ranges::find(junction.arms, arm), junction.arms.end());
  // The arm's link slot now names the junction, so junction_at_end finds it.
  EXPECT_EQ(roadmaker::edit::junction_at_end(fixture.network, arm), fixture.junction);
}

TEST(JunctionControl, AddingAnArmKeepsTheSurvivingConnectingRoadIds) {
  CrossFixture fixture(3);
  fixture.lock();
  const std::vector<RoadId> before = connecting_roads(fixture.network, fixture.junction);
  ASSERT_FALSE(before.empty());

  auto command = add_junction_arm(fixture.network, fixture.junction, end_of(fixture.north));
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(fixture.network).has_value());

  const std::vector<RoadId> after = connecting_roads(fixture.network, fixture.junction);
  EXPECT_GT(after.size(), before.size()) << "the new arm opened turns";
  for (const RoadId road : before) {
    EXPECT_NE(std::ranges::find(after, road), after.end())
        << "a turn that survived must keep its connecting-road id (TurnKey matching)";
    EXPECT_NE(fixture.network.road(road), nullptr);
  }
}

TEST(JunctionControl, AddingAnArmToAnUnlockedJunctionIsRejected) {
  CrossFixture fixture(3);
  expect_command_rejected(
      fixture.network, add_junction_arm(fixture.network, fixture.junction, end_of(fixture.north)));
}

TEST(JunctionControl, AddingAnArmTwiceIsRejected) {
  CrossFixture fixture;
  fixture.lock();
  expect_command_rejected(
      fixture.network, add_junction_arm(fixture.network, fixture.junction, end_of(fixture.west)));
}

TEST(JunctionControl, AddingAnEndOwnedByAnotherJunctionIsRejected) {
  MergeFixture fixture;
  auto lock = set_junction_locked(fixture.network, fixture.left, true);
  ASSERT_TRUE(lock->apply(fixture.network).has_value());
  // end_of(east) is an arm of `right` — the single-owner rule refuses it here.
  expect_command_rejected(fixture.network,
                          add_junction_arm(fixture.network, fixture.left, end_of(fixture.east)));
}

TEST(JunctionControl, AddingAnArmFartherThanTheProximityLimitIsRejected) {
  CrossFixture fixture(3);
  fixture.lock();
  // Push the spare road's free end far away; the union of arm ends then breaks
  // the 50 m pairwise rule the planner enforces.
  auto move =
      roadmaker::edit::move_waypoint(fixture.network, fixture.north, 1, Waypoint{0.0, 300.0});
  ASSERT_TRUE(move->apply(fixture.network).has_value());
  expect_command_rejected(
      fixture.network, add_junction_arm(fixture.network, fixture.junction, end_of(fixture.north)));
}

TEST(JunctionControl, AddingAnArmToASpanJunctionIsRejected) {
  RoadNetwork network;
  const RoadId main = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const RoadId side = author(network, {Waypoint{60.0, 80.0}, Waypoint{60.0, 20.0}}, "2");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = main, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;

  expect_command_rejected(network, add_junction_arm(network, junction, end_of(side)));
  expect_command_rejected(network, remove_junction_arm(network, junction, end_of(side)));
}

TEST(JunctionControl, AddingAnArmToAForeignJunctionIsRejected) {
  CrossFixture fixture(3);
  const JunctionId foreign = fixture.network.create_junction("99", "foreign");
  expect_command_rejected(fixture.network,
                          add_junction_arm(fixture.network, foreign, end_of(fixture.north)));
  expect_command_rejected(fixture.network,
                          remove_junction_arm(fixture.network, foreign, end_of(fixture.north)));
}

TEST(JunctionControl, AddingAnArmToAStaleJunctionIsRejected) {
  CrossFixture fixture(3);
  expect_command_rejected(fixture.network,
                          add_junction_arm(fixture.network, JunctionId{}, end_of(fixture.north)));
  expect_command_rejected(
      fixture.network, remove_junction_arm(fixture.network, JunctionId{}, end_of(fixture.north)));
}

// --- membership: remove_junction_arm ----------------------------------------

TEST(JunctionControl, RemovingAnArmRoundTrips) {
  CrossFixture fixture;
  fixture.lock();
  const RoadEnd arm = end_of(fixture.north);

  auto command = remove_junction_arm(fixture.network, fixture.junction, arm);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Remove Junction Arm");
  EXPECT_TRUE(command->dirty().topology);

  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.arms.size(), 3U);
  EXPECT_EQ(std::ranges::find(junction.arms, arm), junction.arms.end());
  // Its link slot is free again, so the end could join another junction.
  EXPECT_FALSE(roadmaker::edit::junction_at_end(fixture.network, arm).has_value());
  EXPECT_FALSE(fixture.network.road(fixture.north)->successor.has_value());
}

TEST(JunctionControl, RemovingAnArmLeavesItsAuthoredCornerAndStopLineDormant) {
  CrossFixture fixture;
  fixture.lock();
  const RoadEnd arm = end_of(fixture.north);
  const RoadEnd neighbour = end_of(fixture.east);
  {
    Junction& junction = *fixture.network.junction(fixture.junction);
    junction.corners.push_back(
        roadmaker::JunctionCorner{.arm_a = arm, .arm_b = neighbour, .radius = 7.5});
    junction.stoplines.push_back(roadmaker::StopLine{.arm = arm, .distance = 9.0});
  }

  auto command = remove_junction_arm(fixture.network, fixture.junction, arm);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(fixture.network, *command);
  ASSERT_TRUE(command->apply(fixture.network).has_value());

  // Dormancy contract (p4-s1 corners, p4-s3 stop lines): the records keyed by
  // the departed arm STAY on the junction so re-adding the arm brings the
  // authored work back.
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_TRUE(has_corner(junction, arm, neighbour))
      << "the corner override must go dormant, not die";
  EXPECT_TRUE(has_stopline(junction, arm)) << "the stop-line override must go dormant, not die";
}

TEST(JunctionControl, RemovingAnArmDownToOneIsRejected) {
  MergeFixture fixture;
  auto lock = set_junction_locked(fixture.network, fixture.right, true);
  ASSERT_TRUE(lock->apply(fixture.network).has_value());
  ASSERT_EQ(fixture.network.junction(fixture.right)->arms.size(), 2U);
  expect_command_rejected(
      fixture.network, remove_junction_arm(fixture.network, fixture.right, end_of(fixture.east)));
}

TEST(JunctionControl, RemovingANonArmIsRejected) {
  CrossFixture fixture(3);
  fixture.lock();
  expect_command_rejected(
      fixture.network,
      remove_junction_arm(fixture.network, fixture.junction, end_of(fixture.north)));
}

TEST(JunctionControl, RemovingAnArmFromAnUnlockedJunctionIsRejected) {
  CrossFixture fixture;
  expect_command_rejected(
      fixture.network,
      remove_junction_arm(fixture.network, fixture.junction, end_of(fixture.north)));
}

// --- membership: merge_junctions --------------------------------------------

TEST(JunctionControl, MergingTwoNeighbouringJunctionsRoundTrips) {
  MergeFixture fixture;
  const std::string survivor_odr = fixture.network.junction(fixture.left)->odr_id;

  auto command = merge_junctions(fixture.network, fixture.left, fixture.right);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Merge Junctions");
  EXPECT_TRUE(command->dirty().topology);

  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const Junction* survivor = fixture.network.junction(fixture.left);
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(fixture.network.junction(fixture.right), nullptr) << "the absorbed junction is gone";
  EXPECT_EQ(survivor->odr_id, survivor_odr) << "the survivor keeps its identity";
  EXPECT_EQ(survivor->arms.size(), 5U) << "the union of both arm lists";
  EXPECT_TRUE(survivor->locked) << "a hand-authored merge must not be re-derived away";

  // Nothing references the erased junction any more (#311): every arm points at
  // the survivor, and so does every connecting road.
  for (const RoadEnd& arm : survivor->arms) {
    EXPECT_EQ(roadmaker::edit::junction_at_end(fixture.network, arm), fixture.left);
  }
  fixture.network.for_each_road([&](RoadId, const roadmaker::Road& road) {
    if (road.junction.is_valid()) {
      EXPECT_EQ(road.junction, fixture.left);
    }
  });
}

TEST(JunctionControl, UndoingAMergeRestoresBothJunctions) {
  MergeFixture fixture;
  const std::string before = snapshot_xodr(fixture.network);

  auto command = merge_junctions(fixture.network, fixture.left, fixture.right);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(fixture.network).has_value());
  ASSERT_EQ(fixture.network.junction(fixture.right), nullptr);

  ASSERT_TRUE(command->revert(fixture.network).has_value());
  // erase_exact/restore is restore-in-place: the absorbed junction comes back
  // under its OWN id, not a recycled one.
  ASSERT_NE(fixture.network.junction(fixture.right), nullptr);
  EXPECT_EQ(fixture.network.junction(fixture.right)->arms.size(), 2U);
  expect_network_matches(fixture.network, before);
}

TEST(JunctionControl, MergingCarriesTheAbsorbedCornersAndStopLines) {
  MergeFixture fixture;
  const RoadEnd arm = end_of(fixture.north_east);
  const RoadEnd neighbour = end_of(fixture.east);
  {
    Junction& absorbed = *fixture.network.junction(fixture.right);
    absorbed.corners.push_back(
        roadmaker::JunctionCorner{.arm_a = arm, .arm_b = neighbour, .radius = 6.25});
    absorbed.stoplines.push_back(roadmaker::StopLine{.arm = arm, .distance = 11.0});
  }

  auto command = merge_junctions(fixture.network, fixture.left, fixture.right);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(fixture.network, *command);
  ASSERT_TRUE(command->apply(fixture.network).has_value());

  const Junction& survivor = *fixture.network.junction(fixture.left);
  EXPECT_TRUE(has_corner(survivor, arm, neighbour)) << "authored corners carry across the merge";
  EXPECT_TRUE(has_stopline(survivor, arm)) << "authored stop lines carry across the merge";
}

TEST(JunctionControl, MergingAJunctionWithItselfIsRejected) {
  MergeFixture fixture;
  expect_command_rejected(fixture.network,
                          merge_junctions(fixture.network, fixture.left, fixture.left));
}

TEST(JunctionControl, MergingJunctionsFartherApartThanTheProximityLimitIsRejected) {
  MergeFixture fixture;
  // Drag the right junction's arms far east. Neither junction is locked, but
  // nothing regenerates here — the arm ends simply move out of reach.
  for (const RoadId road : {fixture.east, fixture.north_east}) {
    auto move = roadmaker::edit::move_waypoint(fixture.network, road, 1, Waypoint{400.0, 0.0});
    ASSERT_NE(move, nullptr);
    ASSERT_TRUE(move->apply(fixture.network).has_value());
  }
  expect_command_rejected(fixture.network,
                          merge_junctions(fixture.network, fixture.left, fixture.right));
}

TEST(JunctionControl, MergingAStaleOrForeignOrSpanJunctionIsRejected) {
  MergeFixture fixture;
  expect_command_rejected(fixture.network,
                          merge_junctions(fixture.network, fixture.left, JunctionId{}));
  expect_command_rejected(fixture.network,
                          merge_junctions(fixture.network, JunctionId{}, fixture.left));

  const JunctionId foreign = fixture.network.create_junction("98", "foreign");
  expect_command_rejected(fixture.network, merge_junctions(fixture.network, fixture.left, foreign));
  expect_command_rejected(fixture.network, merge_junctions(fixture.network, foreign, fixture.left));

  const JunctionId span = fixture.network.create_junction("97", "crosswalk");
  fixture.network.junction(span)->spans.push_back(
      SpanArm{.road = fixture.west, .s_start = 10.0, .s_end = 16.5});
  fixture.network.junction(span)->locked = true;
  expect_command_rejected(fixture.network, merge_junctions(fixture.network, fixture.left, span));
  expect_command_rejected(fixture.network, merge_junctions(fixture.network, span, fixture.left));
}

// --- span (virtual) junction creation ---------------------------------------

namespace {

/// Two straight parallel through roads — the substrate a span junction covers.
/// Neither is cut and neither belongs to a junction, which is exactly what
/// ASAM OpenDRIVE 1.9.0 §12.7 means by "the main road is uninterrupted".
struct SpanFixture {
  RoadNetwork network;
  RoadId north;
  RoadId south;

  SpanFixture() {
    north = author(network, {Waypoint{0.0, 6.0}, Waypoint{120.0, 6.0}}, "1");
    south = author(network, {Waypoint{0.0, -6.0}, Waypoint{120.0, -6.0}}, "2");
  }

  double length(RoadId road) const { return network.road(road)->length; }
};

/// The only junction the network holds (the span suites build exactly one).
JunctionId sole_junction(const RoadNetwork& network) {
  JunctionId found;
  network.for_each_junction([&](JunctionId id, const Junction&) { found = id; });
  return found;
}

/// Any connecting road of an already-generated junction.
RoadId a_connecting_road(const RoadNetwork& network) {
  RoadId found;
  network.for_each_road([&](RoadId id, const roadmaker::Road& road) {
    if (road.junction.is_valid()) {
      found = id;
    }
  });
  return found;
}

std::vector<SpanArm> one_span(RoadId road) {
  return {SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5}};
}

} // namespace

TEST(JunctionControl, CreatingASingleRoadSpanJunctionRoundTrips) {
  SpanFixture fixture;
  auto command = roadmaker::edit::create_span_junction(fixture.network, one_span(fixture.north));
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Create Span Junction");
  // Nothing is derived behind a span junction, so the editor must not
  // regenerate on top of what was just created.
  EXPECT_TRUE(command->dirty().junctions_are_current);
  EXPECT_TRUE(command->dirty().topology);

  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const JunctionId made = sole_junction(fixture.network);
  ASSERT_TRUE(made.is_valid());
  // The created id is reported so the editor can select it.
  const std::vector<JunctionId> dirty_junctions = command->dirty().junctions;
  EXPECT_NE(std::ranges::find(dirty_junctions, made), dirty_junctions.end());

  const Junction& junction = *fixture.network.junction(made);
  ASSERT_EQ(junction.spans.size(), 1U);
  EXPECT_EQ(junction.spans.front().road, fixture.north);
  EXPECT_DOUBLE_EQ(junction.spans.front().s_start, 50.0);
  EXPECT_DOUBLE_EQ(junction.spans.front().s_end, 56.5);
  // §12.7: the main road is uninterrupted — no arms, no connections, no links.
  EXPECT_TRUE(junction.locked) << "a span junction is locked structurally";
  EXPECT_TRUE(junction.arms.empty());
  EXPECT_TRUE(junction.connections.empty());
  EXPECT_FALSE(fixture.network.road(fixture.north)->predecessor.has_value());
  EXPECT_FALSE(fixture.network.road(fixture.north)->successor.has_value());
  EXPECT_FALSE(fixture.network.road(fixture.north)->junction.is_valid());
  // Only the junction was created: the two authored roads are still all there is.
  std::size_t roads = 0;
  fixture.network.for_each_road([&](RoadId, const roadmaker::Road&) { ++roads; });
  EXPECT_EQ(roads, 2U);
}

TEST(JunctionControl, CreatingAParallelRoadSpanJunctionRoundTrips) {
  SpanFixture fixture;
  const std::vector<SpanArm> spans{SpanArm{.road = fixture.north, .s_start = 40.0, .s_end = 44.0},
                                   SpanArm{.road = fixture.south, .s_start = 41.5, .s_end = 45.5}};
  auto command = roadmaker::edit::create_span_junction(fixture.network, spans);
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(fixture.network, *command);

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const Junction& junction = *fixture.network.junction(sole_junction(fixture.network));
  ASSERT_EQ(junction.spans.size(), 2U);
  EXPECT_EQ(junction.spans[1].road, fixture.south);
  EXPECT_DOUBLE_EQ(junction.spans[1].s_end, 45.5);
  EXPECT_TRUE(junction.locked);
}

TEST(JunctionControl, RedoingASpanJunctionReusesItsId) {
  SpanFixture fixture;
  auto command = roadmaker::edit::create_span_junction(fixture.network, one_span(fixture.north));
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const JunctionId first = sole_junction(fixture.network);

  ASSERT_TRUE(command->revert(fixture.network).has_value());
  EXPECT_FALSE(sole_junction(fixture.network).is_valid());

  ASSERT_TRUE(command->apply(fixture.network).has_value());
  // restore-in-place: redo resurrects the junction under its ORIGINAL id, so
  // references held elsewhere (the selection, the undo stack) stay valid.
  EXPECT_EQ(sole_junction(fixture.network), first);
  EXPECT_EQ(fixture.network.junction(first)->spans.size(), 1U);
}

TEST(JunctionControl, ACreatedSpanJunctionCannotBeUnlocked) {
  SpanFixture fixture;
  auto command = roadmaker::edit::create_span_junction(fixture.network, one_span(fixture.north));
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(fixture.network).has_value());
  const JunctionId made = sole_junction(fixture.network);
  expect_command_rejected(fixture.network, set_junction_locked(fixture.network, made, false));
}

TEST(JunctionControl, ACreatedSpanJunctionSurvivesSaveReloadSave) {
  SpanFixture fixture;
  const std::vector<SpanArm> spans{SpanArm{.road = fixture.north, .s_start = 40.0, .s_end = 44.0},
                                   SpanArm{.road = fixture.south, .s_start = 41.5, .s_end = 45.5}};
  auto command = roadmaker::edit::create_span_junction(fixture.network, spans);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(fixture.network).has_value());

  // The op's output has to survive the WP1 persistence untouched, or the
  // command is authoring something the file cannot carry.
  const auto xml = roadmaker::write_xodr(fixture.network, "span");
  ASSERT_TRUE(xml.has_value()) << xml.error().message;
  EXPECT_NE(xml->find("type=\"virtual\""), std::string::npos) << *xml;

  auto reparsed = roadmaker::parse_xodr(*xml, "span");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty())
      << "never write what you cannot read back; first: " << reparsed->diagnostics.front().message;
  const auto again = roadmaker::write_xodr(reparsed->network, "span");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *xml) << "save->reload->save must be byte-identical";

  EXPECT_TRUE(roadmaker::validate_network(fixture.network).empty());
}

// --- span rejections --------------------------------------------------------

TEST(JunctionControl, ASpanJunctionNeedsOneOrTwoSpans) {
  SpanFixture fixture;
  expect_command_rejected(fixture.network,
                          roadmaker::edit::create_span_junction(fixture.network, {}));

  const RoadId third = author(fixture.network, {Waypoint{0.0, 18.0}, Waypoint{120.0, 18.0}}, "3");
  const std::vector<SpanArm> three{SpanArm{.road = fixture.north, .s_start = 40.0, .s_end = 44.0},
                                   SpanArm{.road = fixture.south, .s_start = 40.0, .s_end = 44.0},
                                   SpanArm{.road = third, .s_start = 40.0, .s_end = 44.0}};
  expect_command_rejected(fixture.network,
                          roadmaker::edit::create_span_junction(fixture.network, three));
}

TEST(JunctionControl, AnInvertedOrDegenerateSpanIsRejected) {
  SpanFixture fixture;
  const auto reject = [&](double s_start, double s_end) {
    const std::vector<SpanArm> spans{
        SpanArm{.road = fixture.north, .s_start = s_start, .s_end = s_end}};
    expect_command_rejected(fixture.network,
                            roadmaker::edit::create_span_junction(fixture.network, spans));
  };
  reject(60.0, 40.0);                                 // inverted
  reject(40.0, 40.0);                                 // zero length
  reject(40.0, 40.0 + roadmaker::tol::kLength / 2.0); // below the length tolerance
}

TEST(JunctionControl, ASpanOutsideItsRoadIsRejected) {
  SpanFixture fixture;
  const double length = fixture.length(fixture.north);
  const auto reject = [&](double s_start, double s_end) {
    const std::vector<SpanArm> spans{
        SpanArm{.road = fixture.north, .s_start = s_start, .s_end = s_end}};
    expect_command_rejected(fixture.network,
                            roadmaker::edit::create_span_junction(fixture.network, spans));
  };
  reject(-5.0, 10.0);                 // before the start of the road
  reject(length - 5.0, length + 5.0); // past the end of the road
}

TEST(JunctionControl, TheSameRoadTwiceIsRejected) {
  SpanFixture fixture;
  const std::vector<SpanArm> spans{SpanArm{.road = fixture.north, .s_start = 40.0, .s_end = 44.0},
                                   SpanArm{.road = fixture.north, .s_start = 60.0, .s_end = 64.0}};
  expect_command_rejected(fixture.network,
                          roadmaker::edit::create_span_junction(fixture.network, spans));
}

TEST(JunctionControl, AConnectingRoadCannotCarryASpan) {
  CrossFixture fixture;
  const RoadId connecting = a_connecting_road(fixture.network);
  ASSERT_TRUE(connecting.is_valid());
  const std::vector<SpanArm> spans{SpanArm{
      .road = connecting, .s_start = 0.0, .s_end = fixture.network.road(connecting)->length}};
  expect_command_rejected(fixture.network,
                          roadmaker::edit::create_span_junction(fixture.network, spans));
}

TEST(JunctionControl, AStaleSpanRoadIsRejected) {
  SpanFixture fixture;
  expect_command_rejected(
      fixture.network, roadmaker::edit::create_span_junction(fixture.network, one_span(RoadId{})));
}
