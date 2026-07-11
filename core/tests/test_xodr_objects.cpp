// OpenDRIVE <objects> (§13) — data model, parse, write, validate (M3a P0,
// docs/design/m3a/01_kernel_objects_signals.md). Round-trip fidelity is the
// gate: typed fields within rm::tol, preserved fragments byte-equal.

#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::Object;
using roadmaker::ObjectId;
using roadmaker::ObjectOrientation;
using roadmaker::ObjectOutline;
using roadmaker::ObjectType;
using roadmaker::OutlineCorner;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;

namespace {

/// One straight 100 m road wrapping `objects_xml` in its <objects> element.
std::string document_with_objects(std::string_view objects_xml) {
  std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="objects-test"/>
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
    <objects>
)";
  doc += objects_xml;
  doc += R"(
    </objects>
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

const Object& only_object(const RoadNetwork& network) {
  EXPECT_EQ(network.object_count(), 1U);
  const Object* found = nullptr;
  network.for_each_object([&](ObjectId, const Object& object) { found = &object; });
  EXPECT_NE(found, nullptr);
  return *found;
}

} // namespace

// --- arena / network API -----------------------------------------------------

TEST(ObjectArena, AddLookupErase) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");

  Object value;
  value.odr_id = "42";
  value.type = ObjectType::Tree;
  value.s = 10.0;
  value.t = -5.0;
  const ObjectId id = network.add_object(road, value);
  ASSERT_TRUE(id.is_valid());
  ASSERT_NE(network.object(id), nullptr);
  EXPECT_EQ(network.object(id)->road, road);
  EXPECT_EQ(network.object(id)->type, ObjectType::Tree);
  EXPECT_EQ(objects_of(network, road).size(), 1U);

  EXPECT_TRUE(network.erase_object(id));
  EXPECT_EQ(network.object(id), nullptr); // handle is stale now
  EXPECT_FALSE(network.erase_object(id));
  EXPECT_EQ(network.object_count(), 0U);
}

TEST(ObjectArena, AddToStaleRoadFails) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  ASSERT_TRUE(network.erase_road(road));
  EXPECT_FALSE(network.add_object(road, Object{}).is_valid());
}

TEST(ObjectArena, EraseRoadCascadesObjects) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const RoadId other = network.create_road("o", "2");
  const ObjectId doomed = network.add_object(road, Object{.odr_id = "1"});
  const ObjectId kept = network.add_object(other, Object{.odr_id = "2"});

  ASSERT_TRUE(network.erase_road(road));
  EXPECT_EQ(network.object(doomed), nullptr);
  ASSERT_NE(network.object(kept), nullptr);
  EXPECT_EQ(network.object_count(), 1U);
}

TEST(ObjectArena, RestoreInPlaceKeepsHandle) {
  // The M2 restore-in-place contract: erase_exact + restore never bump the
  // generation, so ids captured by an edit command survive undo/redo.
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const ObjectId id = network.add_object(road, Object{.odr_id = "7", .s = 3.0});

  const Object snapshot = *network.object(id);
  ASSERT_TRUE(network.erase_object_exact(id).has_value());
  EXPECT_EQ(network.object(id), nullptr);
  const auto restored = network.restore_object(id, snapshot);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, id);
  ASSERT_NE(network.object(id), nullptr);
  EXPECT_EQ(network.object(id)->odr_id, "7");

  // A slot freed by plain erase refuses restore (generation bumped).
  ASSERT_TRUE(network.erase_object(id));
  EXPECT_FALSE(network.restore_object(id, snapshot).has_value());
}

// --- parse ---------------------------------------------------------------

TEST(XodrObjects, ParsesCrosswalkOutline) {
  const auto result = parse(document_with_objects(R"(
      <object type="crosswalk" name="xw" id="10" s="50" t="0" zOffset="0"
              orientation="none" length="8" width="4" hdg="1.5707963267948966">
        <outlines>
          <outline id="0" outer="true" closed="true" fillType="paint">
            <cornerRoad s="48" t="-2" dz="0" height="0" id="0"/>
            <cornerRoad s="52" t="-2" dz="0" height="0" id="1"/>
            <cornerRoad s="52" t="2" dz="0" height="0" id="2"/>
            <cornerRoad s="48" t="2" dz="0" height="0" id="3"/>
          </outline>
        </outlines>
      </object>)"));
  EXPECT_EQ(roadmaker::count_errors(result.diagnostics), 0U);

  const Object& object = only_object(result.network);
  EXPECT_EQ(object.type, ObjectType::Crosswalk);
  EXPECT_EQ(object.type_str, "crosswalk");
  EXPECT_EQ(object.odr_id, "10");
  EXPECT_DOUBLE_EQ(object.s, 50.0);
  EXPECT_DOUBLE_EQ(object.hdg, 1.5707963267948966);
  EXPECT_EQ(object.orientation, ObjectOrientation::None);
  ASSERT_TRUE(object.length.has_value());
  EXPECT_DOUBLE_EQ(*object.length, 8.0);
  EXPECT_FALSE(object.radius.has_value());
  EXPECT_FALSE(object.height.has_value());

  ASSERT_EQ(object.outlines.size(), 1U);
  const ObjectOutline& outline = object.outlines.front();
  EXPECT_TRUE(outline.road_coords);
  EXPECT_TRUE(outline.outer);
  ASSERT_TRUE(outline.closed.has_value());
  EXPECT_TRUE(*outline.closed);
  ASSERT_TRUE(outline.fill_type.has_value());
  EXPECT_EQ(*outline.fill_type, "paint");
  ASSERT_EQ(outline.corners.size(), 4U);
  EXPECT_DOUBLE_EQ(outline.corners[0].a, 48.0);
  EXPECT_DOUBLE_EQ(outline.corners[2].b, 2.0);
  EXPECT_EQ(outline.corners[3].id, 3);
  EXPECT_TRUE(outline.raw.empty());
}

TEST(XodrObjects, ParsesLegacy14OutlineWithoutWrapper) {
  // OpenDRIVE 1.4 outline definitions (no <outlines> parent) shall still be
  // supported (1.9.0 §13.2).
  const auto result = parse(document_with_objects(R"(
      <object type="obstacle" id="2" s="10" t="1" zOffset="0" orientation="+">
        <outline>
          <cornerLocal u="-1" v="-1" z="0" height="1"/>
          <cornerLocal u="1" v="-1" z="0" height="1"/>
          <cornerLocal u="0" v="1" z="0" height="1"/>
        </outline>
      </object>)"));

  const Object& object = only_object(result.network);
  EXPECT_EQ(object.orientation, ObjectOrientation::Plus);
  ASSERT_EQ(object.outlines.size(), 1U);
  EXPECT_FALSE(object.outlines.front().road_coords);
  EXPECT_EQ(object.outlines.front().corners.size(), 3U);
  EXPECT_FALSE(object.outlines.front().closed.has_value()); // absent stays absent
}

TEST(XodrObjects, ParsesTreeLineRepeat) {
  const auto result = parse(document_with_objects(R"(
      <object type="tree" id="3" s="5" t="8" zOffset="0" orientation="none" radius="0.5" height="6">
        <repeat s="5" length="90" distance="15" tStart="8" tEnd="8"
                zOffsetStart="0" zOffsetEnd="0" heightStart="6" heightEnd="8"/>
      </object>)"));

  const Object& object = only_object(result.network);
  EXPECT_EQ(object.type, ObjectType::Tree);
  ASSERT_TRUE(object.radius.has_value());
  ASSERT_EQ(object.repeats.size(), 1U);
  const roadmaker::ObjectRepeat& repeat = object.repeats.front();
  EXPECT_DOUBLE_EQ(repeat.length, 90.0);
  EXPECT_DOUBLE_EQ(repeat.distance, 15.0);
  ASSERT_TRUE(repeat.height_end.has_value());
  EXPECT_DOUBLE_EQ(*repeat.height_end, 8.0);
  EXPECT_FALSE(repeat.width_start.has_value());
  EXPECT_FALSE(repeat.detach_from_reference_line);
}

TEST(XodrObjects, MissingRequiredAttributesCiteRules) {
  const auto result = parse(document_with_objects(R"(
      <object id="4"/>)"));

  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kObjectTypeAttr), nullptr);
  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kObjectStTCoords), nullptr);
  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kObjectOrientation), nullptr);
  // Never a drop: the object still exists, with defaults.
  EXPECT_EQ(result.network.object_count(), 1U);
}

TEST(XodrObjects, UnknownTypeSpellingSurvives) {
  const auto result = parse(document_with_objects(R"(
      <object type="streetLamp" id="5" s="1" t="2" zOffset="0" orientation="none"/>)"));

  const Object& object = only_object(result.network);
  EXPECT_EQ(object.type, ObjectType::Other);
  EXPECT_EQ(object.type_str, "streetLamp");

  const auto written = roadmaker::write_xodr(result.network, "objects-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("type=\"streetLamp\""), std::string::npos);
}

TEST(XodrObjects, MixedCornerKindsPreservedVerbatimWithRule) {
  const auto result = parse(document_with_objects(R"(
      <object type="obstacle" id="6" s="1" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline outer="true">
            <cornerRoad s="0" t="0" dz="0" height="1"/>
            <cornerLocal u="1" v="1" z="0" height="1"/>
          </outline>
        </outlines>
      </object>)"));

  EXPECT_NE(find_by_rule(result.diagnostics, roadmaker::rules::kCornerRoadLocalExcl), nullptr);
  const Object& object = only_object(result.network);
  ASSERT_EQ(object.outlines.size(), 1U);
  EXPECT_FALSE(object.outlines.front().raw.empty());
  EXPECT_TRUE(object.outlines.front().corners.empty());

  // The verbatim outline round-trips, cornerLocal child included.
  const auto written = roadmaker::write_xodr(result.network, "objects-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("cornerLocal"), std::string::npos);
}

TEST(XodrObjects, CurveLocalOutlinePreservedVerbatim) {
  const auto result = parse(document_with_objects(R"(
      <object type="obstacle" id="7" s="1" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline outer="true">
            <curveLocal sOffset="0" length="5"/>
          </outline>
        </outlines>
      </object>)"));

  const Object& object = only_object(result.network);
  ASSERT_EQ(object.outlines.size(), 1U);
  EXPECT_FALSE(object.outlines.front().raw.empty());

  const auto written = roadmaker::write_xodr(result.network, "objects-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("curveLocal"), std::string::npos);
}

TEST(XodrObjects, PreservedTierSurvivesRoundTrip) {
  // Unknown attribute + unmodeled children (<skeleton> on the object,
  // <tunnel> next to it) must survive parse → write → parse unchanged.
  const auto result = parse(document_with_objects(R"(
      <object type="pole" id="8" s="1" t="-4" zOffset="0" orientation="none" vendorFoo="bar">
        <skeleton>
          <polyline id="0">
            <vertexLocal u="0" v="0" z="0" radius="0.1"/>
            <vertexLocal u="0" v="0" z="4" radius="0.1"/>
          </polyline>
        </skeleton>
        <userData code="vendor:x" value="1"/>
      </object>
      <tunnel s="20" length="30" id="t1" type="standard"/>)"));

  const Object& object = only_object(result.network);
  ASSERT_EQ(object.preserved.attributes.size(), 1U);
  EXPECT_EQ(object.preserved.attributes.front().first, "vendorFoo");
  ASSERT_EQ(object.preserved.children.size(), 2U);

  const auto written = roadmaker::write_xodr(result.network, "objects-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("vendorFoo=\"bar\""), std::string::npos);
  EXPECT_NE(written->find("<skeleton>"), std::string::npos);
  EXPECT_NE(written->find("<tunnel"), std::string::npos);

  const auto reparsed = parse(*written);
  const Object& again = only_object(reparsed.network);
  EXPECT_EQ(again.preserved, object.preserved);
  const roadmaker::Road& road = *reparsed.network.road(reparsed.network.find_road("1"));
  ASSERT_EQ(road.object_extras.size(), 1U);
  EXPECT_NE(road.object_extras.front().find("tunnel"), std::string::npos);
}

// --- round trip ------------------------------------------------------------

TEST(XodrObjects, TypedFieldsRoundTripStable) {
  const std::string first = document_with_objects(R"(
      <object type="crosswalk" id="10" s="50" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline id="0" outer="true" closed="true" fillType="paint">
            <cornerRoad s="48" t="-2" dz="0" height="0" id="0"/>
            <cornerRoad s="52" t="-2" dz="0" height="0" id="1"/>
            <cornerRoad s="52" t="2" dz="0" height="0" id="2"/>
          </outline>
        </outlines>
      </object>
      <object type="tree" id="11" s="5" t="8" zOffset="0.1" orientation="none" radius="0.5" height="6">
        <repeat s="5" length="90" distance="15" tStart="8" tEnd="8" zOffsetStart="0" zOffsetEnd="0"/>
      </object>)");

  const auto parsed = parse(first);
  const auto written = roadmaker::write_xodr(parsed.network, "objects-test");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse(*written);
  ASSERT_EQ(reparsed.network.object_count(), 2U);

  // Write of the re-parse is byte-identical (writer output is a fixed point).
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "objects-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);

  std::vector<const Object*> before;
  std::vector<const Object*> after;
  parsed.network.for_each_object(
      [&](ObjectId, const Object& object) { before.push_back(&object); });
  reparsed.network.for_each_object(
      [&](ObjectId, const Object& object) { after.push_back(&object); });
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i]->odr_id, after[i]->odr_id);
    EXPECT_EQ(before[i]->type, after[i]->type);
    EXPECT_DOUBLE_EQ(before[i]->s, after[i]->s);
    EXPECT_DOUBLE_EQ(before[i]->t, after[i]->t);
    EXPECT_DOUBLE_EQ(before[i]->z_offset, after[i]->z_offset);
    EXPECT_EQ(before[i]->outlines, after[i]->outlines);
    EXPECT_EQ(before[i]->repeats, after[i]->repeats);
  }
}

// --- version gating ----------------------------------------------------------

TEST(XodrObjects, TemporaryAndCubicRepeatAttributesAre190Only) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="barrier" id="12" s="1" t="0" zOffset="0" orientation="none" temporary="true">
        <repeat s="0" length="50" distance="0" tStart="0" tEnd="2"
                zOffsetStart="0" zOffsetEnd="0" bT="0.04" cT="0" dT="0"/>
      </object>)"));

  const auto v181 = roadmaker::write_xodr(
      parsed.network, "t", {.target_version = roadmaker::XodrVersion::v1_8_1});
  ASSERT_TRUE(v181.has_value());
  EXPECT_EQ(v181->find("temporary"), std::string::npos);
  EXPECT_EQ(v181->find("bT"), std::string::npos);

  const auto v190 = roadmaker::write_xodr(
      parsed.network, "t", {.target_version = roadmaker::XodrVersion::v1_9_0});
  ASSERT_TRUE(v190.has_value());
  EXPECT_NE(v190->find("temporary=\"true\""), std::string::npos);
  EXPECT_NE(v190->find("bT=\"0.04\""), std::string::npos);
}

// --- validation ------------------------------------------------------------

TEST(XodrObjects, ValidateFlagsShapeExclusivity) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="obstacle" id="13" s="1" t="0" zOffset="0" orientation="none"
              radius="1" length="2" width="1"/>)"));
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kObjectCircularVsAngular), nullptr);
}

TEST(XodrObjects, ValidateFlagsDuplicateObjectIds) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="pole" id="14" s="1" t="0" zOffset="0" orientation="none"/>
      <object type="pole" id="14" s="2" t="0" zOffset="0" orientation="none"/>)"));
  EXPECT_EQ(parsed.network.object_count(), 2U); // never a drop
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kIdUniqueInClass), nullptr);
}

TEST(XodrObjects, ValidateFlagsOuterOutlineCount) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="obstacle" id="15" s="1" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline outer="true">
            <cornerLocal u="0" v="0" z="0" height="1"/>
            <cornerLocal u="1" v="0" z="0" height="1"/>
          </outline>
          <outline outer="true">
            <cornerLocal u="0" v="0" z="0" height="1"/>
            <cornerLocal u="0" v="1" z="0" height="1"/>
          </outline>
        </outlines>
      </object>)"));
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kOutlineExactlyOneOuter), nullptr);
}

TEST(XodrObjects, ValidateFlagsCornerMinimum) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="obstacle" id="16" s="1" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline outer="true">
            <cornerRoad s="0" t="0" dz="0" height="1"/>
          </outline>
        </outlines>
      </object>)"));
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_NE(find_by_rule(findings, roadmaker::rules::kCornerRoadMinAmount), nullptr);
}

TEST(XodrObjects, WellFormedGs1SetProducesNoObjectFindings) {
  const auto parsed = parse(document_with_objects(R"(
      <object type="crosswalk" id="20" s="50" t="0" zOffset="0" orientation="none">
        <outlines>
          <outline id="0" outer="true" closed="true" fillType="paint">
            <cornerRoad s="48" t="-2" dz="0" height="0" id="0"/>
            <cornerRoad s="52" t="-2" dz="0" height="0" id="1"/>
            <cornerRoad s="52" t="2" dz="0" height="0" id="2"/>
            <cornerRoad s="48" t="2" dz="0" height="0" id="3"/>
          </outline>
        </outlines>
      </object>
      <object type="pole" id="21" s="45" t="-6" zOffset="0" orientation="none" radius="0.06" height="4"/>
      <object type="tree" id="22" s="5" t="8" zOffset="0" orientation="none" radius="0.5" height="6">
        <repeat s="5" length="90" distance="15" tStart="8" tEnd="8" zOffsetStart="0" zOffsetEnd="0"/>
      </object>)"));
  EXPECT_EQ(roadmaker::count_errors(parsed.diagnostics), 0U);

  for (const auto& finding : roadmaker::validate_network(parsed.network)) {
    EXPECT_EQ(finding.rule_id.find("road.object"), std::string::npos) << finding.message;
    EXPECT_EQ(finding.rule_id.find("road.corner"), std::string::npos) << finding.message;
  }
}
