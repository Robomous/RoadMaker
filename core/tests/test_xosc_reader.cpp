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

TEST(XoscReader, ATeleportWhosePositionIsNotModeledIsPreservedWhole) {
  // ★ THE TRAP THIS TEST EXISTS FOR. <Position> is an eleven-way choice and the
  // model holds three of them. Reading the teleport anyway and leaving the
  // position at its default would produce a file that parses, writes and
  // simulates — with the actor silently teleported to the origin instead of
  // onto the lane the author named.
  //
  // p8-s2 (#246) modeled <LanePosition> and <RoadPosition>, so the fixture had
  // to move to a position that is still unmodeled — <RelativeLanePosition>,
  // which additionally references another entity and could not be flattened
  // into this model even in principle. The trap is unchanged; only the example
  // of it is.
  const std::string document = with_replacement(
      R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
      R"(<RelativeLanePosition entityRef="Ego" dLane="-1" ds="12.5" offset="0"/>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  EXPECT_FALSE(action.teleport.has_value())
      << "the teleport was modeled without its position — the entity would move to the origin";

  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<RelativeLanePosition entityRef="Ego" dLane="-1" ds="12.5")"));
  EXPECT_FALSE(contains(text, "<WorldPosition"))
      << "a world position was invented for a relatively-positioned teleport";
}

// --- p8-s2 (#246): road- and lane-relative positions -------------------------

TEST(XoscReader, ALanePositionIsModeledAndRoundTrips) {
  const std::string document =
      with_replacement(R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
                       R"(<LanePosition roadId="7" laneId="-2" s="12.5" offset="0.25"/>)");
  const osc::XoscParseResult parsed_document = parsed(document);
  const osc::Scenario& scenario = parsed_document.scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  ASSERT_TRUE(action.teleport.has_value());
  const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
  ASSERT_NE(lane, nullptr) << "a <LanePosition> was read as some other alternative";
  EXPECT_EQ(lane->road_id, "7");
  EXPECT_EQ(lane->lane_id, "-2");
  EXPECT_DOUBLE_EQ(lane->s, 12.5);
  EXPECT_DOUBLE_EQ(lane->offset, 0.25);
  EXPECT_FALSE(lane->orientation.has_value());

  // The ids are STRINGS all the way through (ADR-0014 §5) — no id was parsed
  // into an int and re-rendered, which is how a leading zero or a temporary
  // lane-layer id would quietly change on a round trip.
  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<LanePosition roadId="7" laneId="-2" s="12.5" offset="0.25")"))
      << text;
  EXPECT_FALSE(contains(text, "<WorldPosition")) << text;

  // No warning: a modeled position must not report itself as unmodeled.
  for (const Diagnostic& finding : parsed_document.diagnostics) {
    EXPECT_EQ(finding.message.find("LanePosition"), std::string::npos) << finding.message;
  }
}

TEST(XoscReader, ARoadPositionIsModeledAndRoundTrips) {
  const std::string document = with_replacement(R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
                                                R"(<RoadPosition roadId="3" s="40" t="-1.75"/>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  ASSERT_TRUE(action.teleport.has_value());
  const auto* road = std::get_if<osc::RoadPosition>(&action.teleport->position);
  ASSERT_NE(road, nullptr);
  EXPECT_EQ(road->road_id, "3");
  EXPECT_DOUBLE_EQ(road->s, 40.0);
  EXPECT_DOUBLE_EQ(road->t, -1.75);

  EXPECT_TRUE(contains(written(scenario), R"(<RoadPosition roadId="3" s="40" t="-1.75")"));
}

TEST(XoscReader, ALanePositionsOrientationRoundTripsWithoutInventingAngles) {
  // Every angle is optional and "missing h is interpreted as 0" — so an
  // Orientation that carries only @h must NOT come back carrying p="0" r="0".
  // Writing the interpretation rather than the content is how a round trip
  // stops being byte-identical for everyone who omitted an angle.
  const std::string document = with_replacement(
      R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
      R"(<LanePosition roadId="7" laneId="-1" s="5" offset="0"><Orientation h="1.5708" type="relative"/></LanePosition>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  ASSERT_TRUE(action.teleport.has_value());
  const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
  ASSERT_NE(lane, nullptr);
  ASSERT_TRUE(lane->orientation.has_value());
  ASSERT_TRUE(lane->orientation->h.has_value());
  EXPECT_DOUBLE_EQ(*lane->orientation->h, 1.5708);
  EXPECT_FALSE(lane->orientation->p.has_value());
  EXPECT_FALSE(lane->orientation->r.has_value());
  EXPECT_EQ(lane->orientation->type, "relative");

  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<Orientation h="1.5708" type="relative" />)")) << text;
  EXPECT_FALSE(contains(text, R"(p="0")")) << "an omitted pitch was invented as 0";
  EXPECT_FALSE(contains(text, R"(r="0")")) << "an omitted roll was invented as 0";
}

TEST(XoscReader, AnUnmodeledLanePositionAttributeIsPreservedNotDropped) {
  // @layer was created in 1.4.0 and this build targets 1.2, so it is not
  // modeled. It must still survive: a reader that dropped it would silently
  // move an actor from the temporary lane layer onto the permanent one.
  const std::string document = with_replacement(
      R"(<WorldPosition x="0" y="0" z="0" h="0"/>)",
      R"(<LanePosition roadId="7" laneId="-1" s="5" offset="0" layer="temporary"/>)");
  const osc::Scenario scenario = parsed(document).scenario;

  EXPECT_TRUE(contains(written(scenario), R"(layer="temporary")"));
}

TEST(XoscReader, AnInitialSpeedIsModeledAndRoundTrips) {
  const std::string document = with_replacement(
      "</Private>",
      R"(<PrivateAction><LongitudinalAction><SpeedAction><SpeedActionDynamics dynamicsShape="step" value="0" dynamicsDimension="time"/><SpeedActionTarget><AbsoluteTargetSpeed value="13.89"/></SpeedActionTarget></SpeedAction></LongitudinalAction></PrivateAction></Private>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const std::vector<osc::PrivateAction>& actions =
      scenario.storyboard.init.actions.privates[0].actions;
  ASSERT_EQ(actions.size(), 2U) << "the speed action landed in the teleport's element";
  ASSERT_TRUE(actions[1].longitudinal.has_value());
  ASSERT_TRUE(actions[1].longitudinal->speed.has_value());
  ASSERT_TRUE(actions[1].longitudinal->speed->absolute_target.has_value());
  EXPECT_DOUBLE_EQ(actions[1].longitudinal->speed->absolute_target->value, 13.89);
  EXPECT_EQ(actions[1].longitudinal->speed->dynamics.dynamics_shape, "step");

  const std::string text = written(scenario);
  EXPECT_TRUE(contains(text, R"(<AbsoluteTargetSpeed value="13.89" />)")) << text;
}

TEST(XoscReader, ARelativeTargetSpeedIsPreservedInsideItsTarget) {
  // <SpeedActionTarget> is a 1..1 union. A <RelativeTargetSpeed> must come back
  // INSIDE the target and not as its sibling — the <Properties>/<File> lesson,
  // met on a third element.
  const std::string document = with_replacement(
      "</Private>",
      R"(<PrivateAction><LongitudinalAction><SpeedAction><SpeedActionDynamics dynamicsShape="step" value="0" dynamicsDimension="time"/><SpeedActionTarget><RelativeTargetSpeed entityRef="Ego" value="5" speedTargetValueType="delta"/></SpeedActionTarget></SpeedAction></LongitudinalAction></PrivateAction></Private>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const std::vector<osc::PrivateAction>& actions =
      scenario.storyboard.init.actions.privates[0].actions;
  ASSERT_EQ(actions.size(), 2U);
  ASSERT_TRUE(actions[1].longitudinal.has_value());
  ASSERT_TRUE(actions[1].longitudinal->speed.has_value());
  EXPECT_FALSE(actions[1].longitudinal->speed->absolute_target.has_value());

  // The relative target must sit INSIDE <SpeedActionTarget>. Asserting on the
  // nesting rather than on adjacent text, because pugixml pretty-prints and the
  // two elements land on separate lines.
  const std::string text = written(scenario);
  const std::size_t target_at = text.find("<SpeedActionTarget>");
  const std::size_t relative_at = text.find("<RelativeTargetSpeed");
  const std::size_t close_at = text.find("</SpeedActionTarget>");
  ASSERT_NE(target_at, std::string::npos) << text;
  ASSERT_NE(relative_at, std::string::npos) << text;
  ASSERT_NE(close_at, std::string::npos) << text;
  EXPECT_LT(target_at, relative_at) << "the relative target was emitted before its wrapper";
  EXPECT_LT(relative_at, close_at) << "the relative target was emitted OUTSIDE its wrapper";
}

TEST(XoscReader, ALongitudinalActionThatIsNotASpeedActionIsPreservedWhole) {
  const std::string document = with_replacement(
      "</Private>",
      R"(<PrivateAction><LongitudinalAction><LongitudinalDistanceAction entityRef="Ego" distance="10"/></LongitudinalAction></PrivateAction></Private>)");
  const osc::Scenario scenario = parsed(document).scenario;

  const std::vector<osc::PrivateAction>& actions =
      scenario.storyboard.init.actions.privates[0].actions;
  ASSERT_EQ(actions.size(), 2U);
  EXPECT_FALSE(actions[1].longitudinal.has_value())
      << "a LongitudinalDistanceAction was modeled as a SpeedAction";
  EXPECT_TRUE(contains(written(scenario), "<LongitudinalDistanceAction"));
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

// --- routes (p8-s3, #247) -----------------------------------------------------

namespace {

/// A two-waypoint lane-anchored route, spliced in as a second `<PrivateAction>`
/// of Ego's `<Private>`.
constexpr std::string_view kRouteAction = R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <Route name="EgoRoute" closed="false">
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="1" laneId="-1" s="5" offset="0"/></Position>
                  </Waypoint>
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="2" laneId="-1" s="40" offset="0"/></Position>
                  </Waypoint>
                </Route>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)";

/// `kMinimalDocument` with `extra` appended after Ego's teleport action.
std::string with_extra_private_action(std::string_view extra) {
  return with_replacement("        </Private>", std::string(extra) + "        </Private>");
}

/// The first route assigned in `scenario`'s `<Init>`, or nullptr.
///
/// ★ THE RETURNED POINTER BORROWS `scenario`, so `scenario` MUST OUTLIVE IT.
/// The rvalue overload below is deleted to make that a compile error rather
/// than a use-after-free: `ego_route(parsed(doc).scenario)` borrows into a
/// temporary that dies at the end of the full expression. That exact mistake
/// reached this file while it was being written, and the symptom was a route
/// that read back with zero waypoints — plausible enough to have been
/// investigated as a parser bug. Same discipline `element_slice` states in
/// test_xosc_writer.cpp, met a second time.
const osc::Route* ego_route(const osc::Scenario& scenario) {
  for (const osc::Private& entry : scenario.storyboard.init.actions.privates) {
    for (const osc::PrivateAction& action : entry.actions) {
      if (action.routing.has_value() && action.routing->assign_route.has_value() &&
          action.routing->assign_route->route.has_value()) {
        return &*action.routing->assign_route->route;
      }
    }
  }
  return nullptr;
}

const osc::Route* ego_route(osc::Scenario&&) = delete;

} // namespace

TEST(XoscReader, ARouteIsReadWithItsWaypointsInOrder) {
  const osc::Scenario scenario = parsed(with_extra_private_action(kRouteAction)).scenario;
  const osc::Route* route = ego_route(scenario);
  ASSERT_NE(route, nullptr);

  EXPECT_EQ(route->name, "EgoRoute");
  EXPECT_FALSE(route->closed);
  ASSERT_EQ(route->waypoints.size(), 2U);
  EXPECT_EQ(route->waypoints[0].route_strategy, "shortest");

  // ORDER is part of what a route is: waypoints[0] is where it starts.
  const auto* first = std::get_if<osc::LanePosition>(&route->waypoints[0].position);
  const auto* second = std::get_if<osc::LanePosition>(&route->waypoints[1].position);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->road_id, "1");
  EXPECT_DOUBLE_EQ(first->s, 5.0);
  EXPECT_EQ(second->road_id, "2");
  EXPECT_DOUBLE_EQ(second->s, 40.0);
}

TEST(XoscReader, ARouteRoundTripsByteForByte) {
  const std::string document = with_extra_private_action(kRouteAction);
  const osc::Scenario scenario = parsed(document).scenario;
  const std::string once = written(scenario);
  const std::string twice = written(parsed(once).scenario);
  EXPECT_EQ(once, twice);
  // ...and the route really is in the emitted text, so the equality above is
  // not two copies of a document that lost it.
  EXPECT_TRUE(contains(once, R"(<Route name="EgoRoute")")) << once;
}

TEST(XoscReader, AClosedRouteKeepsItsFlag) {
  const std::string document = with_extra_private_action(kRouteAction);
  std::string closed = document;
  const std::size_t at = closed.find(R"(closed="false")");
  ASSERT_NE(at, std::string::npos);
  closed.replace(at, std::string(R"(closed="false")").size(), R"(closed="true")");

  const osc::XoscParseResult result = parsed(closed);
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  EXPECT_TRUE(route->closed);
}

// ★ `@closed` is xsd:boolean, which admits exactly true/false/1/0. pugixml's
// as_bool() would read ANY other spelling as false, so a typo, an unresolved
// $parameter and a genuine `false` would come back identical — and only one of
// the three is what the file said.
TEST(XoscReader, AMalformedClosedFlagIsReportedRatherThanQuietlyFalse) {
  std::string document = with_extra_private_action(kRouteAction);
  const std::size_t at = document.find(R"(closed="false")");
  ASSERT_NE(at, std::string::npos);
  document.replace(at, std::string(R"(closed="false")").size(), R"(closed="tru")");

  const osc::XoscParseResult result = parsed(document);
  EXPECT_TRUE(any_message_contains(result.diagnostics, "not a valid boolean"))
      << "a malformed boolean was read as false in silence";

  // ...and it is NOT captured into the preserved tier, which would emit @closed
  // twice and produce ill-formed XML.
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  for (const auto& [name, value] : route->preserved.attributes) {
    EXPECT_NE(name, "closed") << name << "=" << value;
  }
  EXPECT_TRUE(osc::write_xosc(result.scenario).has_value());
}

TEST(XoscReader, A1And0AreValidBooleansToo) {
  std::string document = with_extra_private_action(kRouteAction);
  const std::size_t at = document.find(R"(closed="false")");
  ASSERT_NE(at, std::string::npos);
  document.replace(at, std::string(R"(closed="false")").size(), R"(closed="1")");

  const osc::XoscParseResult result = parsed(document);
  EXPECT_FALSE(any_message_contains(result.diagnostics, "not a valid boolean"));
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  EXPECT_TRUE(route->closed);
}

TEST(XoscReader, ARouteCarriesItsOwnParameterDeclarations) {
  // A route's declarations scope to the ROUTE. Folding them into the document's
  // would move them up a level and change what they scope.
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <Route name="EgoRoute" closed="false">
                  <ParameterDeclarations>
                    <ParameterDeclaration name="Speed" parameterType="double" value="13.9"/>
                  </ParameterDeclarations>
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="1" laneId="-1" s="5" offset="0"/></Position>
                  </Waypoint>
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="2" laneId="-1" s="40" offset="0"/></Position>
                  </Waypoint>
                </Route>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->parameter_declarations.size(), 1U);
  EXPECT_EQ(route->parameter_declarations[0].name, "Speed");
  // ...and it did NOT land on the document.
  EXPECT_TRUE(result.scenario.parameter_declarations.empty());
  EXPECT_EQ(written(result.scenario), written(parsed(written(result.scenario)).scenario));
}

// ★ TWO unmodeled children, not one. Every preserved-tier test in the p8-s1
// suite had exactly one, which is the hole a sabotage found there (#245 PR-C) —
// a reader that keeps the LAST fragment it sees passes them all.
TEST(XoscReader, ACatalogReferenceRouteAndItsSiblingsAreBothPreserved) {
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <CatalogReference catalogName="Routes" entryName="R1"/>
                <FutureRouteThing note="two"/>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  ASSERT_EQ(ego_route(result.scenario), nullptr) << "a catalog reference became an inline route";

  const std::string text = written(result.scenario);
  EXPECT_TRUE(contains(text, R"(<CatalogReference catalogName="Routes" entryName="R1")")) << text;
  EXPECT_TRUE(contains(text, "<FutureRouteThing")) << text;
  EXPECT_FALSE(contains(text, "<Route ")) << "an empty <Route> was invented\n" << text;
  EXPECT_EQ(text, written(parsed(text).scenario));
}

// A <RoutingAction> arm this version does not model preserves the WHOLE action,
// rather than writing back an empty <RoutingAction> the file never had.
TEST(XoscReader, AnUnmodeledRoutingArmPreservesTheWholeAction) {
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <FollowTrajectoryAction/>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  EXPECT_TRUE(any_message_contains(result.diagnostics, "FollowTrajectoryAction"));

  const std::string text = written(result.scenario);
  EXPECT_TRUE(contains(text, "<FollowTrajectoryAction")) << text;
  EXPECT_EQ(text, written(parsed(text).scenario));
}

// A waypoint whose position is one of the eight types this version does not
// model preserves the WHOLE waypoint. Dropping it would shorten the route —
// which is a different route, not a lossy copy of the same one.
TEST(XoscReader, AWaypointWithAnUnmodeledPositionIsPreservedRatherThanDropped) {
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <Route name="EgoRoute" closed="false">
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="1" laneId="-1" s="5" offset="0"/></Position>
                  </Waypoint>
                  <Waypoint routeStrategy="shortest">
                    <Position><RelativeLanePosition entityRef="Ego" dLane="1" ds="20"/></Position>
                  </Waypoint>
                </Route>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  EXPECT_TRUE(any_message_contains(result.diagnostics, "RelativeLanePosition"));

  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->waypoints.size(), 1U) << "the unmodeled waypoint became a modeled one";

  const std::string text = written(result.scenario);
  EXPECT_TRUE(contains(text, "<RelativeLanePosition")) << text;
  EXPECT_EQ(text, written(parsed(text).scenario));
}

// A one-waypoint route is REPORTED and kept as read. Padding it would invent an
// end the file never named; dropping it would lose content. validate_scenario
// refuses it at save time, which is where the refusal belongs.
TEST(XoscReader, AOneWaypointRouteIsReportedAndKept) {
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <Route name="Stub" closed="false">
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="1" laneId="-1" s="5" offset="0"/></Position>
                  </Waypoint>
                </Route>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  EXPECT_TRUE(any_message_contains(result.diagnostics, "at least two"));
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->waypoints.size(), 1U);
  EXPECT_FALSE(osc::write_xosc(result.scenario).has_value())
      << "the writer accepted a one-waypoint route";
}

// The reader does not depend on the XSD's child order. Refusing document order
// a schema validator would catch is not a reader's job, and it would cost the
// round trip a file it could have kept.
TEST(XoscReader, AWaypointBeforeTheParameterDeclarationsIsStillRead) {
  const std::string document = with_extra_private_action(R"(          <PrivateAction>
            <RoutingAction>
              <AssignRouteAction>
                <Route name="EgoRoute" closed="false">
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="1" laneId="-1" s="5" offset="0"/></Position>
                  </Waypoint>
                  <ParameterDeclarations>
                    <ParameterDeclaration name="Speed" parameterType="double" value="13.9"/>
                  </ParameterDeclarations>
                  <Waypoint routeStrategy="shortest">
                    <Position><LanePosition roadId="2" laneId="-1" s="40" offset="0"/></Position>
                  </Waypoint>
                </Route>
              </AssignRouteAction>
            </RoutingAction>
          </PrivateAction>
)");
  const osc::XoscParseResult result = parsed(document);
  const osc::Route* route = ego_route(result.scenario);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->waypoints.size(), 2U);
  EXPECT_EQ(route->parameter_declarations.size(), 1U);
}

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
