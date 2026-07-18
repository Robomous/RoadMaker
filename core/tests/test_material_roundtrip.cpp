// Lane <material> (§11.8.2) + lane Preserved tier (p6-s3 / #237). The gate is
// byte-identical round-trip INCLUDING an unmodeled @attr and an unmodeled lane
// child, plus the two normative diagnostics (center_lane_no_material,
// elem_asc_order) and the missing-@friction warning.

#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::Lane;
using roadmaker::LaneMaterial;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;

namespace {

/// One straight 100 m road; `lanes_xml` fills the single <laneSection>.
std::string document_with_lanes(std::string_view lanes_xml) {
  std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="material-test"/>
  <road name="r" length="100" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="100"><line/></geometry>
    </planView>
    <lanes>
      <laneSection s="0">
)";
  doc += lanes_xml;
  doc += R"(
      </laneSection>
    </lanes>
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

const Lane* lane_by_id(const RoadNetwork& network, int odr_id) {
  const RoadId road = network.find_road("1");
  const auto& section = *network.lane_section(network.road(road)->sections.front());
  for (const roadmaker::LaneId id : section.lanes) {
    if (network.lane(id)->odr_id == odr_id) {
      return network.lane(id);
    }
  }
  return nullptr;
}

// Two material records on lane -1 (one carrying an unmodeled @colorHint), a
// roadMark @material, an unmodeled <height> child, and a lane <userData>.
constexpr std::string_view kMaterialLanes = R"(        <center>
          <lane id="0" type="none" level="false"/>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
            <roadMark sOffset="0" type="solid" width="0.12" material="rm:paint_white"/>
            <material sOffset="0" friction="0.85" roughness="0.012" surface="rm:asphalt" colorHint="dark"/>
            <material sOffset="40" friction="0.7" surface="rm:asphalt_worn"/>
            <height sOffset="0" inner="0" outer="0"/>
            <userData code="rm:demo" value="1"/>
          </lane>
        </right>)";

} // namespace

TEST(LaneMaterial, RoundTripByteIdenticalWithUnmodeledAttrAndChild) {
  const std::string first = document_with_lanes(kMaterialLanes);
  const auto parsed = parse(first);

  const Lane* lane = lane_by_id(parsed.network, -1);
  ASSERT_NE(lane, nullptr);
  ASSERT_EQ(lane->materials.size(), 2U);
  EXPECT_DOUBLE_EQ(lane->materials[0].s_offset, 0.0);
  ASSERT_TRUE(lane->materials[0].friction.has_value());
  EXPECT_DOUBLE_EQ(*lane->materials[0].friction, 0.85);
  ASSERT_TRUE(lane->materials[0].roughness.has_value());
  ASSERT_TRUE(lane->materials[0].surface.has_value());
  EXPECT_EQ(*lane->materials[0].surface, "rm:asphalt");
  // Unmodeled @colorHint survives on the record's preserved tier.
  ASSERT_EQ(lane->materials[0].preserved.attributes.size(), 1U);
  EXPECT_EQ(lane->materials[0].preserved.attributes.front().first, "colorHint");
  // Second record: no @roughness (omitted on write), worn surface.
  EXPECT_FALSE(lane->materials[1].roughness.has_value());
  EXPECT_EQ(*lane->materials[1].surface, "rm:asphalt_worn");
  // roadMark @material plumbing.
  ASSERT_FALSE(lane->road_marks.empty());
  ASSERT_TRUE(lane->road_marks.front().material.has_value());
  EXPECT_EQ(*lane->road_marks.front().material, "rm:paint_white");
  // Unmodeled <height> + <userData> captured on the lane Preserved tier.
  ASSERT_EQ(lane->preserved.children.size(), 2U);

  const auto written = roadmaker::write_xodr(parsed.network, "material-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("colorHint=\"dark\""), std::string::npos);
  EXPECT_NE(written->find("material=\"rm:paint_white\""), std::string::npos);
  EXPECT_NE(written->find("<height"), std::string::npos);
  EXPECT_NE(written->find("code=\"rm:demo\""), std::string::npos);

  // Writer is a fixed point: re-parse then re-write must be byte-identical.
  const auto reparsed = parse(*written);
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "material-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);

  const Lane* again = lane_by_id(reparsed.network, -1);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again->materials, lane->materials);
  EXPECT_EQ(again->preserved, lane->preserved);
  EXPECT_EQ(again->road_marks, lane->road_marks);
}

TEST(LaneMaterial, CenterLaneMaterialErrorsButIsKept) {
  const std::string doc = document_with_lanes(R"(        <center>
          <lane id="0" type="none" level="false">
            <material sOffset="0" friction="0.9" surface="rm:asphalt"/>
          </lane>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
          </lane>
        </right>)");
  const auto parsed = parse(doc);
  EXPECT_NE(find_by_rule(parsed.diagnostics, roadmaker::rules::kMaterialCenterLaneNone), nullptr);
  // Diagnose-but-keep: the record survives so the file round-trips.
  const Lane* lane0 = lane_by_id(parsed.network, 0);
  ASSERT_NE(lane0, nullptr);
  EXPECT_EQ(lane0->materials.size(), 1U);
}

TEST(LaneMaterial, NonAscendingSOffsetErrors) {
  const std::string doc = document_with_lanes(R"(        <center>
          <lane id="0" type="none" level="false"/>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
            <material sOffset="40" friction="0.8" surface="rm:asphalt"/>
            <material sOffset="10" friction="0.8" surface="rm:asphalt_worn"/>
          </lane>
        </right>)");
  const auto parsed = parse(doc);
  EXPECT_NE(find_by_rule(parsed.diagnostics, roadmaker::rules::kMaterialElemAscOrder), nullptr);
  // Records kept in document order (not silently reordered or dropped).
  const Lane* lane = lane_by_id(parsed.network, -1);
  ASSERT_NE(lane, nullptr);
  ASSERT_EQ(lane->materials.size(), 2U);
  EXPECT_DOUBLE_EQ(lane->materials[0].s_offset, 40.0);
}

TEST(LaneMaterial, MissingFrictionWarns) {
  const std::string doc = document_with_lanes(R"(        <center>
          <lane id="0" type="none" level="false"/>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
            <material sOffset="0" surface="rm:asphalt"/>
          </lane>
        </right>)");
  const auto parsed = parse(doc);
  const bool warned = std::ranges::any_of(parsed.diagnostics, [](const roadmaker::Diagnostic& d) {
    return d.message.find("friction") != std::string::npos;
  });
  EXPECT_TRUE(warned);
  const Lane* lane = lane_by_id(parsed.network, -1);
  ASSERT_NE(lane, nullptr);
  ASSERT_EQ(lane->materials.size(), 1U);
  EXPECT_FALSE(lane->materials.front().friction.has_value());
}
