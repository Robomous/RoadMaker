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

// The connection engine (gate-extension WS-2): contact/fit primitives, the
// idempotency queries, weld verification, the duplicate-junction invariant, and
// close_gap. Kernel-only — commands are applied directly.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
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
#include <utility>
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

  // Grade, UNLIKE curvature, is NOT contact-adjusted: it stays dz/ds along the
  // road's own +s at BOTH contacts (connection.hpp documents this). Consumers
  // fitting along into_hdg/out_hdg must flip it themselves — forgetting that
  // was issue #398's V-kink. This pins the asymmetry so it cannot drift.
  auto slope = roadmaker::edit::set_elevation_profile(
      network,
      road,
      {roadmaker::edit::ElevationPoint{.s = 0.0, .z = 0.0, .grade = 0.05},
       roadmaker::edit::ElevationPoint{.s = line.length(), .z = 3.0, .grade = 0.01}});
  ASSERT_TRUE(slope->apply(network).has_value());
  const auto start_sloped =
      roadmaker::edit::contact_state(network, RoadEnd{road, ContactPoint::Start});
  const auto end_sloped = roadmaker::edit::contact_state(network, RoadEnd{road, ContactPoint::End});
  ASSERT_TRUE(start_sloped.has_value());
  ASSERT_TRUE(end_sloped.has_value());
  const auto& profile = network.road(road)->elevation;
  EXPECT_NEAR(start_sloped->grade, roadmaker::eval_profile_derivative(profile, 0.0), 1e-9);
  EXPECT_NEAR(end_sloped->grade, roadmaker::eval_profile_derivative(profile, line.length()), 1e-9);
  EXPECT_NEAR(start_sloped->grade, 0.05, 1e-6); // raw +s grade, not flipped for Start
  EXPECT_NEAR(end_sloped->grade, 0.01, 1e-6);
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

TEST(Connection, CloseGapConnectorElevationContinuityAllFourContacts) {
  // Issue #398: contact_state.grade is dz/ds along each road's own +s (see the
  // sign-conventions test above), so close_gap must flip it by contact before
  // fitting the connector's elevation Hermite — exactly as the junction
  // generator does. Unflipped, three of the four contact combinations built an
  // inverted end grade: a V-kink ramp at the join. Only End->Start (the common
  // chain) was correct, which is why it survived testing.
  //
  // The oracle is PHYSICAL, not the fix's formula: walk the composite path
  // a -> connector -> b in the travel direction and finite-difference z across
  // each weld; the one-sided slopes must agree. With the bug the mismatch at a
  // broken weld measures ~2x the road's grade (0.08-0.12 here), far above the
  // Hermite-curvature finite-difference error at eps (=< ~1e-3).
  constexpr double kEps = 0.25; // finite-difference step [m]
  constexpr double kTol = 0.01; // slope-agreement tolerance (bug signal >= 0.08)

  const std::array<std::pair<ContactPoint, ContactPoint>, 4> combos{{
      {ContactPoint::End, ContactPoint::Start}, // the one combo that was correct
      {ContactPoint::End, ContactPoint::End},
      {ContactPoint::Start, ContactPoint::Start},
      {ContactPoint::Start, ContactPoint::End},
  }};

  for (const auto& [contact_a, contact_b] : combos) {
    SCOPED_TRACE(std::string("a=") + (contact_a == ContactPoint::End ? "End" : "Start") +
                 " b=" + (contact_b == ContactPoint::End ? "End" : "Start"));
    RoadNetwork network;
    // Road a occupies x in [0,100], road b x in [130,230]; reversing the
    // waypoint order puts a Start contact on the gap side. Linear profiles
    // (explicit constant grade) so the road-side slope is exact: a rises to
    // z=4 at its contact (|grade| 0.04), b falls from z=2 there (|grade| 0.06).
    const bool a_end = contact_a == ContactPoint::End;
    const bool b_start = contact_b == ContactPoint::Start;
    const RoadId a = author(network,
                            a_end ? std::vector<Waypoint>{{0.0, 0.0}, {100.0, 0.0}}
                                  : std::vector<Waypoint>{{100.0, 0.0}, {0.0, 0.0}},
                            "1");
    const RoadId b = author(network,
                            b_start ? std::vector<Waypoint>{{130.0, 0.0}, {230.0, 0.0}}
                                    : std::vector<Waypoint>{{230.0, 0.0}, {130.0, 0.0}},
                            "2");
    const double len_a = network.road(a)->plan_view.length();
    const double len_b = network.road(b)->plan_view.length();
    const auto linear = [&](RoadId road, double z0, double z1, double length) {
      const double grade = (z1 - z0) / length;
      auto cmd = roadmaker::edit::set_elevation_profile(
          network,
          road,
          {roadmaker::edit::ElevationPoint{.s = 0.0, .z = z0, .grade = grade},
           roadmaker::edit::ElevationPoint{.s = length, .z = z1, .grade = grade}});
      ASSERT_TRUE(cmd->apply(network).has_value());
    };
    // Contact z: a=4, b=2 regardless of orientation.
    linear(a, a_end ? 0.0 : 4.0, a_end ? 4.0 : 0.0, len_a);
    linear(b, b_start ? 2.0 : 8.0, b_start ? 8.0 : 2.0, len_b);

    const std::string before = snapshot(network);
    auto command =
        roadmaker::edit::close_gap(network, RoadEnd{a, contact_a}, RoadEnd{b, contact_b});
    ASSERT_TRUE(command->apply(network).has_value());
    RoadId connector;
    network.for_each_road([&](RoadId id, const roadmaker::Road&) {
      if (id != a && id != b) {
        connector = id;
      }
    });
    ASSERT_TRUE(connector.is_valid());
    const roadmaker::Road& conn = *network.road(connector);
    const double len_c = conn.plan_view.length();
    const auto& elev_a = network.road(a)->elevation;
    const auto& elev_b = network.road(b)->elevation;

    // z continuity at both welds.
    EXPECT_NEAR(roadmaker::eval_profile(conn.elevation, 0.0), 4.0, 1e-6);
    EXPECT_NEAR(roadmaker::eval_profile(conn.elevation, len_c), 2.0, 1e-6);

    // Weld A: step eps back into road a along the travel direction (toward a's
    // interior), and eps forward into the connector.
    const double a_interior = a_end ? len_a - kEps : kEps;
    const double slope_into_a = (4.0 - roadmaker::eval_profile(elev_a, a_interior)) / kEps;
    const double slope_out_conn = (roadmaker::eval_profile(conn.elevation, kEps) - 4.0) / kEps;
    EXPECT_NEAR(slope_out_conn, slope_into_a, kTol) << "V-kink at the a-side weld";

    // Weld B: eps back into the connector, and eps forward into road b along
    // the travel direction (toward b's interior).
    const double slope_into_b =
        (2.0 - roadmaker::eval_profile(conn.elevation, len_c - kEps)) / kEps;
    const double b_interior = b_start ? kEps : len_b - kEps;
    const double slope_out_b = (roadmaker::eval_profile(elev_b, b_interior) - 2.0) / kEps;
    EXPECT_NEAR(slope_out_b, slope_into_b, kTol) << "V-kink at the b-side weld";

    // The exact endpoint grades, stated directionally from each ROAD's own
    // profile (the physical directional derivative, independent of close_gap):
    // travel leaves a along +s only for an End contact, enters b along +s only
    // for a Start contact.
    const double travel_grade_a =
        (a_end ? 1.0 : -1.0) * roadmaker::eval_profile_derivative(elev_a, a_end ? len_a : 0.0);
    const double travel_grade_b =
        (b_start ? 1.0 : -1.0) * roadmaker::eval_profile_derivative(elev_b, b_start ? 0.0 : len_b);
    EXPECT_NEAR(roadmaker::eval_profile_derivative(conn.elevation, 0.0), travel_grade_a, 1e-6);
    EXPECT_NEAR(roadmaker::eval_profile_derivative(conn.elevation, len_c), travel_grade_b, 1e-6);

    // Undo restores the pre-link document byte-for-byte.
    ASSERT_TRUE(command->revert(network).has_value());
    EXPECT_EQ(snapshot(network), before);
  }
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
