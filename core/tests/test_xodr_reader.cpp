#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <variant>

using Catch::Matchers::WithinAbs;
using roadmaker::ContactPoint;
using roadmaker::ErrorCode;
using roadmaker::JunctionId;
using roadmaker::LaneType;
using roadmaker::RoadId;
using roadmaker::RoadMarkType;
using roadmaker::Severity;

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

TEST_CASE("straight_road sample parses without errors", "[xodr]") {
  const auto result = roadmaker::load_xodr(sample("straight_road.xodr"));
  REQUIRE(result.has_value());
  REQUIRE(roadmaker::count_errors(result->diagnostics) == 0);
  REQUIRE_THAT(result->revision, WithinAbs(1.7, 1e-9));

  const roadmaker::RoadNetwork& network = result->network;
  REQUIRE(network.road_count() == 1);

  const RoadId road_id = network.find_road("1");
  REQUIRE(road_id.is_valid());
  const roadmaker::Road& road = *network.road(road_id);
  REQUIRE(road.name == "Main Street");
  REQUIRE_THAT(road.length, WithinAbs(100.0, 1e-9));
  REQUIRE(road.plan_view.records().size() == 1);
  REQUIRE(road.elevation.size() == 1);
  REQUIRE(road.sections.size() == 1);

  const roadmaker::LaneSection& section = *network.lane_section(road.sections[0]);
  REQUIRE(section.lanes.size() == 5);

  // Leftmost first: sidewalk(2), driving(1), center(0), driving(-1), shoulder(-2).
  const roadmaker::Lane& sidewalk = *network.lane(section.lanes.front());
  REQUIRE(sidewalk.odr_id == 2);
  REQUIRE(sidewalk.type == LaneType::Sidewalk);
  REQUIRE_THAT(sidewalk.widths.at(0).a, WithinAbs(2.0, 1e-12));

  const roadmaker::Lane& center = *network.lane(section.lanes[2]);
  REQUIRE(center.odr_id == 0);
  REQUIRE(center.road_marks.size() == 1);
  REQUIRE(center.road_marks[0].type == RoadMarkType::Broken);
}

TEST_CASE("curved_road sample parses all four geometry primitives", "[xodr]") {
  const auto result = roadmaker::load_xodr(sample("curved_road.xodr"));
  REQUIRE(result.has_value());
  REQUIRE(roadmaker::count_errors(result->diagnostics) == 0);

  const roadmaker::RoadNetwork& network = result->network;
  const roadmaker::Road& road = *network.road(network.find_road("1"));

  const auto& records = road.plan_view.records();
  REQUIRE(records.size() == 4);
  REQUIRE(std::holds_alternative<roadmaker::LineGeom>(records[0].shape));
  REQUIRE(std::holds_alternative<roadmaker::SpiralGeom>(records[1].shape));
  REQUIRE(std::holds_alternative<roadmaker::ArcGeom>(records[2].shape));
  REQUIRE(std::holds_alternative<roadmaker::SpiralGeom>(records[3].shape));

  REQUIRE(road.elevation.size() == 2);
  REQUIRE(road.superelevation.size() == 1);

  // Spiral end curvature meets the arc curvature (G2 at that joint).
  const auto joint = road.plan_view.evaluate(50.0 + 1e-9);
  REQUIRE_THAT(joint.curvature, WithinAbs(0.05, 1e-6));
}

TEST_CASE("t_junction sample resolves junction topology", "[xodr]") {
  const auto result = roadmaker::load_xodr(sample("t_junction.xodr"));
  REQUIRE(result.has_value());
  REQUIRE(roadmaker::count_errors(result->diagnostics) == 0);

  const roadmaker::RoadNetwork& network = result->network;
  REQUIRE(network.road_count() == 5);
  REQUIRE(network.junction_count() == 1);

  const JunctionId junction_id = network.find_junction("100");
  REQUIRE(junction_id.is_valid());
  const roadmaker::Junction& junction = *network.junction(junction_id);
  REQUIRE(junction.connections.size() == 2);

  // Connecting roads carry the junction id and resolved links.
  const RoadId through_id = network.find_road("10");
  const roadmaker::Road& through = *network.road(through_id);
  REQUIRE(through.junction == junction_id);
  REQUIRE(through.predecessor.has_value());
  REQUIRE(std::get<RoadId>(through.predecessor->target) == network.find_road("1"));
  REQUIRE(through.predecessor->contact == ContactPoint::End);

  // The west approach's successor is the junction itself.
  const roadmaker::Road& west = *network.road(network.find_road("1"));
  REQUIRE(west.successor.has_value());
  REQUIRE(std::get<JunctionId>(west.successor->target) == junction_id);

  // Lane links parsed.
  REQUIRE(junction.connections[0].lane_links == std::vector<std::pair<int, int>>{{-1, -1}});
}

TEST_CASE("malformed XML is a structural error", "[xodr]") {
  const auto result = roadmaker::parse_xodr("<OpenDRIVE><road", "bad");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == ErrorCode::MalformedXml);
}

TEST_CASE("non-OpenDRIVE XML is a structural error", "[xodr]") {
  const auto result = roadmaker::parse_xodr("<gpx></gpx>", "bad");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == ErrorCode::InvalidDocument);
}

TEST_CASE("missing file reports FileNotFound", "[xodr]") {
  const auto result = roadmaker::load_xodr(sample("does_not_exist.xodr"));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == ErrorCode::FileNotFound);
}

TEST_CASE("unsupported elements warn once and are never silent", "[xodr]") {
  constexpr const char* xml = R"(<?xml version="1.0"?>
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
  const auto result = roadmaker::parse_xodr(xml);
  REQUIRE(result.has_value());
  REQUIRE(has_warning_containing(result->diagnostics, "<objects>"));
  REQUIRE(has_warning_containing(result->diagnostics, "<signals>"));
  REQUIRE(has_warning_containing(result->diagnostics, "<station>"));
  REQUIRE(has_warning_containing(result->diagnostics, "hoverlane"));

  // The unknown lane type still parsed as Other — not dropped.
  const roadmaker::RoadNetwork& network = result->network;
  const roadmaker::Road& road = *network.road(network.find_road("1"));
  const roadmaker::LaneSection& section = *network.lane_section(road.sections[0]);
  REQUIRE(network.lane(section.lanes.back())->type == LaneType::Other);
}

TEST_CASE("bad numbers fall back with a diagnostic", "[xodr]") {
  constexpr const char* xml = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="banana">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry>
    </planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(xml);
  REQUIRE(result.has_value());
  REQUIRE(has_warning_containing(result->diagnostics, "banana"));
  // Geometry length wins regardless.
  const roadmaker::RoadNetwork& network = result->network;
  REQUIRE_THAT(network.road(network.find_road("1"))->length, WithinAbs(10.0, 1e-12));
}

TEST_CASE("CRLF line endings parse fine", "[xodr]") {
  std::string xml =
      "<OpenDRIVE>\r\n<header revMajor=\"1\" revMinor=\"7\"/>\r\n"
      "<road id=\"1\" length=\"5\">\r\n<planView>\r\n"
      "<geometry s=\"0\" x=\"0\" y=\"0\" hdg=\"0\" length=\"5\"><line/></geometry>\r\n"
      "</planView>\r\n"
      "<lanes><laneSection s=\"0\"><center><lane id=\"0\" type=\"none\"/>"
      "</center></laneSection></lanes>\r\n</road>\r\n</OpenDRIVE>\r\n";
  const auto result = roadmaker::parse_xodr(xml);
  REQUIRE(result.has_value());
  REQUIRE(result->network.road_count() == 1);
}

TEST_CASE("duplicate road ids are rejected with an error diagnostic", "[xodr]") {
  constexpr const char* xml = R"(<OpenDRIVE>
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
  const auto result = roadmaker::parse_xodr(xml);
  REQUIRE(result.has_value());
  REQUIRE(result->network.road_count() == 1);
  REQUIRE(roadmaker::count_errors(result->diagnostics) == 1);
}

TEST_CASE("unresolvable junction connection roads are skipped with warning", "[xodr]") {
  constexpr const char* xml = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <junction id="9" name="ghost">
    <connection id="0" incomingRoad="404" connectingRoad="405" contactPoint="start"/>
  </junction>
</OpenDRIVE>)";
  const auto result = roadmaker::parse_xodr(xml);
  REQUIRE(result.has_value());
  REQUIRE(result->network.junction_count() == 1);
  const auto& junction = *result->network.junction(result->network.find_junction("9"));
  REQUIRE(junction.connections.empty());
  REQUIRE(has_warning_containing(result->diagnostics, "unknown road"));
}
