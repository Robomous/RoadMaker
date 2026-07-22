// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Persistence and validation of the junction signal CYCLE (p4-s8, issue #229).
//
// Phases are Layer 1 (ADR-0008): OpenDRIVE §14.6 deliberately excludes signal
// timing ("dynamic content like the signal cycle itself is specified outside of
// this standard, for example, in ASAM OpenSCENARIO"), so the cycle rides
// <userData code="rm:phases"> and a foreign reader loses nothing by ignoring
// it. The record follows the established rm:* discipline: emit nothing at
// defaults (empty cycle ⇒ no element ⇒ byte-identical pre-feature re-export),
// all-or-nothing on the KNOWN grammar with ONE structured warning, warn-and-skip
// an unknown field key, prune dormant/Red state at write, and never write what
// the reader would refuse.
//
// The validator advises (Warning) on every way the writer prunes: a span
// junction with a cycle, a cycle with no live controller, a state naming a
// controller outside the sync group, an out-of-range duration, and a
// past-bound phase list. RoadMaker-only findings carry an EMPTY rule_id (the
// dangling-<control> precedent); only the span case reuses the §12.7 rule.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::Diagnostic;
using roadmaker::Junction;
using roadmaker::JunctionController;
using roadmaker::JunctionId;
using roadmaker::kMaxSignalPhaseDuration;
using roadmaker::kMaxSignalPhases;
using roadmaker::LaneProfile;
using roadmaker::PhaseState;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Signal;
using roadmaker::SignalPhase;
using roadmaker::SignalState;
using roadmaker::SpanArm;
using roadmaker::Waypoint;

namespace {

/// A generated four-arm cross junction over roads "1".."4"; the junction lands
/// with odr_id "1" (the p4-s7 signalization fixture).
struct CrossFixture {
  RoadNetwork network;
  std::array<RoadEnd, 4> arms{};
  JunctionId junction;

  CrossFixture() {
    const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
      return *roadmaker::author_clothoid_road(
          network, waypoints, LaneProfile::two_lane_default(), "", id);
    };
    const RoadId west = arm({Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
    const RoadId east = arm({Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
    const RoadId south = arm({Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
    const RoadId north = arm({Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, "4");
    arms = {RoadEnd{.road = west, .contact = ContactPoint::End},
            RoadEnd{.road = east, .contact = ContactPoint::End},
            RoadEnd{.road = south, .contact = ContactPoint::End},
            RoadEnd{.road = north, .contact = ContactPoint::End}};
    EXPECT_TRUE(roadmaker::edit::create_junction(network, arms)->apply(network).has_value());
    junction = network.find_junction("1");
    EXPECT_TRUE(junction.is_valid());
  }

  /// Wires two live signal groups: signals "s1"/"s2" on the west/south arms,
  /// top-level controllers "c1"/"c2" that control them, and the junction's
  /// synchronization group referencing both. The controller ids are record
  /// tokens, so they survive round trips exactly.
  void wire_two_groups() {
    network.add_signal(arms[0].road,
                       Signal{.odr_id = "s1",
                              .s = 20.0,
                              .t = -6.0,
                              .dynamic = true,
                              .type = "1000001",
                              .subtype = "-1",
                              .country = "OpenDRIVE"});
    network.add_signal(arms[2].road,
                       Signal{.odr_id = "s2",
                              .s = 20.0,
                              .t = -6.0,
                              .dynamic = true,
                              .type = "1000001",
                              .subtype = "-1",
                              .country = "OpenDRIVE"});
    network.add_controller(
        Controller{.odr_id = "c1", .controls = {Control{.signal_odr_id = "s1"}}});
    network.add_controller(
        Controller{.odr_id = "c2", .controls = {Control{.signal_odr_id = "s2"}}});
    Junction& node = *network.junction(junction);
    node.junction_controllers = {JunctionController{.controller_odr_id = "c1"},
                                 JunctionController{.controller_odr_id = "c2"}};
  }

  Junction& node() { return *network.junction(junction); }
};

std::string write(const RoadNetwork& network) {
  const auto xml = roadmaker::write_xodr(network, "phases");
  EXPECT_TRUE(xml.has_value());
  return xml.has_value() ? *xml : std::string{};
}

/// The value= payload of the junction's userData element with `code`, or
/// nullopt when no such element was written.
std::optional<std::string> user_data_value(const std::string& xml, std::string_view code) {
  const std::string key = "code=\"" + std::string(code) + "\" value=\"";
  const std::size_t at = xml.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t begin = at + key.size();
  return xml.substr(begin, xml.find('"', begin) - begin);
}

std::size_t count_warnings_mentioning(const std::vector<Diagnostic>& diagnostics,
                                      std::string_view needle) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::Warning &&
               diagnostic.message.find(needle) != std::string::npos;
      }));
}

/// The findings validate_network reports for the sole junction whose message
/// contains `needle`.
std::vector<Diagnostic> findings_mentioning(const RoadNetwork& network, std::string_view needle) {
  std::vector<Diagnostic> hits;
  for (const Diagnostic& finding : roadmaker::validate_network(network)) {
    if (finding.message.find(needle) != std::string::npos) {
      hits.push_back(finding);
    }
  }
  return hits;
}

/// A four-phase two-axis cycle over the wired groups "c1"/"c2": axis green +
/// yellow clearance for each, matching the design's worked example. Durations
/// are all in band and every controller is live and in the sync group, so this
/// is a RoadMaker-written cycle that round-trips byte-identically.
std::vector<SignalPhase> two_axis_cycle() {
  return {
      SignalPhase{.name = "axis0",
                  .duration = 24.0,
                  .states = {PhaseState{.controller_odr_id = "c1", .state = SignalState::Green}}},
      SignalPhase{.name = "axis0_clear",
                  .duration = 3.0,
                  .states = {PhaseState{.controller_odr_id = "c1", .state = SignalState::Yellow}}},
      SignalPhase{.name = "axis1",
                  .duration = 20.0,
                  .states = {PhaseState{.controller_odr_id = "c2", .state = SignalState::Green}}},
      SignalPhase{.name = "axis1_clear",
                  .duration = 3.0,
                  .states = {PhaseState{.controller_odr_id = "c2", .state = SignalState::Yellow}}}};
}

} // namespace

// --- round trip --------------------------------------------------------------

TEST(SignalPhasePersistence, PreFeatureJunctionEmitsNoPhaseBytes) {
  // The acceptance-critical byte-identity case: a junction that stores no phases
  // must emit no rm:phases element, or every pre-feature fixture breaks at once.
  CrossFixture fixture;
  const std::string xml = write(fixture.network);
  EXPECT_EQ(xml.find("rm:phases"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->phases.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalPhasePersistence, AuthoredCycleRoundTripsByteIdentical) {
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = two_axis_cycle();

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:phases"),
            "name=axis0:dur=24:st=c1,g;name=axis0_clear:dur=3:st=c1,y;"
            "name=axis1:dur=20:st=c2,g;name=axis1_clear:dur=3:st=c2,y");

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.phases.size(), 4U);
  EXPECT_EQ(round.phases[0].name, "axis0");
  EXPECT_DOUBLE_EQ(round.phases[0].duration, 24.0);
  ASSERT_EQ(round.phases[0].states.size(), 1U);
  EXPECT_EQ(round.phases[0].states[0].controller_odr_id, "c1");
  EXPECT_EQ(round.phases[0].states[0].state, SignalState::Green);
  EXPECT_EQ(round.phases[1].states[0].state, SignalState::Yellow);

  // Save -> load -> save is byte-identical.
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalPhasePersistence, AllRedClearancePhaseHasNoStateField) {
  // An all-red phase names no controller: it is "name=..:dur=.." with no st.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = {
      SignalPhase{.name = "axis0",
                  .duration = 20.0,
                  .states = {PhaseState{.controller_odr_id = "c1", .state = SignalState::Green}}},
      SignalPhase{.name = "allred", .duration = 2.0, .states = {}}};

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:phases"), "name=axis0:dur=20:st=c1,g;name=allred:dur=2");

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.phases.size(), 2U);
  EXPECT_TRUE(round.phases[1].states.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalPhasePersistence, PhasesAreWrittenAfterSignalmountAndBeforeJunction) {
  // Document order of the junction userData siblings: … rm:signalmount,
  // rm:phases, rm:junction. Getting this wrong is what an esmini round trip
  // would catch, so it is asserted directly here.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.network.add_object(fixture.arms[0].road,
                             roadmaker::Object{.odr_id = "o1",
                                               .type = roadmaker::ObjectType::Pole,
                                               .s = 20.0,
                                               .t = -6.0,
                                               .height = 3.0});
  Junction& node = fixture.node();
  node.signalization = roadmaker::Signalization{.tmpl = "two_phase"};
  node.signal_mounts = {roadmaker::SignalMount{.signal_odr_id = "s1", .object_odr_ids = {"o1"}}};
  node.default_corner_radius = 4.0; // forces an rm:junction element
  node.phases = two_axis_cycle();

  const std::string xml = write(fixture.network);
  const std::size_t mount = xml.find("rm:signalmount");
  const std::size_t phases = xml.find("rm:phases");
  const std::size_t junction = xml.find("rm:junction");
  ASSERT_NE(mount, std::string::npos);
  ASSERT_NE(phases, std::string::npos);
  ASSERT_NE(junction, std::string::npos);
  EXPECT_LT(mount, phases);
  EXPECT_LT(phases, junction);
}

// --- malformed input (all-or-nothing on the known grammar) -------------------

// Each row hangs a malformed rm:phases value on an otherwise valid junction and
// asserts the whole cycle is dropped with exactly one structured warning. The
// value column exercises one rejection each from the D4 list.
struct MalformedCase {
  const char* label;
  const char* value;
};

class SignalPhaseMalformed : public testing::TestWithParam<MalformedCase> {};

TEST_P(SignalPhaseMalformed, DropsTheWholeCycleWithOneWarning) {
  CrossFixture fixture;
  fixture.wire_two_groups();
  // Author a valid cycle so the writer emits an rm:phases element, then rewrite
  // its value= to the malformed payload before re-reading it.
  fixture.node().phases = two_axis_cycle();
  std::string xml = write(fixture.network);
  const std::string key = "code=\"rm:phases\" value=\"";
  const std::size_t at = xml.find(key);
  ASSERT_NE(at, std::string::npos);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  xml = xml.substr(0, begin) + GetParam().value + xml.substr(end);

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value()) << GetParam().label;
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->phases.empty())
      << GetParam().label;
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:phases userData"), 1U)
      << GetParam().label;
}

INSTANTIATE_TEST_SUITE_P(Cases,
                         SignalPhaseMalformed,
                         testing::Values(MalformedCase{"empty value", ""},
                                         MalformedCase{"duplicate name", "name=a:name=b:dur=5"},
                                         MalformedCase{"duplicate dur", "dur=5:dur=6"},
                                         MalformedCase{"duplicate st", "dur=5:st=c1,g:st=c2,g"},
                                         MalformedCase{"missing dur", "name=a:st=c1,g"},
                                         MalformedCase{"non-finite dur", "dur=inf"},
                                         MalformedCase{"non-positive dur", "dur=0"},
                                         MalformedCase{"negative dur", "dur=-4"},
                                         MalformedCase{"over-max dur", "dur=3601"},
                                         MalformedCase{"non-numeric dur", "dur=soon"},
                                         MalformedCase{"malformed name token", "name=a b:dur=5"},
                                         MalformedCase{"pair without comma", "dur=5:st=c1g"},
                                         MalformedCase{"bad state char", "dur=5:st=c1,x"},
                                         MalformedCase{"multi-char state", "dur=5:st=c1,gg"},
                                         MalformedCase{"non-token ctrl", "dur=5:st=c 1,g"},
                                         MalformedCase{"duplicate ctrl in entry",
                                                       "dur=5:st=c1,g|c1,y"},
                                         MalformedCase{"empty st field", "dur=5:st="},
                                         MalformedCase{"field without '='", "dur=5:garbage"}),
                         [](const testing::TestParamInfo<MalformedCase>& param_info) {
                           std::string name = param_info.param.label;
                           for (char& c : name) {
                             if (std::isalnum(static_cast<unsigned char>(c)) == 0) {
                               c = '_';
                             }
                           }
                           return name;
                         });

TEST(SignalPhasePersistence, OverPhaseCountLimitDropsTheWholeCycle) {
  // More than kMaxSignalPhases entries is longer than any writer would emit and
  // drops the whole value.
  std::string value;
  for (std::size_t i = 0; i <= kMaxSignalPhases; ++i) {
    if (!value.empty()) {
      value += ';';
    }
    value += "dur=2";
  }
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = two_axis_cycle();
  std::string xml = write(fixture.network);
  const std::string key = "code=\"rm:phases\" value=\"";
  const std::size_t at = xml.find(key);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  xml = xml.substr(0, begin) + value + xml.substr(end);

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->phases.empty());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:phases userData"), 1U);
}

TEST(SignalPhasePersistence, UnknownFieldKeyIsSkippedNotFatal) {
  // Forward compat (the rm:junction rule): an unknown "key=" field is warned
  // about and skipped, and the phase keeps its understood name+dur.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = two_axis_cycle();
  std::string xml = write(fixture.network);
  const std::string key = "code=\"rm:phases\" value=\"";
  const std::size_t at = xml.find(key);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  xml = xml.substr(0, begin) + "name=x:dur=5:foo=bar" + xml.substr(end);

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.phases.size(), 1U);
  EXPECT_EQ(round.phases[0].name, "x");
  EXPECT_DOUBLE_EQ(round.phases[0].duration, 5.0);
  EXPECT_TRUE(round.phases[0].states.empty());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "is not understood and was ignored"),
            1U);
}

// --- dormancy: load tolerantly, prune on write, validator speaks -------------

TEST(SignalPhasePersistence, DormantStateLoadsButIsPrunedOnWrite) {
  // A state naming a controller that is not a live member loads (dormant
  // tolerant), but the writer prunes the pair. The live pair survives.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = two_axis_cycle();
  std::string xml = write(fixture.network);
  const std::string key = "code=\"rm:phases\" value=\"";
  const std::size_t at = xml.find(key);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  xml = xml.substr(0, begin) + "name=p:dur=10:st=c1,g|ghost,g" + xml.substr(end);

  auto reparsed = roadmaker::parse_xodr(xml, "phases");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.phases.size(), 1U);
  ASSERT_EQ(round.phases[0].states.size(), 2U); // dormant reference loads intact
  EXPECT_EQ(round.phases[0].states[1].controller_odr_id, "ghost");

  // On write the dormant pair is gone, the live one remains.
  EXPECT_EQ(user_data_value(write(reparsed->network), "rm:phases"), "name=p:dur=10:st=c1,g");

  // And the validator reports the dormant reference (finding 3).
  const std::vector<Diagnostic> dormant =
      findings_mentioning(reparsed->network, "not in this junction's group");
  ASSERT_EQ(dormant.size(), 1U);
  EXPECT_NE(dormant[0].message.find("ghost"), std::string::npos);
  EXPECT_TRUE(dormant[0].rule_id.empty()); // RoadMaker-only advisory
}

TEST(SignalPhasePersistence, RedPairIsNeverWritten) {
  // Red is stored sparsely, so an explicit Red pair (only a foreign file carries
  // one) normalizes away on the next save.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = {
      SignalPhase{.name = "p",
                  .duration = 10.0,
                  .states = {PhaseState{.controller_odr_id = "c1", .state = SignalState::Green},
                             PhaseState{.controller_odr_id = "c2", .state = SignalState::Red}}}};
  EXPECT_EQ(user_data_value(write(fixture.network), "rm:phases"), "name=p:dur=10:st=c1,g");
}

// --- validator findings 1..5 -------------------------------------------------

TEST(SignalPhaseValidator, SpanJunctionWithPhasesReusesTheVirtualRule) {
  // Finding 1: a span (virtual) junction shall have no controllers, so a phase
  // cycle on one is reported with the §12.7 rule id.
  CrossFixture fixture;
  Junction& node = fixture.node();
  node.spans = {SpanArm{.road = fixture.arms[0].road, .s_start = 0.0, .s_end = 5.0}};
  node.phases = {SignalPhase{.name = "p", .duration = 5.0}};

  const std::vector<Diagnostic> hits = findings_mentioning(fixture.network, "signal phase cycle");
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits[0].severity, Severity::Warning);
  EXPECT_EQ(hits[0].rule_id, roadmaker::rules::kVirtualNoControllers);

  // A span junction never writes phase bytes.
  EXPECT_EQ(user_data_value(write(fixture.network), "rm:phases"), std::nullopt);
}

TEST(SignalPhaseValidator, PhasesWithNoLiveControllerDriveNothing) {
  // Finding 2: phases authored on a junction with no live member controller.
  CrossFixture fixture;
  fixture.node().phases = {SignalPhase{
      .name = "p",
      .duration = 5.0,
      .states = {PhaseState{.controller_odr_id = "gone", .state = SignalState::Green}}}};

  const std::vector<Diagnostic> hits =
      findings_mentioning(fixture.network, "the cycle drives nothing");
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_TRUE(hits[0].rule_id.empty());
}

TEST(SignalPhaseValidator, OutOfRangeDurationIsReported) {
  // Finding 4: a phase whose duration is out of band is not written.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = {SignalPhase{.name = "ok", .duration = 10.0},
                           SignalPhase{.name = "bad", .duration = kMaxSignalPhaseDuration + 1.0}};

  const std::vector<Diagnostic> hits =
      findings_mentioning(fixture.network, "out-of-range duration");
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_TRUE(hits[0].rule_id.empty());
}

TEST(SignalPhaseValidator, TooManyPhasesReportsTruncation) {
  // Finding 5: a phase list past kMaxSignalPhases is truncated on write.
  CrossFixture fixture;
  fixture.wire_two_groups();
  std::vector<SignalPhase> phases;
  for (std::size_t i = 0; i < kMaxSignalPhases + 3; ++i) {
    phases.push_back(SignalPhase{.name = "p", .duration = 2.0});
  }
  fixture.node().phases = std::move(phases);

  const std::vector<Diagnostic> hits = findings_mentioning(fixture.network, "only the first");
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_TRUE(hits[0].rule_id.empty());
}

TEST(SignalPhaseValidator, HealthyCycleHasNoFindings) {
  // A wired two-axis cycle over live in-group controllers is silent.
  CrossFixture fixture;
  fixture.wire_two_groups();
  fixture.node().phases = two_axis_cycle();

  EXPECT_TRUE(findings_mentioning(fixture.network, "phase").empty());
  EXPECT_TRUE(findings_mentioning(fixture.network, "cycle").empty());
}

// --- fuzz corpus -------------------------------------------------------------

TEST(SignalPhasePersistence, TheFuzzCorpusSeedsParseAndReExport) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto seed = roadmaker::load_xodr(corpus / "signal_phases_two_phase.xodr");
  ASSERT_TRUE(seed.has_value()) << "signal_phases_two_phase.xodr must parse";
  EXPECT_TRUE(seed->diagnostics.empty());
  const Junction& junction = *seed->network.junction(seed->network.find_junction("1"));
  ASSERT_EQ(junction.phases.size(), 4U);
  EXPECT_EQ(junction.phases[0].name, "axis0");
  EXPECT_FALSE(write(seed->network).empty());

  // The hand-derived malformed companions degrade with a warning; none fails
  // the load and none crashes.
  for (const char* name : {"bad_phases_dup_dur.xodr", "bad_phases_bad_state.xodr"}) {
    auto bad = roadmaker::load_xodr(corpus / name);
    ASSERT_TRUE(bad.has_value()) << name;
    EXPECT_EQ(count_warnings_mentioning(bad->diagnostics, "malformed rm:phases userData"), 1U)
        << name;
    EXPECT_TRUE(bad->network.junction(bad->network.find_junction("1"))->phases.empty()) << name;
    EXPECT_FALSE(write(bad->network).empty()) << name;
  }
}
