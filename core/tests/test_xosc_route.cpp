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

// Resolving a scenario route against a live network (p8-s3, issue #247) —
// osc/route.hpp.
//
// ★ WHAT THIS FILE IS REALLY PINNING is GW-6 steps 7 and 8, which are the two
// steps that distinguish a lane-anchored route from a polyline that merely
// looked right when it was drawn:
//
//   step 7 — MOVE a road the route traverses, and the route follows it. That is
//            true by construction here (a waypoint names a lane, not a point),
//            so the test that matters is that resolution is UNCHANGED by a move.
//   step 8 — DELETE a lane the route traverses, and the route is reported as
//            invalidated: not silently deleted, and not silently re-routed.
//
// A resolver that returned "complete" for everything would pass a suite that
// only ever fed it valid input, so every positive test below has a negative
// twin built by breaking the same network.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/route.hpp"
#include "roadmaker/osc/rules.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using roadmaker::Diagnostic;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Waypoint;
namespace osc = roadmaker::osc;

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

/// Two straight roads laid end to end along +x and LINKED, so lane -1 runs
/// through both. 100 m each.
///
/// Built with `create_linked_road` rather than by authoring two roads and
/// welding them afterwards: that command creates and welds in one step, so the
/// fixture cannot silently end up with two adjacent-but-unlinked roads — which
/// would make every "resolves completely" test below vacuous in the one way
/// that matters.
struct TwoRoads {
  RoadNetwork network;
  RoadId first;
  RoadId second;

  TwoRoads() {
    first = author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
    auto command = roadmaker::edit::create_linked_road(
        network,
        {Waypoint{.x = 100.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}},
        LaneProfile::two_lane_default(),
        "second",
        roadmaker::RoadEnd{.road = first, .contact = roadmaker::ContactPoint::End});
    if (command == nullptr) {
      throw std::runtime_error("create_linked_road returned nullptr");
    }
    if (const auto applied = command->apply(network); !applied.has_value()) {
      throw std::runtime_error("create_linked_road: " + applied.error().message);
    }
    network.for_each_road([&](RoadId id, const roadmaker::Road& road) {
      if (road.name == "second") {
        second = id;
      }
    });
    if (!second.is_valid()) {
      throw std::runtime_error("the linked road was not created");
    }
    // The fixture's whole point is the LINK; assert it rather than assume it.
    const roadmaker::Road* head = network.road(first);
    if (head == nullptr || !head->successor.has_value()) {
      throw std::runtime_error("the two roads were not linked");
    }
  }
};

osc::RouteWaypoint waypoint_on(const char* road_odr_id, const char* lane_odr_id, double s) {
  osc::LanePosition lane;
  lane.road_id = road_odr_id;
  lane.lane_id = lane_odr_id;
  lane.s = s;
  return osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                            .position = osc::Position{lane},
                            .preserved = {}};
}

osc::Route two_road_route() {
  osc::Route route;
  route.name = "EgoRoute";
  route.waypoints.push_back(waypoint_on("1", "-1", 10.0));
  route.waypoints.push_back(waypoint_on("2", "-1", 90.0));
  return route;
}

bool any_rule(const std::vector<Diagnostic>& findings, std::string_view rule) {
  return std::ranges::any_of(findings,
                             [rule](const Diagnostic& found) { return found.rule_id == rule; });
}

bool any_message_contains(const std::vector<Diagnostic>& findings, std::string_view needle) {
  return std::ranges::any_of(findings, [needle](const Diagnostic& found) {
    return found.message.find(needle) != std::string::npos;
  });
}

/// The OUTERMOST right-hand lane of `road` at station `s`.
///
/// Outermost specifically, because `edit::remove_lane` refuses anything else
/// ("only the outermost lane of a side can be removed in M2") — so this is the
/// only lane a test can actually delete, and therefore the one a route has to
/// traverse for the invalidation test to be about deletion at all.
roadmaker::LaneId outermost_right_lane(const RoadNetwork& network, RoadId road, double s) {
  const roadmaker::LaneSection* section =
      network.lane_section(roadmaker::section_at(network, road, s));
  if (section == nullptr) {
    return {};
  }
  roadmaker::LaneId best;
  int best_id = 0;
  for (const roadmaker::LaneId id : section->lanes) {
    const roadmaker::Lane* lane = network.lane(id);
    if (lane != nullptr && lane->odr_id < best_id) {
      best_id = lane->odr_id;
      best = id;
    }
  }
  return best;
}

int lane_odr_id_of(const RoadNetwork& network, roadmaker::LaneId lane) {
  const roadmaker::Lane* value = network.lane(lane);
  return value == nullptr ? 0 : value->odr_id;
}

} // namespace

// --- the happy path ----------------------------------------------------------

TEST(XoscRoute, ARouteAcrossTwoLinkedRoadsResolvesCompletely) {
  const TwoRoads scene;
  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, two_road_route());

  EXPECT_TRUE(resolved.complete) << (resolved.findings.empty() ? "" : resolved.findings[0].message);
  EXPECT_TRUE(resolved.findings.empty());
  ASSERT_GE(resolved.legs.size(), 2U);

  // The legs run road 1 then road 2 — the path went THROUGH the link rather
  // than teleporting between the endpoints.
  EXPECT_EQ(resolved.legs.front().road, scene.first);
  EXPECT_EQ(resolved.legs.back().road, scene.second);
  EXPECT_EQ(resolved.legs.front().lane_odr_id, -1);

  // The first leg starts at the origin waypoint's own station, the last ends at
  // the destination's — not at the section boundaries.
  EXPECT_DOUBLE_EQ(resolved.legs.front().s_start, 10.0);
  EXPECT_DOUBLE_EQ(resolved.legs.back().s_end, 90.0);
}

// A right-hand lane travels with +s, so both legs ascend. The assertion exists
// because `s_end < s_start` is legal for a left-hand lane, and a caller that
// assumed an ascending interval would draw a left-hand route backwards.
TEST(XoscRoute, LegsRunInTheLanesDirectionOfTravel) {
  const TwoRoads scene;
  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, two_road_route());
  ASSERT_FALSE(resolved.legs.empty());
  for (const osc::RouteLeg& leg : resolved.legs) {
    EXPECT_LT(leg.s_start, leg.s_end) << "a right-hand lane leg ran backwards";
  }
}

// ★ GW-6 STEP 7. Moving a road the route traverses must not change how the
// route resolves — that is what "lane-anchored" means, and a polyline that
// merely looked right when it was drawn would fail here.
TEST(XoscRoute, MovingARoadBeneathTheRouteLeavesItResolvable) {
  TwoRoads scene;
  const osc::Route route = two_road_route();
  const osc::ResolvedRoute before = osc::resolve_route(scene.network, route);
  ASSERT_TRUE(before.complete);

  auto move = roadmaker::edit::translate_road(scene.network, scene.second, 0.0, 5.0);
  ASSERT_NE(move, nullptr);
  ASSERT_TRUE(move->apply(scene.network).has_value());

  const osc::ResolvedRoute after = osc::resolve_route(scene.network, route);
  EXPECT_TRUE(after.complete) << "the route stopped resolving because a road moved";
  EXPECT_EQ(after.legs.size(), before.legs.size());
  EXPECT_EQ(after.legs.front().road, before.legs.front().road);
  EXPECT_EQ(after.legs.back().road, before.legs.back().road);
}

// --- invalidation (GW-6 step 8) ----------------------------------------------

// ★ THE STEP THIS WHOLE MODULE EXISTS FOR. Deleting a lane the route traverses
// must be REPORTED — the route is neither silently dropped nor silently
// re-routed onto a lane the user did not choose.
TEST(XoscRoute, DeletingATraversedLaneInvalidatesTheRouteWithADiagnostic) {
  TwoRoads scene;

  // Route through the OUTERMOST right lane, because that is the only one
  // edit::remove_lane will delete — a test that routed through a lane it could
  // not then remove would be about something else.
  const roadmaker::LaneId doomed = outermost_right_lane(scene.network, scene.second, 90.0);
  ASSERT_TRUE(doomed.is_valid());
  const std::string lane_id = std::to_string(lane_odr_id_of(scene.network, doomed));

  osc::Route route;
  route.name = "EgoRoute";
  route.waypoints.push_back(waypoint_on("1", lane_id.c_str(), 10.0));
  route.waypoints.push_back(waypoint_on("2", lane_id.c_str(), 90.0));
  ASSERT_TRUE(osc::resolve_route(scene.network, route).complete)
      << "the fixture route did not resolve before the deletion";

  auto remove = roadmaker::edit::remove_lane(scene.network, doomed);
  ASSERT_NE(remove, nullptr);
  const auto applied = remove->apply(scene.network);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, route);
  EXPECT_FALSE(resolved.complete);
  ASSERT_FALSE(resolved.findings.empty()) << "the route broke in silence";
  EXPECT_TRUE(any_rule(resolved.findings, osc::rules::kRoadLaneExists))
      << resolved.findings[0].message;
  EXPECT_EQ(resolved.findings[0].severity, Severity::Error);
  // ...and it names the waypoint, not just "something is wrong".
  EXPECT_NE(resolved.findings[0].location.find("Waypoint[1]"), std::string::npos)
      << resolved.findings[0].location;
}

TEST(XoscRoute, DeletingARoadTheRouteNamesIsReportedAsAMissingRoad) {
  TwoRoads scene;
  const osc::Route route = two_road_route();
  auto remove = roadmaker::edit::delete_road(scene.network, scene.second);
  ASSERT_NE(remove, nullptr);
  ASSERT_TRUE(remove->apply(scene.network).has_value());

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, route);
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "is not in this network"))
      << (resolved.findings.empty() ? "no findings at all" : resolved.findings[0].message);
}

// Two roads that were never linked have no drivable path between them. Reported
// as a gap, and — the point — the legs that DID resolve are still returned, so
// a caller can draw what it knows and mark the rest.
TEST(XoscRoute, AnUnreachableDestinationIsReportedAsAGapRatherThanReRouted) {
  RoadNetwork network;
  author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}}, "1");
  author(network, {Waypoint{.x = 500.0, .y = 500.0}, Waypoint{.x = 600.0, .y = 500.0}}, "2");

  const osc::ResolvedRoute resolved = osc::resolve_route(network, two_road_route());
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "no drivable path"))
      << (resolved.findings.empty() ? "no findings at all" : resolved.findings[0].message);
  EXPECT_TRUE(any_message_contains(resolved.findings, "rather than re-routed"));
}

// A route that can only be driven the wrong way down its lane is unreachable,
// not solved. The search is directed by the lane's own direction of travel.
TEST(XoscRoute, ARouteThatWouldRunAgainstTheTrafficIsUnreachable) {
  const TwoRoads scene;
  osc::Route backwards;
  backwards.name = "Backwards";
  backwards.waypoints.push_back(waypoint_on("2", "-1", 90.0));
  backwards.waypoints.push_back(waypoint_on("1", "-1", 10.0));

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, backwards);
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "no drivable path"));
}

// --- left-hand traffic (#535) ------------------------------------------------
//
// The resolver walks lane links in the lane's DIRECTION OF TRAVEL, and §11
// makes that direction a function of the road's @rule as well as the lane's
// side. So flipping both roads to LHT — touching no geometry, no link and no
// @direction — must swap which of the two routes above is drivable. A resolver
// that read @rule but did not thread it would keep answering RHT and fail both
// halves; one that ignored travel direction entirely would pass neither.

namespace {

/// `TwoRoads`, with both roads declared left-hand traffic.
TwoRoads left_hand_scene() {
  TwoRoads scene;
  scene.network.road(scene.first)->rule = roadmaker::TrafficRule::LeftHandTraffic;
  scene.network.road(scene.second)->rule = roadmaker::TrafficRule::LeftHandTraffic;
  return scene;
}

} // namespace

TEST(XoscRoute, UnderLeftHandTrafficTheRightHandRouteRunsAgainstTheTraffic) {
  const TwoRoads scene = left_hand_scene();
  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, two_road_route());

  // The very route that resolves completely under RHT is now backwards: under
  // LHT lane -1 runs toward -s, so road 1 -> road 2 is against the traffic.
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "no drivable path"));
}

TEST(XoscRoute, UnderLeftHandTrafficTheReversedRouteIsTheDrivableOne) {
  const TwoRoads scene = left_hand_scene();
  osc::Route backwards;
  backwards.name = "Backwards";
  backwards.waypoints.push_back(waypoint_on("2", "-1", 90.0));
  backwards.waypoints.push_back(waypoint_on("1", "-1", 10.0));

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, backwards);
  EXPECT_TRUE(resolved.complete) << (resolved.findings.empty() ? "" : resolved.findings[0].message);
  ASSERT_GE(resolved.legs.size(), 2U);
  EXPECT_EQ(resolved.legs.front().road, scene.second);
  EXPECT_EQ(resolved.legs.back().road, scene.first);

  // And every leg DESCENDS, which is the other half of the claim: the legs are
  // emitted in the direction of travel, so an LHT right-hand lane runs -s.
  for (const osc::RouteLeg& leg : resolved.legs) {
    EXPECT_GT(leg.s_start, leg.s_end) << "an LHT right-hand lane leg ran forwards";
  }
}

// --- an abruptly splitting lane (#536) ---------------------------------------
//
// §11.6 mandates several <successor> entries where a lane splits abruptly, and
// the resolver walks those records. Reading only the first — which is what it
// did before #536 — makes the second branch of every split UNREACHABLE, and the
// route is reported as having no drivable path when one plainly exists.

TEST(XoscRoute, ARouteDownEitherBranchOfASplitResolves) {
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "lane_link_multi.xodr");
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);

  // Lane -1 of the first section lists successors -1 AND -2. Both branches must
  // be drivable; the SECOND is the one a first-child-only reader loses.
  for (const char* branch : {"-1", "-2"}) {
    osc::Route route;
    route.name = std::string("Branch") + branch;
    route.waypoints.push_back(waypoint_on("1", "-1", 10.0));
    route.waypoints.push_back(waypoint_on("1", branch, 90.0));

    const osc::ResolvedRoute resolved = osc::resolve_route(loaded->network, route);
    EXPECT_TRUE(resolved.complete)
        << "branch " << branch << ": "
        << (resolved.findings.empty() ? "no finding" : resolved.findings[0].message);
  }
}

// --- waypoints that name no lane ---------------------------------------------

TEST(XoscRoute, AWorldPositionWaypointIsReportedAsAmbiguous) {
  const TwoRoads scene;
  osc::Route route = two_road_route();
  route.waypoints[1].position = osc::WorldPosition{};

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, route);
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_rule(resolved.findings, osc::rules::kAmbiguousRouteWaypoints));
  EXPECT_TRUE(any_message_contains(resolved.findings, "names no lane"));
}

TEST(XoscRoute, ARoadPositionWaypointSaysItNamesNoLane) {
  const TwoRoads scene;
  osc::Route route = two_road_route();
  route.waypoints[1].position = osc::RoadPosition{
      .road_id = "2", .s = 90.0, .t = -1.75, .orientation = std::nullopt, .preserved = {}};

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, route);
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "names a road but no lane"));
}

// A non-integer lane id is LEGAL — the temporary lane layer — and simply not
// resolvable here. Reported as that, rather than as a missing lane.
TEST(XoscRoute, ATemporaryLaneLayerIdIsReportedAsUnresolvableNotAsMissing) {
  const TwoRoads scene;
  osc::Route route = two_road_route();
  std::get<osc::LanePosition>(route.waypoints[1].position).lane_id = "T1";

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, route);
  EXPECT_FALSE(resolved.complete);
  EXPECT_TRUE(any_message_contains(resolved.findings, "temporary lane layer"));
}

// ★ An incomplete resolution ALWAYS says why. A silent `complete = false` is
// the failure mode this module exists to prevent, and a one-waypoint route is
// the case with no per-waypoint problem to report.
TEST(XoscRoute, AnIncompleteResolutionAlwaysCarriesAFinding) {
  const TwoRoads scene;
  osc::Route stub;
  stub.name = "Stub";
  stub.waypoints.push_back(waypoint_on("1", "-1", 10.0));

  const osc::ResolvedRoute resolved = osc::resolve_route(scene.network, stub);
  EXPECT_FALSE(resolved.complete);
  EXPECT_FALSE(resolved.findings.empty()) << "an incomplete resolution said nothing";
}

// --- validate_routes ---------------------------------------------------------

namespace {

osc::Scenario scenario_with_route(osc::Route route) {
  osc::Scenario scenario;
  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(ego);

  osc::PrivateAction action;
  action.routing = osc::RoutingAction{
      .assign_route = osc::AssignRouteAction{.route = std::move(route), .preserved = {}},
      .preserved = {}};
  osc::Private entry;
  entry.entity_ref = "Ego";
  entry.actions.push_back(std::move(action));
  scenario.storyboard.init.actions.privates.push_back(std::move(entry));
  return scenario;
}

} // namespace

TEST(XoscRoute, ValidateRoutesIsSilentWhenEveryRouteResolves) {
  const TwoRoads scene;
  const osc::Scenario scenario = scenario_with_route(two_road_route());
  EXPECT_TRUE(osc::validate_routes(scene.network, scenario).empty());
}

TEST(XoscRoute, ValidateRoutesNamesTheEntityWhoseRouteBroke) {
  RoadNetwork network; // no roads at all
  const osc::Scenario scenario = scenario_with_route(two_road_route());

  const std::vector<Diagnostic> findings = osc::validate_routes(network, scenario);
  ASSERT_FALSE(findings.empty());
  // The entity is what a user recognises; the route name alone leaves them
  // hunting for whose route it was.
  EXPECT_NE(findings[0].location.find("Entity[Ego]"), std::string::npos) << findings[0].location;
  EXPECT_NE(findings[0].location.find("EgoRoute"), std::string::npos) << findings[0].location;
}

TEST(XoscRoute, ACatalogReferenceRouteIsNotResolvedAndNotReported) {
  const TwoRoads scene;
  osc::Scenario scenario;
  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(ego);

  osc::AssignRouteAction assign; // `route` unset: the content is a catalog reference
  assign.preserved.children.push_back(R"(<CatalogReference catalogName="Routes" entryName="R1"/>)");
  osc::PrivateAction action;
  action.routing = osc::RoutingAction{.assign_route = assign, .preserved = {}};
  osc::Private entry;
  entry.entity_ref = "Ego";
  entry.actions.push_back(std::move(action));
  scenario.storyboard.init.actions.privates.push_back(std::move(entry));

  EXPECT_TRUE(osc::assigned_routes(scenario).empty());
  EXPECT_TRUE(osc::validate_routes(scene.network, scenario).empty())
      << "a catalog reference was reported as an unresolvable route";
}

TEST(XoscRoute, AssignedRoutesPairsEachRouteWithItsEntity) {
  const osc::Scenario scenario = scenario_with_route(two_road_route());
  const std::vector<osc::AssignedRoute> assigned = osc::assigned_routes(scenario);
  ASSERT_EQ(assigned.size(), 1U);
  EXPECT_EQ(assigned[0].entity_ref, "Ego");
  ASSERT_NE(assigned[0].route, nullptr);
  EXPECT_EQ(assigned[0].route->name, "EgoRoute");
}
