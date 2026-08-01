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

// The OpenSCENARIO DSL v2.2.0 export (p8-s6, issue #327).
//
// ★ THE LOAD-BEARING TEST IS THE DOC↔CODE GATE AT THE BOTTOM. This output has
// no schema in the tree, no parser, and no simulator in CI to contradict it —
// so "a documented subset" is the only promise made about it, and a doc that
// drifts from the emitter turns that promise into a lie that review would not
// catch. The `test_defaults_registry.cpp` mechanism (#413), applied to a second
// document.

#include "roadmaker/osc/osc2.hpp"
#include "roadmaker/osc/writer.hpp"

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

using roadmaker::Diagnostic;
using roadmaker::Severity;
namespace osc = roadmaker::osc;

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string written(const osc::Scenario& scenario) {
  const auto text = osc::write_osc2(scenario);
  EXPECT_TRUE(text.has_value()) << (text.has_value() ? "" : text.error().message);
  return text.value_or(std::string{});
}

/// One entity, one map link — the smallest scenario that is concrete.
osc::Scenario base_scenario() {
  osc::Scenario scenario;
  scenario.road_network.logic_file = osc::FileRef{.filepath = "town.xodr", .preserved = {}};

  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(ego);
  return scenario;
}

void give_speed(osc::Scenario& scenario, const std::string& entity, double speed) {
  osc::SpeedAction action;
  action.absolute_target = osc::AbsoluteTargetSpeed{.value = speed, .preserved = {}};
  osc::LongitudinalAction longitudinal;
  longitudinal.speed = action;
  osc::PrivateAction entry;
  entry.longitudinal = longitudinal;
  osc::Private privates;
  privates.entity_ref = entity;
  privates.actions.push_back(entry);
  scenario.storyboard.init.actions.privates.push_back(privates);
}

bool any_message_contains(const std::vector<Diagnostic>& findings, std::string_view needle) {
  for (const Diagnostic& finding : findings) {
    if (finding.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// --- the emitted language -----------------------------------------------------

TEST(Osc2Writer, TheSkeletonIsTheSpecificationsOwnConcreteScenarioShape) {
  osc::Scenario scenario = base_scenario();
  give_speed(scenario, "Ego", 13.89);
  const std::string text = written(scenario);

  // §7.7.5.2.1 — the complete standard library, and NOT `basic.osc`, which does
  // not exist: the library files are types.osc / domain.osc / standard.osc.
  EXPECT_TRUE(contains(text, "import osc.standard.all")) << text;
  EXPECT_FALSE(contains(text, "basic.osc")) << text;

  EXPECT_TRUE(contains(text, "scenario top:")) << text;
  // §6.3.1.3.1's concrete-scenario example opens with exactly this call.
  EXPECT_TRUE(contains(text, "map.set_map_file(\"town.xodr\")")) << text;
  EXPECT_TRUE(contains(text, "ego: vehicle")) << text;
  EXPECT_TRUE(contains(text, "do parallel:")) << text;
  EXPECT_TRUE(contains(text, "ego.drive()")) << text;

  // ★ `mps`, a normative speed unit (physical types), so the kernel's m/s needs
  // NO conversion. A kph conversion is exactly where a rounding error would
  // enter a file nothing in CI can check.
  EXPECT_TRUE(contains(text, "speed(13.89mps)")) << text;
  EXPECT_FALSE(contains(text, "kph")) << text;

  // §8.7.7: the mandatory parameter is `vehicle_category`. `category` appears
  // only in a loose conceptual example and is NOT the domain model's name.
  EXPECT_TRUE(contains(text, "keep(it.vehicle_category == car)")) << text;
}

TEST(Osc2Writer, TheOutputIsDeterministic) {
  osc::Scenario scenario = base_scenario();
  give_speed(scenario, "Ego", 13.89);
  EXPECT_EQ(written(scenario), written(scenario));
}

TEST(Osc2Writer, IndentationIsConsistentBecauseItIsSyntax) {
  // A Python-style language: a block that is not indented consistently is a
  // different program, or none. Every line inside `scenario` carries at least
  // four spaces, and nothing carries a tab.
  osc::Scenario scenario = base_scenario();
  give_speed(scenario, "Ego", 10.0);
  const std::string text = written(scenario);
  EXPECT_FALSE(contains(text, "\t")) << "a tab in an indentation-structured language";

  std::istringstream lines(text);
  std::string line;
  bool inside = false;
  while (std::getline(lines, line)) {
    if (line.rfind("scenario ", 0) == 0) {
      inside = true;
      continue;
    }
    if (!inside || line.empty() || line.rfind('#', 0) == 0) {
      continue;
    }
    EXPECT_EQ(line.rfind("    ", 0), 0U) << "unindented inside `scenario`: " << line;
  }
  EXPECT_TRUE(inside) << text;
}

TEST(Osc2Writer, AnEntityNameThatIsNotAnIdentifierIsRewritten) {
  // `ScenarioObject/@name` is an XML string and routinely holds spaces or
  // dashes; a DSL identifier cannot.
  osc::Scenario scenario = base_scenario();
  scenario.entities.scenario_objects[0].name = "Lead Vehicle-2";
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, "lead_vehicle_2: vehicle")) << text;
  EXPECT_FALSE(contains(text, "Lead Vehicle-2")) << text;
}

TEST(Osc2Writer, TwoEntitiesThatCollapseToOneIdentifierAreRefused) {
  // ★ REFUSED, NOT DE-DUPLICATED. Two actors silently merged into one is a
  // scenario that describes something else entirely — the opposite of a lossy
  // but honest export.
  osc::Scenario scenario = base_scenario();
  osc::ScenarioObject other;
  other.name = "ego";
  other.entity_object = osc::Vehicle{};
  scenario.entities.scenario_objects.push_back(other);

  const auto text = osc::write_osc2(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("identifier"), std::string::npos);
}

TEST(Osc2Writer, AScenarioWithNoActorIsRefused) {
  osc::Scenario scenario;
  EXPECT_FALSE(osc::write_osc2(scenario).has_value());
}

TEST(Osc2Writer, AnUnmappableVehicleCategoryOmitsTheKeepRatherThanInventingOne) {
  osc::Scenario scenario = base_scenario();
  std::get<osc::Vehicle>(scenario.entities.scenario_objects[0].entity_object).category =
      "something_the_dsl_does_not_have";
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, "ego: vehicle")) << text;
  EXPECT_FALSE(contains(text, "vehicle_category")) << text;
  EXPECT_FALSE(contains(text, "something_the_dsl_does_not_have")) << text;
}

TEST(Osc2Writer, AnActorWithNoInitActionStillGetsABody) {
  // An empty `scenario` block is not valid in an indentation-structured
  // language, so a scenario whose actors do nothing still needs a `do`.
  osc::Scenario scenario = base_scenario();
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, "do parallel:")) << text;
  EXPECT_TRUE(contains(text, "ego.drive()")) << text;
}

TEST(Osc2Writer, APedestrianIsAPedestrianAndAnUnmodeledEntityIsATrafficParticipant) {
  osc::Scenario scenario = base_scenario();

  osc::ScenarioObject walker;
  walker.name = "Walker";
  walker.entity_object = osc::Pedestrian{};
  scenario.entities.scenario_objects.push_back(walker);

  osc::ScenarioObject foreign;
  foreign.name = "FromCatalog";
  // std::monostate: a <CatalogReference> or <MiscObject> riding the preserved
  // tier — declared as the most general traffic participant rather than guessed.
  scenario.entities.scenario_objects.push_back(foreign);

  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, "walker: pedestrian")) << text;
  EXPECT_TRUE(contains(text, "fromcatalog: traffic_participant")) << text;
}

TEST(Osc2Writer, SaveWritesTheFileWithTheDslExtensionsBytes) {
  osc::Scenario scenario = base_scenario();

  // ★ A PER-TEST DIRECTORY, not a fixed name in the shared temp directory.
  // ctest runs cases concurrently and two jobs on one machine share
  // `temp_directory_path()`, so a fixed name is a collision waiting for a busy
  // runner — the per-test temp-dir contract the suite already holds itself to.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      fmt::format("roadmaker_osc2_{}",
                  testing::UnitTest::GetInstance()->current_test_info()->name());
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  ASSERT_TRUE(std::filesystem::create_directories(directory, ec)) << ec.message();
  const std::filesystem::path path = directory / "scenario.osc";

  ASSERT_TRUE(osc::save_osc2(scenario, path).has_value());

  // ★ THE STREAM IS SCOPED SO THE HANDLE IS CLOSED BEFORE THE CLEANUP.
  // POSIX unlinks a file that is still open; Windows refuses with "the process
  // cannot access the file because it is being used by another process", which
  // is a test that fails on one platform for a reason that has nothing to do
  // with what it is testing.
  std::string contents;
  {
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    contents = buffer.str();
  }
  EXPECT_EQ(contents, written(scenario));

  std::filesystem::remove_all(directory, ec);
}

// --- what is NOT exported, and that it says so --------------------------------

TEST(Osc2Writer, EverythingOutsideTheSubsetIsReportedRatherThanSilentlyOmitted) {
  // ★ #327's acceptance in one test: "anything outside it is reported rather
  // than silently omitted".
  osc::Scenario scenario = base_scenario();

  // A story.
  osc::Action action;
  action.name = "a";
  osc::Event event;
  event.name = "e";
  event.actions.push_back(action);
  osc::StoryManeuver maneuver;
  maneuver.name = "m";
  maneuver.events.push_back(event);
  osc::ManeuverGroup group;
  group.name = "g";
  group.maneuvers.push_back(maneuver);
  osc::Act act;
  act.name = "act";
  act.maneuver_groups.push_back(group);
  osc::Story story;
  story.name = "s";
  story.acts.push_back(act);
  scenario.storyboard.stories.push_back(story);

  // A stop trigger.
  osc::Condition condition;
  condition.name = "end";
  condition.simulation_time = osc::SimulationTimeCondition{};
  osc::ConditionGroup conditions;
  conditions.conditions.push_back(condition);
  scenario.storyboard.stop_trigger.condition_groups.push_back(conditions);

  // A traffic-signal controller.
  osc::TrafficSignalController controller;
  controller.name = "1";
  scenario.road_network.traffic_signal_controllers.push_back(controller);

  // A lane-anchored placement and a route.
  osc::LanePosition position;
  position.road_id = "1";
  position.lane_id = "-1";
  osc::TeleportAction teleport;
  teleport.position = position;
  osc::Route route;
  route.name = "r";
  osc::AssignRouteAction assign;
  assign.route = route;
  osc::RoutingAction routing;
  routing.assign_route = assign;
  osc::PrivateAction entry;
  entry.teleport = teleport;
  entry.routing = routing;
  osc::Private privates;
  privates.entity_ref = "Ego";
  privates.actions.push_back(entry);
  scenario.storyboard.init.actions.privates.push_back(privates);

  const std::vector<Diagnostic> findings = osc::validate_osc2_subset(scenario);
  EXPECT_TRUE(any_message_contains(findings, "<Story>"));
  EXPECT_TRUE(any_message_contains(findings, "stop trigger"));
  EXPECT_TRUE(any_message_contains(findings, "TrafficSignalController"));
  EXPECT_TRUE(any_message_contains(findings, "road- or lane-relative placement"));
  EXPECT_TRUE(any_message_contains(findings, "route is not exported"));

  // Warnings, never errors: a 2.x file is a lossy export-only view by
  // definition, and refusing to write one because it is lossy would make the
  // whole feature unusable.
  for (const Diagnostic& finding : findings) {
    EXPECT_EQ(finding.severity, Severity::Warning) << finding.message;
  }
  EXPECT_TRUE(osc::write_osc2(scenario).has_value());
}

TEST(Osc2Writer, AScenarioInsideTheSubsetReportsNothing) {
  osc::Scenario scenario = base_scenario();
  give_speed(scenario, "Ego", 10.0);
  EXPECT_TRUE(osc::validate_osc2_subset(scenario).empty());
}

TEST(Osc2Writer, AMissingRoadNetworkIsReportedBecauseItIsWhatMakesItConcrete) {
  osc::Scenario scenario = base_scenario();
  scenario.road_network.logic_file.reset();
  EXPECT_TRUE(any_message_contains(osc::validate_osc2_subset(scenario), "links no road network"));
  EXPECT_FALSE(contains(written(scenario), "set_map_file"));
}

// --- the doc↔code gate ---------------------------------------------------------

namespace {

std::string committed_doc() {
  const std::filesystem::path page =
      std::filesystem::path(RM_DOCS_DIR) / "domain" / "openscenario.md";
  // TEXT mode, not binary: this is human-edited prose, and a Windows checkout
  // with autocrlf gives it CRLF endings. Comparing bytes would fail on line
  // endings alone, invisibly on macOS and Linux.
  std::ifstream file(page);
  EXPECT_TRUE(file.is_open()) << "missing " << page.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

} // namespace

TEST(Osc2Subset, TheCommittedDocMatchesTheRegistry) {
  // ★ THE PROMISE THIS FEATURE MAKES IS "a documented subset". Nothing else
  // checks the 2.x output — no schema, no parser, no simulator — so a doc that
  // has drifted from the emitter turns that promise into a lie, and only a
  // gate like this one catches it.
  const std::string doc = committed_doc();

  for (const osc::Osc2SubsetRow& row : osc::osc2_supported()) {
    EXPECT_NE(doc.find(row.construct), std::string::npos)
        << "the emitter writes `" << row.construct
        << "` and docs/domain/openscenario.md does not list it";
  }
  for (const osc::Osc2SubsetRow& row : osc::osc2_unsupported()) {
    EXPECT_NE(doc.find(row.construct), std::string::npos)
        << "the emitter refuses `" << row.construct
        << "` and docs/domain/openscenario.md does not list it";
  }

  // And the version, which the doc must name rather than saying "2.x".
  EXPECT_NE(doc.find(osc::kOsc2Version), std::string::npos);
}

TEST(Osc2Subset, EveryRegistryRowNamesSomethingTheEmitterActuallyWrites) {
  // The other direction: a row that describes nothing is documentation of a
  // feature that does not exist, which is worse than no row at all.
  osc::Scenario scenario = base_scenario();
  give_speed(scenario, "Ego", 13.89);
  const std::string text = written(scenario);

  // Only the rows whose construct is a literal fragment of the output can be
  // checked this way; the parameterised ones are covered by the emission tests
  // above. Naming them here rather than skipping silently.
  EXPECT_TRUE(contains(text, "import osc.standard.all"));
  EXPECT_TRUE(contains(text, "map.set_map_file(\""));
  EXPECT_TRUE(contains(text, ": vehicle"));
  EXPECT_TRUE(contains(text, "do parallel:"));
  EXPECT_TRUE(contains(text, ".drive()"));
  EXPECT_TRUE(contains(text, "mps)"));
  EXPECT_TRUE(contains(text, "keep(it.vehicle_category == "));
}

} // namespace
