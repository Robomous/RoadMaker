#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <variant>

using roadmaker::ContactPoint;
using roadmaker::ErrorCode;
using roadmaker::JunctionId;
using roadmaker::LaneType;
using roadmaker::RoadId;
using roadmaker::RoadMarkType;

namespace {

std::filesystem::path sample(const char* name) {
  return std::filesystem::path(RM_SAMPLES_DIR) / name;
}

bool has_warning_containing(const std::vector<roadmaker::Diagnostic>& diagnostics,
                            std::string_view needle) {
  return std::ranges::any_of(diagnostics, [&](const roadmaker::Diagnostic& d) {
    return d.message.find(needle) != std::string::npos;
  });
}

} // namespace

TEST(XodrReader, StraightRoadSampleParsesWithoutErrors) {
  const auto result = roadmaker::load_xodr(sample("straight_road.xodr"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(roadmaker::count_errors(result->diagnostics), 0U);
  EXPECT_NEAR(result->revision, 1.7, 1e-9);

  const roadmaker::RoadNetwork& network = result->network;
  ASSERT_EQ(network.road_count(), 1U);

  const RoadId road_id = network.find_road("1");
  ASSERT_TRUE(road_id.is_valid());
  const roadmaker::Road& road = *network.road(road_id);
  EXPECT_EQ(road.name, "Main Street");
  EXPECT_NEAR(road.length, 100.0, 1e-9);
  EXPECT_EQ(road.plan_view.records().size(), 1U);
  EXPECT_EQ(road.elevation.size(), 1U);
  ASSERT_EQ(road.sections.size(), 1U);

  const roadmaker::LaneSection& section = *network.lane_section(road.sections[0]);
  ASSERT_EQ(section.lanes.size(), 5U);

  // Leftmost first: sidewalk(2), driving(1), center(0), driving(-1), shoulder(-2).
  const roadmaker::Lane& sidewalk = *network.lane(section.lanes.front());
  EXPECT_EQ(sidewalk.odr_id, 2);
  EXPECT_EQ(sidewalk.type, LaneType::Sidewalk);
  ASSERT_FALSE(sidewalk.widths.empty());
  EXPECT_NEAR(sidewalk.widths.at(0).a, 2.0, 1e-12);

  const roadmaker::Lane& center = *network.lane(section.lanes[2]);
  EXPECT_EQ(center.odr_id, 0);
  ASSERT_EQ(center.road_marks.size(), 1U);
  EXPECT_EQ(center.road_marks[0].type, RoadMarkType::Broken);
}

TEST(XodrReader, CurvedRoadSampleParsesAllFourGeometryPrimitives) {
  const auto result = roadmaker::load_xodr(sample("curved_road.xodr"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(roadmaker::count_errors(result->diagnostics), 0U);

  const roadmaker::RoadNetwork& network = result->network;
  const roadmaker::Road& road = *network.road(network.find_road("1"));

  const auto& records = road.plan_view.records();
  ASSERT_EQ(records.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<roadmaker::LineGeom>(records[0].shape));
  EXPECT_TRUE(std::holds_alternative<roadmaker::SpiralGeom>(records[1].shape));
  EXPECT_TRUE(std::holds_alternative<roadmaker::ArcGeom>(records[2].shape));
  EXPECT_TRUE(std::holds_alternative<roadmaker::SpiralGeom>(records[3].shape));

  EXPECT_EQ(road.elevation.size(), 2U);
  EXPECT_EQ(road.superelevation.size(), 1U);

  // Spiral end curvature meets the arc curvature (G2 at that joint).
  const auto joint = road.plan_view.evaluate(50.0 + 1e-9);
  EXPECT_NEAR(joint.curvature, 0.05, 1e-6);
}

TEST(XodrReader, TJunctionSampleResolvesJunctionTopology) {
  const auto result = roadmaker::load_xodr(sample("t_junction.xodr"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(roadmaker::count_errors(result->diagnostics), 0U);

  const roadmaker::RoadNetwork& network = result->network;
  EXPECT_EQ(network.road_count(), 5U);
  ASSERT_EQ(network.junction_count(), 1U);

  const JunctionId junction_id = network.find_junction("100");
  ASSERT_TRUE(junction_id.is_valid());
  const roadmaker::Junction& junction = *network.junction(junction_id);
  ASSERT_EQ(junction.connections.size(), 2U);

  // Connecting roads carry the junction id and resolved links.
  const RoadId through_id = network.find_road("10");
  const roadmaker::Road& through = *network.road(through_id);
  EXPECT_EQ(through.junction, junction_id);
  ASSERT_TRUE(through.predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(through.predecessor->target), network.find_road("1"));
  EXPECT_EQ(through.predecessor->contact, ContactPoint::End);

  // The west approach's successor is the junction itself.
  const roadmaker::Road& west = *network.road(network.find_road("1"));
  ASSERT_TRUE(west.successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(west.successor->target), junction_id);

  // Lane links parsed.
  EXPECT_EQ(junction.connections[0].lane_links, (std::vector<std::pair<int, int>>{{-1, -1}}));
}

TEST(XodrReader, MalformedXmlIsAStructuralError) {
  const auto result = roadmaker::parse_xodr("<OpenDRIVE><road", "bad");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::MalformedXml);
}

TEST(XodrReader, NonOpenDriveXmlIsAStructuralError) {
  const auto result = roadmaker::parse_xodr("<gpx></gpx>", "bad");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::InvalidDocument);
}

TEST(XodrReader, MissingFileReportsFileNotFound) {
  const auto result = roadmaker::load_xodr(sample("does_not_exist.xodr"));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::FileNotFound);
}

TEST(XodrReader, UnsupportedElementsWarnOnceAndAreNeverSilent) {
  constexpr const char* kXml = R"(<?xml version="1.0"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="10">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry>
    </planView>
    <objects/>
    <signals/>
    <lanes>
      <laneSection s="0">
        <center><lane id="0" type="none"/></center>
        <right><lane id="-1" type="hoverlane">
          <width sOffset="0" a="3" b="0" c="0" d="0"/>
        </lane></right>
      </laneSection>
    </lanes>
  </road>
  <station/>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(kXml);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "<objects>"));
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "<signals>"));
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "<station>"));
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "hoverlane"));

  // The unknown lane type still parsed as Other — not dropped.
  const roadmaker::RoadNetwork& network = result->network;
  const roadmaker::Road& road = *network.road(network.find_road("1"));
  const roadmaker::LaneSection& section = *network.lane_section(road.sections[0]);
  EXPECT_EQ(network.lane(section.lanes.back())->type, LaneType::Other);
}

TEST(XodrReader, BadNumbersFallBackWithADiagnostic) {
  constexpr const char* kXml = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="banana">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry>
    </planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(kXml);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "banana"));
  // Geometry length wins regardless.
  const roadmaker::RoadNetwork& network = result->network;
  EXPECT_NEAR(network.road(network.find_road("1"))->length, 10.0, 1e-12);
}

TEST(XodrReader, CrlfLineEndingsParseFine) {
  const std::string xml =
      "<OpenDRIVE>\r\n<header revMajor=\"1\" revMinor=\"7\"/>\r\n"
      "<road id=\"1\" length=\"5\">\r\n<planView>\r\n"
      "<geometry s=\"0\" x=\"0\" y=\"0\" hdg=\"0\" length=\"5\"><line/></geometry>\r\n"
      "</planView>\r\n"
      "<lanes><laneSection s=\"0\"><center><lane id=\"0\" type=\"none\"/>"
      "</center></laneSection></lanes>\r\n</road>\r\n</OpenDRIVE>\r\n";
  const auto result = roadmaker::parse_xodr(xml);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->network.road_count(), 1U);
}

TEST(XodrReader, DuplicateRoadIdsAreRejectedWithAnErrorDiagnostic) {
  constexpr const char* kXml = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(kXml);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->network.road_count(), 1U);
  EXPECT_EQ(roadmaker::count_errors(result->diagnostics), 1U);
}

TEST(XodrReader, UnresolvableJunctionConnectionRoadsAreSkippedWithWarning) {
  constexpr const char* kXml = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <junction id="9" name="ghost">
    <connection id="0" incomingRoad="404" connectingRoad="405" contactPoint="start"/>
  </junction>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(kXml);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->network.junction_count(), 1U);
  const auto& junction = *result->network.junction(result->network.find_junction("9"));
  EXPECT_TRUE(junction.connections.empty());
  EXPECT_TRUE(has_warning_containing(result->diagnostics, "unknown road"));
}
