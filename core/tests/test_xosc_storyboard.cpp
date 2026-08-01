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

// The modeled storyboard (p8-s4, issue #248): Story ▸ Act ▸ ManeuverGroup ▸
// Maneuver ▸ Event ▸ Action, the condition palette, the phase-name trap, the
// round trip, and the two edit commands.
//
// ASSERTIONS SLICE A SECTION, they do not search the document — the discipline
// test_xosc_writer.cpp states at length. A global `find` is satisfied by a
// mention anywhere, which is how a writer that emitted <Act> as a sibling of
// <Story> would pass a checklist of `contains`.

#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/reader.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using roadmaker::Diagnostic;
using roadmaker::Severity;
namespace osc = roadmaker::osc;

/// The text of one element, from its start tag through its close tag.
/// Borrows `doc`, so `doc` must outlive it — the rvalue overload is deleted.
std::string_view
element_slice(const std::string& doc, std::string_view open, std::string_view close) {
  const std::size_t start = doc.find(open);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end = doc.find(close, start);
  if (end == std::string::npos) {
    return {};
  }
  return std::string_view(doc).substr(start, end + close.size() - start);
}

std::string_view element_slice(std::string&&, std::string_view, std::string_view) = delete;

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string written(const osc::Scenario& scenario) {
  const auto text = osc::write_xosc(scenario);
  EXPECT_TRUE(text.has_value()) << (text.has_value() ? "" : text.error().message);
  return text.value_or(std::string{});
}

bool any_message_contains(const std::vector<Diagnostic>& findings, std::string_view needle) {
  for (const Diagnostic& finding : findings) {
    if (finding.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool any_error_cites(const std::vector<Diagnostic>& findings, std::string_view rule) {
  for (const Diagnostic& finding : findings) {
    if (finding.severity == Severity::Error && finding.rule_id == rule) {
      return true;
    }
  }
  return false;
}

bool has_error(const std::vector<Diagnostic>& findings) {
  for (const Diagnostic& finding : findings) {
    if (finding.severity == Severity::Error) {
      return true;
    }
  }
  return false;
}

// --- fixtures ----------------------------------------------------------------

/// A scenario with one entity, a linked network, and no story yet.
osc::Scenario base_scenario() {
  osc::Scenario scenario;
  scenario.road_network.logic_file = osc::FileRef{.filepath = "network.xodr", .preserved = {}};

  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(ego);

  osc::ScenarioObject target;
  target.name = "Target";
  target.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(target);
  return scenario;
}

/// A controller whose phases carry NO names — the shape `SignalPhase::name`
/// being legally empty produces, and the whole reason `osc::phase_names` exists.
osc::TrafficSignalController unnamed_phase_controller() {
  osc::TrafficSignalController controller;
  controller.name = "42";
  osc::Phase go;
  go.duration = 20.0;
  go.semantics = osc::PhaseSemantics::Go;
  osc::Phase stop;
  stop.duration = 20.0;
  stop.semantics = osc::PhaseSemantics::Stop;
  controller.phases.push_back(go);
  controller.phases.push_back(stop);
  return controller;
}

/// The smallest story the schema admits, wrapping `action`.
osc::Story story_around(osc::Action action, std::vector<std::string> actors = {"Ego"}) {
  osc::Event event;
  event.name = "e";
  event.actions.push_back(std::move(action));

  osc::StoryManeuver maneuver;
  maneuver.name = "m";
  maneuver.events.push_back(std::move(event));

  osc::ManeuverGroup group;
  group.name = "g";
  for (const std::string& actor : actors) {
    group.actors.push_back(osc::EntityRef{.entity_ref = actor, .preserved = {}});
  }
  group.maneuvers.push_back(std::move(maneuver));

  osc::Act act;
  act.name = "a";
  act.maneuver_groups.push_back(std::move(group));

  osc::Story story;
  story.name = "s";
  story.acts.push_back(std::move(act));
  return story;
}

/// The cut-in: a lane change one lane left, relative to the actor itself.
osc::Action cut_in_action() {
  osc::LaneChangeAction change;
  change.dynamics.dynamics_shape = std::string{osc::kDefaultLaneChangeShape};
  change.dynamics.value = osc::kDefaultLaneChangeDuration;
  change.target = osc::RelativeTargetLane{.entity_ref = "Ego", .value = 1, .preserved = {}};

  osc::LateralAction lateral;
  lateral.lane_change = std::move(change);

  osc::PrivateAction entry;
  entry.lateral = std::move(lateral);

  osc::Action action;
  action.name = "cut_in";
  action.action = std::move(entry);
  return action;
}

// --- the tree ----------------------------------------------------------------

TEST(XoscStoryboard, TheStoryTreeNestsRatherThanFlattening) {
  osc::Scenario scenario = base_scenario();
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));

  const std::string text = written(scenario);

  // Each assertion is scoped to its PARENT's slice; presence alone would pass
  // on a writer that emitted every level as a sibling of <Storyboard>.
  const std::string_view story = element_slice(text, "<Story ", "</Story>");
  ASSERT_FALSE(story.empty()) << text;
  const std::string_view act = element_slice(text, "<Act ", "</Act>");
  ASSERT_FALSE(act.empty()) << text;
  EXPECT_TRUE(contains(story, "<Act ")) << story;
  EXPECT_TRUE(contains(act, "<ManeuverGroup ")) << act;

  const std::string_view group = element_slice(text, "<ManeuverGroup ", "</ManeuverGroup>");
  ASSERT_FALSE(group.empty()) << text;
  EXPECT_TRUE(contains(group, "<Actors ")) << group;
  EXPECT_TRUE(contains(group, "<Maneuver ")) << group;

  const std::string_view maneuver = element_slice(text, "<Maneuver ", "</Maneuver>");
  ASSERT_FALSE(maneuver.empty()) << text;
  EXPECT_TRUE(contains(maneuver, "<Event ")) << maneuver;

  const std::string_view event = element_slice(text, "<Event ", "</Event>");
  ASSERT_FALSE(event.empty()) << text;
  EXPECT_TRUE(contains(event, "<Action ")) << event;
  EXPECT_TRUE(contains(event, "<LaneChangeAction")) << event;
}

TEST(XoscStoryboard, TheDefaultEventPriorityIsTheSpellingRevisionOnePointTwoAdmits) {
  // "override" was created after 1.2, which is what the writer targets by
  // default (osc/writer.hpp). Emitting it by default would make the writer's own
  // esmini acceptance gate unreachable — so the default is the 1.0 spelling,
  // deprecated in 1.4 but declared in every revision.
  osc::Scenario scenario = base_scenario();
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));

  const std::string text = written(scenario);
  const std::string_view event = element_slice(text, "<Event ", "</Event>");
  ASSERT_FALSE(event.empty()) << text;
  EXPECT_TRUE(contains(event, R"(priority="overwrite")")) << event;
  EXPECT_FALSE(contains(event, R"(priority="override")")) << event;
}

TEST(XoscStoryboard, AnOmittedMaximumExecutionCountStaysOmitted) {
  // Absent and explicitly-1 mean the same thing to a simulator but NOT to the
  // byte-identity contract; writing the default into a file that omitted it is
  // how a round trip stops being a fixed point.
  osc::Scenario scenario = base_scenario();
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));

  const std::string text = written(scenario);
  const std::string_view event = element_slice(text, "<Event ", "</Event>");
  ASSERT_FALSE(event.empty()) << text;
  // The START TAG only — searching the whole element would be satisfied by a
  // maximumExecutionCount on a nested <ManeuverGroup> in some later fixture.
  EXPECT_FALSE(contains(event.substr(0, event.find('>')), "maximumExecutionCount")) << event;

  scenario.storyboard.stories[0]
      .acts[0]
      .maneuver_groups[0]
      .maneuvers[0]
      .events[0]
      .maximum_execution_count = 3;
  const std::string with_count = written(scenario);
  EXPECT_TRUE(contains(with_count, R"(maximumExecutionCount="3")")) << with_count;
}

// --- the phase-name trap (the headline of #248) -------------------------------

TEST(XoscStoryboard, PhaseNamesAreWhatTheWriterEmitsAndNotPhaseName) {
  const osc::TrafficSignalController controller = unnamed_phase_controller();

  // The model's own names are EMPTY — an author reading them would build a
  // reference to "".
  EXPECT_TRUE(controller.phases[0].name.empty());
  EXPECT_TRUE(controller.phases[1].name.empty());

  const std::vector<std::string> names = osc::phase_names(controller);
  ASSERT_EQ(names.size(), 2U);
  EXPECT_EQ(names[0], "go");
  EXPECT_EQ(names[1], "stop");

  // And those are the names the file carries.
  osc::Scenario scenario = base_scenario();
  scenario.road_network.traffic_signal_controllers.push_back(controller);
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<Phase name="go")")) << text;
  EXPECT_TRUE(contains(text, R"(<Phase name="stop")")) << text;
}

TEST(XoscStoryboard, PhaseNamesDeduplicateWithinAControllerAndNotAcrossThem) {
  osc::TrafficSignalController controller;
  controller.name = "1";
  osc::Phase first;
  first.name = "go";
  osc::Phase second;
  second.semantics = osc::PhaseSemantics::Go; // synthesizes "go" -> collides
  controller.phases.push_back(first);
  controller.phases.push_back(second);

  const std::vector<std::string> names = osc::phase_names(controller);
  ASSERT_EQ(names.size(), 2U);
  EXPECT_EQ(names[0], "go") << "an authored name must win over a synthesized one";
  EXPECT_EQ(names[1], "go_1");

  // A SECOND controller starts a fresh pool. Sharing it would rename this
  // controller's phases to "go_2" and break the invariant that every controller
  // decomposed from one junction timeline carries the same phase names — which
  // is exactly what a traffic-signal condition references.
  const std::vector<std::string> again = osc::phase_names(controller);
  EXPECT_EQ(again, names);
}

TEST(XoscStoryboard, APhaseReferenceThatMatchesNoWrittenNameIsRefused) {
  osc::Scenario scenario = base_scenario();
  scenario.road_network.traffic_signal_controllers.push_back(unnamed_phase_controller());

  osc::Condition condition;
  condition.name = "wait_for_green";
  condition.traffic_signal_controller = osc::TrafficSignalControllerCondition{
      .traffic_signal_controller_ref = "42", .phase = "", .preserved = {}};
  osc::ConditionGroup group;
  group.conditions.push_back(condition);
  osc::Trigger trigger;
  trigger.condition_groups.push_back(group);

  osc::Action action = cut_in_action();
  osc::Story story = story_around(action);
  story.acts[0].maneuver_groups[0].maneuvers[0].events[0].start_trigger = trigger;
  scenario.storyboard.stories.push_back(story);

  // ★ THE MODEL'S OWN Phase::name IS "" — so a @phase built from it is "" too,
  // and the reference matches nothing in the file. Refused, not written.
  EXPECT_TRUE(has_error(osc::validate_scenario(scenario)));
  EXPECT_FALSE(osc::write_xosc(scenario).has_value());

  // A name that exists in the MODEL but not in the OUTPUT is refused just the
  // same — this is the trap in its purest form.
  scenario.storyboard.stories[0]
      .acts[0]
      .maneuver_groups[0]
      .maneuvers[0]
      .events[0]
      .start_trigger->condition_groups[0]
      .conditions[0]
      .traffic_signal_controller->phase = "phase_that_is_not_written";
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "has no phase of that name"));

  // Resolved through phase_names, it writes.
  scenario.storyboard.stories[0]
      .acts[0]
      .maneuver_groups[0]
      .maneuvers[0]
      .events[0]
      .start_trigger->condition_groups[0]
      .conditions[0]
      .traffic_signal_controller->phase =
      osc::phase_names(scenario.road_network.traffic_signal_controllers[0])[0];
  EXPECT_FALSE(has_error(osc::validate_scenario(scenario)));
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(
      text, R"(<TrafficSignalControllerCondition trafficSignalControllerRef="42" phase="go")"))
      << text;
}

TEST(XoscStoryboard, ADanglingControllerReferenceIsRefusedWithItsRuleId) {
  osc::Scenario scenario = base_scenario();
  scenario.road_network.traffic_signal_controllers.push_back(unnamed_phase_controller());

  osc::TrafficSignalControllerAction controller_action;
  controller_action.traffic_signal_controller_ref = "999";
  controller_action.phase = "go";

  osc::TrafficSignalAction signal_action;
  signal_action.action = controller_action;
  osc::InfrastructureAction infrastructure;
  infrastructure.traffic_signal = signal_action;
  osc::GlobalAction global;
  global.infrastructure = infrastructure;

  osc::Action action;
  action.name = "force_green";
  action.action = global;
  scenario.storyboard.stories.push_back(story_around(action, {}));

  const std::vector<Diagnostic> findings = osc::validate_scenario(scenario);
  EXPECT_TRUE(any_error_cites(
      findings, "asam.net:xosc:1.0.0:reference_control.traffic_signal_controller_references"));
  EXPECT_FALSE(osc::write_xosc(scenario).has_value());
}

// --- references ---------------------------------------------------------------

TEST(XoscStoryboard, EveryEntityReferenceInTheStoryIsResolved) {
  osc::Scenario scenario = base_scenario();

  // An actor naming no entity.
  scenario.storyboard.stories.push_back(story_around(cut_in_action(), {"Ghost"}));
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "references entity 'Ghost'"));

  // A relative target lane naming no entity.
  scenario.storyboard.stories.clear();
  osc::Action action = cut_in_action();
  std::get<osc::PrivateAction>(action.action).lateral->lane_change->target =
      osc::RelativeTargetLane{.entity_ref = "Ghost", .value = 1, .preserved = {}};
  scenario.storyboard.stories.push_back(story_around(action));
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "references entity 'Ghost'"));

  // A triggering entity naming no entity.
  scenario.storyboard.stories.clear();
  osc::Condition condition;
  condition.name = "close";
  osc::ByEntityCondition by_entity;
  by_entity.triggering_entities.entity_refs.push_back(
      osc::EntityRef{.entity_ref = "Ghost", .preserved = {}});
  by_entity.entity_condition =
      osc::RelativeDistanceCondition{.entity_ref = "Target",
                                     .freespace = true,
                                     .relative_distance_type = "longitudinal",
                                     .rule = "lessThan",
                                     .value = 12.0,
                                     .preserved = {}};
  condition.by_entity = by_entity;
  osc::ConditionGroup group;
  group.conditions.push_back(condition);
  osc::Trigger trigger;
  trigger.condition_groups.push_back(group);

  osc::Story story = story_around(cut_in_action());
  story.acts[0].maneuver_groups[0].maneuvers[0].events[0].start_trigger = trigger;
  scenario.storyboard.stories.push_back(story);
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "references entity 'Ghost'"));
}

TEST(XoscStoryboard, TheSchemaMinimumsAreReportedButStillWritten) {
  // ★ WARNINGS, NOT ERRORS, and that is the never-drop contract rather than
  // leniency: a foreign `.xosc` carrying an <Act> with no <ManeuverGroup> was
  // readable before this version modeled <Story>, and refusing to write it back
  // would mean a document RoadMaker just loaded can no longer be saved.
  // Authoring is guarded a layer up — `osc::edit::set_story` refuses a story
  // with no act — so the editor cannot produce these shapes.
  osc::Scenario scenario = base_scenario();

  osc::Story empty_story;
  empty_story.name = "s";
  scenario.storyboard.stories.push_back(empty_story);
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "<Story> has no <Act>"));
  EXPECT_FALSE(has_error(osc::validate_scenario(scenario)));
  EXPECT_TRUE(osc::write_xosc(scenario).has_value());

  scenario.storyboard.stories[0].acts.push_back(osc::Act{.name = "a",
                                                         .maneuver_groups = {},
                                                         .start_trigger = std::nullopt,
                                                         .stop_trigger = std::nullopt,
                                                         .preserved = {}});
  EXPECT_TRUE(
      any_message_contains(osc::validate_scenario(scenario), "<Act> has no <ManeuverGroup>"));

  osc::ManeuverGroup group;
  group.name = "g";
  osc::StoryManeuver maneuver;
  maneuver.name = "m";
  group.maneuvers.push_back(maneuver);
  scenario.storyboard.stories[0].acts[0].maneuver_groups.push_back(group);
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "<Maneuver> has no <Event>"));

  osc::Event event;
  event.name = "e";
  scenario.storyboard.stories[0].acts[0].maneuver_groups[0].maneuvers[0].events.push_back(event);
  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "<Event> has no <Action>"));

  // Every level reported, and the document still writes at every step.
  EXPECT_FALSE(has_error(osc::validate_scenario(scenario)));
  EXPECT_TRUE(osc::write_xosc(scenario).has_value());
}

TEST(XoscStoryboard, DuplicateSiblingNamesAreReportedWithTheUniquenessRule) {
  osc::Scenario scenario = base_scenario();
  osc::Story story = story_around(cut_in_action());
  osc::Act duplicate = story.acts[0];
  story.acts.push_back(duplicate);
  scenario.storyboard.stories.push_back(story);

  const std::vector<Diagnostic> findings = osc::validate_scenario(scenario);
  bool cited = false;
  for (const Diagnostic& finding : findings) {
    cited =
        cited || finding.rule_id == "asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level";
  }
  EXPECT_TRUE(cited);
  // A warning, for the reason the schema-minimum test above states. The EDIT
  // layer is where a duplicate is refused: `set_story` rejects a duplicate
  // story name, and the panel de-duplicates every name it generates.
  EXPECT_FALSE(has_error(findings));
}

TEST(XoscStoryboard, AConditionCarryingTwoArmsIsRefused) {
  osc::Scenario scenario = base_scenario();

  osc::Condition condition;
  condition.name = "both";
  condition.simulation_time = osc::SimulationTimeCondition{};
  condition.storyboard_element_state =
      osc::StoryboardElementStateCondition{.storyboard_element_ref = "e",
                                           .state = "completeState",
                                           .storyboard_element_type = "event",
                                           .preserved = {}};
  osc::ConditionGroup group;
  group.conditions.push_back(condition);
  scenario.storyboard.stop_trigger.condition_groups.push_back(group);

  EXPECT_TRUE(
      any_error_cites(osc::validate_scenario(scenario), "asam.net:xosc:1.0.0:xml.valid_schema"));
}

TEST(XoscStoryboard, AStoryActionCarryingTwoPrivateArmsIsRefused) {
  // In <Init> a multi-armed PrivateAction legally expands into two elements,
  // because <Private> admits <PrivateAction>*. Inside a story <Action> it
  // cannot: <Action> wraps a single choice arm.
  osc::Scenario scenario = base_scenario();

  osc::Action action = cut_in_action();
  std::get<osc::PrivateAction>(action.action).teleport =
      osc::TeleportAction{.position = osc::WorldPosition{}, .preserved = {}};
  scenario.storyboard.stories.push_back(story_around(action));

  EXPECT_TRUE(any_message_contains(osc::validate_scenario(scenario), "private-action arms"));
}

TEST(XoscStoryboard, ALaneChangeWithoutALogicFileIsRefusedWithItsRuleId) {
  osc::Scenario scenario = base_scenario();
  scenario.road_network.logic_file.reset();
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));

  EXPECT_TRUE(
      any_error_cites(osc::validate_scenario(scenario),
                      "asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network"));
}

// --- the round trip -----------------------------------------------------------

TEST(XoscStoryboard, AWholeCutInAndTrafficLightScenarioRoundTrips) {
  osc::Scenario scenario = base_scenario();
  scenario.road_network.traffic_signal_controllers.push_back(unnamed_phase_controller());

  // The cut-in half: a lane change triggered by closing distance.
  osc::Condition close;
  close.name = "close_enough";
  osc::ByEntityCondition by_entity;
  by_entity.triggering_entities.entity_refs.push_back(
      osc::EntityRef{.entity_ref = "Target", .preserved = {}});
  by_entity.entity_condition =
      osc::RelativeDistanceCondition{.entity_ref = "Ego",
                                     .freespace = true,
                                     .relative_distance_type = "longitudinal",
                                     .rule = "lessThan",
                                     .value = 12.0,
                                     .preserved = {}};
  close.by_entity = by_entity;
  osc::ConditionGroup close_group;
  close_group.conditions.push_back(close);
  osc::Trigger cut_in_trigger;
  cut_in_trigger.condition_groups.push_back(close_group);

  osc::Story story = story_around(cut_in_action(), {"Target"});
  story.acts[0].maneuver_groups[0].maneuvers[0].events[0].start_trigger = cut_in_trigger;

  // The traffic-light half: a controller phase forced when its own phase runs.
  osc::TrafficSignalControllerAction force;
  force.traffic_signal_controller_ref = "42";
  force.phase = osc::phase_names(scenario.road_network.traffic_signal_controllers[0])[1];
  osc::TrafficSignalAction signal_action;
  signal_action.action = force;
  osc::InfrastructureAction infrastructure;
  infrastructure.traffic_signal = signal_action;
  osc::GlobalAction global;
  global.infrastructure = infrastructure;

  osc::Action light_action;
  light_action.name = "go_red";
  light_action.action = global;

  osc::Event light_event;
  light_event.name = "light";
  light_event.actions.push_back(light_action);

  osc::StoryManeuver light_maneuver;
  light_maneuver.name = "lights";
  light_maneuver.events.push_back(light_event);

  osc::ManeuverGroup light_group;
  light_group.name = "infrastructure";
  light_group.maneuvers.push_back(light_maneuver);
  story.acts[0].maneuver_groups.push_back(light_group);

  scenario.storyboard.stories.push_back(story);

  // And the scenario ends when the cut-in event completes.
  osc::Condition done;
  done.name = "cut_in_done";
  done.storyboard_element_state =
      osc::StoryboardElementStateCondition{.storyboard_element_ref = "e",
                                           .state = "completeState",
                                           .storyboard_element_type = "event",
                                           .preserved = {}};
  osc::ConditionGroup done_group;
  done_group.conditions.push_back(done);
  scenario.storyboard.stop_trigger.condition_groups.push_back(done_group);

  const std::string first = written(scenario);
  ASSERT_FALSE(first.empty());

  const auto reparsed = osc::parse_xosc(first, "roundtrip.xosc");
  ASSERT_TRUE(reparsed.has_value()) << (reparsed.has_value() ? "" : reparsed.error().message);
  const std::string second = written(reparsed->scenario);

  // ★ THE BYTES ARE THE ORACLE. A field-by-field comparison passes on a model
  // that silently normalizes something the writer then emits differently.
  EXPECT_EQ(first, second);

  // And the tree really came back, rather than surviving as a preserved blob.
  ASSERT_EQ(reparsed->scenario.storyboard.stories.size(), 1U);
  const osc::Story& back = reparsed->scenario.storyboard.stories[0];
  ASSERT_EQ(back.acts.size(), 1U);
  ASSERT_EQ(back.acts[0].maneuver_groups.size(), 2U);
  EXPECT_EQ(back.acts[0].maneuver_groups[0].actors.size(), 1U);
  EXPECT_TRUE(back.acts[0].maneuver_groups[0].maneuvers[0].events[0].start_trigger.has_value());
}

TEST(XoscStoryboard, AnUnmodeledEntityConditionRidesTheWrapperTierNotTheCondition) {
  // Fourteen of the sixteen <EntityCondition> arms are unmodeled, and they must
  // come back INSIDE <EntityCondition> rather than beside it — the
  // Vehicle::properties_preserved trap, met a third time.
  const std::string document =
      R"(<?xml version="1.0" encoding="UTF-8"?><OpenSCENARIO>)"
      R"(<FileHeader revMajor="1" revMinor="2" date="1970-01-01T00:00:00" description="d" author="a"/>)"
      R"(<ParameterDeclarations/><CatalogLocations/>)"
      R"(<RoadNetwork><LogicFile filepath="n.xodr"/></RoadNetwork>)"
      R"(<Entities><ScenarioObject name="Ego"><Vehicle name="c" vehicleCategory="car">)"
      R"(<BoundingBox><Center x="0" y="0" z="0"/><Dimensions width="2" length="4" height="1.5"/></BoundingBox>)"
      R"(<Performance maxSpeed="70" maxAcceleration="5" maxDeceleration="10"/>)"
      R"(<Axles><FrontAxle maxSteering="0.5" wheelDiameter="0.6" trackWidth="1.8" positionX="3" positionZ="0.3"/>)"
      R"(<RearAxle maxSteering="0" wheelDiameter="0.6" trackWidth="1.8" positionX="0" positionZ="0.3"/></Axles>)"
      R"(<Properties/></Vehicle></ScenarioObject></Entities>)"
      R"(<Storyboard><Init><Actions/></Init><StopTrigger><ConditionGroup>)"
      R"(<Condition name="c" delay="0" conditionEdge="rising"><ByEntityCondition>)"
      R"(<TriggeringEntities triggeringEntitiesRule="any"><EntityRef entityRef="Ego"/></TriggeringEntities>)"
      R"(<EntityCondition><StandStillCondition duration="3"/></EntityCondition>)"
      R"(</ByEntityCondition></Condition></ConditionGroup></StopTrigger></Storyboard></OpenSCENARIO>)";

  const auto result = osc::parse_xosc(document, "unmodeled.xosc");
  ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);

  const osc::Condition& condition =
      result->scenario.storyboard.stop_trigger.condition_groups[0].conditions[0];
  ASSERT_TRUE(condition.by_entity.has_value());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(condition.by_entity->entity_condition));
  ASSERT_EQ(condition.by_entity->entity_condition_preserved.children.size(), 1U);
  EXPECT_TRUE(condition.by_entity->preserved.children.empty())
      << "the arm landed on <ByEntityCondition> and will be emitted beside <EntityCondition>";

  const std::string text = written(result->scenario);
  const std::string_view entity = element_slice(text, "<EntityCondition>", "</EntityCondition>");
  ASSERT_FALSE(entity.empty()) << text;
  EXPECT_TRUE(contains(entity, "<StandStillCondition")) << entity;
}

TEST(XoscStoryboard, ACatalogReferenceComesBackBeforeTheManeuversNotAfterThem) {
  // <CatalogReference>* sits between <Actors> and <Maneuver>* in the sequence,
  // so it cannot ride `preserved.children`, which are re-emitted last.
  osc::Scenario scenario = base_scenario();
  osc::Story story = story_around(cut_in_action());
  story.acts[0].maneuver_groups[0].preserved_catalog_references.push_back(
      R"(<CatalogReference catalogName="ManeuverCatalog" entryName="Overtake"/>)");
  scenario.storyboard.stories.push_back(story);

  const std::string text = written(scenario);
  const std::size_t actors = text.find("<Actors ");
  const std::size_t catalog = text.find("<CatalogReference ");
  const std::size_t maneuver = text.find("<Maneuver ");
  ASSERT_NE(catalog, std::string::npos) << text;
  EXPECT_LT(actors, catalog) << text;
  EXPECT_LT(catalog, maneuver) << text;
}

// --- the edit commands ---------------------------------------------------------

TEST(XoscStoryboard, SetStoryAppendsThenReplacesAndUndoIsByteIdentical) {
  osc::Scenario scenario = base_scenario();
  const std::string before = written(scenario);

  osc::edit::ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, osc::edit::set_story(scenario, 0, story_around(cut_in_action())))
                  .has_value());
  ASSERT_EQ(scenario.storyboard.stories.size(), 1U);
  const std::string appended = written(scenario);
  EXPECT_NE(appended, before);

  osc::Story renamed = story_around(cut_in_action());
  renamed.name = "renamed";
  ASSERT_TRUE(stack.push(scenario, osc::edit::set_story(scenario, 0, renamed)).has_value());
  ASSERT_EQ(scenario.storyboard.stories.size(), 1U) << "a replace must not append";
  EXPECT_EQ(scenario.storyboard.stories[0].name, "renamed");

  // apply -> revert leaves write_xosc byte-identical, both ways down the stack.
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(written(scenario), appended);
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(written(scenario), before);
  ASSERT_TRUE(stack.redo(scenario).has_value());
  EXPECT_EQ(written(scenario), appended);
}

TEST(XoscStoryboard, SetStoryRefusesWhatWouldMakeTheDocumentUnwritable) {
  osc::Scenario scenario = base_scenario();
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));

  osc::edit::ScenarioStack stack;

  osc::Story unnamed = story_around(cut_in_action());
  unnamed.name.clear();
  EXPECT_FALSE(stack.push(scenario, osc::edit::set_story(scenario, 0, unnamed)).has_value());

  osc::Story actless = story_around(cut_in_action());
  actless.name = "other";
  actless.acts.clear();
  EXPECT_FALSE(stack.push(scenario, osc::edit::set_story(scenario, 0, actless)).has_value());

  // A duplicate name against a DIFFERENT index; replacing index 0 with its own
  // name must still be allowed, which is what the `other != index` guard is for.
  scenario.storyboard.stories.push_back(story_around(cut_in_action()));
  scenario.storyboard.stories[1].name = "second";
  osc::Story clash = story_around(cut_in_action());
  clash.name = "second";
  EXPECT_FALSE(stack.push(scenario, osc::edit::set_story(scenario, 0, clash)).has_value());

  osc::Story same_name = story_around(cut_in_action());
  same_name.name = scenario.storyboard.stories[0].name;
  EXPECT_TRUE(stack.push(scenario, osc::edit::set_story(scenario, 0, same_name)).has_value());

  // Never clamped: an index past the end is refused, because a clamped index
  // rewrites a story the caller did not name.
  EXPECT_FALSE(
      stack.push(scenario, osc::edit::set_story(scenario, 99, story_around(cut_in_action())))
          .has_value());
}

TEST(XoscStoryboard, RemoveStoryRestoresItInPlaceOnUndo) {
  osc::Scenario scenario = base_scenario();
  osc::Story first = story_around(cut_in_action());
  first.name = "first";
  osc::Story second = story_around(cut_in_action());
  second.name = "second";
  scenario.storyboard.stories.push_back(first);
  scenario.storyboard.stories.push_back(second);
  const std::string before = written(scenario);

  osc::edit::ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, osc::edit::remove_story(scenario, 0)).has_value());
  ASSERT_EQ(scenario.storyboard.stories.size(), 1U);
  EXPECT_EQ(scenario.storyboard.stories[0].name, "second");

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(written(scenario), before) << "the story must come back at its own index";

  EXPECT_FALSE(stack.push(scenario, osc::edit::remove_story(scenario, 9)).has_value());
}

TEST(XoscStoryboard, SetStopTriggerReplacesAndUndoesByteIdentically) {
  osc::Scenario scenario = base_scenario();
  const std::string before = written(scenario);

  osc::Condition done;
  done.name = "at_ten_seconds";
  done.simulation_time =
      osc::SimulationTimeCondition{.value = 10.0, .rule = "greaterThan", .preserved = {}};
  osc::ConditionGroup group;
  group.conditions.push_back(done);
  osc::Trigger trigger;
  trigger.condition_groups.push_back(group);

  osc::edit::ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, osc::edit::set_stop_trigger(scenario, trigger)).has_value());
  EXPECT_TRUE(contains(written(scenario), "<SimulationTimeCondition"));

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(written(scenario), before);
}

} // namespace
