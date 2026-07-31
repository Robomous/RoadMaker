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

// The OpenSCENARIO traffic-signal export (p8-s1, issue #245) — §6.11, §10.10.
//
// This is where the sprint's two silent-corruption risks live, and both are
// invisible to any whole-document assertion:
//
//   * RED BY OMISSION, one level up. Skipping a red state as "the default" is
//     a natural size optimization and reproduces exactly the defect ADR-0014
//     §8 guards on the plan side — a signal RoadMaker shows as red becomes a
//     signal that is never red. Red states appear in OTHER phases, so a global
//     count still passes. Only a PER-PHASE count catches it.
//   * PHASE-NAME UNIQUENESS SCOPE. Hoisting the de-dup set out of the
//     per-controller loop renames the second controller's phases. The output
//     stays schema-valid, a simulator loads it happily, and the only symptom
//     is that a traffic-signal condition later matches one arm of a junction
//     instead of all of them.

#include "roadmaker/osc/rules.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using roadmaker::Diagnostic;
namespace osc = roadmaker::osc;

std::string_view element_slice(std::string_view doc,
                               std::string_view open,
                               std::string_view close,
                               std::size_t from = 0) {
  const std::size_t start = doc.find(open, from);
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = doc.find(close, start);
  if (end == std::string_view::npos) {
    return {};
  }
  return doc.substr(start, (end + close.size()) - start);
}

/// Every `<Phase>...</Phase>` slice, in document order. Per-phase assertions
/// are the only ones that can see a state dropped from ONE phase.
std::vector<std::string_view> phase_slices(std::string_view controller) {
  std::vector<std::string_view> phases;
  std::size_t cursor = 0;
  while (true) {
    const std::string_view phase = element_slice(controller, "<Phase", "</Phase>", cursor);
    if (phase.empty()) {
      return phases;
    }
    phases.push_back(phase);
    cursor = static_cast<std::size_t>(phase.data() - controller.data()) + phase.size();
  }
}

std::vector<std::string_view> controller_slices(std::string_view doc) {
  std::vector<std::string_view> controllers;
  std::size_t cursor = 0;
  while (true) {
    const std::string_view controller =
        element_slice(doc, "<TrafficSignalController", "</TrafficSignalController>", cursor);
    if (controller.empty()) {
      return controllers;
    }
    controllers.push_back(controller);
    cursor = static_cast<std::size_t>(controller.data() - doc.data()) + controller.size();
  }
}

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

osc::Phase phase(std::string_view name,
                 double duration,
                 osc::PhaseSemantics semantics,
                 const std::vector<std::pair<std::string, std::string>>& states) {
  osc::Phase built;
  built.name = std::string(name);
  built.duration = duration;
  built.semantics = semantics;
  for (const auto& [signal_id, state] : states) {
    built.signal_states.push_back(
        osc::TrafficSignalState{.traffic_signal_id = signal_id, .state = state, .preserved = {}});
  }
  return built;
}

/// Two controllers decomposed from ONE junction timeline: the same phase names
/// and the same durations, differing only in the row of states each carries.
///
/// ★ The fixture shape is doing real work here. The two controllers REUSE the
/// phase names "go" and "stop" — without that, a scenario-wide de-dup set
/// passes identically to a per-controller one and the uniqueness-scope test is
/// vacuous. And phase "stop" gives EVERY signal the same state, which is the
/// only shape that tempts a `<TrafficSignalGroupState>` optimization.
osc::Scenario signalized_scenario() {
  osc::Scenario scenario;
  scenario.road_network.logic_file = osc::FileRef{.filepath = "cross.xodr", .preserved = {}};

  osc::TrafficSignalController north;
  north.name = "17"; // the OpenDRIVE <controller @id>, never a readable label
  north.phases.push_back(
      phase("go", 20.0, osc::PhaseSemantics::Go, {{"17251", "green"}, {"17252", "red"}}));
  north.phases.push_back(phase(
      "clear", 3.0, osc::PhaseSemantics::AttentionStop, {{"17251", "yellow"}, {"17252", "red"}}));
  north.phases.push_back(
      phase("stop", 23.0, osc::PhaseSemantics::Stop, {{"17251", "red"}, {"17252", "red"}}));

  osc::TrafficSignalController east;
  east.name = "18";
  east.phases.push_back(
      phase("go", 20.0, osc::PhaseSemantics::Go, {{"17261", "red"}, {"17262", "red"}}));
  east.phases.push_back(phase(
      "clear", 3.0, osc::PhaseSemantics::AttentionStop, {{"17261", "red"}, {"17262", "red"}}));
  east.phases.push_back(
      phase("stop", 23.0, osc::PhaseSemantics::Stop, {{"17261", "green"}, {"17262", "green"}}));

  scenario.road_network.traffic_signal_controllers.push_back(north);
  scenario.road_network.traffic_signal_controllers.push_back(east);
  return scenario;
}

std::string written(const osc::Scenario& scenario, const osc::WriteOptions& options = {}) {
  const auto text = osc::write_xosc(scenario, options);
  EXPECT_TRUE(text.has_value()) << (text ? "" : text.error().message);
  return text ? *text : std::string{};
}

} // namespace

// --- placement ---------------------------------------------------------------

TEST(XoscTrafficSignals, ControllersNestInsideRoadNetworkTrafficSignals) {
  const std::string text = written(signalized_scenario());
  const std::string_view road_network = element_slice(text, "<RoadNetwork", "</RoadNetwork>");

  ASSERT_FALSE(road_network.empty()) << text;
  EXPECT_TRUE(contains(road_network, "<TrafficSignals")) << road_network;
  EXPECT_TRUE(contains(element_slice(road_network, "<TrafficSignals", "</TrafficSignals>"),
                       "<TrafficSignalController"))
      << road_network;
}

TEST(XoscTrafficSignals, TrafficSignalsFollowTheRoadNetworkFileReferences) {
  // The RoadNetwork sequence is ordered: LogicFile, SceneGraphFile,
  // TrafficSignals, UsedArea.
  const std::string text = written(signalized_scenario());
  EXPECT_LT(text.find("<LogicFile"), text.find("<TrafficSignals")) << text;
}

TEST(XoscTrafficSignals, NoTrafficSignalsElementWhenThereAreNoControllers) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers.clear();

  const std::string text = written(scenario);
  EXPECT_EQ(text.find("<TrafficSignals"), std::string::npos) << text;
}

// --- states ------------------------------------------------------------------

TEST(XoscTrafficSignals, EverySignalCarriesAStateInEveryPhase) {
  // ★ PER PHASE. A document-wide count of 12 is satisfied by a writer that
  // emitted 4 states in one phase and 8 in another, which is exactly what
  // "skip the red ones" produces.
  const std::string text = written(signalized_scenario());
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_EQ(controllers.size(), 2U) << text;

  const std::vector<std::vector<std::string>> expected_ids = {{"17251", "17252"},
                                                              {"17261", "17262"}};
  for (std::size_t index = 0; index < controllers.size(); ++index) {
    const std::vector<std::string_view> phases = phase_slices(controllers[index]);
    ASSERT_EQ(phases.size(), 3U) << controllers[index];
    for (const std::string_view& slice : phases) {
      EXPECT_EQ(count_occurrences(slice, "<TrafficSignalState"), 2U) << slice;
      for (const std::string& id : expected_ids[index]) {
        EXPECT_TRUE(contains(slice, "trafficSignalId=\"" + id + "\"")) << slice;
      }
    }
  }
}

TEST(XoscTrafficSignals, AnAllRedPhaseStillNamesEverySignalAsRed) {
  // The single most important assertion in this file. Controller "17"'s "stop"
  // phase is all red; a Red-by-omission writer emits it with NO states at all
  // and every other test here still passes.
  const std::string text = written(signalized_scenario());
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_EQ(controllers.size(), 2U) << text;

  const std::vector<std::string_view> phases = phase_slices(controllers[0]);
  ASSERT_EQ(phases.size(), 3U) << controllers[0];
  const std::string_view all_red = phases[2];

  EXPECT_TRUE(contains(all_red, R"(name="stop")")) << all_red;
  EXPECT_EQ(count_occurrences(all_red, R"(state="red")"), 2U) << all_red;
  EXPECT_TRUE(contains(all_red, R"(trafficSignalId="17251")")) << all_red;
  EXPECT_TRUE(contains(all_red, R"(trafficSignalId="17252")")) << all_red;
}

TEST(XoscTrafficSignals, StatesAreAlwaysPerSignalAndNeverGrouped) {
  // Vacuous unless a phase gives every signal the SAME state — the group form
  // is not even a candidate otherwise. Controller "17"'s "stop" phase is that
  // shape by construction.
  const std::string text = written(signalized_scenario());
  EXPECT_EQ(text.find("TrafficSignalGroupState"), std::string::npos) << text;
  EXPECT_NE(text.find("<TrafficSignalState"), std::string::npos) << text;
}

TEST(XoscTrafficSignals, AStateSpellingIsCarriedThroughUntouched) {
  // @state is a FREE STRING the specification leaves to the simulation engine,
  // and §10.10 shows a whole-box signal carrying a COMPOSITE value. A model
  // that held a colour enum could not round-trip this at all.
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].signal_states[0].state =
      "on;off;off";

  const std::string text = written(scenario);
  EXPECT_NE(text.find(R"(state="on;off;off")"), std::string::npos) << text;
}

// --- phase names -------------------------------------------------------------

TEST(XoscTrafficSignals, PhaseNameUniquenessIsScopedToOneController) {
  // ★ THE ONE-LINE PLACEMENT TEST. Both controllers name their phases
  // go/clear/stop; a scenario-wide de-dup set renames the second controller's
  // to go_1/clear_1/stop_1, and every other assertion in this file still
  // passes.
  const std::string text = written(signalized_scenario());
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_EQ(controllers.size(), 2U) << text;

  for (const std::string_view& controller : controllers) {
    EXPECT_TRUE(contains(controller, R"(name="go")")) << controller;
    EXPECT_TRUE(contains(controller, R"(name="clear")")) << controller;
    EXPECT_TRUE(contains(controller, R"(name="stop")")) << controller;
  }
  EXPECT_EQ(text.find(R"(name="go_1")"), std::string::npos)
      << "phase names were de-duplicated across controllers instead of within one\n"
      << text;
}

TEST(XoscTrafficSignals, AnEmptyPhaseNameIsSynthesisedFromItsSemantics) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].name.clear();

  const std::string text = written(scenario);
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_FALSE(controllers.empty()) << text;

  EXPECT_TRUE(contains(controllers[0], R"(name="go")")) << controllers[0];
  EXPECT_EQ(controllers[0].find(R"(name="")"), std::string_view::npos) << controllers[0];
}

TEST(XoscTrafficSignals, AnEmptyPhaseNameWithNoSemanticsFallsBackToATokenNotEmptiness) {
  osc::Scenario scenario = signalized_scenario();
  osc::Phase& first = scenario.road_network.traffic_signal_controllers[0].phases[0];
  first.name.clear();
  first.semantics.reset();

  const std::string text = written(scenario);
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_FALSE(controllers.empty()) << text;

  // An empty @name is schema-invalid and unreferenceable, so a fallback must
  // exist; which token it is matters less than that it is never "".
  EXPECT_TRUE(contains(controllers[0], R"(name="phase")")) << controllers[0];
  EXPECT_EQ(controllers[0].find(R"(name="")"), std::string_view::npos) << controllers[0];
}

TEST(XoscTrafficSignals, CollidingPhaseNamesWithinOneControllerAreDeduplicated) {
  // Vacuous unless the fixture actually collides: with three distinct names
  // the de-dup loop can be deleted outright and the suite stays green.
  osc::Scenario scenario = signalized_scenario();
  osc::TrafficSignalController& controller = scenario.road_network.traffic_signal_controllers[0];
  controller.phases[1].name = "go";
  controller.phases[2].name = "go";

  const std::string text = written(scenario);
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_FALSE(controllers.empty()) << text;

  EXPECT_TRUE(contains(controllers[0], R"(name="go")")) << controllers[0];
  EXPECT_TRUE(contains(controllers[0], R"(name="go_1")")) << controllers[0];
  EXPECT_TRUE(contains(controllers[0], R"(name="go_2")")) << controllers[0];
  EXPECT_EQ(count_occurrences(controllers[0], R"(name="go")"), 1U) << controllers[0];
}

TEST(XoscTrafficSignals, AnAuthoredNameWinsOverSynthesisAndPushesTheSynthesisedOne) {
  // Order matters: the author's "go" is emitted as "go", and the LATER
  // unnamed Go phase becomes "go_1" — never the other way round.
  osc::Scenario scenario = signalized_scenario();
  osc::TrafficSignalController& controller = scenario.road_network.traffic_signal_controllers[0];
  controller.phases[2].name.clear();
  controller.phases[2].semantics = osc::PhaseSemantics::Go;

  const std::string text = written(scenario);
  const std::vector<std::string_view> phases = phase_slices(controller_slices(text)[0]);
  ASSERT_EQ(phases.size(), 3U) << text;

  EXPECT_TRUE(contains(phases[0], R"(name="go")")) << phases[0];
  EXPECT_TRUE(contains(phases[2], R"(name="go_1")")) << phases[2];
}

TEST(XoscTrafficSignals, SynthesisNeverRewritesTheModel) {
  // The synthesized name exists only in the output, so two writes of one
  // Scenario stay byte-identical.
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].name.clear();

  EXPECT_EQ(written(scenario), written(scenario));
  EXPECT_TRUE(scenario.road_network.traffic_signal_controllers[0].phases[0].name.empty());
}

// --- the revision conditional ------------------------------------------------

TEST(XoscTrafficSignals, SemanticsIsEmittedOnlyWhenTargetingOneFour) {
  // BOTH halves are required. `find("semantics=") == npos` at the default
  // target is a tautology if the fixture leaves @semantics unset — so the
  // fixture sets it on every phase, and the same fixture must produce the
  // attribute at 1.4.
  const osc::Scenario scenario = signalized_scenario();

  const std::string at_1_2 = written(scenario);
  EXPECT_EQ(at_1_2.find("semantics="), std::string::npos)
      << "a 1.2-declared file must not carry an attribute created in 1.4.0\n"
      << at_1_2;

  const std::string at_1_4 = written(scenario, {.target_version = osc::OscVersion::v1_4});
  EXPECT_NE(at_1_4.find(R"(semantics="go")"), std::string::npos) << at_1_4;
  EXPECT_NE(at_1_4.find(R"(semantics="attention_stop")"), std::string::npos) << at_1_4;
  EXPECT_NE(at_1_4.find(R"(semantics="stop")"), std::string::npos) << at_1_4;
}

TEST(XoscTrafficSignals, SemanticsUsesTheSpecificationSpelling) {
  // The enumeration is snake_case in the specification; attentionStop is the
  // natural C++-ism and is wrong.
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].semantics =
      osc::PhaseSemantics::AttentionGo;

  const std::string text = written(scenario, {.target_version = osc::OscVersion::v1_4});
  EXPECT_NE(text.find(R"(semantics="attention_go")"), std::string::npos) << text;
  EXPECT_EQ(text.find("attentionGo"), std::string::npos) << text;
}

// --- identity and references -------------------------------------------------

TEST(XoscTrafficSignals, ControllerNameIsTheOpenDriveControllerId) {
  // §10.10: "The ASAM OpenDRIVE controller ID is used as the name of the
  // TrafficSignalController to reference it." GW-6 step 11 requires the same.
  const std::string text = written(signalized_scenario());
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_EQ(controllers.size(), 2U) << text;

  EXPECT_TRUE(contains(controllers[0], R"(name="17")")) << controllers[0];
  EXPECT_TRUE(contains(controllers[1], R"(name="18")")) << controllers[1];
}

TEST(XoscTrafficSignals, RefusesATrafficSignalStateThatNamesNoSignal) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0]
      .phases[0]
      .signal_states[0]
      .traffic_signal_id.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("names no signal"), std::string::npos)
      << text.error().message;
  EXPECT_NE(text.error().context.find("TrafficSignalState[0]"), std::string::npos)
      << text.error().context;
}

TEST(XoscTrafficSignals, RefusesAReferenceToAControllerThatIsNotInTheScenario) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[1].reference = "99";

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("99"), std::string::npos) << text.error().message;
}

TEST(XoscTrafficSignals, AcceptsAReferenceToAControllerThatIsInTheScenario) {
  // The counterpart: without it, a check that refused EVERY reference would
  // pass the test above.
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[1].reference = "17";
  scenario.road_network.traffic_signal_controllers[1].delay = 5.0;

  const std::string text = written(scenario);
  EXPECT_NE(text.find(R"(reference="17")"), std::string::npos) << text;
  EXPECT_NE(text.find(R"(delay="5")"), std::string::npos) << text;
}

TEST(XoscTrafficSignals, RefusesADelayWithNoControllerToBeRelativeTo) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].delay = 5.0;

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("delay"), std::string::npos) << text.error().message;
}

TEST(XoscTrafficSignals, RefusesAControllerWithNoName) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].name.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("no name"), std::string::npos) << text.error().message;
}

// --- durations ---------------------------------------------------------------

TEST(XoscTrafficSignals, RefusesANegativePhaseDuration) {
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].duration = -1.0;

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("negative"), std::string::npos) << text.error().message;
}

TEST(XoscTrafficSignals, AcceptsAZeroPhaseDuration) {
  // ★ The rule is data_type.phase_duration_positive but its TEXT says "shall
  // contain non-negative values". Writing the guard as `duration <= 0` passes
  // the refusal test above while wrongly rejecting legal input — which reading
  // the rule's NAME instead of its text produces.
  osc::Scenario scenario = signalized_scenario();
  scenario.road_network.traffic_signal_controllers[0].phases[0].duration = 0.0;

  const auto text = osc::write_xosc(scenario);
  ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().message);
  EXPECT_NE(text->find(R"(duration="0")"), std::string::npos) << *text;
}

TEST(XoscTrafficSignals, DurationsSurviveDecompositionIdenticallyAcrossControllers) {
  // Every controller of one junction timeline carries the same durations; only
  // the row of states differs (ADR-0014 §8).
  const std::string text = written(signalized_scenario());
  const std::vector<std::string_view> controllers = controller_slices(text);
  ASSERT_EQ(controllers.size(), 2U) << text;

  for (const std::string_view& controller : controllers) {
    EXPECT_TRUE(contains(controller, R"(duration="20")")) << controller;
    EXPECT_TRUE(contains(controller, R"(duration="3")")) << controller;
    EXPECT_TRUE(contains(controller, R"(duration="23")")) << controller;
  }
}

// --- the preserved tier on signals -------------------------------------------

TEST(XoscTrafficSignals, ForeignPhaseAttributesAndChildrenSurvive) {
  osc::Scenario scenario = signalized_scenario();
  osc::Phase& first = scenario.road_network.traffic_signal_controllers[0].phases[0];
  first.preserved.attributes.emplace_back("vendorHint", "priority");
  first.preserved.children.emplace_back(R"(<TrafficSignalGroupState state="on"/>)");

  const std::string text = written(scenario);
  const std::vector<std::string_view> phases = phase_slices(controller_slices(text)[0]);
  ASSERT_FALSE(phases.empty()) << text;

  EXPECT_TRUE(contains(phases[0], R"(vendorHint="priority")")) << phases[0];
  // A group state RoadMaker never AUTHORS is still carried when a foreign file
  // brought one — preserving is not the same as emitting.
  EXPECT_TRUE(contains(phases[0], "<TrafficSignalGroupState")) << phases[0];
  EXPECT_LT(phases[0].find("duration="), phases[0].find("vendorHint=")) << phases[0];
}
