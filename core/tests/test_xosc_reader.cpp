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

// The OpenSCENARIO reader's contract (p8-s1, issue #245): what it refuses
// outright, what it round-trips byte for byte, and — the bulk of this file —
// every place an element it does not model has to survive anyway.
//
// THE PRESERVED TIER IS TESTED AGAINST REAL FOREIGN INPUT HERE FOR THE FIRST
// TIME. Every `RawXml` in the PR-B suite was populated by test code, so the
// never-drop contract was asserted and never exercised; each test below feeds
// the reader a document it cannot fully model and checks the content comes back
// out. A preserved fragment comes back RE-CANONICALIZED (quoting, whitespace,
// indentation) — that is fmt-s2's caveat (#326), and the assertions are written
// against the canonical form rather than pretending otherwise.

#include "roadmaker/osc/reader.hpp"
#include "roadmaker/osc/rules.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using roadmaker::Diagnostic;
using roadmaker::Severity;
namespace osc = roadmaker::osc;

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

/// True iff some diagnostic's message contains `needle`.
bool any_message_contains(const std::vector<Diagnostic>& findings, std::string_view needle) {
  return std::any_of(findings.begin(), findings.end(), [needle](const Diagnostic& finding) {
    return contains(finding.message, needle);
  });
}

/// A complete, minimal, valid document — the esmini wrapper's element set
/// (scripts/esmini_smoke.py:49-101), which CI already proves a shipping parser
/// accepts at revision 1.2. Tests below splice extra content into it rather
/// than hand-writing a whole document each time, so what a test is ABOUT is
/// the part that differs.
constexpr std::string_view kMinimalDocument = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenSCENARIO>
  <FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="fixture" author="RoadMaker"/>
  <ParameterDeclarations/>
  <CatalogLocations/>
  <RoadNetwork>
    <LogicFile filepath="town.xodr"/>
  </RoadNetwork>
  <Entities>
    <ScenarioObject name="Ego">
      <Vehicle name="car" vehicleCategory="car">
        <Performance maxSpeed="70" maxAcceleration="5" maxDeceleration="10"/>
        <BoundingBox>
          <Center x="1.4" y="0" z="0.75"/>
          <Dimensions width="2" length="5" height="1.5"/>
        </BoundingBox>
        <Axles>
          <FrontAxle maxSteering="0.5" wheelDiameter="0.6" trackWidth="1.8"
                     positionX="2.98" positionZ="0.3"/>
          <RearAxle maxSteering="0" wheelDiameter="0.6" trackWidth="1.8"
                    positionX="0" positionZ="0.3"/>
        </Axles>
        <Properties/>
      </Vehicle>
    </ScenarioObject>
  </Entities>
  <Storyboard>
    <Init>
      <Actions>
        <Private entityRef="Ego">
          <PrivateAction>
            <TeleportAction>
              <Position>
                <WorldPosition x="0" y="0" z="0" h="0"/>
              </Position>
            </TeleportAction>
          </PrivateAction>
        </Private>
      </Actions>
    </Init>
    <StopTrigger>
      <ConditionGroup>
        <Condition name="end" delay="0" conditionEdge="rising">
          <ByValueCondition>
            <SimulationTimeCondition value="0.5" rule="greaterThan"/>
          </ByValueCondition>
        </Condition>
      </ConditionGroup>
    </StopTrigger>
  </Storyboard>
</OpenSCENARIO>
)";

/// `kMinimalDocument` with `marker` replaced by `replacement`.
std::string with_replacement(std::string_view marker, std::string_view replacement) {
  std::string document(kMinimalDocument);
  const std::size_t at = document.find(marker);
  EXPECT_NE(at, std::string::npos) << "the splice marker '" << marker
                                   << "' is not in the minimal document — the test would be "
                                      "asserting against unmodified input";
  if (at == std::string::npos) {
    return document;
  }
  return document.replace(at, marker.size(), replacement);
}

osc::XoscParseResult parsed(std::string_view text) {
  auto result = osc::parse_xosc(text, "<test>");
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  return result ? std::move(*result) : osc::XoscParseResult{};
}

std::string written(const osc::Scenario& scenario, const osc::WriteOptions& options = {}) {
  const auto text = osc::write_xosc(scenario, options);
  EXPECT_TRUE(text.has_value()) << (text ? "" : text.error().message);
  return text ? *text : std::string{};
}

/// The scenario the reader builds from the minimal document.
osc::Scenario minimal_scenario() {
  return parsed(kMinimalDocument).scenario;
}

const osc::Vehicle& ego_vehicle(const osc::Scenario& scenario) {
  static const osc::Vehicle kFallback;
  if (scenario.entities.scenario_objects.empty()) {
    ADD_FAILURE() << "the scenario declares no entities";
    return kFallback;
  }
  const auto* vehicle =
      std::get_if<osc::Vehicle>(&scenario.entities.scenario_objects[0].entity_object);
  if (vehicle == nullptr) {
    ADD_FAILURE() << "the first entity's object is not a Vehicle";
    return kFallback;
  }
  return *vehicle;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "could not open " << path;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return std::move(buffer).str();
}

} // namespace

// --- the round trip ----------------------------------------------------------

TEST(XoscReader, RoundTripFromTheWrittenFormIsByteIdentical) {
  // THE CLAIM THIS SPRINT ACTUALLY OWES, and it is narrower than "byte-stable
  // for any input": idempotence from the RoadMaker-authored form. A foreign
  // file re-canonicalizes on its FIRST write (attribute order, the
  // always-present skeleton, whitespace) — after that, writing is a fixed
  // point. This is the property the GW-6 replay fingerprints state with.
  const std::string first = written(minimal_scenario());
  ASSERT_FALSE(first.empty());
  const std::string second = written(parsed(first).scenario);
  EXPECT_EQ(first, second);
}

TEST(XoscReader, RoundTripIsByteIdenticalAtRevisionOneFour) {
  // Separately, because 1.4 is the revision that emits @semantics — the one
  // piece of content the two targets do not share. A reader that dropped it
  // would still pass the 1.2 test above.
  const osc::WriteOptions options{.target_version = osc::OscVersion::v1_4};
  osc::Scenario scenario = minimal_scenario();
  osc::TrafficSignalController controller;
  controller.name = "42";
  osc::Phase phase;
  phase.name = "stop_short";
  phase.duration = 4.0;
  phase.semantics = osc::PhaseSemantics::Stop;
  phase.signal_states.push_back(
      osc::TrafficSignalState{.traffic_signal_id = "17251", .state = "on", .preserved = {}});
  controller.phases.push_back(phase);
  scenario.road_network.traffic_signal_controllers.push_back(controller);

  const std::string first = written(scenario, options);
  ASSERT_TRUE(contains(first, "semantics=\"stop\""));
  const std::string second = written(parsed(first).scenario, options);
  EXPECT_EQ(first, second);
}

TEST(XoscReader, ARoundTripDoesNotInventOrLoseAPhaseSemantic) {
  // The 1.2 half of the pair above. Reading a 1.4 document and writing it at
  // the default target must DROP the attribute (it did not exist at 1.2)
  // without losing it from the model, so the same scenario written at 1.4
  // still carries it.
  const std::string at_one_four = R"(<Phase name="go" duration="5" semantics="go"/>)";
  const std::string document =
      with_replacement("<LogicFile filepath=\"town.xodr\"/>",
                       std::string(R"(<LogicFile filepath="town.xodr"/>
    <TrafficSignals><TrafficSignalController name="42">)") +
                           at_one_four + R"(</TrafficSignalController></TrafficSignals>)");

  const osc::Scenario scenario = parsed(document).scenario;
  ASSERT_EQ(scenario.road_network.traffic_signal_controllers.size(), 1U);
  ASSERT_EQ(scenario.road_network.traffic_signal_controllers[0].phases.size(), 1U);
  EXPECT_EQ(scenario.road_network.traffic_signal_controllers[0].phases[0].semantics,
            osc::PhaseSemantics::Go);

  EXPECT_FALSE(contains(written(scenario), "semantics="));
  EXPECT_TRUE(
      contains(written(scenario, {.target_version = osc::OscVersion::v1_4}), "semantics=\"go\""));
}

// --- structural refusals -----------------------------------------------------

TEST(XoscReader, RefusesMalformedXml) {
  const auto result = osc::parse_xosc("<OpenSCENARIO><Entities>", "<test>");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::MalformedXml);
}

TEST(XoscReader, RefusesADocumentWithNoOpenScenarioRoot) {
  const auto result = osc::parse_xosc("<OpenDRIVE><header/></OpenDRIVE>", "<test>");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidDocument);
}

TEST(XoscReader, RefusesACatalogRatherThanReturningAnEmptyScenario) {
  // A catalog shares the <OpenSCENARIO> root and differs only in the child
  // that follows the header. Parsing it "successfully" into a scenario with no
  // entities is the failure worth naming: it looks like a load of an empty
  // file rather than a refusal to load a file of the wrong kind.
  const auto result = osc::parse_xosc(
      R"(<OpenSCENARIO><FileHeader revMajor="1" revMinor="2"/><Catalog name="v"/></OpenSCENARIO>)",
      "<test>");
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error().message, "catalog"));
}

TEST(XoscReader, AnEmptyBufferIsRefusedAndDoesNotCrash) {
  EXPECT_FALSE(osc::parse_xosc("", "<test>").has_value());
}

// --- the revision ------------------------------------------------------------

TEST(XoscReader, TheDeclaredRevisionIsRecordedOnTheResult) {
  const osc::XoscParseResult result = parsed(kMinimalDocument);
  EXPECT_EQ(result.rev_major, 1);
  EXPECT_EQ(result.rev_minor, 2);
}

TEST(XoscReader, TheDeclaredRevisionIsNotPreservedAndSoIsNeverEmittedTwice) {
  // The writer re-derives revMajor/revMinor from the target revision. If the
  // reader ALSO put them on the preserved tier, the writer would emit each
  // attribute twice — and a duplicate attribute is not well-formed XML, so the
  // file would stop loading anywhere. The count is the assertion.
  const osc::Scenario scenario = parsed(kMinimalDocument).scenario;
  for (const auto& [name, value] : scenario.header.preserved.attributes) {
    EXPECT_NE(name, "revMajor");
    EXPECT_NE(name, "revMinor");
  }

  const std::string text = written(scenario);
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n') > 0, true);
  const std::size_t first = text.find("revMinor=");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(text.find("revMinor=", first + 1), std::string::npos)
      << "revMinor was emitted twice — the reader preserved a writer-owned attribute";
}

TEST(XoscReader, ARevisionOutsideOnePointXIsReported) {
  const std::string document = with_replacement("revMajor=\"1\"", "revMajor=\"2\"");
  EXPECT_TRUE(any_message_contains(parsed(document).diagnostics, "outside the supported 1.x"));
}

// --- the preserved tier, against foreign input -------------------------------

TEST(XoscReader, AnUnmodeledElementIsPreservedAndReported) {
  const std::string document =
      with_replacement("<LogicFile filepath=\"town.xodr\"/>",
                       R"(<LogicFile filepath="town.xodr"/><UsedArea><Position/></UsedArea>)");
  const osc::XoscParseResult result = parsed(document);

  ASSERT_EQ(result.scenario.road_network.preserved.children.size(), 1U);
  EXPECT_TRUE(contains(result.scenario.road_network.preserved.children[0], "<UsedArea>"));
  EXPECT_TRUE(any_message_contains(result.diagnostics, "<UsedArea>"));
  // And it comes back out — the half that "it was captured" does not prove.
  EXPECT_TRUE(contains(written(result.scenario), "<UsedArea>"));
}

TEST(XoscReader, EveryUnmodeledSiblingIsPreserved) {
  // ★ FOUND BY THE SABOTAGE MATRIX, not by review. Preserving only the FIRST
  // unmodeled child of an element passed every other test in this file,
  // because none of them had two — so "the preserved tier works" rested
  // entirely on inputs with exactly one thing to preserve. The count and the
  // ORDER are both the assertion.
  const std::string document =
      with_replacement("<LogicFile filepath=\"town.xodr\"/>",
                       R"(<LogicFile filepath="town.xodr"/><UsedArea/><Vendor a="1"/><Extra/>)");
  const osc::XoscParseResult result = parsed(document);

  const std::vector<std::string>& kept = result.scenario.road_network.preserved.children;
  ASSERT_EQ(kept.size(), 3U) << "an unmodeled sibling was dropped";
  EXPECT_TRUE(contains(kept[0], "<UsedArea"));
  EXPECT_TRUE(contains(kept[1], "<Vendor"));
  EXPECT_TRUE(contains(kept[2], "<Extra"));

  const std::string text = written(result.scenario);
  EXPECT_LT(text.find("<UsedArea"), text.find("<Vendor"));
  EXPECT_LT(text.find("<Vendor"), text.find("<Extra"));
}

TEST(XoscReader, TrafficSignalGroupStateIsPreservedAndNeverNormalised) {
  // ★ The group form carries a state list and NO signal ids at all, so
  // normalizing it into per-signal states would INVENT the identity fact
  // ADR-0014 §5 exists to protect. It must survive untouched and produce no
  // TrafficSignalState.
  const std::string document = with_replacement(
      "<LogicFile filepath=\"town.xodr\"/>",
      R"(<LogicFile filepath="town.xodr"/><TrafficSignals><TrafficSignalController name="42">)"
      R"(<Phase name="go" duration="5"><TrafficSignalGroupState state="on;off;off"/></Phase>)"
      R"(</TrafficSignalController></TrafficSignals>)");

  const osc::XoscParseResult result = parsed(document);
  ASSERT_EQ(result.scenario.road_network.traffic_signal_controllers.size(), 1U);
  const osc::Phase& phase = result.scenario.road_network.traffic_signal_controllers[0].phases[0];

  EXPECT_TRUE(phase.signal_states.empty())
      << "the group state was normalised into per-signal states, inventing signal ids";
  ASSERT_EQ(phase.preserved.children.size(), 1U);
  EXPECT_TRUE(contains(phase.preserved.children[0], "on;off;off"));
  EXPECT_TRUE(contains(written(result.scenario), "<TrafficSignalGroupState state=\"on;off;off\""));
}

TEST(XoscReader, AnUnmodeledEntityObjectStaysMonostateAndSurvives) {
  const std::string document =
      with_replacement("<Vehicle name=\"car\" vehicleCategory=\"car\">",
                       "<MiscObject name=\"cone\"><Extra/></MiscObject><Vehicle name=\"car\" "
                       "vehicleCategory=\"car\">");
  const osc::Scenario scenario = parsed(document).scenario;

  ASSERT_EQ(scenario.entities.scenario_objects.size(), 1U);
  const osc::ScenarioObject& object = scenario.entities.scenario_objects[0];
  // The Vehicle still wins the variant (it is the modeled arm); the MiscObject
  // rides the preserved tier rather than being dropped.
  EXPECT_TRUE(std::holds_alternative<osc::Vehicle>(object.entity_object));
  EXPECT_TRUE(contains(written(scenario), "<MiscObject name=\"cone\">"));
}

TEST(XoscReader, AnEntityWithOnlyAnUnmodeledObjectIsMonostate) {
  const std::string document = with_replacement(
      R"(<Vehicle name="car" vehicleCategory="car">
        <Performance maxSpeed="70" maxAcceleration="5" maxDeceleration="10"/>
        <BoundingBox>
          <Center x="1.4" y="0" z="0.75"/>
          <Dimensions width="2" length="5" height="1.5"/>
        </BoundingBox>
        <Axles>
          <FrontAxle maxSteering="0.5" wheelDiameter="0.6" trackWidth="1.8"
                     positionX="2.98" positionZ="0.3"/>
          <RearAxle maxSteering="0" wheelDiameter="0.6" trackWidth="1.8"
                    positionX="0" positionZ="0.3"/>
        </Axles>
        <Properties/>
      </Vehicle>)",
      R"(<CatalogReference catalogName="Vehicles" entryName="car"/>)");

  const osc::Scenario scenario = parsed(document).scenario;
  ASSERT_EQ(scenario.entities.scenario_objects.size(), 1U);
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(scenario.entities.scenario_objects[0].entity_object));
  EXPECT_TRUE(contains(written(scenario), "<CatalogReference catalogName=\"Vehicles\""));
}

TEST(XoscReader, ANonTeleportPrivateActionIsPreservedWhole) {
  const std::string document = with_replacement(
      "<TeleportAction>",
      R"(<LongitudinalAction><SpeedAction/></LongitudinalAction><TeleportAction>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  EXPECT_TRUE(action.teleport.has_value());
  EXPECT_TRUE(contains(written(scenario), "<LongitudinalAction>"));
}

TEST(XoscReader, ATeleportWhosePositionIsNotAWorldPositionIsPreservedWhole) {
  // ★ THE TRAP THIS TEST EXISTS FOR. <Position> is a ten-way choice and the
  // model holds only a WorldPosition. Reading the teleport anyway and leaving
  // the position at its default would produce a file that parses, writes and
  // simulates — with the actor silently teleported to the origin instead of
  // onto the lane the author named.
  const std::string document =
      with_replacement(R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
                       R"(<LanePosition roadId="1" laneId="-1" s="12.5" offset="0"/>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  EXPECT_FALSE(action.teleport.has_value())
      << "the teleport was modeled without its position — the entity would move to the origin";

  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<LanePosition roadId="1" laneId="-1" s="12.5")"));
  EXPECT_FALSE(contains(text, "<WorldPosition"))
      << "a world position was invented for a lane-positioned teleport";
}

TEST(XoscReader, ANonSimulationTimeConditionIsPreservedWhole) {
  const std::string document =
      with_replacement(R"(<SimulationTimeCondition value="0.5" rule="greaterThan"/>)",
                       R"(<StoryboardElementStateCondition state="completeState"/>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::Condition& condition =
      scenario.storyboard.stop_trigger.condition_groups[0].conditions[0];
  EXPECT_FALSE(condition.simulation_time.has_value());
  EXPECT_TRUE(contains(written(scenario), "<StoryboardElementStateCondition"));
}

TEST(XoscReader, AFileInsidePropertiesIsReEmittedInsideProperties) {
  // <Properties> holds File* and CustomContent* as well as Property*, and an
  // esmini vehicle catalog routinely carries a <File>. Preserving it on the
  // VEHICLE would re-emit it as a sibling of <Properties>, one level too high —
  // the InitActions rationale, met a second time (osc/scenario.hpp).
  const std::string document =
      with_replacement("<Properties/>", R"(<Properties><File filepath="car.osgb"/></Properties>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::Vehicle& car = ego_vehicle(scenario);
  EXPECT_TRUE(car.preserved.children.empty())
      << "the <File> landed on the vehicle and will be emitted beside <Properties>";
  ASSERT_EQ(car.properties_preserved.children.size(), 1U);

  const std::string text = written(scenario);
  const std::size_t properties = text.find("<Properties>");
  const std::size_t file = text.find("<File filepath=\"car.osgb\"");
  const std::size_t close = text.find("</Properties>");
  ASSERT_NE(properties, std::string::npos);
  ASSERT_NE(file, std::string::npos);
  ASSERT_NE(close, std::string::npos);
  EXPECT_LT(properties, file);
  EXPECT_LT(file, close) << "the <File> was emitted outside <Properties>";
}

TEST(XoscReader, UnknownAttributesArePreservedInDocumentOrder) {
  const std::string document =
      with_replacement(R"(<Vehicle name="car" vehicleCategory="car">)",
                       R"(<Vehicle name="car" vehicleCategory="car" role="ego" vendorFlag="7">)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::Vehicle& car = ego_vehicle(scenario);
  ASSERT_EQ(car.preserved.attributes.size(), 2U);
  EXPECT_EQ(car.preserved.attributes[0].first, "role");
  EXPECT_EQ(car.preserved.attributes[1].first, "vendorFlag");
  EXPECT_TRUE(contains(written(scenario), R"(role="ego" vendorFlag="7")"));
}

TEST(XoscReader, AnUnknownPhaseSemanticIsPreservedRatherThanDropped) {
  const std::string document = with_replacement(
      "<LogicFile filepath=\"town.xodr\"/>",
      R"(<LogicFile filepath="town.xodr"/><TrafficSignals><TrafficSignalController name="42">)"
      R"(<Phase name="go" duration="5" semantics="flashing"/>)"
      R"(</TrafficSignalController></TrafficSignals>)");
  const osc::XoscParseResult result = parsed(document);

  const osc::Phase& phase = result.scenario.road_network.traffic_signal_controllers[0].phases[0];
  EXPECT_FALSE(phase.semantics.has_value());
  ASSERT_EQ(phase.preserved.attributes.size(), 1U);
  EXPECT_EQ(phase.preserved.attributes[0].second, "flashing");
  EXPECT_TRUE(any_message_contains(result.diagnostics, "flashing"));

  // Emitted exactly once, not twice: the modeled enum stayed unset precisely so
  // the preserved spelling is the only source of the attribute.
  const std::string text = written(result.scenario, {.target_version = osc::OscVersion::v1_4});
  const std::size_t first = text.find("semantics=");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(text.find("semantics=", first + 1), std::string::npos);
}

TEST(XoscReader, AStoryIsPreservedInItsSchemaSlotNotAfterTheStopTrigger) {
  const std::string document =
      with_replacement("<StopTrigger>", R"(<Story name="s"><Act name="a"/></Story><StopTrigger>)");
  const osc::Scenario scenario = parsed(document).scenario;

  ASSERT_EQ(scenario.storyboard.preserved_stories.size(), 1U);
  EXPECT_TRUE(scenario.storyboard.preserved.children.empty())
      << "the story landed on the generic tier and will be emitted after <StopTrigger>";

  const std::string text = written(scenario);
  const std::size_t init = text.find("</Init>");
  const std::size_t story = text.find("<Story name=\"s\">");
  const std::size_t stop = text.find("<StopTrigger>");
  ASSERT_NE(story, std::string::npos);
  EXPECT_LT(init, story);
  EXPECT_LT(story, stop);
}

TEST(XoscReader, AGlobalActionIsReportedBecausePreservingItReordersIt) {
  // The one place appending preserved children last is NOT schema-safe:
  // InitActions sequences globalActions*, privates*, userDefinedActions*, so a
  // preserved <GlobalAction> moves past the privates. Reported rather than
  // silently reordered.
  const std::string document = with_replacement(
      R"(<Private entityRef="Ego">)",
      R"(<GlobalAction><EnvironmentAction/></GlobalAction><Private entityRef="Ego">)");
  const osc::XoscParseResult result = parsed(document);

  EXPECT_TRUE(any_message_contains(result.diagnostics, "schema position"));
  EXPECT_TRUE(contains(written(result.scenario), "<GlobalAction>"));
}

TEST(XoscReader, AnUnknownAttributeOnAFlattenedContainerIsLiftedAndReported) {
  // <Center> and <Dimensions> are flattened into BoundingBox, so an attribute
  // they alone could own has nowhere of its own to live. Lifting it is stated
  // out loud; dropping it silently is what this refuses to do.
  const std::string document = with_replacement(
      R"(<Center x="1.4" y="0" z="0.75"/>)", R"(<Center x="1.4" y="0" z="0.75" vendorHint="a"/>)");
  const osc::XoscParseResult result = parsed(document);

  const osc::Vehicle& car = ego_vehicle(result.scenario);
  ASSERT_EQ(car.bounding_box.preserved.attributes.size(), 1U);
  EXPECT_EQ(car.bounding_box.preserved.attributes[0].first, "vendorHint");
  EXPECT_TRUE(any_message_contains(result.diagnostics, "flattened"));
}

// --- diagnostics that cite a rule --------------------------------------------

TEST(XoscReader, AMissingRequiredElementCitesTheSchemaRule) {
  // Removed rather than renamed: renaming <Entities> orphans its closing tag,
  // and the document then fails as malformed XML long before any element is
  // found missing — a splice that tests the wrong code path entirely.
  // <CatalogLocations/> is self-closing, so deleting it leaves the rest valid.
  const osc::XoscParseResult result = parsed(with_replacement("<CatalogLocations/>", ""));
  const std::vector<Diagnostic> findings =
      findings_with_rule(result.diagnostics, osc::rules::kValidSchema);
  ASSERT_EQ(findings.size(), 1U);
  EXPECT_TRUE(contains(findings[0].message, "CatalogLocations"));
}

TEST(XoscReader, AMissingFileHeaderCitesTheSchemaRule) {
  const std::string document = with_replacement(
      R"(<FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="fixture" author="RoadMaker"/>)",
      "");
  EXPECT_FALSE(findings_with_rule(parsed(document).diagnostics, osc::rules::kValidSchema).empty());
}

TEST(XoscReader, AScenarioWithNoLogicFileCitesTheRoadNetworkRule) {
  const std::string document = with_replacement(R"(<LogicFile filepath="town.xodr"/>)", "");
  const osc::XoscParseResult result = parsed(document);
  EXPECT_FALSE(findings_with_rule(result.diagnostics, osc::rules::kRoadNetworkReference).empty());
}

TEST(XoscReader, AnAbsentRoadNetworkIsAnAdvisoryAndNotASchemaBreach) {
  // The rule's own text says "the scenario is also valid without a defined road
  // network", so citing the schema rule here would report legal input as a
  // violation. Coding from the rule's NAME is how that mistake happens.
  const std::string document = with_replacement(
      "<RoadNetwork>\n    <LogicFile filepath=\"town.xodr\"/>\n  </RoadNetwork>", "");
  const osc::XoscParseResult result = parsed(document);
  EXPECT_FALSE(findings_with_rule(result.diagnostics, osc::rules::kRoadNetworkReference).empty());
  EXPECT_TRUE(findings_with_rule(result.diagnostics, osc::rules::kValidSchema).empty());
}

TEST(XoscReader, ANegativeConditionDelayIsAnErrorCitingItsRule) {
  // The gap PR-B left: scenario.hpp named this rule from the first commit and
  // nothing enforced it, which was invisible while every Condition in the tree
  // was built by test code.
  const std::string document = with_replacement(R"(delay="0")", R"(delay="-1")");
  const osc::Scenario scenario = parsed(document).scenario;

  const std::vector<Diagnostic> findings =
      findings_with_rule(osc::validate_scenario(scenario), osc::rules::kConditionDelayNonNegative);
  ASSERT_EQ(findings.size(), 1U);
  EXPECT_EQ(findings[0].severity, Severity::Error);
  EXPECT_FALSE(osc::write_xosc(scenario).has_value());
}

TEST(XoscReader, AZeroConditionDelayIsLegal) {
  // The paired assertion. "Non negative" means zero passes; a guard written
  // from the phrase "delay must be positive" would refuse valid input.
  EXPECT_TRUE(findings_with_rule(osc::validate_scenario(minimal_scenario()),
                                 osc::rules::kConditionDelayNonNegative)
                  .empty());
}

TEST(XoscReader, AMalformedNumberKeepsItsSpellingInTheDiagnostic) {
  // It is NOT routed into the preserved tier, and that is deliberate: the
  // writer emits modeled attributes and then preserved ones with no
  // de-duplication, so preserving the spelling would emit @duration twice and
  // a duplicate attribute is not well-formed XML. The spelling survives in the
  // diagnostic instead.
  const std::string document =
      with_replacement(R"(<SimulationTimeCondition value="0.5" rule="greaterThan"/>)",
                       R"(<SimulationTimeCondition value="soon" rule="greaterThan"/>)");
  const osc::XoscParseResult result = parsed(document);

  EXPECT_TRUE(any_message_contains(result.diagnostics, "'soon'"));
  const std::string text = written(result.scenario);
  const std::size_t first = text.find("value=");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(text.find("value=", first + 1), std::string::npos);
}

TEST(XoscReader, ASecondLogicFileIsReportedAndTheFirstWins) {
  const std::string document =
      with_replacement(R"(<LogicFile filepath="town.xodr"/>)",
                       R"(<LogicFile filepath="town.xodr"/><LogicFile filepath="other.xodr"/>)");
  const osc::XoscParseResult result = parsed(document);

  ASSERT_TRUE(result.scenario.road_network.logic_file.has_value());
  EXPECT_EQ(result.scenario.road_network.logic_file->filepath, "town.xodr");
  EXPECT_TRUE(any_message_contains(result.diagnostics, "more than one"));
}

// --- load_xosc, the path-derived findings ------------------------------------

namespace {

/// A scenario document and its road network, written into a temp directory.
struct OnDisk {
  std::filesystem::path directory;
  std::filesystem::path scenario;

  explicit OnDisk(std::string_view stem, bool with_network) {
    directory =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    scenario = directory / std::string(stem);
    std::ofstream(scenario, std::ios::binary) << kMinimalDocument;
    if (with_network) {
      std::ofstream(directory / "town.xodr", std::ios::binary) << "<OpenDRIVE/>";
    }
  }

  ~OnDisk() {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }

  OnDisk(const OnDisk&) = delete;
  OnDisk& operator=(const OnDisk&) = delete;
};

} // namespace

TEST(XoscReader, LoadReportsNothingWhenTheNetworkIsBesideTheScenario) {
  const OnDisk files("scene.xosc", true);
  const auto result = osc::load_xosc(files.scenario);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(
      findings_with_rule(result->diagnostics, osc::rules::kRoadNetworkAvailability).empty());
  EXPECT_TRUE(findings_with_rule(result->diagnostics, osc::rules::kFileEnding).empty());
}

TEST(XoscReader, LoadReportsAnUnresolvableLogicFile) {
  const OnDisk files("scene.xosc", false);
  const auto result = osc::load_xosc(files.scenario);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(findings_with_rule(result->diagnostics, osc::rules::kRoadNetworkAvailability).size(),
            1U);
}

TEST(XoscReader, LoadReportsAFileExtensionThatIsNotXosc) {
  const OnDisk files("scene.xml", true);
  const auto result = osc::load_xosc(files.scenario);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(findings_with_rule(result->diagnostics, osc::rules::kFileEnding).size(), 1U);
}

TEST(XoscReader, LoadReportsAMissingFile) {
  const auto result = osc::load_xosc(std::filesystem::temp_directory_path() / "no-such.xosc");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::FileNotFound);
}

// --- the tracked esmini fixture ----------------------------------------------

TEST(XoscFixture, TheTrackedScenarioIsExactlyWhatTheWriterEmitsToday) {
  // ONE ASSERTION DOING TWO JOBS. It is the round trip against real tracked
  // content, and it is the drift guard: tests/esmini/signalized.xosc is the
  // file the CI esmini job feeds to a shipping simulator, so it must never
  // diverge from what this build would write. If the writer changes, this goes
  // red and the fixture is regenerated deliberately rather than by accident.
  const std::filesystem::path fixture =
      std::filesystem::path(RM_ESMINI_FIXTURES_DIR) / "signalized.xosc";
  ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture;

  const std::string tracked = read_file(fixture);
  const auto result = osc::load_xosc(fixture);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  EXPECT_EQ(written(result->scenario), tracked);
}

TEST(XoscFixture, TheTrackedScenarioCarriesTrafficSignalsWithResolvableIds) {
  // The fixture's whole reason for existing: it is the first .xosc CI has ever
  // fed to esmini, and the thing being proven is the traffic-signal half —
  // @state's spelling and a controller named by its OpenDRIVE @id. A fixture
  // that quietly lost its signals would still load in esmini and prove nothing.
  const std::filesystem::path fixture =
      std::filesystem::path(RM_ESMINI_FIXTURES_DIR) / "signalized.xosc";
  const auto result = osc::load_xosc(fixture);
  ASSERT_TRUE(result.has_value());

  const auto& controllers = result->scenario.road_network.traffic_signal_controllers;
  ASSERT_GE(controllers.size(), 2U)
      << "the decomposition target is one controller per <controller>";
  for (const osc::TrafficSignalController& controller : controllers) {
    EXPECT_FALSE(controller.name.empty());
    EXPECT_FALSE(controller.phases.empty());
    for (const osc::Phase& phase : controller.phases) {
      EXPECT_FALSE(phase.signal_states.empty())
          << "a phase with no states is a red-by-omission bug";
      for (const osc::TrafficSignalState& state : phase.signal_states) {
        EXPECT_FALSE(state.traffic_signal_id.empty());
        EXPECT_FALSE(state.state.empty());
      }
    }
  }
  EXPECT_TRUE(osc::validate_scenario(result->scenario).empty());
}

// --- the fuzz corpus ---------------------------------------------------------

TEST(XoscFuzzCorpus, EverySeedParsesOrFailsWithoutCrashing) {
  // The fuzz TARGET is Linux-only — AppleClang ships no libFuzzer runtime, so
  // core/tests/fuzz is skipped on the development platform and the corpus would
  // otherwise be exercised by nothing until CI. This runs each seed through the
  // same entry point the fuzzer calls, which under the sanitizer build is the
  // check that matters: the parser may return an error, it may not crash.
  const std::filesystem::path corpus(RM_FUZZ_CORPUS_XOSC_DIR);
  ASSERT_TRUE(std::filesystem::is_directory(corpus)) << corpus;

  std::size_t seeds = 0;
  std::size_t accepted = 0;
  for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    ++seeds;
    const auto result = osc::parse_xosc(read_file(entry.path()), entry.path().string());
    if (result.has_value()) {
      ++accepted;
      // A seed that parses must also survive a write — an accepted document
      // that cannot be re-emitted is the more interesting bug of the two.
      EXPECT_TRUE(osc::write_xosc(result->scenario).has_value() ||
                  !osc::validate_scenario(result->scenario).empty())
          << entry.path() << " parsed but neither wrote nor reported why";
    }
  }
  EXPECT_GE(seeds, 8U) << "the corpus lost seeds";
  // Both outcomes must be represented, or the corpus only exercises one branch.
  EXPECT_GT(accepted, 0U) << "no seed parses — the corpus tests only the refusal path";
  EXPECT_LT(accepted, seeds) << "every seed parses — the corpus has no malformed input left";
}
