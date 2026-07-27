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

// Linked-neighbour follow on move (cascade-s1, #461).
//
// A move that changes a road end takes its linked neighbour with it, or severs
// the link and SAYS SO. The oracle throughout is edit::verify_link_weld, which
// #403 built for exactly this: the assertion is "the joint still satisfies the
// connection contract", not a hand-compared coordinate.
//
// Every fixture here is GENUINELY coincident and GENUINELY welded (close_gap),
// because a link that is merely declared between two roads 100 m apart cannot
// test continuity — it has none to preserve.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/follow.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::Road;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadLink;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::FollowOutcome;
using roadmaker::edit::FollowRecord;
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

/// The four contact combinations, mirroring test_connection.cpp. #398 hid in
/// three of them, which is why every behavioural claim here sweeps all four.
const std::array<std::pair<ContactPoint, ContactPoint>, 4> kAllContactCombos{{
    {ContactPoint::End, ContactPoint::Start}, // the common chain
    {ContactPoint::End, ContactPoint::End},
    {ContactPoint::Start, ContactPoint::Start},
    {ContactPoint::Start, ContactPoint::End},
}};

std::string combo_label(ContactPoint a, ContactPoint b) {
  return std::string("a=") + (a == ContactPoint::End ? "End" : "Start") +
         " b=" + (b == ContactPoint::End ? "End" : "Start");
}

/// A linear profile reaching `z` with `grade` (dz/ds along the road's OWN +s) at
/// `contact` — the fixture states the contract's inputs directly.
void linear_profile(
    RoadNetwork& network, RoadId road, ContactPoint contact, double z, double grade) {
  if (std::abs(z) < 1e-12 && std::abs(grade) < 1e-12) {
    return; // leave it flat: the shipped "no <elevationProfile>" convention
  }
  const double length = network.road(road)->plan_view.length();
  const double station = contact == ContactPoint::End ? length : 0.0;
  auto cmd = roadmaker::edit::set_elevation_profile(
      network,
      road,
      {roadmaker::edit::ElevationPoint{
           .s = 0.0, .z = z + (grade * (0.0 - station)), .grade = grade},
       roadmaker::edit::ElevationPoint{
           .s = length, .z = z + (grade * (length - station)), .grade = grade}});
  if (!cmd->apply(network).has_value()) {
    throw std::runtime_error("set_elevation_profile");
  }
}

struct Pair {
  RoadId a;
  RoadId b;
};

/// Two roads meeting at (100, 0) and WELDED there by close_gap, so the joint is
/// real: position, heading, elevation and grade all continuous before the move.
///
/// `z`/`grade_a` describe the joint in road A's frame; road B is given the grade
/// that makes the folded sum zero, i.e. the one the contract calls continuous.
Pair welded_pair(RoadNetwork& network,
                 ContactPoint contact_a,
                 ContactPoint contact_b,
                 double z = 0.0,
                 double grade_a = 0.0) {
  const RoadId a =
      author(network,
             contact_a == ContactPoint::End ? std::vector<Waypoint>{{0.0, 0.0}, {100.0, 0.0}}
                                            : std::vector<Waypoint>{{100.0, 0.0}, {0.0, 0.0}},
             "1");
  const RoadId b =
      author(network,
             contact_b == ContactPoint::Start ? std::vector<Waypoint>{{100.0, 0.0}, {200.0, 0.0}}
                                              : std::vector<Waypoint>{{200.0, 0.0}, {100.0, 0.0}},
             "2");
  const double grade_b = -roadmaker::edit::grade_sign_into(contact_a) *
                         roadmaker::edit::grade_sign_into(contact_b) * grade_a;
  linear_profile(network, a, contact_a, z, grade_a);
  linear_profile(network, b, contact_b, z, grade_b);
  auto weld = roadmaker::edit::close_gap(network, RoadEnd{a, contact_a}, RoadEnd{b, contact_b});
  if (!weld->apply(network).has_value()) {
    throw std::runtime_error("close_gap refused a fixture that should weld");
  }
  return Pair{a, b};
}

/// Every road-to-road joint in the network satisfies the contract. This is the
/// "no silently stale link" sweep: a gesture may sever a link, but it may never
/// leave one that no longer describes the geometry.
void expect_no_stale_links(const RoadNetwork& network) {
  network.for_each_road([&](RoadId id, const Road& road) {
    for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
      const auto& link = contact == ContactPoint::Start ? road.predecessor : road.successor;
      if (!link.has_value() || std::get_if<RoadId>(&link->target) == nullptr) {
        continue;
      }
      const auto weld = roadmaker::edit::verify_link_weld(network, RoadEnd{id, contact});
      if (!weld.has_value()) {
        continue; // junction-owned: verify_junction_welds' business
      }
      EXPECT_FALSE(weld->breaches)
          << "road " << road.odr_id << (contact == ContactPoint::Start ? " start" : " end")
          << " is stale: " << weld->max_position_gap << " m, " << weld->max_heading_gap << " rad, "
          << weld->max_elevation_gap << " m z, " << weld->max_grade_gap << " grade";
    }
  });
}

void expect_joint_holds(const RoadNetwork& network, const RoadEnd& end) {
  const auto weld = roadmaker::edit::verify_link_weld(network, end);
  ASSERT_TRUE(weld.has_value()) << weld.error().message;
  EXPECT_LE(weld->max_position_gap, roadmaker::tol::kWeldPosition);
  EXPECT_LE(weld->max_heading_gap, roadmaker::tol::kWeldHeading);
  EXPECT_LE(weld->max_elevation_gap, roadmaker::tol::kWeldElevation);
  EXPECT_LE(weld->max_grade_gap, roadmaker::tol::kWeldGrade);
  EXPECT_FALSE(weld->breaches);
}

/// The §8 round-trip oracle, as the move tests spell it.
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

std::vector<FollowRecord> severed(const Command& command) {
  std::vector<FollowRecord> out;
  for (const FollowRecord& record : command.follow_records()) {
    if (record.outcome == FollowOutcome::Severed) {
      out.push_back(record);
    }
  }
  return out;
}

/// The serialized <road> element with this odr id — a byte-level "this road did
/// not move" assertion.
std::string road_block(const std::string& xml, const std::string& odr_id) {
  const std::string key = "<road ";
  for (std::size_t at = xml.find(key); at != std::string::npos; at = xml.find(key, at + 1)) {
    const std::size_t end = xml.find("</road>", at);
    const std::string block = xml.substr(at, end - at);
    if (block.find("id=\"" + odr_id + "\"") != std::string::npos) {
      return block;
    }
  }
  return {};
}

} // namespace

// --- the combination matrix ---------------------------------------------------

TEST(LinkFollow, TranslateTakesTheNeighbourWithIt) {
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    SCOPED_TRACE(combo_label(contact_a, contact_b));
    RoadNetwork network;
    const Pair pair = welded_pair(network, contact_a, contact_b);

    auto command = roadmaker::edit::translate_road(network, pair.a, 5.0, 7.0);
    ASSERT_TRUE(command->apply(network).has_value());

    // The link survives on both sides, and the joint still holds.
    expect_joint_holds(network, RoadEnd{pair.a, contact_a});
    expect_joint_holds(network, RoadEnd{pair.b, contact_b});
    expect_no_stale_links(network);
    EXPECT_TRUE(severed(*command).empty());
    // The neighbour re-meshes: its geometry changed.
    const auto& dirty = command->dirty().roads;
    EXPECT_NE(std::ranges::find(dirty, pair.b), dirty.end());
  }
}

TEST(LinkFollow, RotateTakesTheNeighbourWithIt) {
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    SCOPED_TRACE(combo_label(contact_a, contact_b));
    RoadNetwork network;
    const Pair pair = welded_pair(network, contact_a, contact_b);

    // Rotate about the road's own far end so the joint swings but stays near.
    auto command = roadmaker::edit::rotate_road(
        network, pair.a, 0.20, contact_a == ContactPoint::End ? 0.0 : 100.0, 0.0);
    ASSERT_TRUE(command->apply(network).has_value());

    expect_joint_holds(network, RoadEnd{pair.a, contact_a});
    expect_joint_holds(network, RoadEnd{pair.b, contact_b});
    expect_no_stale_links(network);
    EXPECT_TRUE(severed(*command).empty());
  }
}

TEST(LinkFollow, MovingAWaypointTakesTheNeighbourWithIt) {
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    SCOPED_TRACE(combo_label(contact_a, contact_b));
    RoadNetwork network;
    const Pair pair = welded_pair(network, contact_a, contact_b);

    // Drag the JOINT waypoint itself: position and heading both change there.
    const std::size_t joint_index =
        contact_a == ContactPoint::End
            ? roadmaker::edit::effective_waypoints(*network.road(pair.a)).size() - 1
            : 0;
    auto command = roadmaker::edit::move_waypoint(
        network, pair.a, joint_index, Waypoint{.x = 108.0, .y = 6.0});
    ASSERT_TRUE(command->apply(network).has_value());

    expect_joint_holds(network, RoadEnd{pair.a, contact_a});
    expect_joint_holds(network, RoadEnd{pair.b, contact_b});
    expect_no_stale_links(network);
    EXPECT_TRUE(severed(*command).empty());
  }
}

TEST(LinkFollow, TheElevationBoundaryFollowsToo) {
  // A planar translate is NOT enough to test this: it leaves the moved end's own
  // z alone, so a follow that never touched elevation would still look right.
  // Stretching the road from its far end DOES move the joint's height — the
  // profile it already carries now evaluates at a different station — and that
  // is the case a boundary node has to chase.
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    SCOPED_TRACE(combo_label(contact_a, contact_b));
    RoadNetwork network;
    // A joint 3 m up at a 4 % grade — the half a planar-only follow would miss.
    const Pair pair = welded_pair(network, contact_a, contact_b, 3.0, 0.04);
    ASSERT_NO_FATAL_FAILURE(expect_joint_holds(network, RoadEnd{pair.a, contact_a}));
    const auto before = roadmaker::edit::contact_state(network, RoadEnd{pair.a, contact_a});
    ASSERT_TRUE(before.has_value());

    const std::size_t far_index = contact_a == ContactPoint::End ? 0 : 1;
    auto command =
        roadmaker::edit::move_waypoint(network, pair.a, far_index, Waypoint{.x = -40.0, .y = 30.0});
    ASSERT_TRUE(command->apply(network).has_value());

    const auto moved = roadmaker::edit::contact_state(network, RoadEnd{pair.a, contact_a});
    const auto followed = roadmaker::edit::contact_state(network, RoadEnd{pair.b, contact_b});
    ASSERT_TRUE(moved.has_value());
    ASSERT_TRUE(followed.has_value());
    if (contact_a == ContactPoint::End) {
      // Non-vacuity: the joint's height really did move, so matching it means
      // something. (At a Start contact s = 0 is fixed, so z cannot move.)
      EXPECT_GT(std::abs(moved->z - before->z), 0.5)
          << "the fixture must actually change the joint's height";
    }
    EXPECT_NEAR(followed->z, moved->z, roadmaker::tol::kWeldElevation);
    expect_joint_holds(network, RoadEnd{pair.a, contact_a});
    expect_no_stale_links(network);
  }
}

TEST(LinkFollow, AMoveLeavesNoCoincidenceFinding) {
  RoadNetwork network;
  const Pair pair = welded_pair(network, ContactPoint::End, ContactPoint::Start);
  (void)pair;
  // Control: the welded fixture is clean before anything moves.
  const auto before = roadmaker::validate_network(network);
  const auto with_rule = [](const std::vector<roadmaker::Diagnostic>& findings,
                            std::string_view rule) {
    std::size_t count = 0;
    for (const roadmaker::Diagnostic& finding : findings) {
      count += static_cast<std::size_t>(finding.rule_id == rule);
    }
    return count;
  };
  ASSERT_EQ(with_rule(before, roadmaker::rules::kLinkEndsCoincide), 0U);

  auto command =
      roadmaker::edit::move_waypoint(network, pair.a, 1, Waypoint{.x = 140.0, .y = 20.0});
  ASSERT_TRUE(command->apply(network).has_value());

  const auto after = roadmaker::validate_network(network);
  EXPECT_EQ(with_rule(after, roadmaker::rules::kLinkEndsCoincide), 0U);
  EXPECT_EQ(with_rule(after, roadmaker::rules::kLinkElevationContinuity), 0U);
}

// --- undo -----------------------------------------------------------------------

TEST(LinkFollow, UndoRestoresTheNeighbourByteIdentically) {
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    SCOPED_TRACE(combo_label(contact_a, contact_b));
    RoadNetwork network;
    const Pair pair = welded_pair(network, contact_a, contact_b, 3.0, 0.04);
    auto command = roadmaker::edit::translate_road(network, pair.a, 6.0, 4.0);
    ASSERT_NO_FATAL_FAILURE(expect_command_round_trip(network, *command));
  }
}

// --- the one-hop bound ----------------------------------------------------------

TEST(LinkFollow, TheHopStopsAtTheFirstNeighbour) {
  // A—B—C, every joint real. Moving A must refit B and leave C ALONE, and B's
  // far joint must survive that: the follower's far end is locked in position,
  // heading, z and grade, so the joint beyond it cannot move.
  RoadNetwork network;
  const RoadId a = author(network, {{0.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId b = author(network, {{100.0, 0.0}, {200.0, 0.0}}, "2");
  const RoadId c = author(network, {{200.0, 0.0}, {300.0, 0.0}}, "3");
  ASSERT_TRUE(roadmaker::edit::close_gap(
                  network, RoadEnd{a, ContactPoint::End}, RoadEnd{b, ContactPoint::Start})
                  ->apply(network)
                  .has_value());
  ASSERT_TRUE(roadmaker::edit::close_gap(
                  network, RoadEnd{b, ContactPoint::End}, RoadEnd{c, ContactPoint::Start})
                  ->apply(network)
                  .has_value());

  const std::string before = snapshot_xodr(network);
  auto command = roadmaker::edit::translate_road(network, a, 0.0, 8.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_joint_holds(network, RoadEnd{a, ContactPoint::End});
  expect_joint_holds(network, RoadEnd{b, ContactPoint::End});
  expect_no_stale_links(network);

  const std::string after = snapshot_xodr(network);
  EXPECT_NE(road_block(before, "2"), road_block(after, "2")) << "B should have followed";
  EXPECT_EQ(road_block(before, "3"), road_block(after, "3"))
      << "C is two hops out and must be untouched, byte for byte";
  const auto& dirty = command->dirty().roads;
  EXPECT_EQ(std::ranges::find(dirty, c), dirty.end()) << "C should not even be dirty";
}

// --- sever ----------------------------------------------------------------------

TEST(LinkFollow, AnImpossibleRefitSeversAndSaysWhy) {
  RoadNetwork network;
  const Pair pair = welded_pair(network, ContactPoint::End, ContactPoint::Start);
  // A lane section 90 m along B: any re-fit that leaves B shorter than that is
  // refused rather than forced (refit's own long-standing guard).
  ASSERT_TRUE(
      roadmaker::edit::split_lane_section(network, pair.b, 90.0)->apply(network).has_value());

  // Push A's end almost all the way to B's far end: B would have to shrink to
  // ~5 m, taking its lane section past its own end.
  auto command = roadmaker::edit::translate_road(network, pair.a, 95.0, 0.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<FollowRecord> cut = severed(*command);
  ASSERT_EQ(cut.size(), 1U);
  EXPECT_EQ(cut.front().moved, (RoadEnd{pair.a, ContactPoint::End}));
  EXPECT_EQ(cut.front().neighbour, (RoadEnd{pair.b, ContactPoint::Start}));
  EXPECT_FALSE(cut.front().reason.empty()) << "a sever with no reason is a silent sever";
  EXPECT_NE(cut.front().reason.find("lane section"), std::string::npos) << cut.front().reason;

  // Cleared on BOTH sides — a half-link is worse than no link.
  EXPECT_FALSE(network.road(pair.a)->successor.has_value());
  EXPECT_FALSE(network.road(pair.b)->predecessor.has_value());
  expect_no_stale_links(network);
}

TEST(LinkFollow, ASeverUndoesByteIdentically) {
  RoadNetwork network;
  const Pair pair = welded_pair(network, ContactPoint::End, ContactPoint::Start);
  ASSERT_TRUE(
      roadmaker::edit::split_lane_section(network, pair.b, 90.0)->apply(network).has_value());
  auto command = roadmaker::edit::translate_road(network, pair.a, 95.0, 0.0);
  ASSERT_NO_FATAL_FAILURE(expect_command_round_trip(network, *command));
}

TEST(LinkFollow, NoGestureLeavesASilentlyStaleLink) {
  // The blanket claim, swept over every gesture and every contact combination:
  // afterwards each link either still describes the geometry or is gone AND
  // named in the command's report. Nothing is left quietly wrong.
  for (const auto& [contact_a, contact_b] : kAllContactCombos) {
    for (int gesture = 0; gesture < 3; ++gesture) {
      SCOPED_TRACE(combo_label(contact_a, contact_b) + " gesture=" + std::to_string(gesture));
      RoadNetwork network;
      const Pair pair = welded_pair(network, contact_a, contact_b, 2.0, 0.03);
      const bool linked_before = network.road(pair.a)->predecessor.has_value() ||
                                 network.road(pair.a)->successor.has_value();
      ASSERT_TRUE(linked_before);

      std::unique_ptr<Command> command;
      switch (gesture) {
      case 0:
        command = roadmaker::edit::translate_road(network, pair.a, 30.0, 25.0);
        break;
      case 1:
        command = roadmaker::edit::rotate_road(
            network, pair.a, 0.6, contact_a == ContactPoint::End ? 0.0 : 100.0, 0.0);
        break;
      default:
        command = roadmaker::edit::move_waypoint(network,
                                                 pair.a,
                                                 contact_a == ContactPoint::End ? 1 : 0,
                                                 Waypoint{.x = 130.0, .y = 30.0});
        break;
      }
      ASSERT_TRUE(command->apply(network).has_value());
      expect_no_stale_links(network);

      const auto& slot = contact_a == ContactPoint::Start ? network.road(pair.a)->predecessor
                                                          : network.road(pair.a)->successor;
      if (!slot.has_value()) {
        EXPECT_FALSE(severed(*command).empty())
            << "the link vanished without the command reporting it";
      }
    }
  }
}

// --- junction arms ---------------------------------------------------------------

TEST(LinkFollow, AFollowedJunctionArmMarksItsJunctionDirty) {
  RoadNetwork network;
  // Road 1 is a junction ARM: its END is welded into the junction, its START is
  // free and welded to road 4, the road we move.
  const RoadId arm = author(network, {{100.0, 0.0}, {200.0, 0.0}}, "1");
  const RoadId connecting = author(network, {{200.0, 0.0}, {240.0, 0.0}}, "2");
  const JunctionId junction = network.create_junction("100", "X");
  network.road(arm)->successor = RoadLink{.target = junction, .contact = ContactPoint::Start};
  Road& inner = *network.road(connecting);
  inner.junction = junction;
  inner.predecessor = RoadLink{.target = arm, .contact = ContactPoint::End};
  network.junction(junction)->connections.push_back(
      JunctionConnection{.incoming_road = arm,
                         .connecting_road = connecting,
                         .contact_point = ContactPoint::Start,
                         .lane_links = {{-1, -1}}});

  const RoadId approach = author(network, {{0.0, 0.0}, {100.0, 0.0}}, "4");
  ASSERT_TRUE(roadmaker::edit::close_gap(
                  network, RoadEnd{approach, ContactPoint::End}, RoadEnd{arm, ContactPoint::Start})
                  ->apply(network)
                  .has_value());

  auto command = roadmaker::edit::translate_road(network, approach, 0.0, 5.0);
  ASSERT_TRUE(command->apply(network).has_value());

  expect_joint_holds(network, RoadEnd{approach, ContactPoint::End});
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_NE(std::ranges::find(dirty.junctions, junction), dirty.junctions.end())
      << "the followed arm moved, so its junction must regenerate";
  EXPECT_FALSE(dirty.junctions_are_current)
      << "this stage regenerates nothing itself — the editor must";
}

// --- what follow does NOT do -----------------------------------------------------

TEST(LinkFollow, AlreadyBrokenJointsAreLeftExactlyAsTheyWere) {
  // A link declared between ends that never met — the state a foreign file can
  // land in. A move must neither repair it (that is the validator's report) nor
  // DESTROY it: severing a link the gesture never touched would turn "nudge a
  // road" into silent data loss.
  RoadNetwork network;
  const RoadId a = author(network, {{0.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId b = author(network, {{0.0, 100.0}, {100.0, 100.0}}, "2");
  network.road(a)->successor = RoadLink{.target = b, .contact = ContactPoint::Start};
  network.road(b)->predecessor = RoadLink{.target = a, .contact = ContactPoint::End};

  auto command = roadmaker::edit::translate_road(network, a, 5.0, 5.0);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_TRUE(network.road(a)->successor.has_value());
  EXPECT_TRUE(network.road(b)->predecessor.has_value());
  EXPECT_TRUE(command->follow_records().empty());
  // B is untouched, so it is not even dirty.
  const auto& dirty = command->dirty().roads;
  EXPECT_EQ(std::ranges::find(dirty, b), dirty.end());
}

TEST(LinkFollow, ARigidMoveOfBothRoadsRewritesNothing) {
  RoadNetwork network;
  const Pair pair = welded_pair(network, ContactPoint::End, ContactPoint::Start, 2.0, 0.05);
  const std::string before = snapshot_xodr(network);

  const std::array<RoadId, 2> both{pair.a, pair.b};
  auto command = roadmaker::edit::translate_roads(network, both, 9.0, -4.0);
  ASSERT_TRUE(command->apply(network).has_value());

  // The joint travelled with them, so the follow stage found nothing to do.
  expect_joint_holds(network, RoadEnd{pair.a, ContactPoint::End});
  EXPECT_TRUE(command->follow_records().empty());
  ASSERT_TRUE(command->revert(network).has_value());
  expect_network_matches(network, before);
}
