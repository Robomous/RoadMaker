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

// `osc::validate_scenario_against_network` (issue #533).
//
// ★ THE ACCEPTANCE IS LITERAL AND IT IS THE FIRST TEST BELOW: each of the four
// dangling-reference mutations esmini was MEASURED to accept in silence must
// produce a cited RoadMaker diagnostic. Everything else here guards the shape
// of that check — that it walks the whole document, that it reports each
// mistake exactly once, and that it stays silent on a scenario that resolves.
//
// The scenario under test is built through the same command layer the editor
// and `scripts/gen_xosc_fixtures.py` drive, so a clean run here is a statement
// about what RoadMaker actually writes.

#include "roadmaker/edit/command.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/network_validation.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using roadmaker::ContactPoint;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::Diagnostic;
using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Signal;
using roadmaker::SignalId;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::SignalizeTemplate;
namespace osc = roadmaker::osc;

void run(RoadNetwork& network, const std::unique_ptr<Command>& command) {
  if (command == nullptr) {
    throw std::runtime_error("factory returned nullptr");
  }
  const auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error(applied.error().message);
  }
}

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

/// A signalized four-arm crossing plus a scenario decomposed from it — the
/// shape `tests/esmini/signalized.*` ships as, so a clean run here is a
/// statement about the tracked fixture too.
struct Scene {
  RoadNetwork network;
  JunctionId junction;
  osc::Scenario scenario;
  osc::edit::ScenarioStack stack;

  Scene() {
    const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    const std::vector<RoadEnd> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::End},
                                    RoadEnd{.road = south, .contact = ContactPoint::End},
                                    RoadEnd{.road = north, .contact = ContactPoint::End}};
    run(network, roadmaker::edit::create_junction(network, ends));
    network.for_each_junction([this](JunctionId id, const Junction&) { junction = id; });
    run(network,
        roadmaker::edit::signalize_junction(
            network, junction, {.tmpl = SignalizeTemplate::TwoPhase}));

    if (!stack.push(scenario, osc::edit::set_logic_file(scenario, "scene.xodr"))) {
      throw std::runtime_error("set_logic_file");
    }
    if (!stack.push(scenario, osc::edit::sync_traffic_signals(scenario, network, junction))) {
      throw std::runtime_error("sync_traffic_signals");
    }

    osc::LanePosition position;
    position.road_id = "1";
    position.lane_id = "-1";
    position.s = 20.0;
    if (!stack.push(scenario,
                    osc::edit::place_scenario_object(
                        scenario, osc::make_actor(osc::ActorKind::Car, "Ego"), position))) {
      throw std::runtime_error("place_scenario_object");
    }
  }

  [[nodiscard]] std::string first_controller() const {
    return scenario.road_network.traffic_signal_controllers.at(0).name;
  }

  [[nodiscard]] std::string first_signal() const {
    for (const osc::Phase& phase : scenario.road_network.traffic_signal_controllers.at(0).phases) {
      if (!phase.signal_states.empty()) {
        return phase.signal_states.front().traffic_signal_id;
      }
    }
    return {};
  }

  /// A story whose event carries `action`, gated by `condition`.
  void add_story(osc::Action action, osc::Condition condition) {
    osc::Event event;
    event.name = "e";
    event.actions.push_back(std::move(action));
    osc::ConditionGroup group;
    group.conditions.push_back(std::move(condition));
    osc::Trigger trigger;
    trigger.condition_groups.push_back(std::move(group));
    event.start_trigger = std::move(trigger);

    osc::StoryManeuver maneuver;
    maneuver.name = "m";
    maneuver.events.push_back(std::move(event));
    osc::ManeuverGroup maneuver_group;
    maneuver_group.name = "g";
    maneuver_group.maneuvers.push_back(std::move(maneuver));
    osc::Act act;
    act.name = "a";
    act.maneuver_groups.push_back(std::move(maneuver_group));
    osc::Story story;
    story.name = "s";
    story.acts.push_back(std::move(act));

    if (!stack.push(scenario, osc::edit::set_story(scenario, 0, std::move(story)))) {
      throw std::runtime_error("set_story");
    }
  }
};

osc::Action controller_action(std::string controller_ref, std::string phase) {
  osc::TrafficSignalAction signal;
  signal.action =
      osc::TrafficSignalControllerAction{.traffic_signal_controller_ref = std::move(controller_ref),
                                         .phase = std::move(phase),
                                         .preserved = {}};
  osc::InfrastructureAction infrastructure;
  infrastructure.traffic_signal = std::move(signal);
  osc::GlobalAction global;
  global.infrastructure = std::move(infrastructure);

  osc::Action action;
  action.name = "hold";
  action.action = std::move(global);
  return action;
}

osc::Condition signal_condition(std::string signal_id) {
  osc::Condition condition;
  condition.name = "green";
  condition.traffic_signal =
      osc::TrafficSignalCondition{.name = std::move(signal_id), .state = "green", .preserved = {}};
  return condition;
}

bool cites(const std::vector<Diagnostic>& findings, std::string_view rule) {
  for (const Diagnostic& finding : findings) {
    if (finding.severity == Severity::Error && finding.rule_id == rule) {
      return true;
    }
  }
  return false;
}

bool mentions(const std::vector<Diagnostic>& findings, std::string_view needle) {
  for (const Diagnostic& finding : findings) {
    if (finding.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// A finding's LOCATION, which is the half that says where in the document a
/// dangling reference lives — checked separately from the message so a test
/// asserting "the story was walked" cannot pass on a message that happens to
/// contain the word.
bool locates(const std::vector<Diagnostic>& findings, std::string_view needle) {
  for (const Diagnostic& finding : findings) {
    if (finding.location.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

constexpr std::string_view kSignalRule =
    "asam.net:xosc:1.0.0:reference_control.traffic_signal_state_references";
constexpr std::string_view kControllerRule =
    "asam.net:xosc:1.0.0:reference_control.traffic_signal_controller_references";
constexpr std::string_view kRoadLaneRule = "asam.net:xosc:1.0.0:reference_control.road_lane_exists";
constexpr std::string_view kBoundsRule =
    "asam.net:xosc:1.0.0:positioning.road_lane_offset_in_bounds";

// --- the acceptance -----------------------------------------------------------

TEST(XoscNetworkValidation, EachMutationEsminiAcceptsSilentlyProducesACitedDiagnostic) {
  // ★ #533's acceptance, literally. All four of these were MEASURED on the
  // pinned esmini v3.5.0 to load with exit 0 and no error line (2026-07-30 and
  // 2026-08-01, recorded on #257 and in .github/workflows/ci.yml).
  {
    Scene scene;
    scene.scenario.road_network.traffic_signal_controllers.at(0)
        .phases.at(0)
        .signal_states.at(0)
        .traffic_signal_id = "999";
    const std::vector<Diagnostic> findings =
        osc::validate_scenario_against_network(scene.scenario, scene.network);
    EXPECT_TRUE(cites(findings, kSignalRule)) << "a dangling trafficSignalId";
    EXPECT_TRUE(mentions(findings, "'999'"));
  }
  {
    Scene scene;
    scene.scenario.road_network.traffic_signal_controllers.at(0).name = "999";
    const std::vector<Diagnostic> findings =
        osc::validate_scenario_against_network(scene.scenario, scene.network);
    EXPECT_TRUE(cites(findings, kControllerRule)) << "a dangling controller @name";
  }
  {
    Scene scene;
    scene.add_story(controller_action("999", "axis0"), signal_condition(scene.first_signal()));
    const std::vector<Diagnostic> findings =
        osc::validate_scenario_against_network(scene.scenario, scene.network);
    EXPECT_TRUE(cites(findings, kControllerRule))
        << "a dangling trafficSignalControllerRef in a story action";
  }
  {
    Scene scene;
    scene.add_story(controller_action(scene.first_controller(), "axis0"), signal_condition("999"));
    const std::vector<Diagnostic> findings =
        osc::validate_scenario_against_network(scene.scenario, scene.network);
    EXPECT_TRUE(cites(findings, kSignalRule)) << "a dangling signal id in a TrafficSignalCondition";
  }
}

TEST(XoscNetworkValidation, AScenarioThatResolvesProducesNoFindings) {
  // The other half of the acceptance, and the one a false-positive check would
  // break: everything the editor and the fixture generator author must be
  // silent here, or the Diagnostics dock cries wolf on every scene.
  Scene scene;
  scene.add_story(
      controller_action(
          scene.first_controller(),
          osc::phase_names(scene.scenario.road_network.traffic_signal_controllers.at(0)).at(0)),
      signal_condition(scene.first_signal()));

  EXPECT_TRUE(osc::validate_scenario_against_network(scene.scenario, scene.network).empty());
  // And it is a document `write_xosc` accepts, so the two validators agree.
  EXPECT_TRUE(osc::validate_scenario(scene.scenario).empty());
  EXPECT_TRUE(osc::write_xosc(scene.scenario).has_value());
}

// --- positions -----------------------------------------------------------------

TEST(XoscNetworkValidation, ADanglingRoadOrLaneAnchorIsReportedOnce) {
  {
    Scene scene;
    auto& teleport = scene.scenario.storyboard.init.actions.privates.at(0).actions.at(0).teleport;
    ASSERT_TRUE(teleport.has_value());
    std::get<osc::LanePosition>(teleport->position).road_id = "999";
    const std::vector<Diagnostic> findings =
        osc::validate_scenario_against_network(scene.scenario, scene.network);
    EXPECT_TRUE(cites(findings, kRoadLaneRule));
    // ★ ONE finding, not three. Without the early return, a dangling road also
    // reports "s past the end" and "no such lane" — three lines for one typo,
    // two of which name a road that does not exist.
    EXPECT_EQ(findings.size(), 1U) << findings.size();
  }
  {
    Scene scene;
    auto& teleport = scene.scenario.storyboard.init.actions.privates.at(0).actions.at(0).teleport;
    std::get<osc::LanePosition>(teleport->position).lane_id = "-99";
    EXPECT_TRUE(cites(osc::validate_scenario_against_network(scene.scenario, scene.network),
                      kRoadLaneRule));
  }
}

TEST(XoscNetworkValidation, TheUpperSBoundIsTheCheckOnlyTheNetworkCanMake) {
  // `validate_scenario` refuses a NEGATIVE s on its own — the lower bound is 0
  // on every road that can exist. The upper bound is the road's length, which
  // lives in the .xodr, so this is the only place it can be checked; esmini was
  // measured to TRUNCATE rather than refuse it.
  Scene scene;
  auto& teleport = scene.scenario.storyboard.init.actions.privates.at(0).actions.at(0).teleport;
  std::get<osc::LanePosition>(teleport->position).s = 9999.0;

  EXPECT_TRUE(osc::validate_scenario(scene.scenario).empty())
      << "the document-only validator cannot see this, which is the point";
  EXPECT_TRUE(
      cites(osc::validate_scenario_against_network(scene.scenario, scene.network), kBoundsRule));
}

TEST(XoscNetworkValidation, AStoryActionsTeleportIsWalkedTooNotJustTheInitOne) {
  // A checker that stopped at <Init> would pass a scenario whose story
  // teleports an actor onto a road deleted an hour ago.
  Scene scene;
  osc::TeleportAction teleport;
  osc::LanePosition position;
  position.road_id = "999";
  position.lane_id = "-1";
  position.s = 10.0;
  teleport.position = position;
  osc::PrivateAction entry;
  entry.teleport = teleport;
  osc::Action action;
  action.name = "jump";
  action.action = entry;

  scene.add_story(std::move(action), signal_condition(scene.first_signal()));
  const std::vector<Diagnostic> findings =
      osc::validate_scenario_against_network(scene.scenario, scene.network);
  EXPECT_TRUE(cites(findings, kRoadLaneRule));
  EXPECT_TRUE(locates(findings, "Storyboard/Story"));
}

TEST(XoscNetworkValidation, AWorldPositionIsSkippedBecauseItNamesNoRoad) {
  Scene scene;
  auto& teleport = scene.scenario.storyboard.init.actions.privates.at(0).actions.at(0).teleport;
  teleport->position = osc::WorldPosition{};
  EXPECT_TRUE(osc::validate_scenario_against_network(scene.scenario, scene.network).empty());
}

TEST(XoscNetworkValidation, ANonNumericLaneIdIsNotReported) {
  // The schema types `@laneId` as a STRING so a temporary lane layer's id can
  // be spelled. Nothing here can resolve one, and reporting it would fire on
  // legal input.
  Scene scene;
  auto& teleport = scene.scenario.storyboard.init.actions.privates.at(0).actions.at(0).teleport;
  std::get<osc::LanePosition>(teleport->position).lane_id = "roadworks-A";
  EXPECT_TRUE(osc::validate_scenario_against_network(scene.scenario, scene.network).empty());
}

// --- the seam with the other two checkers ---------------------------------------

TEST(XoscNetworkValidation, AnEmptyReferenceIsLeftToTheDocumentValidator) {
  // ★ ONE MISTAKE, ONE LINE. `validate_scenario` already refuses an empty
  // `@trafficSignalId` with the SAME rule id, so reporting it here as well
  // would show a single typo as two findings in one dock.
  Scene scene;
  scene.scenario.road_network.traffic_signal_controllers.at(0)
      .phases.at(0)
      .signal_states.at(0)
      .traffic_signal_id.clear();

  EXPECT_TRUE(cites(osc::validate_scenario(scene.scenario), kSignalRule))
      << "the document validator owns the empty case";
  EXPECT_TRUE(osc::validate_scenario_against_network(scene.scenario, scene.network).empty());
}

TEST(XoscNetworkValidation, RouteFindingsArriveThroughTheOneCall) {
  // `validate_routes` stays public for the route overlay, but no caller should
  // have to remember to run both.
  Scene scene;
  osc::Route route;
  route.name = "EgoRoute";
  for (const char* road : {"1", "999"}) {
    osc::LanePosition position;
    position.road_id = road;
    position.lane_id = "-1";
    position.s = 10.0;
    osc::RouteWaypoint waypoint;
    waypoint.route_strategy = std::string{osc::kDefaultRouteStrategy};
    waypoint.position = position;
    route.waypoints.push_back(waypoint);
  }
  ASSERT_TRUE(
      scene.stack.push(scene.scenario, osc::edit::assign_route(scene.scenario, "Ego", route)));

  const std::vector<Diagnostic> findings =
      osc::validate_scenario_against_network(scene.scenario, scene.network);
  EXPECT_FALSE(findings.empty());
  EXPECT_TRUE(locates(findings, "Entity[Ego]"));
}

TEST(XoscNetworkValidation, AnEmptyNetworkReportsEveryReferenceRatherThanCrashing) {
  // The state a scene is in for a moment after File ▸ New with a scenario still
  // loaded — every reference dangles, and the checker must say so rather than
  // walk off the end of an empty arena.
  Scene scene;
  const RoadNetwork empty;
  const std::vector<Diagnostic> findings =
      osc::validate_scenario_against_network(scene.scenario, empty);
  EXPECT_TRUE(cites(findings, kControllerRule));
  EXPECT_TRUE(cites(findings, kSignalRule));
  EXPECT_TRUE(cites(findings, kRoadLaneRule));
}

} // namespace
