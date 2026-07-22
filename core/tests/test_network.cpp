// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

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

TEST(Network, CreateAndFindRoadsByOpenDriveId) {
  RoadNetwork network;
  const RoadId a = network.create_road("Main St", "1");
  network.create_road("Side St", "2");

  EXPECT_EQ(network.road_count(), 2U);
  EXPECT_EQ(network.find_road("1"), a);
  ASSERT_NE(network.road(a), nullptr);
  EXPECT_EQ(network.road(a)->name, "Main St");
  EXPECT_FALSE(network.find_road("99").is_valid());
}

TEST(Network, LaneSectionsStaySortedByS0AndRejectDuplicates) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");

  const LaneSectionId s100 = network.add_lane_section(road, 100.0);
  const LaneSectionId s0 = network.add_lane_section(road, 0.0);
  const LaneSectionId s50 = network.add_lane_section(road, 50.0);
  ASSERT_TRUE(s100.is_valid());
  ASSERT_TRUE(s0.is_valid());
  ASSERT_TRUE(s50.is_valid());

  EXPECT_EQ(network.road(road)->sections, (std::vector<LaneSectionId>{s0, s50, s100}));

  EXPECT_FALSE(network.add_lane_section(road, 50.0).is_valid());    // duplicate s0
  EXPECT_FALSE(network.add_lane_section(RoadId{}, 0.0).is_valid()); // stale road
  EXPECT_EQ(network.lane_section_count(), 3U);
}

TEST(Network, LanesStaySortedLeftmostFirstAndRejectDuplicates) {
  RoadNetwork network;
  const RoadId road = network.create_road("r", "1");
  const LaneSectionId section = network.add_lane_section(road, 0.0);

  const LaneId right = network.add_lane(section, -1, LaneType::Driving);
  const LaneId left = network.add_lane(section, 1, LaneType::Driving);
  const LaneId center = network.add_lane(section, 0, LaneType::None);
  ASSERT_TRUE(right.is_valid());
  ASSERT_TRUE(left.is_valid());
  ASSERT_TRUE(center.is_valid());

  // Descending odr id: left (+1), center (0), right (-1).
  EXPECT_EQ(network.lane_section(section)->lanes, (std::vector<LaneId>{left, center, right}));

  EXPECT_FALSE(network.add_lane(section, -1, LaneType::Shoulder).is_valid()); // duplicate
  EXPECT_FALSE(network.add_lane(LaneSectionId{}, 5, LaneType::Driving).is_valid());
  EXPECT_EQ(network.lane_count(), 3U);
}

TEST(Network, EraseRoadCascadesToSectionsLanesAndJunctionConnections) {
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

  EXPECT_TRUE(network.erase_road(connecting));

  EXPECT_EQ(network.road(connecting), nullptr);
  EXPECT_EQ(network.lane_section(section), nullptr);
  EXPECT_EQ(network.lane(lane), nullptr);
  EXPECT_TRUE(network.junction(junction)->connections.empty());
  EXPECT_NE(network.road(unrelated), nullptr);
  EXPECT_FALSE(network.erase_road(connecting)); // already gone
}

TEST(Network, ExactEraseDoesNotCascadeAndRestoreResurrectsOriginalIds) {
  RoadNetwork network;
  const RoadId incoming = network.create_road("in", "1");
  const RoadId doomed = network.create_road("victim", "2");
  const LaneSectionId section = network.add_lane_section(doomed, 0.0);
  const LaneId lane = network.add_lane(section, -1, LaneType::Driving);

  const JunctionId junction = network.create_junction("10", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = doomed,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });

  // The command layer snapshots values, then exact-erases leaf-to-root.
  const roadmaker::Lane lane_copy = *network.lane(lane);
  const roadmaker::LaneSection section_copy = *network.lane_section(section);
  const roadmaker::Road road_copy = *network.road(doomed);

  ASSERT_TRUE(network.erase_lane_exact(lane).has_value());
  ASSERT_TRUE(network.erase_lane_section_exact(section).has_value());
  ASSERT_TRUE(network.erase_road_exact(doomed).has_value());

  // No cascade, no fix-ups: the junction still holds the (now dangling)
  // connection — restoring must make it valid again.
  ASSERT_NE(network.junction(junction), nullptr);
  ASSERT_EQ(network.junction(junction)->connections.size(), 1U);

  ASSERT_TRUE(network.restore_road(doomed, road_copy).has_value());
  ASSERT_TRUE(network.restore_lane_section(section, section_copy).has_value());
  ASSERT_TRUE(network.restore_lane(lane, lane_copy).has_value());

  // Ids held by other objects resolve again — same index, same generation.
  const JunctionConnection& connection = network.junction(junction)->connections[0];
  ASSERT_NE(network.road(connection.connecting_road), nullptr);
  EXPECT_EQ(network.road(connection.connecting_road)->name, "victim");
  EXPECT_EQ(network.road(doomed)->sections, (std::vector<LaneSectionId>{section}));
  EXPECT_EQ(network.lane_section(section)->lanes, (std::vector<LaneId>{lane}));
}

TEST(Network, RestoreJunctionKeepsRoadBackReferenceValid) {
  RoadNetwork network;
  const RoadId connecting = network.create_road("conn", "2");
  const JunctionId junction = network.create_junction("10", "X");
  network.road(connecting)->junction = junction;

  const roadmaker::Junction junction_copy = *network.junction(junction);
  ASSERT_TRUE(network.erase_junction_exact(junction).has_value());
  // Non-cascading: the road still points at the erased junction.
  EXPECT_EQ(network.road(connecting)->junction, junction);

  ASSERT_TRUE(network.restore_junction(junction, junction_copy).has_value());
  ASSERT_NE(network.junction(network.road(connecting)->junction), nullptr);
  EXPECT_EQ(network.junction(junction)->odr_id, "10");
}

TEST(Network, EraseJunctionDetachesConnectingRoads) {
  RoadNetwork network;
  const RoadId connecting = network.create_road("conn", "2");
  const JunctionId junction = network.create_junction("10", "X");
  network.road(connecting)->junction = junction;

  EXPECT_TRUE(network.erase_junction(junction));
  EXPECT_EQ(network.junction(junction), nullptr);
  ASSERT_NE(network.road(connecting), nullptr);
  EXPECT_FALSE(network.road(connecting)->junction.is_valid());
}

TEST(Network, JunctionsTouchingFindsEveryParticipationKind) {
  RoadNetwork network;
  const RoadId incoming = network.create_road("in", "1");
  const RoadId connecting = network.create_road("conn", "2");
  const RoadId linked = network.create_road("linked", "3");
  const RoadId bystander = network.create_road("far", "4");

  const JunctionId junction = network.create_junction("10", "X");
  network.road(connecting)->junction = junction;
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = connecting,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });
  network.road(linked)->successor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};

  EXPECT_EQ(roadmaker::junctions_touching(network, incoming), (std::vector<JunctionId>{junction}));
  // The connecting road participates twice (back-ref + connection) but is
  // reported once.
  EXPECT_EQ(roadmaker::junctions_touching(network, connecting),
            (std::vector<JunctionId>{junction}));
  EXPECT_EQ(roadmaker::junctions_touching(network, linked), (std::vector<JunctionId>{junction}));
  EXPECT_TRUE(roadmaker::junctions_touching(network, bystander).empty());
  EXPECT_TRUE(roadmaker::junctions_touching(network, RoadId{}).empty());
}

TEST(Poly3, EvaluatesOpenDriveCubicsWithLocalDs) {
  // width(ds) = 1 + 2·ds + 3·ds² + 4·ds³ starting at s = 10.
  constexpr Poly3 kPoly{.s = 10.0, .a = 1.0, .b = 2.0, .c = 3.0, .d = 4.0};
  static_assert(kPoly.eval(10.0) == 1.0);
  EXPECT_NEAR(kPoly.eval(11.0), 10.0, 1e-12);
  EXPECT_NEAR(kPoly.eval_derivative(11.0), 20.0, 1e-12);
}

TEST(Poly3, EvalProfilePicksTheRecordCoveringS) {
  const std::vector<Poly3> profile{
      {.s = 0.0, .a = 1.0},
      {.s = 100.0, .a = 2.0},
  };
  EXPECT_EQ(roadmaker::eval_profile(profile, 0.0), 1.0);
  EXPECT_EQ(roadmaker::eval_profile(profile, 99.9), 1.0);
  EXPECT_EQ(roadmaker::eval_profile(profile, 100.0), 2.0);
  EXPECT_EQ(roadmaker::eval_profile(profile, 500.0), 2.0);
  EXPECT_EQ(roadmaker::eval_profile({}, 5.0), 0.0);
  // Query before the first record still evaluates the first record.
  EXPECT_EQ(roadmaker::eval_profile(profile, -1.0), 1.0);
}
