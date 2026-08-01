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

// The reader is included for the rule-catalogue gate at the bottom of this
// file only: three of the declared UIDs are cited by parse_xosc/load_xosc and
// by nothing else, so a gate that swept only the writer would report them as
// uncited. Nothing else here reads a document.
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
/// ★ THE RETURNED VIEW BORROWS `doc`, so `doc` MUST OUTLIVE IT. The rvalue
/// overload below is deleted to make that a compile error rather than a
/// use-after-free: `element_slice(written(scenario), ...)` slices a temporary
/// that dies at the end of the full expression, and the view left behind reads
/// freed memory. That exact mistake reached this file in p8-s2 (#246) and the
/// plain test run passed 3027/3027 with it live — only the sanitizer build
/// caught it. Bind the document to a named `std::string` first.
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

std::string_view element_slice(std::string&&, std::string_view, std::string_view) = delete;

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
  teleport.teleport = osc::TeleportAction{.position = osc::WorldPosition{.x = 0.0,
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

/// `minimal_scenario()` with a two-waypoint lane-anchored route assigned to
/// Ego — the shape the editor authors (p8-s3, #247).
osc::Scenario routed_scenario() {
  osc::Scenario scenario = minimal_scenario();
  osc::Route route;
  route.name = "EgoRoute";
  route.waypoints.push_back(
      osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                         .position = osc::LanePosition{.road_id = "1",
                                                       .lane_id = "-1",
                                                       .s = 5.0,
                                                       .offset = 0.0,
                                                       .orientation = std::nullopt,
                                                       .preserved = {}},
                         .preserved = {}});
  route.waypoints.push_back(
      osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                         .position = osc::LanePosition{.road_id = "2",
                                                       .lane_id = "-1",
                                                       .s = 40.0,
                                                       .offset = 0.0,
                                                       .orientation = std::nullopt,
                                                       .preserved = {}},
                         .preserved = {}});
  osc::PrivateAction routing;
  routing.routing = osc::RoutingAction{
      .assign_route = osc::AssignRouteAction{.route = route, .preserved = {}}, .preserved = {}};
  scenario.storyboard.init.actions.privates[0].actions.push_back(routing);
  return scenario;
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
  std::get<osc::WorldPosition>(
      scenario.storyboard.init.actions.privates[0].actions[0].teleport->position)
      .x = -0.0;

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
  // Modeled since p8-s4 (#248), and the smallest story the schema admits: an
  // act needs a maneuver group, a maneuver needs an event, an event needs an
  // action. validate_scenario refuses anything shorter, so this shape is not
  // padding — it is the minimum a <Story> can be.
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
  act.name = "a";
  act.maneuver_groups.push_back(group);
  osc::Story story;
  story.name = "s";
  story.acts.push_back(act);
  scenario.storyboard.stories.push_back(story);
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

// --- p8-s2 (#246): positions and initial speed --------------------------------

TEST(XoscWriter, ALanePositionIsEmittedInsideItsPositionWrapper) {
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = "7", .lane_id = "-2", .s = 42.5, .offset = 0.25};

  const std::string text = written(scenario);
  // Asserting inside the <Position> slice is what proves NESTING — a flat
  // find() would pass on a writer that emitted the lane position as a sibling.
  const std::string_view position = element_slice(text, "<Position>", "</Position>");
  ASSERT_FALSE(position.empty()) << text;
  EXPECT_TRUE(contains(position, R"(<LanePosition roadId="7" laneId="-2" s="42.5" offset="0.25")"))
      << position;
  EXPECT_FALSE(contains(text, "<WorldPosition")) << text;
}

TEST(XoscWriter, ARoadPositionIsEmittedInsideItsPositionWrapper) {
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::RoadPosition{.road_id = "3", .s = 40.0, .t = -1.75};

  // ★ The document is bound to a NAMED string first, deliberately.
  // `element_slice` returns a string_view INTO its argument, so slicing the
  // temporary `written()` returns leaves the view dangling the instant the full
  // expression ends. ASan caught exactly that here; the plain ctest run passed
  // 3027/3027 with the use-after-free live in it.
  const std::string text = written(scenario);
  const std::string_view position = element_slice(text, "<Position>", "</Position>");
  ASSERT_FALSE(position.empty());
  EXPECT_TRUE(contains(position, R"(<RoadPosition roadId="3" s="40" t="-1.75")")) << position;
}

TEST(XoscWriter, AnAbsentOrientationIsNotInvented) {
  // "Missing Orientation is interpreted as the relative reference context with
  // Heading=Pitch=Roll=0" — so writing one that the model does not carry adds
  // content the document never had, and breaks idempotency for every file that
  // omitted it.
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = "7", .lane_id = "-1", .s = 5.0, .offset = 0.0};

  EXPECT_FALSE(contains(written(scenario), "<Orientation"));
}

TEST(XoscWriter, EachSetArmOfAPrivateActionGetsItsOwnElement) {
  // ★ <PrivateAction> is a per-element CHOICE. Emitting a teleport and a
  // longitudinal action inside ONE <PrivateAction> produces a document no
  // parser accepts, however plausible the model that asked for it looks.
  osc::Scenario scenario = minimal_scenario();
  osc::PrivateAction& action = scenario.storyboard.init.actions.privates[0].actions[0];
  osc::SpeedAction speed;
  speed.absolute_target = osc::AbsoluteTargetSpeed{.value = 13.89, .preserved = {}};
  osc::LongitudinalAction longitudinal;
  longitudinal.speed = std::move(speed);
  action.longitudinal = std::move(longitudinal);

  const std::string text = written(scenario);
  std::size_t count = 0;
  for (std::size_t at = text.find("<PrivateAction>"); at != std::string::npos;
       at = text.find("<PrivateAction>", at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, 2U) << "the two arms shared one <PrivateAction> element:\n" << text;

  const std::string_view first = element_slice(text, "<PrivateAction>", "</PrivateAction>");
  EXPECT_TRUE(contains(first, "<TeleportAction>")) << first;
  EXPECT_FALSE(contains(first, "<LongitudinalAction>"))
      << "a longitudinal action rode inside the teleport's element";
}

TEST(XoscWriter, ASpeedActionCarriesBothItsRequiredChildren) {
  // <SpeedActionDynamics> and <SpeedActionTarget> are both 1..1. A writer that
  // emitted only the target produces a file that looks right and does not
  // validate.
  osc::Scenario scenario = minimal_scenario();
  osc::SpeedAction speed;
  speed.absolute_target = osc::AbsoluteTargetSpeed{.value = 13.89, .preserved = {}};
  osc::LongitudinalAction longitudinal;
  longitudinal.speed = std::move(speed);
  scenario.storyboard.init.actions.privates[0].actions[0].longitudinal = std::move(longitudinal);

  const std::string text = written(scenario);
  const std::string_view speed_slice = element_slice(text, "<SpeedAction>", "</SpeedAction>");
  ASSERT_FALSE(speed_slice.empty()) << text;
  EXPECT_TRUE(contains(speed_slice, R"(<SpeedActionDynamics dynamicsShape="step" value="0")"))
      << speed_slice;
  EXPECT_TRUE(contains(speed_slice, "<SpeedActionTarget>")) << speed_slice;
  EXPECT_TRUE(contains(speed_slice, R"(<AbsoluteTargetSpeed value="13.89")")) << speed_slice;
}

TEST(XoscWriter, ALanePositionWithNoRoadNetworkIsRefused) {
  // asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network — a
  // "shall not", and one of the few reference rules checkable in full, because
  // both ends are inside one document.
  osc::Scenario scenario = minimal_scenario();
  scenario.road_network.logic_file.reset();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = "7", .lane_id = "-1", .s = 5.0, .offset = 0.0};

  EXPECT_FALSE(osc::write_xosc(scenario).has_value())
      << "an actor was placed on a road network the document never links";
  EXPECT_FALSE(findings_with_rule(osc::validate_scenario(scenario),
                                  osc::rules::kInvalidElementsIfNoRoadNetwork)
                   .empty());
}

TEST(XoscWriter, AWorldPositionWithNoRoadNetworkIsFine) {
  // The counterpart. The rule names specific elements; a <WorldPosition> is not
  // one of them, and refusing it would reject a legal scenario.
  osc::Scenario scenario = minimal_scenario();
  scenario.road_network.logic_file.reset();

  EXPECT_TRUE(osc::write_xosc(scenario).has_value());
  EXPECT_TRUE(findings_with_rule(osc::validate_scenario(scenario),
                                 osc::rules::kInvalidElementsIfNoRoadNetwork)
                  .empty());
}

TEST(XoscWriter, ALanePositionWithNoIdsIsRefused) {
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = {}, .lane_id = {}, .s = 5.0, .offset = 0.0};

  EXPECT_FALSE(osc::write_xosc(scenario).has_value());
  EXPECT_EQ(
      findings_with_rule(osc::validate_scenario(scenario), osc::rules::kRoadLaneExists).size(), 2U)
      << "the empty roadId and the empty laneId were not both reported";
}

TEST(XoscWriter, ANegativeStationIsRefused) {
  osc::Scenario scenario = minimal_scenario();
  scenario.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = "7", .lane_id = "-1", .s = -0.5, .offset = 0.0};

  EXPECT_FALSE(osc::write_xosc(scenario).has_value());
  EXPECT_FALSE(
      findings_with_rule(osc::validate_scenario(scenario), osc::rules::kRoadLaneOffsetInBounds)
          .empty());
}

TEST(XoscWriter, ANegativeTargetSpeedIsRefused) {
  osc::Scenario scenario = minimal_scenario();
  osc::SpeedAction speed;
  speed.absolute_target = osc::AbsoluteTargetSpeed{.value = -30.0, .preserved = {}};
  osc::LongitudinalAction longitudinal;
  longitudinal.speed = std::move(speed);
  scenario.storyboard.init.actions.privates[0].actions[0].longitudinal = std::move(longitudinal);

  EXPECT_FALSE(osc::write_xosc(scenario).has_value());
}

// --- the rule catalogue ------------------------------------------------------

// --- routes (p8-s3, #247) -----------------------------------------------------

TEST(XoscWriter, ARouteIsEmittedInsideItsRoutingAction) {
  const std::string text = written(routed_scenario());
  const std::string_view routing = element_slice(text, "<RoutingAction>", "</RoutingAction>");
  ASSERT_FALSE(routing.empty()) << text;

  // NESTING, not mere presence: RoutingAction > AssignRouteAction > Route, each
  // sliced inside the last. A flat search would pass on siblings.
  const std::string routing_text{routing};
  const std::string_view assign =
      element_slice(routing_text, "<AssignRouteAction>", "</AssignRouteAction>");
  ASSERT_FALSE(assign.empty()) << routing;
  const std::string assign_text{assign};
  const std::string_view route = element_slice(assign_text, "<Route ", "</Route>");
  ASSERT_FALSE(route.empty()) << assign;

  EXPECT_TRUE(contains(route, R"(name="EgoRoute")")) << route;
  // @closed is REQUIRED with no default, so it is always emitted.
  EXPECT_TRUE(contains(route, R"(closed="false")")) << route;
}

TEST(XoscWriter, EachWaypointCarriesItsStrategyAndItsPositionWrapper) {
  const std::string text = written(routed_scenario());
  const std::string_view route = element_slice(text, "<Route ", "</Route>");
  ASSERT_FALSE(route.empty()) << text;
  const std::string route_text{route};

  const std::string_view first = element_slice(route_text, "<Waypoint ", "</Waypoint>");
  ASSERT_FALSE(first.empty()) << route;
  EXPECT_TRUE(contains(first, R"(routeStrategy="shortest")")) << first;
  // The <Position> wrapper is not optional between a waypoint and its position:
  // dropping it would emit a <LanePosition> the schema does not allow there.
  const std::string first_text{first};
  const std::string_view position = element_slice(first_text, "<Position>", "</Position>");
  ASSERT_FALSE(position.empty()) << first;
  EXPECT_TRUE(contains(position, R"(<LanePosition roadId="1" laneId="-1")")) << position;

  // Both waypoints are emitted, IN ORDER. The second one starts after the
  // first's close tag — searching from index 1 would re-find the first, which
  // is a comparison that passes on a document holding only one waypoint.
  const std::size_t first_start = route_text.find("<Waypoint ");
  ASSERT_NE(first_start, std::string::npos) << route;
  const std::size_t first_end = route_text.find("</Waypoint>", first_start);
  ASSERT_NE(first_end, std::string::npos) << route;
  const std::size_t second_start = route_text.find("<Waypoint ", first_end);
  ASSERT_NE(second_start, std::string::npos) << route << "\nonly one waypoint was emitted";
  EXPECT_LT(route_text.find(R"(roadId="1")"), second_start) << route;
  EXPECT_GT(route_text.find(R"(roadId="2")"), second_start) << route;
}

// The XSD sequence is ParameterDeclarations? then Waypoint{2,}. A route that
// declares parameters must emit them BEFORE its waypoints, or no schema-aware
// reader accepts the document.
TEST(XoscWriter, RouteParameterDeclarationsPrecedeTheWaypoints) {
  osc::Scenario scenario = routed_scenario();
  osc::Route& route =
      *scenario.storyboard.init.actions.privates[0].actions[1].routing->assign_route->route;
  route.parameter_declarations.push_back(osc::ParameterDeclaration{
      .name = "Speed", .parameter_type = "double", .value = "13.9", .preserved = {}});

  const std::string text = written(scenario);
  const std::string_view route_slice = element_slice(text, "<Route ", "</Route>");
  ASSERT_FALSE(route_slice.empty()) << text;
  const std::string route_text{route_slice};
  const std::size_t parameters = route_text.find("<ParameterDeclarations>");
  const std::size_t waypoint = route_text.find("<Waypoint ");
  ASSERT_NE(parameters, std::string::npos) << route_text;
  ASSERT_NE(waypoint, std::string::npos) << route_text;
  EXPECT_LT(parameters, waypoint) << route_text;
}

// ...and a route WITHOUT parameters emits no wrapper at all. The element is
// 0..1, and an empty one is a child the input did not have — which is how a
// round trip stops being a fixed point in bytes.
TEST(XoscWriter, ARouteWithNoParametersEmitsNoParameterDeclarations) {
  const std::string text = written(routed_scenario());
  const std::string_view route = element_slice(text, "<Route ", "</Route>");
  ASSERT_FALSE(route.empty()) << text;
  EXPECT_FALSE(contains(route, "<ParameterDeclarations")) << route;
}

// The routing arm gets its OWN <PrivateAction> element, like the other two: the
// schema's choice is per-element, so ONE action carrying all three arms becomes
// three elements rather than one invalid one.
//
// ★ THE THREE ARMS ARE ON A SINGLE `PrivateAction` HERE, DELIBERATELY. Building
// them as three separate actions would produce three elements no matter what
// the writer did with the arms, so the assertion would hold on a writer that
// nested routing inside the teleport's element — the exact defect it exists to
// catch. A both-arms action is also unreachable from parsing (each
// `<PrivateAction>` in a file holds exactly one child), so this writer test is
// its ONLY guard.
TEST(XoscWriter, TheRoutingArmGetsItsOwnPrivateActionElement) {
  osc::Scenario scenario = routed_scenario();
  osc::Private& ego = scenario.storyboard.init.actions.privates[0];
  osc::PrivateAction all_three = ego.actions[0]; // the teleport
  all_three.longitudinal = osc::LongitudinalAction{
      .speed = osc::SpeedAction{.dynamics = {},
                                .absolute_target =
                                    osc::AbsoluteTargetSpeed{.value = 10.0, .preserved = {}},
                                .target_preserved = {},
                                .preserved = {}},
      .preserved = {}};
  all_three.routing = ego.actions[1].routing;
  ego.actions.clear();
  ego.actions.push_back(all_three);

  const std::string text = written(scenario);
  const std::string_view init = element_slice(text, "<Private ", "</Private>");
  ASSERT_FALSE(init.empty()) << text;
  const std::string init_text{init};
  std::size_t count = 0;
  for (std::size_t at = init_text.find("<PrivateAction>"); at != std::string::npos;
       at = init_text.find("<PrivateAction>", at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, 3U) << init_text;
  // ...and the routing is not nested inside the teleport's element.
  const std::string_view first = element_slice(init_text, "<PrivateAction>", "</PrivateAction>");
  ASSERT_FALSE(first.empty()) << init_text;
  EXPECT_FALSE(contains(first, "<RoutingAction")) << first;
}

// ★ An <AssignRouteAction> whose content is a <CatalogReference> keeps it. The
// route arm is an OPTIONAL, and emitting an empty <Route> for an unset one
// would replace a legal catalog reference with an invalid element.
TEST(XoscWriter, AnAssignRouteActionWithNoInlineRouteEmitsNoRoute) {
  osc::Scenario scenario = minimal_scenario();
  osc::AssignRouteAction assign;
  assign.preserved.children.push_back(R"(<CatalogReference catalogName="Routes" entryName="R1"/>)");
  osc::PrivateAction routing;
  routing.routing = osc::RoutingAction{.assign_route = assign, .preserved = {}};
  scenario.storyboard.init.actions.privates[0].actions.push_back(routing);

  const std::string text = written(scenario);
  const std::string_view action =
      element_slice(text, "<AssignRouteAction>", "</AssignRouteAction>");
  ASSERT_FALSE(action.empty()) << text;
  EXPECT_FALSE(contains(action, "<Route")) << action;
  EXPECT_TRUE(contains(action, R"(<CatalogReference catalogName="Routes")")) << action;
}

TEST(XoscWriter, ARouteWithFewerThanTwoWaypointsIsRefused) {
  osc::Scenario scenario = routed_scenario();
  osc::Route& route =
      *scenario.storyboard.init.actions.privates[0].actions[1].routing->assign_route->route;
  route.waypoints.pop_back();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("at least two"), std::string::npos) << text.error().message;
}

TEST(XoscWriter, ARouteWithNoNameIsRefused) {
  osc::Scenario scenario = routed_scenario();
  scenario.storyboard.init.actions.privates[0]
      .actions[1]
      .routing->assign_route->route->name.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("no name"), std::string::npos) << text.error().message;
}

TEST(XoscWriter, TwoRoutesWithTheSameNameAreRefused) {
  osc::Scenario scenario = routed_scenario();
  // A second entity, routed under the SAME route name. Route names are siblings
  // once flattened, and a simulator resolves a route by name.
  osc::ScenarioObject other = scenario.entities.scenario_objects[0];
  other.name = "Other";
  scenario.entities.scenario_objects.push_back(other);
  osc::Private other_init;
  other_init.entity_ref = "Other";
  other_init.actions.push_back(scenario.storyboard.init.actions.privates[0].actions[1]);
  scenario.storyboard.init.actions.privates.push_back(other_init);

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("already declared"), std::string::npos)
      << text.error().message;
}

TEST(XoscWriter, AWaypointWithNoRouteStrategyIsRefused) {
  osc::Scenario scenario = routed_scenario();
  scenario.storyboard.init.actions.privates[0]
      .actions[1]
      .routing->assign_route->route->waypoints[0]
      .route_strategy.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("routeStrategy"), std::string::npos) << text.error().message;
}

// A waypoint's position is held to the SAME road-relative rules an init
// teleport is: the check is shared, so a route cannot smuggle past it a
// reference the placement layer would have refused.
TEST(XoscWriter, ARouteWaypointWithNoRoadIdIsRefused) {
  osc::Scenario scenario = routed_scenario();
  std::get<osc::LanePosition>(scenario.storyboard.init.actions.privates[0]
                                  .actions[1]
                                  .routing->assign_route->route->waypoints[0]
                                  .position)
      .road_id.clear();

  const auto text = osc::write_xosc(scenario);
  ASSERT_FALSE(text.has_value());
  EXPECT_NE(text.error().message.find("names no road"), std::string::npos) << text.error().message;
}

// A world-position waypoint is LEGAL and reported anyway: it does not say which
// lane the route takes where several meet, so the same file routes differently
// in two simulators. A warning, never a refusal.
TEST(XoscWriter, AWorldPositionWaypointWarnsWithoutBlockingTheWrite) {
  osc::Scenario scenario = routed_scenario();
  scenario.storyboard.init.actions.privates[0]
      .actions[1]
      .routing->assign_route->route->waypoints[0]
      .position = osc::WorldPosition{};

  const std::vector<Diagnostic> findings = osc::validate_scenario(scenario);
  const std::vector<Diagnostic> ambiguous =
      findings_with_rule(findings, osc::rules::kAmbiguousRouteWaypoints);
  ASSERT_EQ(ambiguous.size(), 1U) << findings.size();
  EXPECT_EQ(ambiguous[0].severity, Severity::Warning);
  EXPECT_TRUE(osc::write_xosc(scenario).has_value()) << "a warning must not block the write";
}

TEST(XoscWriter, WritingARouteIsDeterministic) {
  const osc::Scenario scenario = routed_scenario();
  EXPECT_EQ(written(scenario), written(scenario));
}

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
    {"kConditionDelayNonNegative", osc::rules::kConditionDelayNonNegative},
    {"kRoadNetworkReference", osc::rules::kRoadNetworkReference},
    {"kRoadNetworkAvailability", osc::rules::kRoadNetworkAvailability},
    {"kFileEnding", osc::rules::kFileEnding},
    {"kValidSchema", osc::rules::kValidSchema},
    // p8-s2 (#246).
    {"kInvalidElementsIfNoRoadNetwork", osc::rules::kInvalidElementsIfNoRoadNetwork},
    {"kRoadLaneExists", osc::rules::kRoadLaneExists},
    {"kRoadLaneOffsetInBounds", osc::rules::kRoadLaneOffsetInBounds},
    // p8-s3 (#247).
    {"kAmbiguousRouteWaypoints", osc::rules::kAmbiguousRouteWaypoints},
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
  //
  // ★ IT SWEEPS ALL THREE CITING ENTRY POINTS, which is why a writer test file
  // includes the reader. Five of the constants are cited by validate_scenario,
  // three only by parse_xosc/load_xosc (a scenario in memory knows neither its
  // file name nor its directory). A gate that swept only the validator would
  // report the reader-side rules as uncited and push someone to delete them.
  std::set<std::string> cited;
  const auto sweep = [&cited](const std::vector<Diagnostic>& findings) {
    for (const Diagnostic& finding : findings) {
      if (!finding.rule_id.empty()) {
        cited.insert(finding.rule_id);
      }
    }
  };

  // Reader-side: a document missing a required element, missing a LogicFile,
  // loaded from a path with the wrong extension and naming an absent network.
  const auto reader_result = osc::parse_xosc(
      R"(<OpenSCENARIO><FileHeader revMajor="1" revMinor="2"/><RoadNetwork/></OpenSCENARIO>)",
      "<rules>");
  ASSERT_TRUE(reader_result.has_value());
  sweep(reader_result->diagnostics);

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "xosc_rule_catalogue";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path scenario_path = directory / "scenario.xml"; // not .xosc
  {
    std::ofstream out(scenario_path, std::ios::binary);
    out << R"(<OpenSCENARIO><FileHeader revMajor="1" revMinor="2"/><CatalogLocations/>)"
           R"(<RoadNetwork><LogicFile filepath="absent.xodr"/></RoadNetwork>)"
           R"(<Entities/><Storyboard><Init/></Storyboard></OpenSCENARIO>)";
  }
  const auto loaded = osc::load_xosc(scenario_path);
  ASSERT_TRUE(loaded.has_value());
  sweep(loaded->diagnostics);
  std::filesystem::remove_all(directory);

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

  osc::Scenario negative_delay = minimal_scenario();
  negative_delay.storyboard.stop_trigger.condition_groups[0].conditions[0].delay = -1.0;
  provoking.push_back(negative_delay);

  // p8-s2 (#246): a lane position with no road network, no ids and a negative
  // station provokes all three of the new rules at once.
  osc::Scenario stranded_lane = minimal_scenario();
  stranded_lane.road_network.logic_file.reset();
  stranded_lane.storyboard.init.actions.privates[0].actions[0].teleport->position =
      osc::LanePosition{.road_id = {}, .lane_id = {}, .s = -1.0, .offset = 0.0};
  provoking.push_back(stranded_lane);

  // p8-s3 (#247): a route whose waypoints are world positions is the ambiguous
  // case the routing rule names.
  osc::Scenario ambiguous_route = minimal_scenario();
  {
    osc::Route route;
    route.name = "AmbiguousRoute";
    route.waypoints.push_back(
        osc::RouteWaypoint{.route_strategy = std::string(osc::kDefaultRouteStrategy),
                           .position = osc::WorldPosition{},
                           .preserved = {}});
    route.waypoints.push_back(route.waypoints.front());
    osc::PrivateAction routing;
    routing.routing = osc::RoutingAction{
        .assign_route = osc::AssignRouteAction{.route = route, .preserved = {}}, .preserved = {}};
    ambiguous_route.storyboard.init.actions.privates[0].actions.push_back(routing);
  }
  provoking.push_back(ambiguous_route);

  for (const osc::Scenario& scenario : provoking) {
    sweep(osc::validate_scenario(scenario));
  }

  for (const NamedRule& rule : kRules) {
    EXPECT_TRUE(cited.count(std::string(rule.uid)) != 0)
        << rule.name << " is declared but no finding cites it";
  }
}
