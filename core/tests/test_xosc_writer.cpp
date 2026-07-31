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

// The OpenSCENARIO writer's document-level contract (p8-s1, issue #245):
// the revision conditional, document order, the always-present skeleton, the
// preserved tier, determinism, and every refusal. The traffic-signal half is
// pinned separately in test_xosc_signals.cpp.
//
// ASSERTIONS SLICE A SECTION, they do not search the document. A global find
// is satisfied by a mention anywhere — the scar that let a swapped tolerance
// row pass in #403 and `contains("0.8")` pass on 0.800000011920929 in #325.

#include "roadmaker/osc/rules.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using roadmaker::Diagnostic;
using roadmaker::Severity;
namespace osc = roadmaker::osc;

/// The text of one element, from its start tag through its close tag.
///
/// The XML twin of test_connection_contract.cpp's `section_after`, and it
/// exists for the same reason: asserting inside a parent's slice is what
/// proves NESTING. A flat `find(...) != npos` checklist would pass on a writer
/// that emitted `<Performance>` as a sibling of `<Vehicle>`.
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
  return std::string_view(doc).substr(start, (end + close.size()) - start);
}

std::size_t offset_of(const std::string& doc, std::string_view marker) {
  return doc.find(marker);
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::vector<Diagnostic> findings_with_rule(const std::vector<Diagnostic>& findings,
                                           std::string_view rule_id) {
  std::vector<Diagnostic> matching;
  std::copy_if(findings.begin(),
               findings.end(),
               std::back_inserter(matching),
               [rule_id](const Diagnostic& finding) { return finding.rule_id == rule_id; });
  return matching;
}

/// The shape scripts/esmini_smoke.py:49-101 uses — the element set CI already
/// proves a shipping parser accepts at revision 1.2.
osc::Scenario minimal_scenario() {
  osc::Scenario scenario;
  scenario.road_network.logic_file = osc::FileRef{.filepath = "town.xodr", .preserved = {}};

  osc::Vehicle car;
  car.name = "car";
  car.bounding_box = {.center_x = 1.4,
                      .center_y = 0.0,
                      .center_z = 0.75,
                      .width = 2.0,
                      .length = 5.0,
                      .height = 1.5,
                      .preserved = {}};

  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = car;
  scenario.entities.scenario_objects.push_back(ego);

  osc::PrivateAction teleport;
  teleport.teleport = osc::TeleportAction{.position = {.x = 0.0,
                                                       .y = 0.0,
                                                       .z = 0.5,
                                                       .h = 0.0,
                                                       .p = std::nullopt,
                                                       .r = std::nullopt,
                                                       .preserved = {}},
                                          .preserved = {}};

  osc::Private ego_init;
  ego_init.entity_ref = "Ego";
  ego_init.actions.push_back(teleport);
  scenario.storyboard.init.actions.privates.push_back(ego_init);

  osc::Condition end;
  end.name = "end";
  end.simulation_time =
      osc::SimulationTimeCondition{.value = 0.5, .rule = "greaterThan", .preserved = {}};
  osc::ConditionGroup group;
  group.conditions.push_back(end);
  scenario.storyboard.stop_trigger.condition_groups.push_back(group);

  return scenario;
}

std::string written(const osc::Scenario& scenario, const osc::WriteOptions& options = {}) {
  const auto text = osc::write_xosc(scenario, options);
  EXPECT_TRUE(text.has_value()) << (text ? "" : text.error().message);
  return text ? *text : std::string{};
}

} // namespace

// --- the revision conditional ------------------------------------------------

TEST(XoscWriter, DefaultTargetDeclaresRevisionOneTwo) {
  const std::string text = written(minimal_scenario());
  const std::string_view header = element_slice(text, "<FileHeader", "/>");

  ASSERT_FALSE(header.empty()) << text;
  EXPECT_TRUE(contains(header, R"(revMajor="1")")) << header;
  EXPECT_TRUE(contains(header, R"(revMinor="2")")) << header;
  // Absence asserted document-wide as well: the point is that NOTHING declares
  // 1.4, not merely that the header says 1.2.
  EXPECT_EQ(text.find(R"(revMinor="4")"), std::string::npos) << text;
}

TEST(XoscWriter, TargetingOneFourDeclaresRevisionOneFour) {
  const std::string text = written(minimal_scenario(), {.target_version = osc::OscVersion::v1_4});
  const std::string_view header = element_slice(text, "<FileHeader", "/>");

  ASSERT_FALSE(header.empty()) << text;
  EXPECT_TRUE(contains(header, R"(revMinor="4")")) << header;
  EXPECT_EQ(text.find(R"(revMinor="2")"), std::string::npos) << text;
}

// --- determinism -------------------------------------------------------------

TEST(XoscWriter, HeaderDateIsTheFixedDefaultAndNeverAClock) {
  // Asserts the LITERAL, not that two writes agree. A system_clock date with
  // second resolution passes a two-write comparison almost every time, so the
  // comparison test below cannot stand in for this one.
  const std::string text = written(minimal_scenario());
  const std::string_view header = element_slice(text, "<FileHeader", "/>");

  ASSERT_FALSE(header.empty()) << text;
  EXPECT_TRUE(contains(header, R"(date="1970-01-01T00:00:00")")) << header;
}

TEST(XoscWriter, RepeatedWritesAreByteIdentical) {
  // The cheap canary for a stateful writer. The SPECIFIC determinism
  // guarantees live in HeaderDateIsTheFixedDefault (a clock), the two number
  // tests below (formatting) and SaveWritesExactlyWhatWriteReturns (stream
  // mode) — do not delete those as redundant with this one.
  const osc::Scenario scenario = minimal_scenario();
  EXPECT_EQ(written(scenario), written(scenario));
}

TEST(XoscWriter, NumbersUseTheShortestRoundTrippableForm) {
  const std::string text = written(minimal_scenario());
  const std::string_view world = element_slice(text, "<WorldPosition", "/>");

  ASSERT_FALSE(world.empty()) << text;
  EXPECT_TRUE(contains(world, R"(z="0.5")")) << world;
  // What actually catches std::to_string / a fixed precision.
  EXPECT_FALSE(contains(world, "0.500000")) << world;
  EXPECT_TRUE(contains(world, R"(x="0")")) << world;
  EXPECT_FALSE(contains(world, "0.000000")) << world;
}

TEST(XoscWriter, NegativeZeroIsNormalisedToZero) {
  // The copied num() carries a `-0` -> `0` branch. This is a COPY of the
  // OpenDRIVE helper, so no existing test covers it: delete that one line and
  // the whole xodr suite still passes while a -0 reaches every .xosc.
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position.x = -0.0;

  const std::string text = written(scenario);
  const std::string_view world = element_slice(text, "<WorldPosition", "/>");

  ASSERT_FALSE(world.empty()) << text;
  EXPECT_TRUE(contains(world, R"(x="0")")) << world;
  EXPECT_FALSE(contains(world, R"(x="-0")")) << world;
}

// --- document order and the always-present skeleton --------------------------

TEST(XoscWriter, DocumentOrderFollowsTheScenarioDefinitionSequence) {
  // Written entirely in offsets: any find-based assertion is order-blind by
  // construction, and the XSD sequences are ORDERED, so this is correctness.
  const std::string text = written(minimal_scenario());

  const std::size_t header = offset_of(text, "<FileHeader");
  const std::size_t parameters = offset_of(text, "<ParameterDeclarations");
  const std::size_t catalogs = offset_of(text, "<CatalogLocations");
  const std::size_t road_network = offset_of(text, "<RoadNetwork");
  const std::size_t entities = offset_of(text, "<Entities");
  const std::size_t storyboard = offset_of(text, "<Storyboard");

  ASSERT_NE(storyboard, std::string::npos) << text;
  EXPECT_LT(header, parameters) << text;
  EXPECT_LT(parameters, catalogs) << text;
  EXPECT_LT(catalogs, road_network) << text;
  EXPECT_LT(road_network, entities) << text;
  EXPECT_LT(entities, storyboard) << text;

  // Inside <Storyboard>: <Init> then <StopTrigger>.
  EXPECT_LT(offset_of(text, "<Init"), offset_of(text, "<StopTrigger")) << text;
}

TEST(XoscWriter, TheAlwaysPresentSkeletonIsEmittedEvenWhenEmpty) {
  // ParameterDeclarations, CatalogLocations, Properties and StopTrigger were
  // required at 1.0/1.2 and relaxed later. Wrapping any of them in an
  // if-not-empty is the natural "cleanup" that silently breaks the 1.2 target,
  // which is the default.
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.stop_trigger.condition_groups.clear();

  const std::string text = written(scenario);
  EXPECT_NE(text.find("<ParameterDeclarations"), std::string::npos) << text;
  EXPECT_NE(text.find("<CatalogLocations"), std::string::npos) << text;
  EXPECT_NE(text.find("<StopTrigger"), std::string::npos) << text;
  EXPECT_NE(element_slice(text, "<Vehicle", "</Vehicle>").find("<Properties"),
            std::string_view::npos)
      << text;
}

TEST(XoscWriter, AStoryElementIsEmittedOnlyWhenTheScenarioHasOne) {
  // NOTE the exact needles. `find("<Story")` ALWAYS matches, because
  // "<Storyboard" contains it — a test written the obvious way can never fail.
  // Do not "simplify" these back.
  const std::string bare = written(minimal_scenario());
  EXPECT_EQ(bare.find("<Story>"), std::string::npos) << bare;
  EXPECT_EQ(bare.find("<Story "), std::string::npos) << bare;

  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.preserved_stories.push_back(R"(<Story name="s"><Act name="a"/></Story>)");
  const std::string text = written(scenario);
  EXPECT_NE(text.find("<Story "), std::string::npos) << text;
  // And it sits between <Init> and <StopTrigger>, per the Storyboard sequence.
  EXPECT_LT(offset_of(text, "<Init"), offset_of(text, "<Story ")) << text;
  EXPECT_LT(offset_of(text, "<Story "), offset_of(text, "<StopTrigger")) << text;
}

TEST(XoscWriter, TheMinimalScenarioNestsLikeTheEsminiWrapper) {
  const std::string text = written(minimal_scenario());

  const std::string_view vehicle = element_slice(text, "<Vehicle", "</Vehicle>");
  ASSERT_FALSE(vehicle.empty()) << text;
  // Each assertion is scoped to its PARENT's slice; presence alone would pass
  // on a writer that emitted these as siblings.
  EXPECT_TRUE(contains(vehicle, "<BoundingBox")) << vehicle;
  EXPECT_TRUE(contains(vehicle, "<Performance")) << vehicle;
  EXPECT_TRUE(contains(vehicle, "<Axles")) << vehicle;
  EXPECT_TRUE(contains(vehicle, "<Properties")) << vehicle;

  const std::string_view box = element_slice(text, "<BoundingBox", "</BoundingBox>");
  EXPECT_TRUE(contains(box, "<Center")) << box;
  EXPECT_TRUE(contains(box, "<Dimensions")) << box;

  const std::string_view axles = element_slice(text, "<Axles", "</Axles>");
  EXPECT_TRUE(contains(axles, "<FrontAxle")) << axles;
  EXPECT_TRUE(contains(axles, "<RearAxle")) << axles;

  const std::string_view teleport = element_slice(text, "<TeleportAction", "</TeleportAction>");
  EXPECT_TRUE(contains(teleport, "<WorldPosition")) << teleport;

  const std::string_view stop = element_slice(text, "<StopTrigger", "</StopTrigger>");
  EXPECT_TRUE(contains(stop, "<SimulationTimeCondition")) << stop;

  const std::string_view entity = element_slice(text, "<ScenarioObject", "</ScenarioObject>");
  EXPECT_TRUE(contains(entity, R"(name="Ego")")) << entity;
}

// --- the preserved tier ------------------------------------------------------

TEST(XoscWriter, PreservedAttributesFollowTheModeledOnes) {
  osc::Scenario scenario = minimal_scenario();
  scenario.header.preserved.attributes.emplace_back("foreignFlag", "7");

  const std::string text = written(scenario);
  const std::string_view header = element_slice(text, "<FileHeader", "/>");

  ASSERT_FALSE(header.empty()) << text;
  EXPECT_TRUE(contains(header, R"(foreignFlag="7")")) << header;
  EXPECT_LT(header.find("author="), header.find("foreignFlag=")) << header;
}

TEST(XoscWriter, PreservedChildrenFollowTheModeledOnes) {
  osc::Scenario scenario = minimal_scenario();
  scenario.entities.preserved.children.emplace_back(R"(<EntitySelection name="all"/>)");

  const std::string text = written(scenario);
  const std::string_view entities = element_slice(text, "<Entities", "</Entities>");

  ASSERT_FALSE(entities.empty()) << text;
  EXPECT_TRUE(contains(entities, "<EntitySelection")) << entities;
  EXPECT_LT(entities.find("<ScenarioObject"), entities.find("<EntitySelection")) << entities;
}

TEST(XoscWriter, AnUnmodeledEntityObjectSurvivesOnThePreservedTier) {
  // std::monostate is the case where a MiscObject / CatalogReference rode in
  // whole on the preserved tier. Nothing modeled is emitted for it, and it
  // must still come back out.
  osc::Scenario scenario = minimal_scenario();
  osc::ScenarioObject foreign;
  foreign.name = "Barrier";
  foreign.preserved.children.emplace_back(
      R"(<MiscObject name="cone" miscObjectCategory="obstacle" mass="3"/>)");
  scenario.entities.scenario_objects.push_back(foreign);

  const std::string text = written(scenario);
  EXPECT_NE(text.find("<MiscObject"), std::string::npos) << text;
  EXPECT_NE(text.find(R"(miscObjectCategory="obstacle")"), std::string::npos) << text;
}

TEST(XoscWriter, APreservedFragmentSurvivesReEmissionWithItsContent) {
  // "Verbatim" is the same promise the OpenDRIVE writer makes: the fragment is
  // re-parsed and re-serialized, so its CONTENT survives while quoting and
  // whitespace re-canonicalize (ADR-0014 §6, fmt-s2's caveat). Assert the
  // content, and say so, rather than claiming byte identity we do not have.
  osc::Scenario scenario = minimal_scenario();
  scenario.preserved.children.emplace_back(R"(<Custom depth='2'><Inner note="a&amp;b"/></Custom>)");

  const std::string text = written(scenario);
  const std::string_view custom = element_slice(text, "<Custom", "</Custom>");

  ASSERT_FALSE(custom.empty()) << text;
  EXPECT_TRUE(contains(custom, R"(depth="2")")) << custom;
  EXPECT_TRUE(contains(custom, "<Inner")) << custom;
  EXPECT_TRUE(contains(custom, "a&amp;b")) << custom;
}

// --- refusals ----------------------------------------------------------------

TEST(XoscWriter, RefusesAnEntityWithNoName) {
  osc::Scenario scenario = minimal_scenario();
  scenario.entities.scenario_objects[0].name.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  // Assert WHY it refused, not merely that it did: "it errors" is not
  // coverage, and this scenario is also malformed in a second way (the init
  // action now dangles), so a bare has_value() check proves nothing.
  EXPECT_EQ(text.error().code, roadmaker::ErrorCode::InvalidArgument);
  EXPECT_NE(text.error().message.find("no name"), std::string::npos) << text.error().message;
  EXPECT_NE(text.error().context.find("ScenarioObject[0]"), std::string::npos)
      << text.error().context;
}

TEST(XoscWriter, RefusesDuplicateEntityNames) {
  osc::Scenario scenario = minimal_scenario();
  scenario.entities.scenario_objects.push_back(scenario.entities.scenario_objects[0]);

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("duplicate"), std::string::npos) << text.error().message;

  const auto findings = osc::validate_scenario(scenario);
  EXPECT_FALSE(findings_with_rule(findings, osc::rules::kUniqueElementNames).empty());
}

TEST(XoscWriter, RefusesANameContainingTheReferenceSeparator) {
  osc::Scenario scenario = minimal_scenario();
  scenario.entities.scenario_objects[0].name = "Traffic::Ego";
  scenario.storyboard.init.actions.privates[0].entity_ref = "Traffic::Ego";

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("::"), std::string::npos) << text.error().message;

  const auto findings = osc::validate_scenario(scenario);
  EXPECT_FALSE(findings_with_rule(findings, osc::rules::kNoDoubleColonPrefix).empty());
}

TEST(XoscWriter, RefusesAnInitActionThatNamesNoDeclaredEntity) {
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].entity_ref = "Ghost";

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("Ghost"), std::string::npos) << text.error().message;
}

TEST(XoscWriter, RefusesAPreservedFragmentThatIsNotWellFormed) {
  // A deliberate strengthening of the OpenDRIVE writer, which ignores
  // append_buffer's parse status and can drop a corrupt fragment in silence.
  osc::Scenario scenario = minimal_scenario();
  scenario.preserved.children.emplace_back("<Custom><Unclosed></Custom>");

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("well-formed"), std::string::npos) << text.error().message;
}

TEST(XoscWriter, AcceptsAValidScenarioWithNoFindingsAtAll) {
  // The counterpart every refusal test needs: without it, a validator that
  // refused EVERYTHING would pass all of them.
  EXPECT_TRUE(osc::validate_scenario(minimal_scenario()).empty());
  EXPECT_TRUE(osc::write_xosc(minimal_scenario()).has_value());
}

// --- save --------------------------------------------------------------------

TEST(XoscWriter, SaveWritesExactlyWhatWriteReturns) {
  const osc::Scenario scenario = minimal_scenario();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rm_xosc_save_roundtrip.xosc";
  std::filesystem::remove(path);

  ASSERT_TRUE(osc::save_xosc(scenario, path).has_value());

  std::ifstream stream(path, std::ios::binary);
  ASSERT_TRUE(stream.good());
  const std::string on_disk((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
  stream.close();
  std::filesystem::remove(path);

  // Byte equality, which is also what catches a missing std::ios::binary —
  // though only on Windows, where the newline translation actually happens.
  EXPECT_EQ(on_disk, written(scenario));
}

TEST(XoscWriter, SaveReportsIoFailureForAnUnwritablePath) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rm_xosc_no_such_dir" / "scenario.xosc";

  const auto result = osc::save_xosc(minimal_scenario(), path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::IoFailure);
}

TEST(XoscWriter, SaveDoesNotWriteAFileForARefusedScenario) {
  osc::Scenario scenario = minimal_scenario();
  scenario.entities.scenario_objects[0].name.clear();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rm_xosc_refused.xosc";
  std::filesystem::remove(path);

  EXPECT_FALSE(osc::save_xosc(scenario, path).has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
}

// --- the rule catalogue ------------------------------------------------------

namespace {

struct NamedRule {
  std::string_view name;
  std::string_view uid;
};

/// Every constant `roadmaker/osc/rules.hpp` declares. Kept beside the header
/// deliberately: a constant added there without a row here fails
/// EveryRuleConstantIsCitedBySomeFinding rather than sitting unused.
constexpr NamedRule kRules[] = {
    {"kUniqueElementNames", osc::rules::kUniqueElementNames},
    {"kNoDoubleColonPrefix", osc::rules::kNoDoubleColonPrefix},
    {"kPhaseDurationNonNegative", osc::rules::kPhaseDurationNonNegative},
    {"kTrafficSignalStateReferences", osc::rules::kTrafficSignalStateReferences},
    {"kTrafficSignalControllerReferences", osc::rules::kTrafficSignalControllerReferences},
};

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t next = text.find(separator, start);
    if (next == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, next - start));
    start = next + 1;
  }
}

} // namespace

TEST(XoscRules, EveryUidIsWellFormed) {
  // Parses the UID rather than checking it is non-empty: a loop asserting
  // "not empty" over a hand-written array proves nothing at all.
  for (const NamedRule& rule : kRules) {
    const std::vector<std::string_view> fields = split(rule.uid, ':');
    ASSERT_EQ(fields.size(), 4U) << rule.name << " -> " << rule.uid;
    EXPECT_EQ(fields[0], "asam.net") << rule.name;
    EXPECT_EQ(fields[1], "xosc") << rule.name << " is not an OpenSCENARIO rule id";
    EXPECT_EQ(split(fields[3], '.').size(), 2U) << rule.name << " lacks a <rule_set>.<rule_name>";
  }
}

TEST(XoscRules, NoUidIsStampedLaterThanTheDefaultTarget) {
  // ADR-0014 §3's argument that defaulting to 1.2 is FREE rests on this: every
  // id in the 1.4.0 catalogue first appeared at 1.0.0, 1.1.0 or 1.2.0. A
  // 1.3.0- or 1.4.0-stamped rule would be uncitable at the default target, and
  // the "costs nothing" claim would quietly stop being true.
  const std::set<std::string_view> citable = {"1.0.0", "1.1.0", "1.2.0"};
  for (const NamedRule& rule : kRules) {
    const std::vector<std::string_view> fields = split(rule.uid, ':');
    ASSERT_EQ(fields.size(), 4U) << rule.name;
    EXPECT_TRUE(citable.count(fields[2]) != 0)
        << rule.name << " is stamped " << fields[2] << ", which the default 1.2 target cannot cite";
  }
}

TEST(XoscRules, EveryRuleConstantIsCitedBySomeFinding) {
  // The gate that stops rules.hpp becoming a wall of declared-and-never-used
  // constants. Without it, "validation cites normative rule ids" — this
  // sprint's acceptance — is entirely unmeasured.
  std::vector<osc::Scenario> provoking;

  osc::Scenario duplicate_names = minimal_scenario();
  duplicate_names.entities.scenario_objects.push_back(duplicate_names.entities.scenario_objects[0]);
  provoking.push_back(duplicate_names);

  osc::Scenario double_colon = minimal_scenario();
  double_colon.entities.scenario_objects[0].name = "a::b";
  provoking.push_back(double_colon);

  osc::Scenario bad_signals = minimal_scenario();
  osc::TrafficSignalController controller;
  controller.name = "17";
  controller.reference = "no-such-controller";
  osc::Phase phase;
  phase.name = "go";
  phase.duration = -1.0;
  phase.signal_states.push_back(osc::TrafficSignalState{});
  controller.phases.push_back(phase);
  bad_signals.road_network.traffic_signal_controllers.push_back(controller);
  provoking.push_back(bad_signals);

  std::set<std::string> cited;
  for (const osc::Scenario& scenario : provoking) {
    for (const Diagnostic& finding : osc::validate_scenario(scenario)) {
      if (!finding.rule_id.empty()) {
        cited.insert(finding.rule_id);
      }
    }
  }

  for (const NamedRule& rule : kRules) {
    EXPECT_TRUE(cited.count(std::string(rule.uid)) != 0)
        << rule.name << " is declared but no finding cites it";
  }
}
