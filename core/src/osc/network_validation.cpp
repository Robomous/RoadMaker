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

/// The cross-document checks (issue #533). See osc/network_validation.hpp for
/// why they exist and what they deliberately do not cover.
///
/// TWO DISCIPLINES HOLD THROUGHOUT:
///
///   1. EVERY POSITION IN THE DOCUMENT IS WALKED, not just the ones a
///      particular feature happens to author. An init teleport, a story
///      action's teleport and a route waypoint are the same `osc::Position`
///      with the same failure mode, and a checker that knew about only the
///      first would pass a scenario whose story teleports an actor onto a road
///      that was deleted an hour ago.
///   2. A LOOKUP TABLE IS BUILT ONCE. A scenario with a hundred signal states
///      would otherwise walk the network's signal arena a hundred times, and
///      `for_each_signal` is arena-SLOT order rather than creation order — so
///      the map is also what makes the traversal deterministic.

#include "roadmaker/osc/network_validation.hpp"

#include "roadmaker/osc/route.hpp"
#include "roadmaker/osc/rules.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal.hpp"

#include <fmt/format.h>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace roadmaker::osc {
namespace {

Diagnostic error(std::string location, std::string message, std::string_view rule = {}) {
  return Diagnostic{.severity = Severity::Error,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = std::string(rule),
                    .road = {},
                    .lane = {}};
}

/// The network's reference surface, resolved once. See this file's header.
struct NetworkIndex {
  std::set<std::string> signal_odr_ids;
  std::set<std::string> controller_odr_ids;
};

NetworkIndex index_of(const RoadNetwork& network) {
  NetworkIndex index;
  network.for_each_signal([&index](SignalId, const Signal& signal) {
    if (!signal.odr_id.empty()) {
      index.signal_odr_ids.insert(signal.odr_id);
    }
  });
  network.for_each_controller([&index](ControllerId, const Controller& controller) {
    if (!controller.odr_id.empty()) {
      index.controller_odr_ids.insert(controller.odr_id);
    }
  });
  return index;
}

/// A `@trafficSignalId`-shaped reference, wherever it appears.
///
/// An EMPTY id is not reported here: `validate_scenario` already refuses it,
/// with the same rule id, and reporting it twice would make the Diagnostics
/// dock show one mistake as two.
void check_signal_ref(std::vector<Diagnostic>& findings,
                      const NetworkIndex& index,
                      const std::string& odr_id,
                      std::string_view what,
                      const std::string& location) {
  if (odr_id.empty() || index.signal_odr_ids.count(odr_id) != 0) {
    return;
  }
  findings.push_back(
      error(location,
            fmt::format("{} references signal '{}', which no <signal> in the road network "
                        "carries",
                        what,
                        odr_id),
            rules::kTrafficSignalStateReferences));
}

void check_controller_ref(std::vector<Diagnostic>& findings,
                          const NetworkIndex& index,
                          const std::string& odr_id,
                          std::string_view what,
                          const std::string& location) {
  if (odr_id.empty() || index.controller_odr_ids.count(odr_id) != 0) {
    return;
  }
  findings.push_back(
      error(location,
            fmt::format("{} references controller '{}', which no <controller> in the road "
                        "network carries",
                        what,
                        odr_id),
            rules::kTrafficSignalControllerReferences));
}

/// A `<RoadPosition>` or `<LanePosition>`, resolved against the network.
///
/// A `<WorldPosition>` names no road and is skipped — it cannot dangle.
void check_position(std::vector<Diagnostic>& findings,
                    const RoadNetwork& network,
                    const Position& position,
                    const std::string& location) {
  const std::string* road_odr_id = nullptr;
  const std::string* lane_odr_id = nullptr;
  double station = 0.0;
  const char* element = nullptr;

  if (const auto* road_position = std::get_if<RoadPosition>(&position)) {
    road_odr_id = &road_position->road_id;
    station = road_position->s;
    element = "RoadPosition";
  } else if (const auto* lane_position = std::get_if<LanePosition>(&position)) {
    road_odr_id = &lane_position->road_id;
    lane_odr_id = &lane_position->lane_id;
    station = lane_position->s;
    element = "LanePosition";
  } else {
    return;
  }

  // An EMPTY id is validate_scenario's finding, not this one's — see
  // check_signal_ref.
  if (road_odr_id->empty()) {
    return;
  }

  const std::string element_location = fmt::format("{}/{}", location, element);
  const RoadId road = network.find_road(*road_odr_id);
  if (!road.is_valid()) {
    findings.push_back(error(element_location,
                             fmt::format("<{}> names road '{}', which the road network does not "
                                         "contain",
                                         element,
                                         *road_odr_id),
                             rules::kRoadLaneExists));
    return; // Everything below resolves THROUGH the road; without it there is
            // nothing further to say, and saying it anyway reports one mistake
            // three times.
  }

  const Road* live = network.road(road);
  if (live == nullptr) {
    return;
  }

  // ★ THE UPPER BOUND, which is the whole reason this check needs a network.
  // `validate_scenario` refuses a negative `s` already; the road's length lives
  // in the `.xodr` and nowhere else.
  if (station > live->length) {
    findings.push_back(
        error(element_location,
              fmt::format("s-coordinate {} is past the end of road '{}', which is {} m long",
                          station,
                          *road_odr_id,
                          live->length),
              rules::kRoadLaneOffsetInBounds));
  }

  if (lane_odr_id == nullptr || lane_odr_id->empty()) {
    return;
  }

  // The lane is looked up in the section GOVERNING `s`, not in the road's first
  // section: a lane that exists at s=0 may legitimately not exist at s=90, and
  // checking the wrong section reports a live anchor as dangling.
  int numeric_lane_id = 0;
  try {
    numeric_lane_id = std::stoi(*lane_odr_id);
  } catch (const std::exception&) {
    // A non-numeric lane id is the temporary-lane-layer spelling the schema
    // permits (`LanePosition::lane_id`'s note). Nothing here can resolve it,
    // and reporting it would fire on legal input.
    return;
  }

  const double clamped = station > live->length ? live->length : station;
  const LaneSection* section = network.lane_section(section_at(network, road, clamped));
  if (section == nullptr) {
    return;
  }
  for (const LaneId id : section->lanes) {
    const Lane* lane = network.lane(id);
    if (lane != nullptr && lane->odr_id == numeric_lane_id) {
      return;
    }
  }
  findings.push_back(error(element_location,
                           fmt::format("<LanePosition> names lane '{}', which road '{}' does not "
                                       "have at s = {}",
                                       *lane_odr_id,
                                       *road_odr_id,
                                       clamped),
                           rules::kRoadLaneExists));
}

/// Every position and every signal reference a `PrivateAction` can carry.
void check_private_action(std::vector<Diagnostic>& findings,
                          const RoadNetwork& network,
                          const PrivateAction& action,
                          const std::string& location) {
  if (action.teleport.has_value()) {
    check_position(
        findings, network, action.teleport->position, location + "/TeleportAction/Position");
  }
  // A route's waypoints are checked by `validate_routes`, which resolves them
  // as a PATH rather than as isolated anchors — a stricter check than this one,
  // so running both would report the same dangling waypoint twice.
}

void check_global_action(std::vector<Diagnostic>& findings,
                         const NetworkIndex& index,
                         const GlobalAction& global,
                         const std::string& location) {
  if (!global.infrastructure.has_value()) {
    return;
  }
  const std::string signal_location = location + "/InfrastructureAction/TrafficSignalAction";
  const auto& arm = global.infrastructure->traffic_signal.action;
  if (const auto* state = std::get_if<TrafficSignalStateAction>(&arm)) {
    check_signal_ref(findings,
                     index,
                     state->name,
                     "traffic signal state action",
                     signal_location + "/TrafficSignalStateAction");
  } else if (const auto* controller = std::get_if<TrafficSignalControllerAction>(&arm)) {
    check_controller_ref(findings,
                         index,
                         controller->traffic_signal_controller_ref,
                         "traffic signal controller action",
                         signal_location + "/TrafficSignalControllerAction");
  }
}

void check_trigger(std::vector<Diagnostic>& findings,
                   const NetworkIndex& index,
                   const Trigger& trigger,
                   const std::string& location) {
  for (std::size_t group_index = 0; group_index < trigger.condition_groups.size(); ++group_index) {
    const ConditionGroup& group = trigger.condition_groups[group_index];
    for (std::size_t index_of_condition = 0; index_of_condition < group.conditions.size();
         ++index_of_condition) {
      const Condition& condition = group.conditions[index_of_condition];
      const std::string condition_location = fmt::format(
          "{}/ConditionGroup[{}]/Condition[{}]", location, group_index, index_of_condition);

      if (condition.traffic_signal.has_value()) {
        check_signal_ref(findings,
                         index,
                         condition.traffic_signal->name,
                         "traffic signal condition",
                         condition_location + "/ByValueCondition/TrafficSignalCondition");
      }
      if (condition.traffic_signal_controller.has_value()) {
        check_controller_ref(findings,
                             index,
                             condition.traffic_signal_controller->traffic_signal_controller_ref,
                             "traffic signal controller condition",
                             condition_location +
                                 "/ByValueCondition/TrafficSignalControllerCondition");
      }
    }
  }
}

void check_optional_trigger(std::vector<Diagnostic>& findings,
                            const NetworkIndex& index,
                            const std::optional<Trigger>& trigger,
                            const std::string& location) {
  if (trigger.has_value()) {
    check_trigger(findings, index, *trigger, location);
  }
}

} // namespace

std::vector<Diagnostic> validate_scenario_against_network(const Scenario& scenario,
                                                          const RoadNetwork& network) {
  std::vector<Diagnostic> findings;
  const NetworkIndex index = index_of(network);

  // --- <TrafficSignals> ------------------------------------------------------
  //
  // The half `rules.hpp` says twice that `validate_scenario` cannot make: it
  // can only see that an id is non-empty, because "whether it names a live
  // <signal> depends on the .xodr this scenario points at, which the writer
  // does not read".
  const auto& controllers = scenario.road_network.traffic_signal_controllers;
  for (std::size_t controller_index = 0; controller_index < controllers.size();
       ++controller_index) {
    const TrafficSignalController& controller = controllers[controller_index];
    const std::string location =
        fmt::format("RoadNetwork/TrafficSignals/TrafficSignalController[{}]", controller_index);
    check_controller_ref(findings, index, controller.name, "traffic signal controller", location);

    for (std::size_t phase_index = 0; phase_index < controller.phases.size(); ++phase_index) {
      const Phase& phase = controller.phases[phase_index];
      for (std::size_t state_index = 0; state_index < phase.signal_states.size(); ++state_index) {
        check_signal_ref(
            findings,
            index,
            phase.signal_states[state_index].traffic_signal_id,
            "traffic signal state",
            fmt::format("{}/Phase[{}]/TrafficSignalState[{}]", location, phase_index, state_index));
      }
    }
  }

  // --- <Init> ----------------------------------------------------------------
  const auto& privates = scenario.storyboard.init.actions.privates;
  for (std::size_t private_index = 0; private_index < privates.size(); ++private_index) {
    const Private& entry = privates[private_index];
    for (std::size_t action_index = 0; action_index < entry.actions.size(); ++action_index) {
      check_private_action(findings,
                           network,
                           entry.actions[action_index],
                           fmt::format("Storyboard/Init/Actions/Private[{}]/PrivateAction[{}]",
                                       private_index,
                                       action_index));
    }
  }

  // --- the storyboard --------------------------------------------------------
  //
  // Walked in full (p8-s4, #248 modeled it): a story action's teleport and a
  // story trigger's signal reference dangle exactly as an init one does, and a
  // checker that stopped at <Init> would pass a scenario whose traffic-light
  // half references nothing — which is the failure this whole file exists for.
  check_trigger(findings, index, scenario.storyboard.stop_trigger, "Storyboard/StopTrigger");

  for (std::size_t story_index = 0; story_index < scenario.storyboard.stories.size();
       ++story_index) {
    const Story& story = scenario.storyboard.stories[story_index];
    const std::string story_location = fmt::format("Storyboard/Story[{}]", story_index);
    for (std::size_t act_index = 0; act_index < story.acts.size(); ++act_index) {
      const Act& act = story.acts[act_index];
      const std::string act_location = fmt::format("{}/Act[{}]", story_location, act_index);
      check_optional_trigger(findings, index, act.start_trigger, act_location + "/StartTrigger");
      check_optional_trigger(findings, index, act.stop_trigger, act_location + "/StopTrigger");

      for (std::size_t group_index = 0; group_index < act.maneuver_groups.size(); ++group_index) {
        const ManeuverGroup& group = act.maneuver_groups[group_index];
        const std::string group_location =
            fmt::format("{}/ManeuverGroup[{}]", act_location, group_index);
        for (std::size_t maneuver_index = 0; maneuver_index < group.maneuvers.size();
             ++maneuver_index) {
          const StoryManeuver& maneuver = group.maneuvers[maneuver_index];
          const std::string maneuver_location =
              fmt::format("{}/Maneuver[{}]", group_location, maneuver_index);
          for (std::size_t event_index = 0; event_index < maneuver.events.size(); ++event_index) {
            const Event& event = maneuver.events[event_index];
            const std::string event_location =
                fmt::format("{}/Event[{}]", maneuver_location, event_index);
            check_optional_trigger(
                findings, index, event.start_trigger, event_location + "/StartTrigger");

            for (std::size_t action_index = 0; action_index < event.actions.size();
                 ++action_index) {
              const Action& action = event.actions[action_index];
              const std::string action_location =
                  fmt::format("{}/Action[{}]", event_location, action_index);
              if (const auto* global = std::get_if<GlobalAction>(&action.action)) {
                check_global_action(findings, index, *global, action_location + "/GlobalAction");
              } else if (const auto* entry = std::get_if<PrivateAction>(&action.action)) {
                check_private_action(findings, network, *entry, action_location + "/PrivateAction");
              }
            }
          }
        }
      }
    }
  }

  // --- routes ----------------------------------------------------------------
  //
  // Appended rather than reimplemented: `validate_routes` resolves a route as a
  // drivable PATH, which is strictly more than checking each waypoint's anchor,
  // and duplicating the anchor check here would report one broken waypoint
  // twice.
  std::vector<Diagnostic> route_findings = validate_routes(network, scenario);
  findings.insert(findings.end(),
                  std::make_move_iterator(route_findings.begin()),
                  std::make_move_iterator(route_findings.end()));

  return findings;
}

} // namespace roadmaker::osc
