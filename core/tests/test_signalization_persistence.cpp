// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Persistence of signal controllers and of the junction's signalization
// records (p4-s7, issue #228).
//
// Layer 0 (ADR-0008) carries almost all of it. ASAM OpenDRIVE 1.9.0 §14.6
// defines a top-level `<controller>` (Table 128) with `<control>` children
// (Table 129), and §12.14 defines `<junction><controller>` (Table 84) as a
// REFERENCE into a signal synchronization group. Both were previously lost:
// the root element was warned about and dropped, the junction child dropped
// without even a warning.
//
// Layer 1 adds only what ASAM cannot say: WHICH template produced the signals
// (`rm:signal`) and WHICH props represent each one (`rm:signalmount`, the #323
// assemblies extension point). Both follow the established rm:* discipline —
// omit at defaults, drop stale, emit no element when empty, all-or-nothing on
// the known grammar with ONE structured warning, warn-and-skip an unknown
// field key, and never write what the reader would refuse.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::Junction;
using roadmaker::JunctionController;
using roadmaker::JunctionId;
using roadmaker::kMaxSignalMountParts;
using roadmaker::LaneProfile;
using roadmaker::Object;
using roadmaker::ObjectType;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Signal;
using roadmaker::Signalization;
using roadmaker::SignalMount;
using roadmaker::SpanArm;
using roadmaker::Waypoint;

namespace {

/// A generated four-arm cross junction over roads "1".."4"; the junction lands
/// with odr_id "1" and a full rm:arms list (the p4-s6 fixture).
struct CrossFixture {
  RoadNetwork network;
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

  std::array<RoadEnd, 4> arms{};

  /// Places one dynamic signal on the west arm and returns its odr id.
  std::string place_signal(const char* odr_id) {
    network.add_signal(arms[0].road,
                       Signal{.odr_id = odr_id,
                              .s = 25.0,
                              .t = -6.0,
                              .dynamic = true,
                              .type = "1000001",
                              .subtype = "-1",
                              .country = "OpenDRIVE"});
    return odr_id;
  }

  /// Places one prop object on the west arm and returns its odr id.
  std::string place_prop(const char* odr_id) {
    network.add_object(
        arms[0].road,
        Object{.odr_id = odr_id, .type = ObjectType::Pole, .s = 25.0, .t = -6.0, .height = 3.0});
    return odr_id;
  }
};

std::string write(const RoadNetwork& network) {
  const auto xml = roadmaker::write_xodr(network, "signals");
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

std::string
with_user_data_value(const std::string& xml, std::string_view code, const std::string& value) {
  const std::string key = "code=\"" + std::string(code) + "\" value=\"";
  const std::size_t at = xml.find(key);
  EXPECT_NE(at, std::string::npos);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  return xml.substr(0, begin) + value + xml.substr(end);
}

std::size_t count_warnings_mentioning(const std::vector<roadmaker::Diagnostic>& diagnostics,
                                      std::string_view needle) {
  std::size_t total = 0;
  for (const roadmaker::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Warning &&
        diagnostic.message.find(needle) != std::string::npos) {
      ++total;
    }
  }
  return total;
}

std::size_t count_findings_with_rule(const std::vector<roadmaker::Diagnostic>& findings,
                                     std::string_view rule) {
  return static_cast<std::size_t>(
      std::count_if(findings.begin(), findings.end(), [&](const roadmaker::Diagnostic& finding) {
        return finding.rule_id == rule;
      }));
}

} // namespace

// --- Layer 0: <controller> -------------------------------------------------

TEST(SignalizationPersistence, UnsignalizedJunctionEmitsNothingNew) {
  // The acceptance-critical byte-identity case: every new emission path must
  // produce NOTHING when empty, or every pre-existing fixture breaks at once.
  CrossFixture fixture;
  const std::string xml = write(fixture.network);
  EXPECT_EQ(xml.find("<controller"), std::string::npos);
  EXPECT_EQ(xml.find("rm:signal"), std::string::npos);
  EXPECT_EQ(xml.find("rm:signalmount"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->network.controller_count(), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_TRUE(round.junction_controllers.empty());
  EXPECT_TRUE(round.signalization.tmpl.empty());
  EXPECT_TRUE(round.signal_mounts.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalizationPersistence, TopLevelControllerRoundTripsWithEveryAttribute) {
  CrossFixture fixture;
  const std::string signal = fixture.place_signal("7");
  fixture.network.add_controller(
      Controller{.odr_id = "42",
                 .name = "north-south through",
                 .sequence = 3U,
                 .controls = {Control{.signal_odr_id = signal, .type = "green"},
                              Control{.signal_odr_id = signal}}});

  const std::string xml = write(fixture.network);
  EXPECT_NE(xml.find("<controller id=\"42\" name=\"north-south through\" sequence=\"3\">"),
            std::string::npos)
      << xml;
  EXPECT_NE(xml.find("<control signalId=\"7\" type=\"green\" />"), std::string::npos) << xml;

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  ASSERT_EQ(reparsed->network.controller_count(), 1U);
  reparsed->network.for_each_controller([&](ControllerId, const Controller& controller) {
    EXPECT_EQ(controller.odr_id, "42");
    EXPECT_EQ(controller.name, "north-south through");
    EXPECT_EQ(controller.sequence, std::optional<unsigned>(3U));
    ASSERT_EQ(controller.controls.size(), 2U);
    EXPECT_EQ(controller.controls[0].signal_odr_id, "7");
    EXPECT_EQ(controller.controls[0].type, "green");
    EXPECT_TRUE(controller.controls[1].type.empty());
  });
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalizationPersistence, ThirdPartyControllerIsNoLongerDropped) {
  // Before p4-s7 a top-level <controller> was warned about as an unsupported
  // root child and then silently lost. Unknown attributes and unmodeled
  // children ride the preserved tier, so a foreign file survives verbatim.
  const std::string source = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="9" name="foreign" vendor="other" />
  <controller id="42" name="grp" sequence="1" vendorFlag="x">
    <control signalId="99" type="stop" />
    <userData code="other:thing" value="kept" />
  </controller>
</OpenDRIVE>
)";
  auto parsed = roadmaker::parse_xodr(source, "foreign");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(parsed->diagnostics, "unsupported"), 0U);
  ASSERT_EQ(parsed->network.controller_count(), 1U);

  const std::string xml = write(parsed->network);
  EXPECT_NE(xml.find("vendorFlag=\"x\""), std::string::npos) << xml;
  EXPECT_NE(xml.find("other:thing"), std::string::npos) << xml;
  EXPECT_NE(xml.find("signalId=\"99\""), std::string::npos) << xml;

  auto again = roadmaker::parse_xodr(xml, "foreign");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(write(again->network), xml);
}

TEST(SignalizationPersistence, ControllersAreWrittenBetweenTheRoadsAndTheJunctions) {
  // VERIFIED document order (1.9.0 §6.5, Table 12): <road> … <controller> …
  // <junction>. Getting this wrong is exactly what the esmini round-trip smoke
  // check would catch, so it is asserted here instead.
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.network.add_controller(
      Controller{.odr_id = "42", .controls = {Control{.signal_odr_id = "7"}}});

  const std::string xml = write(fixture.network);
  const std::size_t last_road = xml.rfind("<road ");
  const std::size_t controller = xml.find("<controller ");
  const std::size_t first_junction = xml.find("<junction ");
  ASSERT_NE(last_road, std::string::npos);
  ASSERT_NE(controller, std::string::npos);
  ASSERT_NE(first_junction, std::string::npos);
  EXPECT_LT(last_road, controller);
  EXPECT_LT(controller, first_junction);
}

TEST(SignalizationPersistence, MalformedSequenceIsPreservedRatherThanInvented) {
  const std::string source = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="9" name="foreign" vendor="other" />
  <controller id="42" sequence="00x">
    <control signalId="99" />
  </controller>
</OpenDRIVE>
)";
  auto parsed = roadmaker::parse_xodr(source, "foreign");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(parsed->diagnostics, "nonNegativeInteger"), 1U);
  const std::string xml = write(parsed->network);
  EXPECT_NE(xml.find("sequence=\"00x\""), std::string::npos) << xml;

  auto again = roadmaker::parse_xodr(xml, "foreign");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(write(again->network), xml);
}

// --- Layer 0: <junction><controller> ---------------------------------------

TEST(SignalizationPersistence, JunctionSyncGroupRoundTrips) {
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.network.add_controller(
      Controller{.odr_id = "42", .controls = {Control{.signal_odr_id = "7"}}});
  fixture.network.junction(fixture.junction)->junction_controllers = {
      JunctionController{.controller_odr_id = "42", .sequence = 1U, .type = "sync"},
      JunctionController{.controller_odr_id = "43"}};

  const std::string xml = write(fixture.network);
  EXPECT_NE(xml.find("<controller id=\"42\" sequence=\"1\" type=\"sync\" />"), std::string::npos)
      << xml;
  // The sync group is a normative child: it precedes <userData> and the derived
  // geometry (1.9.0 §6.5, Table 12).
  const std::size_t junction_at = xml.find("<junction ");
  ASSERT_NE(junction_at, std::string::npos);
  const std::size_t group_at = xml.find("<controller id=\"42\"", junction_at);
  const std::size_t user_data_at = xml.find("<userData", junction_at);
  ASSERT_NE(group_at, std::string::npos);
  ASSERT_NE(user_data_at, std::string::npos);
  EXPECT_LT(group_at, user_data_at);

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.junction_controllers.size(), 2U);
  EXPECT_EQ(round.junction_controllers[0].controller_odr_id, "42");
  EXPECT_EQ(round.junction_controllers[0].sequence, std::optional<unsigned>(1U));
  EXPECT_EQ(round.junction_controllers[0].type, "sync");
  EXPECT_EQ(round.junction_controllers[1].controller_odr_id, "43");
  EXPECT_FALSE(round.junction_controllers[1].sequence.has_value());
  EXPECT_EQ(write(reparsed->network), xml);
}

// --- Layer 1: rm:signal ----------------------------------------------------

TEST(SignalizationPersistence, SignalTemplateRecordRoundTripsEverySpelling) {
  for (const std::string_view tmpl : roadmaker::kSignalizationTemplates) {
    CrossFixture fixture;
    fixture.network.junction(fixture.junction)->signalization =
        Signalization{.tmpl = std::string(tmpl)};

    const std::string xml = write(fixture.network);
    EXPECT_EQ(user_data_value(xml, "rm:signal"),
              std::optional<std::string>("template=" + std::string(tmpl)));

    auto reparsed = roadmaker::parse_xodr(xml, "signals");
    ASSERT_TRUE(reparsed.has_value()) << tmpl;
    const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
    EXPECT_EQ(round.signalization.tmpl, tmpl);
    EXPECT_TRUE(round.signalization.mount_model.empty());
    EXPECT_EQ(write(reparsed->network), xml) << tmpl;
  }
}

TEST(SignalizationPersistence, SignalTemplateRecordCarriesTheMountModel) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->signalization =
      Signalization{.tmpl = "two_phase", .mount_model = "signal_light"};

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:signal"),
            std::optional<std::string>("template=two_phase:mount=signal_light"));

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_EQ(round.signalization.mount_model, "signal_light");
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalizationPersistence, MalformedSignalRecordWarnsOnceAndDropsTheWholeValue) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->signalization = Signalization{.tmpl = "two_phase"};
  const std::string xml = write(fixture.network);

  for (const std::string& bad : {std::string("template=diagonal"),
                                 std::string("template=two_phase:template=all_way_stop"),
                                 std::string("mount=signal_light"),
                                 std::string("template=two_phase:mount=a b"),
                                 std::string("")}) {
    auto reparsed = roadmaker::parse_xodr(with_user_data_value(xml, "rm:signal", bad), "signals");
    ASSERT_TRUE(reparsed.has_value()) << bad;
    EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:signal userData"), 1U)
        << bad;
    EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))
                    ->signalization.tmpl.empty())
        << bad;
  }
}

TEST(SignalizationPersistence, UnknownSignalFieldKeyWarnsButKeepsTheRecord) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->signalization = Signalization{.tmpl = "two_phase"};
  const std::string xml = write(fixture.network);

  auto reparsed = roadmaker::parse_xodr(
      with_user_data_value(xml, "rm:signal", "template=two_phase:cycle=60"), "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "'cycle=60' is not understood"), 1U);
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:signal userData"), 0U);
  EXPECT_EQ(reparsed->network.junction(reparsed->network.find_junction("1"))->signalization.tmpl,
            "two_phase");
}

// --- Layer 1: rm:signalmount -----------------------------------------------

TEST(SignalizationPersistence, MountRecordRoundTripsAListPerSignal) {
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.place_prop("70");
  fixture.place_prop("71");
  fixture.network.junction(fixture.junction)->signal_mounts = {
      SignalMount{.signal_odr_id = "7", .object_odr_ids = {"70", "71"}}};

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:signalmount"), std::optional<std::string>("7=70,71"));

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.signal_mounts.size(), 1U);
  EXPECT_EQ(round.signal_mounts[0].signal_odr_id, "7");
  EXPECT_EQ(round.signal_mounts[0].object_odr_ids, (std::vector<std::string>{"70", "71"}));
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SignalizationPersistence, StaleMountEntriesAreNotWritten) {
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.place_prop("70");
  fixture.network.junction(fixture.junction)->signal_mounts = {
      SignalMount{.signal_odr_id = "7", .object_odr_ids = {"70", "gone"}},
      SignalMount{.signal_odr_id = "missing", .object_odr_ids = {"70"}},
      SignalMount{.signal_odr_id = "7b", .object_odr_ids = {}}};

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:signalmount"), std::optional<std::string>("7=70"));

  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:signalmount"), 0U);
}

TEST(SignalizationPersistence, OverLongMountListIsTruncatedRatherThanEmitted) {
  // Never write what the reader would refuse: a hand-built network with more
  // parts than kMaxSignalMountParts is truncated, not emitted whole.
  CrossFixture fixture;
  fixture.place_signal("7");
  SignalMount mount{.signal_odr_id = "7"};
  for (std::size_t i = 0; i < kMaxSignalMountParts + 5; ++i) {
    mount.object_odr_ids.push_back(fixture.place_prop(("p" + std::to_string(i)).c_str()));
  }
  fixture.network.junction(fixture.junction)->signal_mounts = {mount};

  const std::string xml = write(fixture.network);
  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:signalmount"), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.signal_mounts.size(), 1U);
  EXPECT_EQ(round.signal_mounts[0].object_odr_ids.size(), kMaxSignalMountParts);
}

TEST(SignalizationPersistence, MalformedMountValueWarnsOnceAndDropsTheWholeValue) {
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.place_prop("70");
  fixture.network.junction(fixture.junction)->signal_mounts = {
      SignalMount{.signal_odr_id = "7", .object_odr_ids = {"70"}}};
  const std::string xml = write(fixture.network);

  std::string long_list = "7=";
  for (std::size_t i = 0; i <= kMaxSignalMountParts; ++i) {
    if (i != 0) {
      long_list += ',';
    }
    long_list += "p" + std::to_string(i);
  }

  for (const std::string& bad : {std::string("7"),
                                 std::string("7="),
                                 std::string("=70"),
                                 std::string("7=70;7=71"),
                                 std::string("7=70,"),
                                 std::string("7=7 0"),
                                 long_list}) {
    auto reparsed =
        roadmaker::parse_xodr(with_user_data_value(xml, "rm:signalmount", bad), "signals");
    ASSERT_TRUE(reparsed.has_value()) << bad;
    EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:signalmount userData"),
              1U)
        << bad;
    EXPECT_TRUE(
        reparsed->network.junction(reparsed->network.find_junction("1"))->signal_mounts.empty())
        << bad;
  }
}

// --- validator -------------------------------------------------------------

TEST(SignalizationPersistence, ValidatorReportsControllerDefects) {
  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.network.add_controller(Controller{.odr_id = "42"}); // no <control>
  fixture.network.add_controller(
      Controller{.odr_id = "42", .controls = {Control{.signal_odr_id = "7"}}}); // duplicate id
  fixture.network.add_controller(
      Controller{.odr_id = "43", .controls = {Control{.signal_odr_id = "nope"}}}); // dangling

  const std::vector<roadmaker::Diagnostic> findings =
      roadmaker::validate_network(fixture.network, {});
  EXPECT_EQ(count_findings_with_rule(findings, roadmaker::rules::kControllerValidForSignals), 1U);
  EXPECT_EQ(static_cast<std::size_t>(std::count_if(
                findings.begin(),
                findings.end(),
                [](const roadmaker::Diagnostic& finding) {
                  return finding.message.find("duplicate controller id") != std::string::npos;
                })),
            1U);
  // A dangling <control> has NO normative rule id — the spec defines none, and
  // the project never invents one.
  const auto dangling =
      std::find_if(findings.begin(), findings.end(), [](const roadmaker::Diagnostic& finding) {
        return finding.message.find("which does not exist") != std::string::npos;
      });
  ASSERT_NE(dangling, findings.end());
  EXPECT_TRUE(dangling->rule_id.empty());
}

TEST(SignalizationPersistence, ValidatorReportsAVirtualJunctionWithControllers) {
  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}};
  const RoadId road = *roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", "1");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;
  network.junction(junction)->junction_controllers = {
      JunctionController{.controller_odr_id = "42"}};

  EXPECT_EQ(count_findings_with_rule(roadmaker::validate_network(network, {}),
                                     roadmaker::rules::kVirtualNoControllers),
            1U);

  // Foreign input is written back verbatim, never silently stripped.
  const std::string xml = write(network);
  EXPECT_NE(xml.find("<controller id=\"42\" />"), std::string::npos) << xml;
  auto reparsed = roadmaker::parse_xodr(xml, "signals");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(write(reparsed->network), xml);
}

// --- fuzz corpus -----------------------------------------------------------

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed p4-s7 seed with
// --gtest_also_run_disabled_tests --gtest_filter='*Signalization*WriteCorpusSeed'.
// The malformed companions (bad_signalmount_dup.xodr, bad_signal_template.xodr)
// are hand-derived from it and are NOT regenerated.
TEST(SignalizationPersistence, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  CrossFixture fixture;
  fixture.place_signal("7");
  fixture.place_prop("70");
  fixture.place_prop("71");
  fixture.network.add_controller(
      Controller{.odr_id = "42",
                 .name = "east-west",
                 .sequence = 1U,
                 .controls = {Control{.signal_odr_id = "7", .type = "green"}}});
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.junction_controllers = {
      JunctionController{.controller_odr_id = "42", .sequence = 1U, .type = "sync"}};
  junction.signalization = Signalization{.tmpl = "protected_left", .mount_model = "signal_light"};
  junction.signal_mounts = {SignalMount{.signal_odr_id = "7", .object_odr_ids = {"70", "71"}}};

  ASSERT_TRUE(
      roadmaker::save_xodr(fixture.network, corpus / "junction_signals.xodr", "junction_signals")
          .has_value());
}

TEST(SignalizationPersistence, TheFuzzCorpusSeedsParseAndReExport) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto seed = roadmaker::load_xodr(corpus / "junction_signals.xodr");
  ASSERT_TRUE(seed.has_value()) << "junction_signals.xodr must parse";
  EXPECT_TRUE(seed->diagnostics.empty());
  ASSERT_EQ(seed->network.controller_count(), 1U);
  seed->network.for_each_controller([](ControllerId, const Controller& controller) {
    EXPECT_EQ(controller.odr_id, "42");
    EXPECT_EQ(controller.sequence, std::optional<unsigned>(1U));
    ASSERT_EQ(controller.controls.size(), 1U);
    EXPECT_EQ(controller.controls[0].signal_odr_id, "7");
  });
  const Junction& junction = *seed->network.junction(seed->network.find_junction("1"));
  ASSERT_EQ(junction.junction_controllers.size(), 1U);
  EXPECT_EQ(junction.junction_controllers[0].controller_odr_id, "42");
  EXPECT_EQ(junction.signalization.tmpl, "protected_left");
  EXPECT_EQ(junction.signalization.mount_model, "signal_light");
  ASSERT_EQ(junction.signal_mounts.size(), 1U);
  EXPECT_EQ(junction.signal_mounts[0].object_odr_ids, (std::vector<std::string>{"70", "71"}));
  EXPECT_FALSE(write(seed->network).empty());

  // The hand-derived malformed companions degrade with a warning; none fails
  // the load and none crashes.
  for (const char* name : {"bad_signal_template.xodr", "bad_signalmount_dup.xodr"}) {
    auto bad = roadmaker::load_xodr(corpus / name);
    ASSERT_TRUE(bad.has_value()) << name;
    EXPECT_FALSE(bad->diagnostics.empty()) << name;
    EXPECT_FALSE(write(bad->network).empty()) << name;
  }
}
