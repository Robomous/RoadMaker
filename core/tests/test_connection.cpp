// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// The connection engine (gate-extension WS-2): contact/fit primitives, the
// idempotency queries, weld verification, the duplicate-junction invariant, and
// close_gap. Kernel-only — commands are applied directly.

#include "roadmaker/edit/connection.hpp"
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
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::ConnectorEndpoint;
using roadmaker::edit::ConnectorParams;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

std::string snapshot(const RoadNetwork& network) {
  auto text = roadmaker::write_xodr(network);
  if (!text.has_value()) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

double norm_angle(double a) {
  return std::remainder(a, 2.0 * std::numbers::pi);
}

} // namespace

TEST(Connection, ContactStateStartAndEndSignConventions) {
  RoadNetwork network;
  const RoadId road = author(network,
                             {Waypoint{.x = 0.0, .y = 0.0},
                              Waypoint{.x = 60.0, .y = 20.0},
                              Waypoint{.x = 120.0, .y = 0.0}},
                             "1");
  const auto& line = network.road(road)->plan_view;
  const auto start = roadmaker::edit::contact_state(network, RoadEnd{road, ContactPoint::Start});
  const auto end = roadmaker::edit::contact_state(network, RoadEnd{road, ContactPoint::End});
  ASSERT_TRUE(start.has_value());
  ASSERT_TRUE(end.has_value());

  const auto s0 = line.evaluate(0.0);
  const auto s1 = line.evaluate(line.length());
  // Start: travel INTO the junction runs opposite +s (heading + pi), the body
  // continues along +s, and the plan-view curvature is sign-flipped.
  EXPECT_NEAR(norm_angle(start->into_hdg - (s0.hdg + std::numbers::pi)), 0.0, 1e-9);
  EXPECT_NEAR(norm_angle(start->out_hdg - s0.hdg), 0.0, 1e-9);
  EXPECT_NEAR(start->curvature, -s0.curvature, 1e-9);
  // End: travel into the junction is +s; the body continues along -s.
  EXPECT_NEAR(norm_angle(end->into_hdg - s1.hdg), 0.0, 1e-9);
  EXPECT_NEAR(norm_angle(end->out_hdg - (s1.hdg + std::numbers::pi)), 0.0, 1e-9);
  EXPECT_NEAR(end->curvature, s1.curvature, 1e-9);
}

TEST(Connection, AlignedPoseFollowsTangentAndSide) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const auto plain = roadmaker::edit::aligned_pose_on_road(network, road, 40.0, std::nullopt);
  ASSERT_TRUE(plain.has_value());
  EXPECT_NEAR(plain->x, 40.0, 1e-6);
  EXPECT_NEAR(norm_angle(plain->heading), 0.0, 1e-9); // straight east road

  const auto left =
      roadmaker::edit::aligned_pose_on_road(network, road, 40.0, roadmaker::edit::Side::Left);
  const auto right =
      roadmaker::edit::aligned_pose_on_road(network, road, 40.0, roadmaker::edit::Side::Right);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  EXPECT_NEAR(norm_angle(left->heading - std::numbers::pi / 2.0), 0.0, 1e-9);
  EXPECT_NEAR(norm_angle(right->heading + std::numbers::pi / 2.0), 0.0, 1e-9);

  // A station off the road is refused.
  EXPECT_FALSE(
      roadmaker::edit::aligned_pose_on_road(network, road, 1000.0, std::nullopt).has_value());
}

TEST(Connection, FitConnectorStraightThroughAndLoopRejection) {
  // Collinear endpoints, headings aligned: a short connector ~ the gap length.
  const auto through =
      roadmaker::edit::fit_connector(ConnectorEndpoint{.x = 0.0, .y = 0.0, .heading = 0.0},
                                     ConnectorEndpoint{.x = 20.0, .y = 0.0, .heading = 0.0},
                                     ConnectorParams{});
  ASSERT_TRUE(through.has_value());
  EXPECT_NEAR(through->line.length(), 20.0, 0.5);

  // A right-angle turn detours well past a tight loop factor — rejected.
  const auto loop = roadmaker::edit::fit_connector(
      ConnectorEndpoint{.x = 0.0, .y = 0.0, .heading = 0.0},
      ConnectorEndpoint{.x = 30.0, .y = 30.0, .heading = std::numbers::pi / 2.0},
      ConnectorParams{.max_loop_factor = 1.05});
  EXPECT_FALSE(loop.has_value());

  // A tight right-angle turn breaches a large minimum-radius bound.
  const auto tight = roadmaker::edit::fit_connector(
      ConnectorEndpoint{.x = 0.0, .y = 0.0, .heading = 0.0},
      ConnectorEndpoint{.x = 5.0, .y = 5.0, .heading = std::numbers::pi / 2.0},
      ConnectorParams{.min_turn_radius_m = 50.0});
  EXPECT_FALSE(tight.has_value());
}

namespace {

/// Three roads pointing at the origin, each ending ~6 m short of it (all End
/// contacts) — the canonical junction layout that generates connecting roads.
struct ArmedJunction {
  RoadNetwork network;
  RoadId a;
  RoadId b;
  RoadId c;
  JunctionId junction;

  ArmedJunction() {
    a = author(network, {Waypoint{.x = -40.0, .y = 0.0}, Waypoint{.x = -6.0, .y = 0.0}}, "1");
    b = author(network, {Waypoint{.x = 40.0, .y = 0.0}, Waypoint{.x = 6.0, .y = 0.0}}, "2");
    c = author(network, {Waypoint{.x = 0.0, .y = -40.0}, Waypoint{.x = 0.0, .y = -6.0}}, "3");
    const std::array<RoadEnd, 3> ends{RoadEnd{a, ContactPoint::End},
                                      RoadEnd{b, ContactPoint::End},
                                      RoadEnd{c, ContactPoint::End}};
    auto command = roadmaker::edit::create_junction(network, ends);
    if (!command->apply(network).has_value()) {
      throw std::runtime_error("junction apply failed");
    }
    network.for_each_junction([&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  }
};

} // namespace

TEST(Connection, JunctionAtEndAndMatchingJunction) {
  ArmedJunction fx;
  EXPECT_EQ(roadmaker::edit::junction_at_end(fx.network, RoadEnd{fx.a, ContactPoint::End}),
            fx.junction);
  // The start of road A is not an arm.
  EXPECT_FALSE(
      roadmaker::edit::junction_at_end(fx.network, RoadEnd{fx.a, ContactPoint::Start}).has_value());

  // matching_junction is order-free over the exact arm set.
  const std::array<RoadEnd, 3> reversed{RoadEnd{fx.c, ContactPoint::End},
                                        RoadEnd{fx.b, ContactPoint::End},
                                        RoadEnd{fx.a, ContactPoint::End}};
  EXPECT_EQ(roadmaker::edit::matching_junction(fx.network, reversed), fx.junction);
  // A partial / different set does not match.
  const std::array<RoadEnd, 1> partial{RoadEnd{fx.a, ContactPoint::End}};
  EXPECT_FALSE(roadmaker::edit::matching_junction(fx.network, partial).has_value());
}

TEST(Connection, VerifyJunctionWeldsCleanAndBreachesOnDisplacement) {
  ArmedJunction fx;
  const auto clean = roadmaker::edit::verify_junction_welds(fx.network, fx.junction);
  ASSERT_TRUE(clean.has_value());
  EXPECT_FALSE(clean->breaches);
  EXPECT_LE(clean->max_position_gap, roadmaker::tol::kWeldPosition);

  // Shove a connecting road's reference line off its anchor → a position breach.
  RoadId connecting;
  fx.network.for_each_road([&](RoadId id, const roadmaker::Road& road) {
    if (road.junction == fx.junction) {
      connecting = id;
    }
  });
  ASSERT_TRUE(connecting.is_valid());
  auto shifted = *fx.network.road(connecting);
  shifted.plan_view = *roadmaker::fit_clothoid_path(
      std::vector<Waypoint>{Waypoint{.x = 5.0, .y = 5.0}, Waypoint{.x = 40.0, .y = 40.0}});
  *fx.network.road(connecting) = shifted;
  const auto dirty = roadmaker::edit::verify_junction_welds(fx.network, fx.junction);
  ASSERT_TRUE(dirty.has_value());
  EXPECT_TRUE(dirty->breaches);
  EXPECT_GT(dirty->max_position_gap, roadmaker::tol::kWeldPosition);
}

TEST(Connection, RegenerateJunctionFollowsADraggedArmAndIsDeterministic) {
  ArmedJunction fx;
  const std::size_t connections = fx.network.junction(fx.junction)->connections.size();
  ASSERT_GT(connections, 0U);

  // Drag the west arm's far node so its approach angle changes — the junction
  // must re-fit its connecting roads to follow (finding 2), not freeze.
  auto move = roadmaker::edit::move_waypoint(fx.network, fx.a, 0, Waypoint{.x = -40.0, .y = 14.0});
  ASSERT_TRUE(move->apply(fx.network).has_value());
  auto regen = roadmaker::edit::regenerate_junction(fx.network, fx.junction);
  ASSERT_TRUE(regen->apply(fx.network).has_value());

  // The turn set is unchanged, so keyed matching keeps every connection...
  EXPECT_EQ(fx.network.junction(fx.junction)->connections.size(), connections);
  // ...and the regenerated connecting roads coincide with the moved arm.
  const auto welds = roadmaker::edit::verify_junction_welds(fx.network, fx.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
  EXPECT_LE(welds->max_position_gap, roadmaker::tol::kWeldPosition);

  // Regenerating again changes nothing (determinism / idempotence).
  const std::string once = snapshot(fx.network);
  auto again = roadmaker::edit::regenerate_junction(fx.network, fx.junction);
  ASSERT_TRUE(again->apply(fx.network).has_value());
  EXPECT_EQ(snapshot(fx.network), once);
}

TEST(Connection, CreateJunctionRefusesAnEndAlreadyOwned) {
  ArmedJunction fx;
  // Road A's End is already a junction arm; a fresh road meeting it cannot form
  // a second junction there.
  const RoadId d =
      author(fx.network, {Waypoint{.x = -6.0, .y = 0.0}, Waypoint{.x = -6.0, .y = 60.0}}, "50");
  const std::array<RoadEnd, 2> ends{RoadEnd{fx.a, ContactPoint::End},
                                    RoadEnd{d, ContactPoint::Start}};
  const std::string before = snapshot(fx.network);
  auto command = roadmaker::edit::create_junction(fx.network, ends);
  EXPECT_FALSE(command->apply(fx.network).has_value());
  EXPECT_EQ(snapshot(fx.network), before); // a refused command changes nothing
}

TEST(Connection, ValidatorFlagsAnArmSharedByTwoJunctions) {
  RoadNetwork network;
  const RoadId r0 =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const RoadId r1 =
      author(network, {Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 80.0}}, "2");
  const RoadId r2 =
      author(network, {Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 180.0, .y = 0.0}}, "3");
  // Two hand-built junctions both claim road 1's End as an arm.
  const JunctionId ja = network.create_junction("100", "");
  network.junction(ja)->arms = {RoadEnd{r0, ContactPoint::End}, RoadEnd{r1, ContactPoint::Start}};
  const JunctionId jb = network.create_junction("101", "");
  network.junction(jb)->arms = {RoadEnd{r0, ContactPoint::End}, RoadEnd{r2, ContactPoint::Start}};

  const auto findings = roadmaker::validate_network(network, {});
  const bool flagged = std::any_of(findings.begin(), findings.end(), [](const auto& d) {
    return d.rule_id == std::string(roadmaker::rules::kJunctionArmSingleOwner) &&
           d.severity == roadmaker::Severity::Error;
  });
  EXPECT_TRUE(flagged);
}

TEST(Connection, CloseGapLinksCoincidentEndsWithByteIdenticalUndo) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const RoadId b =
      author(network, {Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}}, "2");
  const std::string before = snapshot(network);
  auto command = roadmaker::edit::close_gap(
      network, RoadEnd{a, ContactPoint::End}, RoadEnd{b, ContactPoint::Start});
  ASSERT_TRUE(command->apply(network).has_value());
  // The ends are now linked to each other, no new road.
  EXPECT_TRUE(network.road(a)->successor.has_value());
  EXPECT_TRUE(network.road(b)->predecessor.has_value());
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot(network), before);
}

TEST(Connection, CloseGapBridgesARealGapWithAConnectorRoad) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const RoadId b =
      author(network, {Waypoint{.x = 130.0, .y = 0.0}, Waypoint{.x = 230.0, .y = 0.0}}, "2");
  const std::size_t roads_before = network.road_count();
  const std::string before = snapshot(network);
  auto command = roadmaker::edit::close_gap(
      network, RoadEnd{a, ContactPoint::End}, RoadEnd{b, ContactPoint::Start});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), roads_before + 1); // a connector road was added
  EXPECT_TRUE(network.road(a)->successor.has_value());
  EXPECT_TRUE(network.road(b)->predecessor.has_value());
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot(network), before);
}

TEST(Connection, CreateLinkedRoadWeldsToTheSourceEnd) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const std::string before = snapshot(network);
  // A new road whose start coincides with A's END — the Create Road snap weld.
  std::vector<Waypoint> waypoints{Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}};
  auto command = roadmaker::edit::create_linked_road(network,
                                                     waypoints,
                                                     roadmaker::LaneProfile::two_lane_default(),
                                                     "B",
                                                     RoadEnd{a, ContactPoint::End},
                                                     roadmaker::EndpointHeadings{});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);                 // pure link, no connector
  EXPECT_TRUE(network.road(a)->successor.has_value()); // A now links to the new road
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot(network), before); // byte-identical undo
}

TEST(Connection, CloseGapNoCurvatureKinkWhenArcStartsAtJoint) {
  // The maintainer's finding-3 case: a road whose arc starts right at the joint.
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const RoadId b = author(network,
                          {Waypoint{.x = 130.0, .y = 0.0},
                           Waypoint{.x = 160.0, .y = 22.0},
                           Waypoint{.x = 210.0, .y = 22.0}},
                          "2");
  const double a_terminal =
      network.road(a)->plan_view.evaluate(network.road(a)->plan_view.length()).curvature;
  const double b_start = network.road(b)->plan_view.evaluate(0.0).curvature;
  ASSERT_GT(std::abs(b_start), 1e-3); // b really does arc at its start

  auto command = roadmaker::edit::close_gap(
      network, RoadEnd{a, ContactPoint::End}, RoadEnd{b, ContactPoint::Start});
  ASSERT_TRUE(command->apply(network).has_value());
  RoadId connector;
  network.for_each_road([&](RoadId id, const roadmaker::Road&) {
    if (id != a && id != b) {
      connector = id;
    }
  });
  ASSERT_TRUE(connector.is_valid());
  const auto& line = network.road(connector)->plan_view;
  // G2 weld: the connector's curvature meets each neighbour's without a step.
  EXPECT_NEAR(line.evaluate(0.0).curvature, a_terminal, roadmaker::tol::kWeldCurvature);
  EXPECT_NEAR(line.evaluate(line.length()).curvature, b_start, roadmaker::tol::kWeldCurvature);
}

TEST(Connection, CloseGapAndCheckLinkableRefusals) {
  RoadNetwork network;
  const RoadId a =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  const RoadId far =
      author(network, {Waypoint{.x = 0.0, .y = 500.0}, Waypoint{.x = 100.0, .y = 500.0}}, "2");
  // Same road end → refused.
  EXPECT_FALSE(roadmaker::edit::check_linkable(
                   network, RoadEnd{a, ContactPoint::End}, RoadEnd{a, ContactPoint::Start})
                   .has_value());
  // Ends too far apart → refused.
  EXPECT_FALSE(roadmaker::edit::check_linkable(
                   network, RoadEnd{a, ContactPoint::End}, RoadEnd{far, ContactPoint::Start})
                   .has_value());
  // The refused command changes nothing.
  const std::string before = snapshot(network);
  auto command = roadmaker::edit::close_gap(
      network, RoadEnd{a, ContactPoint::End}, RoadEnd{far, ContactPoint::Start});
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot(network), before);
}
