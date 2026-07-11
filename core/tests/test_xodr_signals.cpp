// OpenDRIVE <signals> (§14) — data model, parse, write, validate (M3a P1,
// docs/design/m3a/01_kernel_objects_signals.md). Round-trip fidelity is the
// gate: typed fields within rm::tol, preserved fragments byte-equal. Mirrors
// the as-built Phase 0 <objects> patterns (§9).

#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using roadmaker::ObjectOrientation;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
using roadmaker::SignalId;

namespace {

/// One straight 100 m road wrapping `signals_xml` in its <signals> element.
std::string document_with_signals(std::string_view signals_xml) {
  std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="signals-test"/>
  <road name="r" length="100" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="100"><line/></geometry>
    </planView>
    <lanes>
      <laneSection s="0">
        <center><lane id="0" type="none" level="false"/></center>
        <right>
          <lane id="-1" type="driving" level="false"><width sOffset="0" a="3.5" b="0" c="0" d="0"/></lane>
        </right>
      </laneSection>
    </lanes>
    <signals>
)";
  doc += signals_xml;
  doc += R"(
    </signals>
  </road>
</OpenDRIVE>
)";
  return doc;
}

roadmaker::XodrParseResult parse(std::string_view xml) {
  auto result = roadmaker::parse_xodr(xml, "test");
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

const roadmaker::Diagnostic* find_by_rule(const std::vector<roadmaker::Diagnostic>& diagnostics,
                                          std::string_view rule) {
  const auto it = std::ranges::find_if(
      diagnostics, [&](const roadmaker::Diagnostic& d) { return d.rule_id == rule; });
  return it == diagnostics.end() ? nullptr : &*it;
}

const Signal& only_signal(const RoadNetwork& network) {
  EXPECT_EQ(network.signal_count(), 1U);
  const Signal* found = nullptr;
  network.for_each_signal([&](SignalId, const Signal& signal) { found = &signal; });
  EXPECT_NE(found, nullptr);
  return *found;
}

} // namespace

// --- arena / network API -----------------------------------------------------

TEST(SignalArena, AddLookupErase) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");

  Signal value;
  value.odr_id = "42";
  value.type = "274";
  value.subtype = "50";
  value.s = 10.0;
  value.t = -5.0;
  const SignalId id = network.add_signal(road, value);
  ASSERT_TRUE(id.is_valid());
  ASSERT_NE(network.signal(id), nullptr);
  EXPECT_EQ(network.signal(id)->road, road);
  EXPECT_EQ(network.signal(id)->type, "274");
  EXPECT_EQ(signals_of(network, road).size(), 1U);

  EXPECT_TRUE(network.erase_signal(id));
  EXPECT_EQ(network.signal(id), nullptr); // handle is stale now
  EXPECT_FALSE(network.erase_signal(id));
  EXPECT_EQ(network.signal_count(), 0U);
}

TEST(SignalArena, AddToStaleRoadFails) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  ASSERT_TRUE(network.erase_road(road));
  EXPECT_FALSE(network.add_signal(road, Signal{}).is_valid());
}

TEST(SignalArena, EraseRoadCascadesSignals) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const RoadId other = network.create_road("o", "2");
  const SignalId doomed = network.add_signal(road, Signal{.odr_id = "1"});
  const SignalId kept = network.add_signal(other, Signal{.odr_id = "2"});

  ASSERT_TRUE(network.erase_road(road));
  EXPECT_EQ(network.signal(doomed), nullptr);
  ASSERT_NE(network.signal(kept), nullptr);
  EXPECT_EQ(network.signal_count(), 1U);
}

TEST(SignalArena, RestoreInPlaceKeepsHandle) {
  // The M2 restore-in-place contract: erase_exact + restore never bump the
  // generation, so ids captured by an edit command survive undo/redo.
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const SignalId id = network.add_signal(road, Signal{.odr_id = "7", .s = 3.0});

  const Signal snapshot = *network.signal(id);
  ASSERT_TRUE(network.erase_signal_exact(id).has_value());
  EXPECT_EQ(network.signal(id), nullptr);
  const auto restored = network.restore_signal(id, snapshot);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, id);
  ASSERT_NE(network.signal(id), nullptr);
  EXPECT_EQ(network.signal(id)->odr_id, "7");

  // A slot freed by plain erase refuses restore (generation bumped).
  ASSERT_TRUE(network.erase_signal(id));
  EXPECT_FALSE(network.restore_signal(id, snapshot).has_value());
}

// --- parse -------------------------------------------------------------------

TEST(XodrSignals, ParsesStaticSpeedLimit) {
  const auto result = parse(document_with_signals(R"(
      <signal s="50" t="-4" id="10" name="SpeedLimit50" dynamic="no" orientation="+"
              zOffset="1.9" country="DE" countryRevision="2017" type="274" subtype="50"
              value="50" unit="km/h" height="0.77" width="0.77" hOffset="0.1"/>)"));
  EXPECT_EQ(roadmaker::count_errors(result.diagnostics), 0U);

  const Signal& signal = only_signal(result.network);
  EXPECT_EQ(signal.odr_id, "10");
  EXPECT_EQ(signal.name, "SpeedLimit50");
  EXPECT_DOUBLE_EQ(signal.s, 50.0);
  EXPECT_DOUBLE_EQ(signal.t, -4.0);
  EXPECT_DOUBLE_EQ(signal.z_offset, 1.9);
  ASSERT_TRUE(signal.dynamic.has_value());
  EXPECT_FALSE(*signal.dynamic);
  EXPECT_EQ(signal.orientation, ObjectOrientation::Plus);
  EXPECT_EQ(signal.type, "274");
  EXPECT_EQ(signal.subtype, "50");
  EXPECT_EQ(signal.country, "DE");
  EXPECT_EQ(signal.country_revision, "2017");
  ASSERT_TRUE(signal.value.has_value());
  EXPECT_DOUBLE_EQ(*signal.value, 50.0);
  EXPECT_EQ(signal.unit, "km/h");
  ASSERT_TRUE(signal.height.has_value());
  EXPECT_DOUBLE_EQ(*signal.height, 0.77);
  EXPECT_DOUBLE_EQ(signal.h_offset, 0.1);
  EXPECT_FALSE(signal.length.has_value());
}

TEST(XodrSignals, ParsesDynamicTrafficLight) {
  const auto result = parse(document_with_signals(R"(
      <signal s="95" t="-6" id="20" name="light" dynamic="yes" orientation="+"
              zOffset="3.0" country="OpenDRIVE" countryRevision="2023" type="1000001"
              subtype="-1" height="1.0" width="0.3"/>)"));
  EXPECT_EQ(roadmaker::count_errors(result.diagnostics), 0U);

  const Signal& signal = only_signal(result.network);
  ASSERT_TRUE(signal.dynamic.has_value());
  EXPECT_TRUE(*signal.dynamic);
  EXPECT_EQ(signal.country, "OpenDRIVE");
  EXPECT_EQ(signal.type, "1000001");
  EXPECT_FALSE(signal.value.has_value());
  EXPECT_EQ(signal.unit, "");
}

TEST(XodrSignals, MissingRequiredAttributesCiteRules) {
  const auto result = parse(document_with_signals(R"(
      <signal id="4"/>)"));

  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kSignalType), nullptr);
  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kSignalUseCountryCode), nullptr);
  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kObjectStTCoords), nullptr);
  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kObjectOrientation), nullptr);
  // Never a drop: the signal still exists, with defaults.
  EXPECT_EQ(result.network.signal_count(), 1U);
}

TEST(XodrSignals, PreservedTierSurvivesRoundTrip) {
  // Unknown attribute + unmodeled children (<validity>, <dependency>,
  // <userData> on the signal, <signalReference> next to it) must survive
  // parse -> write -> parse unchanged.
  const auto result = parse(document_with_signals(R"(
      <signal s="1" t="-1" id="5" dynamic="yes" orientation="+" zOffset="3.03"
              type="1000002" subtype="-1" country="OpenDRIVE" vendorFoo="bar">
        <validity fromLane="-1" toLane="-1"/>
        <reference elementId="7" elementType="signal" type="stopline"/>
        <userData code="vendor:x" value="1"/>
      </signal>
      <signalReference s="13" t="0" id="5" orientation="-"/>)"));

  const Signal& signal = only_signal(result.network);
  ASSERT_EQ(signal.preserved.attributes.size(), 1U);
  EXPECT_EQ(signal.preserved.attributes.front().first, "vendorFoo");
  ASSERT_EQ(signal.preserved.children.size(), 3U);

  const auto written = roadmaker::write_xodr(result.network, "signals-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("vendorFoo=\"bar\""), std::string::npos);
  EXPECT_NE(written->find("<validity"), std::string::npos);
  EXPECT_NE(written->find("<reference"), std::string::npos);
  EXPECT_NE(written->find("<signalReference"), std::string::npos);

  const auto reparsed = parse(*written);
  const Signal& again = only_signal(reparsed.network);
  EXPECT_EQ(again.preserved, signal.preserved);
  const roadmaker::Road& road = *reparsed.network.road(reparsed.network.find_road("1"));
  ASSERT_EQ(road.signal_extras.size(), 1U);
  EXPECT_NE(road.signal_extras.front().find("signalReference"), std::string::npos);
}

// --- round trip --------------------------------------------------------------

TEST(XodrSignals, TypedFieldsRoundTripStable) {
  const std::string first = document_with_signals(R"(
      <signal s="50" t="-4" id="10" name="SpeedLimit50" dynamic="no" orientation="+"
              zOffset="1.9" country="DE" countryRevision="2017" type="274" subtype="50"
              value="50" unit="km/h" height="0.77" width="0.77"/>
      <signal s="95" t="-6" id="11" dynamic="yes" orientation="+" zOffset="3.0"
              country="OpenDRIVE" type="1000001" subtype="-1"/>)");

  const auto parsed = parse(first);
  const auto written = roadmaker::write_xodr(parsed.network, "signals-test");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse(*written);
  ASSERT_EQ(reparsed.network.signal_count(), 2U);

  // Write of the re-parse is byte-identical (writer output is a fixed point).
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "signals-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);

  std::vector<const Signal*> before;
  std::vector<const Signal*> after;
  parsed.network.for_each_signal(
      [&](SignalId, const Signal& signal) { before.push_back(&signal); });
  reparsed.network.for_each_signal(
      [&](SignalId, const Signal& signal) { after.push_back(&signal); });
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i]->odr_id, after[i]->odr_id);
    EXPECT_EQ(before[i]->type, after[i]->type);
    EXPECT_EQ(before[i]->subtype, after[i]->subtype);
    EXPECT_DOUBLE_EQ(before[i]->s, after[i]->s);
    EXPECT_DOUBLE_EQ(before[i]->t, after[i]->t);
    EXPECT_DOUBLE_EQ(before[i]->z_offset, after[i]->z_offset);
    EXPECT_EQ(before[i]->value, after[i]->value);
    EXPECT_EQ(before[i]->unit, after[i]->unit);
  }
}

// --- version gating ----------------------------------------------------------

TEST(XodrSignals, TemporaryAndInvalidatedAre190Only) {
  const auto parsed = parse(document_with_signals(R"(
      <signal s="1" t="0" id="12" dynamic="no" orientation="+" zOffset="2" country="DE"
              type="274" subtype="50" value="30" unit="km/h" length="0.1"
              temporary="true" invalidated="false"/>)"));

  const auto v181 = roadmaker::write_xodr(
      parsed.network, "t", {.target_version = roadmaker::XodrVersion::v1_8_1});
  ASSERT_TRUE(v181.has_value());
  EXPECT_EQ(v181->find("temporary"), std::string::npos);
  EXPECT_EQ(v181->find("invalidated"), std::string::npos);
  EXPECT_NE(v181->find("length=\"0.1\""), std::string::npos); // @length is 1.8.0

  const auto v190 = roadmaker::write_xodr(
      parsed.network, "t", {.target_version = roadmaker::XodrVersion::v1_9_0});
  ASSERT_TRUE(v190.has_value());
  EXPECT_NE(v190->find("temporary=\"true\""), std::string::npos);
  EXPECT_NE(v190->find("invalidated=\"false\""), std::string::npos);
}

// --- validation --------------------------------------------------------------

TEST(XodrSignals, ValidateFlagsDuplicateSignalIds) {
  const auto parsed = parse(document_with_signals(R"(
      <signal s="1" t="0" id="14" dynamic="no" orientation="+" zOffset="2" country="DE" type="274" subtype="50"/>
      <signal s="2" t="0" id="14" dynamic="no" orientation="+" zOffset="2" country="DE" type="274" subtype="50"/>)"));
  EXPECT_EQ(parsed.network.signal_count(), 2U); // never a drop
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kIdUniqueInClass), nullptr);
}

TEST(XodrSignals, ValidateFlagsMissingCountryAndType) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  Signal bad;
  bad.odr_id = "1";
  bad.type = "274"; // no subtype, no country
  network.add_signal(road, bad);

  const auto findings = roadmaker::validate_network(network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kSignalType), nullptr);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kSignalUseCountryCode), nullptr);
}

TEST(XodrSignals, WellFormedGs1SetProducesNoSignalFindings) {
  const auto parsed = parse(document_with_signals(R"(
      <signal s="50" t="-4" id="20" name="SpeedLimit50" dynamic="no" orientation="+"
              zOffset="1.9" country="DE" countryRevision="2017" type="274" subtype="50"
              value="50" unit="km/h" height="0.77" width="0.77"/>
      <signal s="95" t="-6" id="21" dynamic="yes" orientation="+" zOffset="3.0"
              country="OpenDRIVE" type="1000001" subtype="-1"/>
      <signal s="40" t="-4" id="22" dynamic="no" orientation="+" zOffset="2.0"
              country="DE" type="101" subtype="11"/>)"));
  EXPECT_EQ(roadmaker::count_errors(parsed.diagnostics), 0U);

  for (const auto& finding : roadmaker::validate_network(parsed.network)) {
    EXPECT_EQ(finding.rule_id.find("road.signal"), std::string::npos) << finding.message;
  }
}
