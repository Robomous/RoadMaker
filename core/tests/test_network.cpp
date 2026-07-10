#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneId;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::Poly3;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;

TEST_CASE("create and find roads by OpenDRIVE id", "[network]") {
  RoadNetwork network;
  const RoadId a = network.create_road("Main St", "1");
  network.create_road("Side St", "2");

  REQUIRE(network.road_count() == 2);
  REQUIRE(network.find_road("1") == a);
  REQUIRE(network.road(a)->name == "Main St");
  REQUIRE_FALSE(network.find_road("99").is_valid());
}

TEST_CASE("lane sections stay sorted by s0 and reject duplicates", "[network]") {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");

  const LaneSectionId s100 = network.add_lane_section(road, 100.0);
  const LaneSectionId s0 = network.add_lane_section(road, 0.0);
  const LaneSectionId s50 = network.add_lane_section(road, 50.0);
  REQUIRE(s100.is_valid());
  REQUIRE(s0.is_valid());
  REQUIRE(s50.is_valid());

  REQUIRE(network.road(road)->sections == std::vector<LaneSectionId>{s0, s50, s100});

  REQUIRE_FALSE(network.add_lane_section(road, 50.0).is_valid());    // duplicate s0
  REQUIRE_FALSE(network.add_lane_section(RoadId{}, 0.0).is_valid()); // stale road
  REQUIRE(network.lane_section_count() == 3);
}

TEST_CASE("lanes stay sorted leftmost-first and reject duplicates", "[network]") {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const LaneSectionId section = network.add_lane_section(road, 0.0);

  const LaneId right = network.add_lane(section, -1, LaneType::Driving);
  const LaneId left = network.add_lane(section, 1, LaneType::Driving);
  const LaneId center = network.add_lane(section, 0, LaneType::None);
  REQUIRE(right.is_valid());
  REQUIRE(left.is_valid());
  REQUIRE(center.is_valid());

  // Descending odr id: left (+1), center (0), right (-1).
  REQUIRE(network.lane_section(section)->lanes == std::vector<LaneId>{left, center, right});

  REQUIRE_FALSE(network.add_lane(section, -1, LaneType::Shoulder).is_valid()); // duplicate
  REQUIRE_FALSE(network.add_lane(LaneSectionId{}, 5, LaneType::Driving).is_valid());
  REQUIRE(network.lane_count() == 3);
}

TEST_CASE("erase_road cascades to sections, lanes, and junction connections", "[network]") {
  RoadNetwork network;
  const RoadId incoming = network.create_road("in", "1");
  const RoadId connecting = network.create_road("conn", "2");
  const RoadId unrelated = network.create_road("other", "3");

  const LaneSectionId section = network.add_lane_section(connecting, 0.0);
  const LaneId lane = network.add_lane(section, -1, LaneType::Driving);

  const JunctionId junction = network.create_junction("10", "X");
  network.road(connecting)->junction = junction;
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = connecting,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });

  REQUIRE(network.erase_road(connecting));

  REQUIRE(network.road(connecting) == nullptr);
  REQUIRE(network.lane_section(section) == nullptr);
  REQUIRE(network.lane(lane) == nullptr);
  REQUIRE(network.junction(junction)->connections.empty());
  REQUIRE(network.road(unrelated) != nullptr);
  REQUIRE_FALSE(network.erase_road(connecting)); // already gone
}

TEST_CASE("erase_junction detaches connecting roads", "[network]") {
  RoadNetwork network;
  const RoadId connecting = network.create_road("conn", "2");
  const JunctionId junction = network.create_junction("10", "X");
  network.road(connecting)->junction = junction;

  REQUIRE(network.erase_junction(junction));
  REQUIRE(network.junction(junction) == nullptr);
  REQUIRE(network.road(connecting) != nullptr);
  REQUIRE_FALSE(network.road(connecting)->junction.is_valid());
}

TEST_CASE("Poly3 evaluates OpenDRIVE cubics with local ds", "[poly3]") {
  // width(ds) = 1 + 2·ds + 3·ds² + 4·ds³ starting at s = 10.
  constexpr Poly3 poly{.s = 10.0, .a = 1.0, .b = 2.0, .c = 3.0, .d = 4.0};
  STATIC_REQUIRE(poly.eval(10.0) == 1.0);
  REQUIRE_THAT(poly.eval(11.0), Catch::Matchers::WithinAbs(10.0, 1e-12));
  REQUIRE_THAT(poly.eval_derivative(11.0), Catch::Matchers::WithinAbs(20.0, 1e-12));
}

TEST_CASE("eval_profile picks the record covering s", "[poly3]") {
  const std::vector<Poly3> profile{
      {.s = 0.0, .a = 1.0},
      {.s = 100.0, .a = 2.0},
  };
  REQUIRE(roadmaker::eval_profile(profile, 0.0) == 1.0);
  REQUIRE(roadmaker::eval_profile(profile, 99.9) == 1.0);
  REQUIRE(roadmaker::eval_profile(profile, 100.0) == 2.0);
  REQUIRE(roadmaker::eval_profile(profile, 500.0) == 2.0);
  REQUIRE(roadmaker::eval_profile({}, 5.0) == 0.0);
  // Query before the first record still evaluates the first record.
  REQUIRE(roadmaker::eval_profile(profile, -1.0) == 1.0);
}
