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

// Road-mark completions (§11.9 color + multi-line, §13.14 object markings) —
// M3a P2, docs/design/m3a/02_road_marks.md. Gate: marking round-trip
// byte-stable (M2 single-line unchanged), dual-strip + arrow meshes
// deterministic.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::MeshOptions;
using roadmaker::NetworkMesh;
using roadmaker::Object;
using roadmaker::ObjectType;
using roadmaker::RoadId;
using roadmaker::RoadMark;
using roadmaker::RoadMarkColor;
using roadmaker::RoadMarkLine;
using roadmaker::RoadMarkType;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;

namespace {

/// A straight 100 m road; `center_mark_xml` fills lane 0's <roadMark>.
std::string document_with_center_mark(std::string_view center_mark_xml) {
  return std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="marks-test"/>
  <road name="r" length="100" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="100"><line/></geometry>
    </planView>
    <lanes>
      <laneSection s="0">
        <center><lane id="0" type="none" level="false">)") +
         std::string(center_mark_xml) + R"(</lane></center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
            <roadMark sOffset="0" type="broken" width="0.12"/>
          </lane>
        </right>
      </laneSection>
    </lanes>
  </road>
</OpenDRIVE>
)";
}

roadmaker::XodrParseResult parse(std::string_view xml) {
  auto result = roadmaker::parse_xodr(xml, "test");
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

/// Lane 0's first <roadMark> on `road`, or nullptr when it carries none.
const RoadMark* center_mark(const RoadNetwork& network, RoadId road) {
  const roadmaker::LaneSection& section =
      *network.lane_section(network.road(road)->sections.front());
  for (const roadmaker::LaneId id : section.lanes) {
    const roadmaker::Lane* lane = network.lane(id);
    if (lane->odr_id == 0 && !lane->road_marks.empty()) {
      return &lane->road_marks.front();
    }
  }
  return nullptr;
}

const SubMesh* find_marking(const NetworkMesh& mesh, std::string_view needle) {
  for (const roadmaker::RoadMesh& road : mesh.roads) {
    for (const SubMesh& marking : road.markings) {
      if (marking.name.find(needle) != std::string::npos) {
        return &marking;
      }
    }
  }
  return nullptr;
}

} // namespace

// --- round trip --------------------------------------------------------------

TEST(RoadMarks, M2SingleLineFormStaysByteIdentical) {
  // A bare solid mark (no color, no <line>) must round-trip byte-identical:
  // no @color attribute, no <type> child appears.
  const auto parsed =
      parse(document_with_center_mark(R"(<roadMark sOffset="0" type="solid" width="0.12"/>)"));
  const auto written = roadmaker::write_xodr(parsed.network, "marks-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find("color="), std::string::npos);
  EXPECT_EQ(written->find("<type"), std::string::npos);

  // Fixed point: parse -> write -> parse -> write is byte-stable.
  const auto reparsed = parse(*written);
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "marks-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

TEST(RoadMarks, ColorAndExplicitLinesRoundTrip) {
  const auto parsed = parse(document_with_center_mark(R"(
      <roadMark sOffset="0" type="solid solid" color="yellow" width="0.3">
        <type name="solid solid" width="0.3">
          <line length="0" space="0" tOffset="0.1" sOffset="0" width="0.12"/>
          <line length="0" space="0" tOffset="-0.1" sOffset="0" width="0.12"/>
        </type>
      </roadMark>)"));

  RoadId road_id = parsed.network.find_road("1");
  const roadmaker::Lane* lane0 = nullptr;
  const roadmaker::LaneSection& section =
      *parsed.network.lane_section(parsed.network.road(road_id)->sections.front());
  for (const roadmaker::LaneId id : section.lanes) {
    if (parsed.network.lane(id)->odr_id == 0) {
      lane0 = parsed.network.lane(id);
    }
  }
  ASSERT_NE(lane0, nullptr);
  ASSERT_EQ(lane0->road_marks.size(), 1U);
  const RoadMark& mark = lane0->road_marks.front();
  EXPECT_EQ(mark.type, RoadMarkType::SolidSolid);
  EXPECT_EQ(mark.color, RoadMarkColor::Yellow);
  ASSERT_EQ(mark.lines.size(), 2U);
  EXPECT_DOUBLE_EQ(mark.lines[0].t_offset, 0.1);
  EXPECT_DOUBLE_EQ(mark.lines[1].t_offset, -0.1);

  // Fixed point + fields preserved on reparse.
  const auto written = roadmaker::write_xodr(parsed.network, "marks-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("color=\"yellow\""), std::string::npos);
  EXPECT_NE(written->find("<type"), std::string::npos);
  const auto reparsed = parse(*written);
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "marks-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

TEST(RoadMarks, SplitRoadPreservesColorAndLinesOnBothHalves) {
  auto parsed = parse(document_with_center_mark(R"(
      <roadMark sOffset="0" type="solid solid" color="yellow" width="0.3">
        <type name="solid solid" width="0.3">
          <line length="0" space="0" tOffset="0.1" sOffset="0" width="0.12"/>
          <line length="0" space="0" tOffset="-0.1" sOffset="0" width="0.12"/>
        </type>
      </roadMark>)"));
  RoadNetwork& network = parsed.network;
  const RoadId head = network.find_road("1");
  ASSERT_TRUE(head.is_valid());

  const auto before = roadmaker::write_xodr(network, "marks-test");
  ASSERT_TRUE(before.has_value());

  auto command = roadmaker::edit::split_road(network, head, 40.0);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  const RoadId tail = network.find_road("2");
  ASSERT_TRUE(tail.is_valid());

  // Rebuilding the tail's marks field by field used to drop @color and the
  // <line> block, silently reverting a dual-yellow centre line to a single
  // standard-colour stripe.
  for (const RoadId road : {head, tail}) {
    const RoadMark* mark = center_mark(network, road);
    ASSERT_NE(mark, nullptr);
    EXPECT_EQ(mark->type, RoadMarkType::SolidSolid);
    EXPECT_EQ(mark->color, RoadMarkColor::Yellow);
    EXPECT_DOUBLE_EQ(mark->width, 0.3);
    ASSERT_EQ(mark->lines.size(), 2U);
    EXPECT_DOUBLE_EQ(mark->lines[0].t_offset, 0.1);
    EXPECT_DOUBLE_EQ(mark->lines[1].t_offset, -0.1);
  }

  ASSERT_TRUE(command->revert(network).has_value());
  const auto reverted = roadmaker::write_xodr(network, "marks-test");
  ASSERT_TRUE(reverted.has_value());
  EXPECT_EQ(*reverted, *before);
}

TEST(RoadMarks, BrokenBrokenRoundTripsAsDoubleDashed) {
  // "broken broken" (§11.9, Annex A.3.4 Table 173 — the double-dashed family
  // member) parses to BrokenBroken and writes back with the space spelling,
  // byte-stable on the fixed point.
  const auto parsed = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="broken broken" color="yellow" width="0.12"/>)"));
  const RoadMark* mark = center_mark(parsed.network, parsed.network.find_road("1"));
  ASSERT_NE(mark, nullptr);
  EXPECT_EQ(mark->type, RoadMarkType::BrokenBroken);
  EXPECT_EQ(mark->color, RoadMarkColor::Yellow);

  const auto written = roadmaker::write_xodr(parsed.network, "marks-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find(R"(type="broken broken")"), std::string::npos);
  const auto reparsed = parse(*written);
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "marks-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

TEST(RoadMarks, UnknownColorSurvivesAsOtherWithDiagnostic) {
  const auto parsed = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="solid" color="chartreuse" width="0.12"/>)"));
  const bool warned = std::ranges::any_of(parsed.diagnostics, [](const roadmaker::Diagnostic& d) {
    return d.message.find("chartreuse") != std::string::npos;
  });
  EXPECT_TRUE(warned);
}

// --- mesh: dual-strip lane marks --------------------------------------------

TEST(RoadMarks, SolidSolidRendersTwoStrips) {
  // Straight road along +x: lateral offset t maps to world y, so a synthesized
  // solid_solid mark spreads its two stripes to y ~ +/-width — a span wider
  // than a single strip.
  const auto parsed = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="solid solid" color="yellow" width="0.15"/>)"));
  const NetworkMesh mesh = roadmaker::build_network_mesh(parsed.network);

  const SubMesh* center = find_marking(mesh, "lane 0");
  ASSERT_NE(center, nullptr);
  double min_y = 1e9;
  double max_y = -1e9;
  for (std::size_t i = 1; i < center->positions.size(); i += 3) {
    min_y = std::min(min_y, center->positions[i]);
    max_y = std::max(max_y, center->positions[i]);
  }
  // Two stripes at +/-0.15 (each 0.15 wide) span ~0.45 m; a single strip only
  // ~0.15 m. The gap between stripes leaves the origin unpainted.
  EXPECT_GT(max_y - min_y, 0.15 * 1.5);
  EXPECT_GT(max_y, 0.0);
  EXPECT_LT(min_y, 0.0);
}

TEST(RoadMarks, BrokenBrokenRendersTwoDashedStrips) {
  // A synthesized broken_broken mark spreads two stripes to y ~ +/-width (like
  // solid_solid), but each stripe is dashed (3 m paint / 6 m gap) so the road
  // decomposes into many quads — unlike the two continuous quads of solid_solid.
  const auto dashed = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="broken broken" color="yellow" width="0.15"/>)"));
  const NetworkMesh mesh = roadmaker::build_network_mesh(dashed.network);
  const SubMesh* center = find_marking(mesh, "lane 0");
  ASSERT_NE(center, nullptr);

  double min_y = 1e9;
  double max_y = -1e9;
  for (std::size_t i = 1; i < center->positions.size(); i += 3) {
    min_y = std::min(min_y, center->positions[i]);
    max_y = std::max(max_y, center->positions[i]);
  }
  EXPECT_GT(max_y - min_y, 0.15 * 1.5); // two stripes, spread apart
  EXPECT_GT(max_y, 0.0);
  EXPECT_LT(min_y, 0.0);

  // solid_solid over the same road paints two continuous stripes; dashing the
  // pair into 3 m paint / 6 m gap cycles omits the gaps, so broken_broken has
  // strictly fewer painted quads than the continuous double.
  const auto solid = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="solid solid" color="yellow" width="0.15"/>)"));
  const NetworkMesh solid_mesh = roadmaker::build_network_mesh(solid.network);
  const SubMesh* solid_center = find_marking(solid_mesh, "lane 0");
  ASSERT_NE(solid_center, nullptr);
  EXPECT_LT(center->indices.size(), solid_center->indices.size());
  EXPECT_GT(center->indices.size(), 0U);
}

TEST(RoadMarks, MarkColorReachesTheSubMesh) {
  // The colour is normative data (§11.9), so the mesh carries it rather than
  // leaving the render layer to assume white — which is what it did, so a
  // yellow centre line still painted white.
  const auto yellow = parse(document_with_center_mark(
      R"(<roadMark sOffset="0" type="solid solid" color="yellow" width="0.15"/>)"));
  const NetworkMesh mesh = roadmaker::build_network_mesh(yellow.network);
  const SubMesh* center = find_marking(mesh, "lane 0");
  ASSERT_NE(center, nullptr);
  EXPECT_EQ(center->mark_color, RoadMarkColor::Yellow);

  // An uncoloured mark stays Standard, so nothing that never asked for a
  // colour changes appearance.
  const auto plain =
      parse(document_with_center_mark(R"(<roadMark sOffset="0" type="broken" width="0.12"/>)"));
  const NetworkMesh plain_mesh = roadmaker::build_network_mesh(plain.network);
  const SubMesh* plain_center = find_marking(plain_mesh, "lane 0");
  ASSERT_NE(plain_center, nullptr);
  EXPECT_EQ(plain_center->mark_color, RoadMarkColor::Standard);
}

TEST(RoadMarks, MeshIsDeterministic) {
  const auto parsed = parse(
      document_with_center_mark(R"(<roadMark sOffset="0" type="solid solid" width="0.15"/>)"));
  const NetworkMesh a = roadmaker::build_network_mesh(parsed.network);
  const NetworkMesh b = roadmaker::build_network_mesh(parsed.network);
  const SubMesh* ma = find_marking(a, "lane 0");
  const SubMesh* mb = find_marking(b, "lane 0");
  ASSERT_NE(ma, nullptr);
  ASSERT_NE(mb, nullptr);
  EXPECT_EQ(ma->positions, mb->positions);
  EXPECT_EQ(ma->indices, mb->indices);
}

// --- mesh: object markings ---------------------------------------------------

RoadNetwork straight_road_network(RoadId& road_out) {
  auto parsed = parse(document_with_center_mark(""));
  road_out = parsed.network.find_road("1");
  return std::move(parsed.network);
}

TEST(RoadMarks, StopLineObjectMeshesAsFilledQuad) {
  RoadId road;
  RoadNetwork network = straight_road_network(road);
  Object stop;
  stop.odr_id = "s1";
  stop.type_str = "roadMark";
  stop.subtype = "signalLines";
  stop.s = 50.0;
  stop.width = 3.5;
  stop.length = 0.3;
  network.add_object(road, stop);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh* line = find_marking(mesh, "stop line");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->indices.size(), 6U); // one quad
}

TEST(RoadMarks, ArrowGlyphMeshesAndVariesBySubtype) {
  RoadId road;
  RoadNetwork network = straight_road_network(road);
  Object arrow;
  arrow.odr_id = "a1";
  arrow.type_str = "roadMark";
  arrow.subtype = "arrowStraight";
  arrow.s = 45.0;
  arrow.t = -1.75;
  arrow.width = 1.75;
  arrow.length = 4.0;
  network.add_object(road, arrow);

  const NetworkMesh straight = roadmaker::build_network_mesh(network);
  const SubMesh* glyph = find_marking(straight, "arrow");
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(glyph->indices.size(), 9U); // shaft quad (6) + head triangle (3)
  const std::vector<double> straight_positions = glyph->positions;

  // A left arrow shifts the head tip laterally — geometry differs.
  network.object(roadmaker::objects_of(network, road).front())->subtype = "arrowLeft";
  const NetworkMesh left = roadmaker::build_network_mesh(network);
  const SubMesh* left_glyph = find_marking(left, "arrow");
  ASSERT_NE(left_glyph, nullptr);
  EXPECT_NE(left_glyph->positions, straight_positions);
}

TEST(RoadMarks, CrosswalkObjectMeshesAsZebraBars) {
  RoadId road;
  RoadNetwork network = straight_road_network(road);
  Object crossing;
  crossing.odr_id = "c1";
  crossing.type = ObjectType::Crosswalk;
  crossing.type_str = "crosswalk";
  crossing.subtype = "zebra";
  crossing.s = 60.0;
  crossing.hdg = 1.5707963267948966; // across the road
  crossing.length = 4.0;
  crossing.width = 3.0;
  network.add_object(road, crossing);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh* zebra = find_marking(mesh, "crosswalk");
  ASSERT_NE(zebra, nullptr);
  // 4 m at 0.5 stripe / 0.5 gap => 4 painted bars, one quad (6 indices) each.
  EXPECT_GE(zebra->indices.size(), 4U * 6U);
}

// --- remesh_objects: object-layer-only re-mesh -------------------------------

TEST(RoadMarks, RemeshObjectsRebuildsMarkingsOnly) {
  RoadId road;
  RoadNetwork network = straight_road_network(road);
  Object stop;
  stop.odr_id = "s1";
  stop.type_str = "roadMark";
  stop.subtype = "signalLines";
  stop.s = 50.0;
  stop.width = 3.5;
  stop.length = 0.3;
  network.add_object(road, stop);

  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.roads.size(), 1U);
  const std::vector<double> surface_before = mesh.roads.front().positions;
  const std::size_t markings_before = mesh.roads.front().markings.size();

  const std::vector<RoadId> dirty{road};
  roadmaker::remesh_objects(network, mesh, dirty);

  ASSERT_EQ(mesh.roads.size(), 1U);
  // Road surface grid untouched; markings regenerated to the same count.
  EXPECT_EQ(mesh.roads.front().positions, surface_before);
  EXPECT_EQ(mesh.roads.front().markings.size(), markings_before);
  EXPECT_NE(find_marking(mesh, "stop line"), nullptr);
}

// --- validation --------------------------------------------------------------

TEST(RoadMarks, ValidateFlagsMultiLineMarkWithWrongLineCount) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const roadmaker::LaneSectionId section = network.add_lane_section(road, 0.0);
  const roadmaker::LaneId lane = network.add_lane(section, 0, roadmaker::LaneType::None);
  RoadMark mark;
  mark.type = RoadMarkType::SolidSolid;
  mark.lines = {RoadMarkLine{}}; // only one line — advisory expects two
  network.lane(lane)->road_marks.push_back(mark);

  const auto findings = roadmaker::validate_network(network);
  const bool advisory = std::ranges::any_of(findings, [](const roadmaker::Diagnostic& d) {
    return d.rule_id.empty() && d.message.find("multi-line road mark") != std::string::npos;
  });
  EXPECT_TRUE(advisory);
}
