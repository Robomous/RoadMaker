// Round-trip invariants are first-class tests: author → write → re-parse →
// compare within rm::tol (position 1e-4 m, heading 1e-6 rad).

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinAbs;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

namespace {

/// Compares two roads' plan-view geometry by dense sampling.
void require_same_geometry(const roadmaker::Road& a, const roadmaker::Road& b) {
  REQUIRE_THAT(a.plan_view.length(),
               WithinAbs(b.plan_view.length(), roadmaker::tol::kRoundTripPosition));
  const double length = a.plan_view.length();
  constexpr int kSamples = 200;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = length * i / kSamples;
    const auto pa = a.plan_view.evaluate(s);
    const auto pb = b.plan_view.evaluate(s);
    CAPTURE(s);
    REQUIRE_THAT(pa.x, WithinAbs(pb.x, roadmaker::tol::kRoundTripPosition));
    REQUIRE_THAT(pa.y, WithinAbs(pb.y, roadmaker::tol::kRoundTripPosition));
    REQUIRE_THAT(std::remainder(pa.hdg - pb.hdg, 2.0 * 3.141592653589793),
                 WithinAbs(0.0, roadmaker::tol::kRoundTripHeading));
  }
}

} // namespace

TEST_CASE("authored clothoid road is G1 continuous", "[authoring]") {
  RoadNetwork network;
  const std::array<Waypoint, 4> waypoints{
      Waypoint{0.0, 0.0}, Waypoint{50.0, 10.0}, Waypoint{90.0, 50.0}, Waypoint{100.0, 100.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "Test Road", "1");
  REQUIRE(road_id.has_value());

  const roadmaker::Road& road = *network.road(*road_id);
  REQUIRE(road.plan_view.records().size() >= 3);
  REQUIRE(road.length > 100.0); // longer than the straight-line chain

  // The fitted path passes through every waypoint (record joints).
  // build_G1 makes one clothoid per waypoint pair.
  const auto& records = road.plan_view.records();
  REQUIRE(records.size() == waypoints.size() - 1);
  for (std::size_t i = 0; i < records.size(); ++i) {
    REQUIRE_THAT(records[i].x, WithinAbs(waypoints[i].x, 1e-9));
    REQUIRE_THAT(records[i].y, WithinAbs(waypoints[i].y, 1e-9));
  }
  const auto end = road.plan_view.evaluate(road.length);
  REQUIRE_THAT(end.x, WithinAbs(waypoints.back().x, 1e-6));
  REQUIRE_THAT(end.y, WithinAbs(waypoints.back().y, 1e-6));

  // G1 continuity at every joint.
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto before = road.plan_view.evaluate(records[i].s - 1e-9);
    const auto after = road.plan_view.evaluate(records[i].s + 1e-9);
    REQUIRE_THAT(before.x, WithinAbs(after.x, roadmaker::tol::kRoundTripPosition));
    REQUIRE_THAT(before.y, WithinAbs(after.y, roadmaker::tol::kRoundTripPosition));
    REQUIRE_THAT(before.hdg, WithinAbs(after.hdg, 1e-6));
  }

  // Lane structure per the default profile: 0, +1, -1, -2.
  const roadmaker::LaneSection& section = *network.lane_section(road.sections.at(0));
  REQUIRE(section.lanes.size() == 4);
}

TEST_CASE("authoring rejects bad input", "[authoring]") {
  RoadNetwork network;
  const LaneProfile profile = LaneProfile::two_lane_default();

  REQUIRE_FALSE(
      roadmaker::author_clothoid_road(network, std::array<Waypoint, 1>{Waypoint{0, 0}}, profile)
          .has_value());
  REQUIRE_FALSE(roadmaker::author_clothoid_road(
                    network, std::array<Waypoint, 2>{Waypoint{1, 1}, Waypoint{1, 1}}, profile)
                    .has_value());
  REQUIRE_FALSE(roadmaker::author_clothoid_road(
                    network, std::array<Waypoint, 2>{Waypoint{0, 0}, Waypoint{9, 9}}, LaneProfile{})
                    .has_value());
}

TEST_CASE("author -> write -> parse round-trips within tolerance", "[roundtrip]") {
  RoadNetwork authored;
  const std::array<Waypoint, 5> waypoints{Waypoint{0.0, 0.0},
                                          Waypoint{40.0, 5.0},
                                          Waypoint{80.0, 30.0},
                                          Waypoint{100.0, 70.0},
                                          Waypoint{90.0, 120.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      authored, waypoints, LaneProfile::two_lane_default(), "Loop", "7");
  REQUIRE(road_id.has_value());

  const auto xml = roadmaker::write_xodr(authored, "round_trip");
  REQUIRE(xml.has_value());

  const auto reparsed = roadmaker::parse_xodr(*xml, "round_trip");
  REQUIRE(reparsed.has_value());
  REQUIRE(roadmaker::count_errors(reparsed->diagnostics) == 0);
  REQUIRE(reparsed->network.road_count() == 1);

  const roadmaker::Road& original = *authored.road(*road_id);
  const roadmaker::Road& round = *reparsed->network.road(reparsed->network.find_road("7"));
  require_same_geometry(original, round);

  // Lane structure survives.
  const auto& section_a = *authored.lane_section(original.sections[0]);
  const auto& section_b = *reparsed->network.lane_section(round.sections[0]);
  REQUIRE(section_a.lanes.size() == section_b.lanes.size());
  for (std::size_t i = 0; i < section_a.lanes.size(); ++i) {
    const auto& lane_a = *authored.lane(section_a.lanes[i]);
    const auto& lane_b = *reparsed->network.lane(section_b.lanes[i]);
    REQUIRE(lane_a.odr_id == lane_b.odr_id);
    REQUIRE(lane_a.type == lane_b.type);
    REQUIRE(lane_a.widths.size() == lane_b.widths.size());
    REQUIRE(lane_a.road_marks.size() == lane_b.road_marks.size());
  }
}

TEST_CASE("parsed sample -> write -> parse preserves topology and geometry", "[roundtrip]") {
  auto first = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  REQUIRE(first.has_value());

  const auto xml = roadmaker::write_xodr(first->network, "t_junction");
  REQUIRE(xml.has_value());
  const auto second = roadmaker::parse_xodr(*xml, "rewritten");
  REQUIRE(second.has_value());
  REQUIRE(roadmaker::count_errors(second->diagnostics) == 0);

  REQUIRE(second->network.road_count() == first->network.road_count());
  REQUIRE(second->network.junction_count() == first->network.junction_count());

  first->network.for_each_road([&](RoadId, const roadmaker::Road& road) {
    const RoadId other_id = second->network.find_road(road.odr_id);
    REQUIRE(other_id.is_valid());
    require_same_geometry(road, *second->network.road(other_id));
  });

  // Junction connections survive with lane links.
  const auto j1 = *first->network.junction(first->network.find_junction("100"));
  const auto j2 = *second->network.junction(second->network.find_junction("100"));
  REQUIRE(j1.connections.size() == j2.connections.size());
  REQUIRE(j2.connections[0].lane_links == j1.connections[0].lane_links);
}

TEST_CASE("writer refuses invalid networks", "[writer]") {
  RoadNetwork network;
  network.create_road("empty", "1"); // no geometry, no sections
  const auto result = roadmaker::write_xodr(network);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == roadmaker::ErrorCode::InvalidArgument);
}

TEST_CASE("writer refuses discontinuous geometry", "[writer]") {
  RoadNetwork network;
  const RoadId road_id = network.create_road("broken", "1");
  roadmaker::Road& road = *network.road(road_id);
  road.plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 10.0, .shape = roadmaker::LineGeom{}});
  road.plan_view.append(
      {.x = 999.0, .y = 0.0, .hdg = 0.0, .length = 10.0, .shape = roadmaker::LineGeom{}});
  network.add_lane_section(road_id, 0.0);

  const auto result = roadmaker::write_xodr(network);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("discontinuity") != std::string::npos);
}

TEST_CASE("writer refuses dangling lane links", "[writer]") {
  RoadNetwork network;
  const RoadId road_id = network.create_road("links", "1");
  roadmaker::Road& road = *network.road(road_id);
  road.plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 50.0, .shape = roadmaker::LineGeom{}});
  const auto s0 = network.add_lane_section(road_id, 0.0);
  const auto s1 = network.add_lane_section(road_id, 25.0);
  const auto lane = network.add_lane(s0, -1, roadmaker::LaneType::Driving);
  network.add_lane(s1, -1, roadmaker::LaneType::Driving);
  network.lane(lane)->successor = -5; // does not exist in next section

  const auto result = roadmaker::write_xodr(network);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("successor") != std::string::npos);
}
