#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMark;
using roadmaker::RoadMarkType;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

RoadId author_default(RoadNetwork& network, const char* odr_id, double y_offset = 0.0) {
  return author(network,
                {Waypoint{.x = 0.0, .y = y_offset},
                 Waypoint{.x = 60.0, .y = y_offset + 8.0},
                 Waypoint{.x = 120.0, .y = y_offset}},
                odr_id);
}

/// The §8 round-trip oracle for every factory: apply changes the document,
/// revert restores it byte-identically, re-apply reproduces the applied
/// state byte-identically, and a final revert leaves it pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  {
    SCOPED_TRACE("first apply");
    ASSERT_TRUE(command.apply(network).has_value());
  }
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after); // a command that changes nothing is a bug
  {
    SCOPED_TRACE("revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
  {
    SCOPED_TRACE("re-apply (idempotence)");
    ASSERT_TRUE(command.apply(network).has_value());
    expect_network_matches(network, after);
  }
  {
    SCOPED_TRACE("final revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
}

/// A failed apply must leave the serialized network untouched.
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

} // namespace

// --- document / lane value edits ---------------------------------------------

TEST(EditOperations, RenameRoadRoundTripsAndRenames) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  auto command = roadmaker::edit::rename_road(network, road, "Main Street");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->name, "Main Street");
  EXPECT_FALSE(command->dirty().topology);
}

TEST(EditOperations, SetLaneTypeWidthAndMarkRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();

  auto set_type = roadmaker::edit::set_lane_type(network, outer_right, LaneType::Parking);
  expect_command_round_trip(network, *set_type);

  auto set_width = roadmaker::edit::set_lane_width(network, outer_right, 2.75);
  expect_command_round_trip(network, *set_width);

  auto set_mark = roadmaker::edit::set_road_mark(
      network, outer_right, RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid, .width = 0.25});
  expect_command_round_trip(network, *set_mark);

  ASSERT_TRUE(set_width->apply(network).has_value());
  EXPECT_NEAR(network.lane(outer_right)->widths.at(0).a, 2.75, 1e-12);
}

TEST(EditOperations, LaneEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  LaneId center;
  for (const LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      center = lane_id;
    }
  }

  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, center, 3.0));
  const LaneId outer = network.lane_section(section)->lanes.back();
  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, outer, 0.0));
  expect_command_rejected(network,
                          roadmaker::edit::set_lane_type(network, LaneId{}, LaneType::Driving));
}

// --- waypoint edits ------------------------------------------------------------

TEST(EditOperations, MoveWaypointRefitsAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->length;

  auto command = roadmaker::edit::move_waypoint(network, road, 1, Waypoint{.x = 60.0, .y = 30.0});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NE(network.road(road)->length, old_length);
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_NEAR(network.road(road)->authoring_waypoints->at(1).y, 30.0, 1e-12);
  // The fitted path still passes through the moved waypoint.
  const auto& records = network.road(road)->plan_view.records();
  EXPECT_NEAR(records.at(1).x, 60.0, 1e-9);
  EXPECT_NEAR(records.at(1).y, 30.0, 1e-9);
}

TEST(EditOperations, InsertAndDeleteWaypointRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto insert = roadmaker::edit::insert_waypoint(network, road, 1, Waypoint{.x = 30.0, .y = 20.0});
  expect_command_round_trip(network, *insert);

  auto erase = roadmaker::edit::delete_waypoint(network, road, 1);
  expect_command_round_trip(network, *erase);
}

TEST(EditOperations, WaypointEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");

  expect_command_rejected(network, roadmaker::edit::move_waypoint(network, road, 5, {}));
  // 2 waypoints is the minimum — deleting one must fail.
  expect_command_rejected(network, roadmaker::edit::delete_waypoint(network, road, 0));
  // Moving onto the neighbor creates coincident waypoints.
  expect_command_rejected(
      network, roadmaker::edit::move_waypoint(network, road, 0, Waypoint{.x = 50.0, .y = 0.0}));
}

TEST(EditOperations, FirstEditOfForeignRoadDerivesWaypoints) {
  // Build a road without authoring waypoints (as loaded from a foreign file).
  RoadNetwork network;
  const RoadId road = network.create_road("foreign", "1");
  network.road(road)->plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 40.0, .shape = roadmaker::LineGeom{}});
  network.road(road)->plan_view.append({.x = 40.0,
                                        .y = 0.0,
                                        .hdg = 0.0,
                                        .length = 30.0,
                                        .shape = roadmaker::ArcGeom{.curvature = 0.01}});
  network.road(road)->length = network.road(road)->plan_view.length();
  const LaneSectionId section = network.add_lane_section(road, 0.0);
  network.add_lane(section, 0, LaneType::None);
  network.add_lane(section, -1, LaneType::Driving);
  network.lane(network.lane_section(section)->lanes.back())->widths.push_back({.a = 3.5});

  auto command = roadmaker::edit::move_waypoint(network, road, 2, Waypoint{.x = 75.0, .y = 8.0});
  ASSERT_TRUE(command->apply(network).has_value());
  // Derived waypoints: 2 record starts + endpoint.
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_EQ(network.road(road)->authoring_waypoints->size(), 3U);
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_FALSE(network.road(road)->authoring_waypoints.has_value());
}

TEST(EditOperations, InsertWaypointOnCurvePreservesEndpoints) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const roadmaker::PathPoint start = network.road(road)->plan_view.evaluate(0.0);
  const roadmaker::PathPoint end =
      network.road(road)->plan_view.evaluate(network.road(road)->length);

  // Insert ON the fitted curve, midway through the first segment — the Edit
  // Nodes midpoint-marker flow (02 §3). Stations come from the public helper.
  const auto stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(stations.has_value());
  ASSERT_EQ(stations->size(), 3U);
  const double mid_s = (stations->at(0) + stations->at(1)) / 2.0;
  const roadmaker::PathPoint mid = network.road(road)->plan_view.evaluate(mid_s);

  auto command =
      roadmaker::edit::insert_waypoint(network, road, 1, Waypoint{.x = mid.x, .y = mid.y});
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(roadmaker::edit::effective_waypoints(*network.road(road)).size(), 4U);

  const roadmaker::ReferenceLine& line = network.road(road)->plan_view;
  EXPECT_NEAR(line.evaluate(0.0).x, start.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(0.0).y, start.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(line.length()).x, end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(line.length()).y, end.y, roadmaker::tol::kRoundTripPosition);

  // The re-fit interpolates the inserted node at its (new) station.
  const auto refit_stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(refit_stations.has_value());
  ASSERT_EQ(refit_stations->size(), 4U);
  EXPECT_NEAR(line.evaluate(refit_stations->at(1)).x, mid.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(line.evaluate(refit_stations->at(1)).y, mid.y, roadmaker::tol::kRoundTripPosition);
}

TEST(EditOperations, ForeignRoadEditGoldenCurvedRoad) {
  // The issue #10 golden: load the line→spiral→arc→spiral fixture (no
  // rm:waypoints), edit a node, save — derived-waypoint re-fit and the
  // written file stay within tolerance of the original geometry.
  auto loaded = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "curved_road.xodr");
  ASSERT_TRUE(loaded.has_value());
  RoadNetwork& network = loaded->network;
  const RoadId road = network.find_road("1");
  ASSERT_TRUE(network.road(road) != nullptr);
  ASSERT_FALSE(network.road(road)->authoring_waypoints.has_value());

  // Dense sample of the original curve, the deviation oracle below. The
  // polyline chord error (~2.5e-5 m sagitta at this density on the R=20 arc)
  // stays below the tolerance the oracle asserts.
  const roadmaker::ReferenceLine original = network.road(road)->plan_view;
  const double original_length = original.length();
  constexpr int kGoldenSamples = 1600;
  constexpr int kSamples = 400;
  std::vector<roadmaker::PathPoint> golden;
  golden.reserve(kGoldenSamples + 1);
  for (int i = 0; i <= kGoldenSamples; ++i) {
    golden.push_back(original.evaluate(original_length * i / kGoldenSamples));
  }

  // Derived nodes: 4 record starts + endpoint. Insert an on-curve node in
  // the middle of the arc segment, then apply — the first edit derives
  // waypoints and re-fits the whole chain.
  const std::vector<Waypoint> nodes = roadmaker::edit::effective_waypoints(*network.road(road));
  ASSERT_EQ(nodes.size(), 5U);
  const auto stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(stations.has_value());
  const double arc_mid_s = (stations->at(2) + stations->at(3)) / 2.0;
  const roadmaker::PathPoint arc_mid = original.evaluate(arc_mid_s);
  auto command =
      roadmaker::edit::insert_waypoint(network, road, 3, Waypoint{.x = arc_mid.x, .y = arc_mid.y});
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.road(road)->authoring_waypoints->size(), 6U);

  // Every pre-edit node (all of them points of the original curve) is still
  // interpolated, endpoints included.
  const roadmaker::ReferenceLine& refit = network.road(road)->plan_view;
  const auto refit_stations = roadmaker::edit::waypoint_stations(*network.road(road));
  ASSERT_TRUE(refit_stations.has_value());
  for (std::size_t i = 0; i < network.road(road)->authoring_waypoints->size(); ++i) {
    SCOPED_TRACE("node " + std::to_string(i));
    const Waypoint& node = network.road(road)->authoring_waypoints->at(i);
    const roadmaker::PathPoint fitted = refit.evaluate(refit_stations->at(i));
    EXPECT_NEAR(fitted.x, node.x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(fitted.y, node.y, roadmaker::tol::kRoundTripPosition);
  }

  // Shape fidelity, the 01 §2.5 promise: the derivation re-fit interpolates
  // the chain's headings (G1 Hermite), so this pure line/spiral/arc/spiral
  // chain — edited with an on-curve node — is reproduced within rm::tol.
  constexpr double kGoldenShapeTolerance = roadmaker::tol::kRoundTripPosition;
  const double refit_length = refit.length();
  EXPECT_NEAR(refit_length, original_length, roadmaker::tol::kRoundTripPosition);
  const auto distance_to_golden = [&golden](const roadmaker::PathPoint& p) {
    double nearest = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < golden.size(); ++i) {
      const double ax = golden[i].x;
      const double ay = golden[i].y;
      const double bx = golden[i + 1].x - ax;
      const double by = golden[i + 1].y - ay;
      const double t =
          std::clamp(((p.x - ax) * bx + (p.y - ay) * by) / (bx * bx + by * by), 0.0, 1.0);
      nearest = std::min(nearest, std::hypot(p.x - (ax + t * bx), p.y - (ay + t * by)));
    }
    return nearest;
  };
  double max_deviation = 0.0;
  for (int i = 0; i <= kSamples; ++i) {
    max_deviation =
        std::max(max_deviation, distance_to_golden(refit.evaluate(refit_length * i / kSamples)));
  }
  EXPECT_LT(max_deviation, kGoldenShapeTolerance);

  // Save → reload: the written file (now carrying rm:waypoints) parses back
  // to the same geometry.
  const std::string written = snapshot_xodr(network);
  auto reparsed = roadmaker::parse_xodr(written);
  ASSERT_TRUE(reparsed.has_value());
  const RoadId road2 = reparsed->network.find_road("1");
  ASSERT_TRUE(reparsed->network.road(road2) != nullptr);
  ASSERT_TRUE(reparsed->network.road(road2)->authoring_waypoints.has_value());
  EXPECT_EQ(reparsed->network.road(road2)->authoring_waypoints->size(), 6U);
  const roadmaker::ReferenceLine& reread = reparsed->network.road(road2)->plan_view;
  ASSERT_NEAR(reread.length(), refit_length, roadmaker::tol::kRoundTripPosition);
  for (int i = 0; i <= kSamples; ++i) {
    const double s = refit_length * i / kSamples;
    SCOPED_TRACE("station " + std::to_string(s));
    EXPECT_NEAR(reread.evaluate(s).x, refit.evaluate(s).x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(reread.evaluate(s).y, refit.evaluate(s).y, roadmaker::tol::kRoundTripPosition);
  }
}

// --- elevation ------------------------------------------------------------------

TEST(EditOperations, SetNodeElevationBuildsPiecewiseLinearProfile) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto raise = roadmaker::edit::set_node_elevation(network, road, 1, 4.0);
  expect_command_round_trip(network, *raise);

  ASSERT_TRUE(raise->apply(network).has_value());
  const roadmaker::Road& value = *network.road(road);
  ASSERT_EQ(value.elevation.size(), 2U); // two linear segments through 3 nodes
  const double mid_s = value.plan_view.records().at(1).s;
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, 0.0), 0.0, 1e-9);
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, mid_s), 4.0, 1e-9);
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, value.length), 0.0, 1e-9);

  // Lowering the node back to zero drops the profile entirely.
  auto lower = roadmaker::edit::set_node_elevation(network, road, 1, 0.0);
  ASSERT_TRUE(lower->apply(network).has_value());
  EXPECT_TRUE(network.road(road)->elevation.empty());
}

// --- topology: create / delete / split roads --------------------------------------

TEST(EditOperations, CreateRoadRoundTripsWithStableIds) {
  RoadNetwork network;
  author_default(network, "1");

  auto command =
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 80.0, .y = 60.0}},
                                   LaneProfile::two_lane_default(),
                                   "Second");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId created = network.find_road("2"); // auto-assigned next free id
  ASSERT_TRUE(created.is_valid());
  EXPECT_TRUE(command->dirty().topology);
  ASSERT_EQ(command->dirty().roads.size(), 1U);
  EXPECT_EQ(command->dirty().roads[0], created);
}

TEST(EditOperations, CreateRoadRejectsDegenerateWaypoints) {
  RoadNetwork network;
  author_default(network, "1");
  expect_command_rejected(
      network,
      roadmaker::edit::create_road({Waypoint{.x = 1.0, .y = 1.0}, Waypoint{.x = 1.0, .y = 1.0}},
                                   LaneProfile::two_lane_default(),
                                   ""));
}

TEST(EditOperations, CreateRoadAutoNamesFromTheAssignedOdrId) {
  RoadNetwork network;
  author_default(network, "1");

  auto command =
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 80.0, .y = 60.0}},
                                   LaneProfile::two_lane_rural(),
                                   "");
  ASSERT_TRUE(command->apply(network).has_value());
  const RoadId created = network.find_road("2");
  ASSERT_TRUE(created.is_valid());
  EXPECT_EQ(network.road(created)->name, "Road 2");
}

// The Create Road tangent-snap chain (02 §2): locking the new road's start
// heading to the snapped road's continuation heading joins the two G1.
TEST(EditOperations, CreateRoadLockedStartHeadingChainsG1) {
  RoadNetwork network;
  const RoadId first = author_default(network, "1");
  const roadmaker::Road& source = *network.road(first);
  const auto end = source.plan_view.evaluate(source.plan_view.length());

  auto command = roadmaker::edit::create_road(
      {Waypoint{.x = end.x, .y = end.y}, Waypoint{.x = end.x + 70.0, .y = end.y - 25.0}},
      LaneProfile::two_lane_rural(),
      "Chained",
      {.start = end.hdg});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const RoadId chained = network.find_road("2");
  ASSERT_TRUE(chained.is_valid());
  const auto start = network.road(chained)->plan_view.evaluate(0.0);
  EXPECT_NEAR(start.x, end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(start.y, end.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(
      std::remainder(start.hdg - end.hdg, 2.0 * std::numbers::pi), 0.0, roadmaker::tol::kAngle);
}

TEST(EditOperations, DeleteRoadDetachesAndUndoResurrectsOriginalIds) {
  RoadNetwork network;
  const RoadId incoming = author_default(network, "1");
  const RoadId doomed = author_default(network, "2", 40.0);
  const RoadId neighbor = author_default(network, "3", 80.0);
  network.road(neighbor)->predecessor =
      roadmaker::RoadLink{.target = doomed, .contact = ContactPoint::End};

  const JunctionId junction = network.create_junction("100", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = doomed,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });

  auto command = roadmaker::edit::delete_road(network, doomed);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(doomed), nullptr);
  EXPECT_TRUE(network.junction(junction)->connections.empty());
  EXPECT_FALSE(network.road(neighbor)->predecessor.has_value());

  ASSERT_TRUE(command->revert(network).has_value());
  // The acceptance criterion: ids held elsewhere are valid again.
  ASSERT_EQ(network.junction(junction)->connections.size(), 1U);
  const JunctionConnection& connection = network.junction(junction)->connections[0];
  ASSERT_NE(network.road(connection.connecting_road), nullptr);
  EXPECT_EQ(network.road(connection.connecting_road)->odr_id, "2");
  ASSERT_TRUE(network.road(neighbor)->predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(neighbor)->predecessor->target), doomed);
}

/// §7 closure setup shared by the two closure tests: `incoming` feeds
/// `connecting` (a proper junction-internal road) which exits onto
/// `outgoing`.
struct JunctionClosureFixture {
  RoadId incoming;
  RoadId connecting;
  RoadId outgoing;
  JunctionId junction;
};

JunctionClosureFixture make_junction_closure(RoadNetwork& network) {
  JunctionClosureFixture fixture;
  fixture.incoming = author_default(network, "1");
  fixture.connecting = author_default(network, "2", 40.0);
  fixture.outgoing = author_default(network, "3", 80.0);
  fixture.junction = network.create_junction("100", "X");

  network.road(fixture.incoming)->successor =
      roadmaker::RoadLink{.target = fixture.junction, .contact = ContactPoint::Start};
  network.road(fixture.outgoing)->predecessor =
      roadmaker::RoadLink{.target = fixture.junction, .contact = ContactPoint::Start};
  roadmaker::Road& connecting = *network.road(fixture.connecting);
  connecting.junction = fixture.junction;
  connecting.predecessor =
      roadmaker::RoadLink{.target = fixture.incoming, .contact = ContactPoint::End};
  connecting.successor =
      roadmaker::RoadLink{.target = fixture.outgoing, .contact = ContactPoint::Start};
  network.junction(fixture.junction)
      ->connections.push_back(JunctionConnection{
          .incoming_road = fixture.incoming,
          .connecting_road = fixture.connecting,
          .contact_point = ContactPoint::Start,
          .lane_links = {{-1, -1}},
      });
  return fixture;
}

TEST(EditOperations, DeleteIncomingRoadDeletesItsConnectingRoads) {
  RoadNetwork network;
  const JunctionClosureFixture fixture = make_junction_closure(network);

  auto command = roadmaker::edit::delete_road(network, fixture.incoming);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  // The connection referencing the incoming road is gone, and its connecting
  // road went with it — the §7 closure.
  EXPECT_EQ(network.road(fixture.incoming), nullptr);
  EXPECT_EQ(network.road(fixture.connecting), nullptr);
  ASSERT_NE(network.junction(fixture.junction), nullptr);
  EXPECT_TRUE(network.junction(fixture.junction)->connections.empty());
  // The outgoing road survives; its link into the surviving junction stays.
  ASSERT_NE(network.road(fixture.outgoing), nullptr);
  EXPECT_TRUE(network.road(fixture.outgoing)->predecessor.has_value());

  // Both doomed roads are in the dirty set, plus the junction they touched.
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(dirty.topology);
  EXPECT_NE(std::ranges::find(dirty.roads, fixture.connecting), dirty.roads.end());
  EXPECT_NE(std::ranges::find(dirty.junctions, fixture.junction), dirty.junctions.end());

  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.road(fixture.connecting), nullptr);
  EXPECT_EQ(network.road(fixture.connecting)->odr_id, "2");
  ASSERT_EQ(network.junction(fixture.junction)->connections.size(), 1U);
  EXPECT_EQ(network.junction(fixture.junction)->connections[0].connecting_road, fixture.connecting);
}

TEST(EditOperations, DeleteJunctionDeletesConnectingRoadsAndClearsLinks) {
  RoadNetwork network;
  const JunctionClosureFixture fixture = make_junction_closure(network);

  auto command = roadmaker::edit::delete_junction(network, fixture.junction);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  // The junction takes its connecting road along; the incoming and outgoing
  // roads survive with their links into the junction cleared.
  EXPECT_EQ(network.junction(fixture.junction), nullptr);
  EXPECT_EQ(network.road(fixture.connecting), nullptr);
  ASSERT_NE(network.road(fixture.incoming), nullptr);
  EXPECT_FALSE(network.road(fixture.incoming)->successor.has_value());
  ASSERT_NE(network.road(fixture.outgoing), nullptr);
  EXPECT_FALSE(network.road(fixture.outgoing)->predecessor.has_value());

  ASSERT_TRUE(command->revert(network).has_value());
  // Every removed object and link is back under its original id.
  ASSERT_NE(network.junction(fixture.junction), nullptr);
  ASSERT_NE(network.road(fixture.connecting), nullptr);
  EXPECT_EQ(network.road(fixture.connecting)->junction, fixture.junction);
  ASSERT_TRUE(network.road(fixture.incoming)->successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(fixture.incoming)->successor->target),
            fixture.junction);
}

TEST(EditOperations, SplitRoadPreservesGeometryAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;
  const double split_s = length * 0.4;
  const roadmaker::PathPoint at_split = network.road(road)->plan_view.evaluate(split_s);

  auto command = roadmaker::edit::split_road(network, road, split_s);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId tail = network.find_road("2");
  ASSERT_TRUE(tail.is_valid());

  const roadmaker::Road& head_road = *network.road(road);
  const roadmaker::Road& tail_road = *network.road(tail);
  EXPECT_NEAR(head_road.length, split_s, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(head_road.length + tail_road.length, length, roadmaker::tol::kRoundTripPosition);

  // The seam is G1: tail starts exactly where the head ends.
  const roadmaker::PathPoint tail_start = tail_road.plan_view.evaluate(0.0);
  EXPECT_NEAR(tail_start.x, at_split.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.y, at_split.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.hdg, at_split.hdg, roadmaker::tol::kRoundTripHeading);

  // Linked head->tail with identity lane links on the seam.
  ASSERT_TRUE(head_road.successor.has_value());
  EXPECT_EQ(std::get<RoadId>(head_road.successor->target), tail);
  ASSERT_TRUE(tail_road.predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(tail_road.predecessor->target), road);
  ASSERT_EQ(tail_road.sections.size(), 1U);
  EXPECT_EQ(network.lane_section(tail_road.sections[0])->lanes.size(),
            network.lane_section(head_road.sections[0])->lanes.size());
}

TEST(EditOperations, SplitRoadRejectsBadStationsAndJunctionRoads) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;

  expect_command_rejected(network, roadmaker::edit::split_road(network, road, 0.0));
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length));

  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->successor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length * 0.5));
}

// --- junctions ---------------------------------------------------------------------

TEST(EditOperations, CreateJunctionLinksArmsAndRoundTrips) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);

  const std::array<RoadEnd, 2> ends{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  auto command = roadmaker::edit::create_junction(network, ends);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  ASSERT_TRUE(network.road(a)->successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(a)->successor->target), junction);
  ASSERT_TRUE(network.road(b)->predecessor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(b)->predecessor->target), junction);
}

TEST(EditOperations, CreateJunctionRejectsBadEnds) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);

  const std::array<RoadEnd, 1> too_few{RoadEnd{.road = a, .contact = ContactPoint::End}};
  expect_command_rejected(network, roadmaker::edit::create_junction(network, too_few));

  const std::array<RoadEnd, 2> duplicate{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = a, .contact = ContactPoint::End},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, duplicate));

  network.road(b)->predecessor = roadmaker::RoadLink{.target = a, .contact = ContactPoint::End};
  const std::array<RoadEnd, 2> occupied{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, occupied));
}

TEST(EditOperations, DeleteJunctionDetachesRoadsAndRoundTrips) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);
  const std::array<RoadEnd, 2> ends{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  auto create = roadmaker::edit::create_junction(network, ends);
  ASSERT_TRUE(create->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  auto command = roadmaker::edit::delete_junction(network, junction);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.junction(junction), nullptr);
  EXPECT_FALSE(network.road(a)->successor.has_value());
  EXPECT_FALSE(network.road(b)->predecessor.has_value());
}

// --- lanes: add / remove --------------------------------------------------------

TEST(EditOperations, AddLaneAppendsOutermostAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const std::size_t lanes_before = network.lane_section(section)->lanes.size();

  auto command = roadmaker::edit::add_lane(network, section, -1, LaneType::Biking);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.lane_section(section)->lanes.size(), lanes_before + 1);
  const LaneId added = network.lane_section(section)->lanes.back(); // outermost right
  EXPECT_EQ(network.lane(added)->odr_id, -3);                       // default profile has -1, -2
  EXPECT_EQ(network.lane(added)->type, LaneType::Biking);
  // Width copied from the previous outermost (shoulder, 1.0 m).
  ASSERT_FALSE(network.lane(added)->widths.empty());
  EXPECT_NEAR(network.lane(added)->widths[0].a, 1.0, 1e-12);
}

TEST(EditOperations, RemoveLaneOutermostOnlyAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back(); // -2 shoulder
  const LaneId inner_right = *std::prev(network.lane_section(section)->lanes.end(), 2); // -1

  expect_command_rejected(network, roadmaker::edit::remove_lane(network, inner_right));

  auto command = roadmaker::edit::remove_lane(network, outer_right);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.lane(outer_right), nullptr);
  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.lane(outer_right), nullptr); // original id resurrected
  EXPECT_EQ(network.lane(outer_right)->type, LaneType::Shoulder);
}

/// The issue #14 integrity criterion: junction lane_links referencing the
/// removed lane (on either side of a pair) are dropped with the lane and
/// restored exactly on undo.
TEST(EditOperations, RemoveLaneDropsJunctionLaneLinksAndRestoresThem) {
  RoadNetwork network;
  const RoadId incoming = author_default(network, "1");
  const RoadId connecting = author_default(network, "2", 40.0);
  const JunctionId junction = network.create_junction("100", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = connecting,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}, {-2, -2}},
  });

  const auto outer_right = [&](RoadId road_id) {
    const LaneSectionId section = network.road(road_id)->sections[0];
    return network.lane_section(section)->lanes.back(); // -2 shoulder
  };

  // Removing the INCOMING road's -2 drops the pair by its first element.
  auto from_incoming = roadmaker::edit::remove_lane(network, outer_right(incoming));
  expect_command_round_trip(network, *from_incoming);
  ASSERT_TRUE(from_incoming->apply(network).has_value());
  {
    const auto& links = network.junction(junction)->connections[0].lane_links;
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(links[0], (std::pair<int, int>{-1, -1}));
  }
  const roadmaker::edit::DirtySet dirty = from_incoming->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions[0], junction);
  ASSERT_TRUE(from_incoming->revert(network).has_value());
  EXPECT_EQ(network.junction(junction)->connections[0].lane_links.size(), 2U);

  // Removing the CONNECTING road's -2 drops the pair by its second element.
  auto from_connecting = roadmaker::edit::remove_lane(network, outer_right(connecting));
  ASSERT_TRUE(from_connecting->apply(network).has_value());
  {
    const auto& links = network.junction(junction)->connections[0].lane_links;
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(links[0], (std::pair<int, int>{-1, -1}));
  }
  ASSERT_TRUE(from_connecting->revert(network).has_value());
  EXPECT_EQ(network.junction(junction)->connections[0].lane_links.size(), 2U);
}

/// Spec 02 §4: multiple <roadMark> records per lane stay supported in data —
/// the M2 edit touches the first (sOffset 0) record only, and the surviving
/// tail keeps ascending sOffset order (…road_mark.elem_asc_order).
TEST(EditOperations, SetRoadMarkEditsTheFirstRecordOnly) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();
  network.lane(outer_right)->road_marks = {
      RoadMark{.s_offset = 0.0, .type = RoadMarkType::Broken, .width = 0.12},
      RoadMark{.s_offset = 40.0, .type = RoadMarkType::Solid, .width = 0.12},
  };

  auto command = roadmaker::edit::set_road_mark(
      network, outer_right, RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid, .width = 0.25});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const std::vector<RoadMark>& marks = network.lane(outer_right)->road_marks;
  ASSERT_EQ(marks.size(), 2U);
  EXPECT_EQ(marks[0].type, RoadMarkType::Solid);
  EXPECT_NEAR(marks[0].width, 0.25, 1e-12);
  EXPECT_EQ(marks[1].type, RoadMarkType::Solid); // tail untouched
  EXPECT_NEAR(marks[1].s_offset, 40.0, 1e-12);

  // An sOffset at or past the next record would break ascending order.
  expect_command_rejected(
      network,
      roadmaker::edit::set_road_mark(
          network, outer_right, RoadMark{.s_offset = 40.0, .type = RoadMarkType::None}));
}

// --- integration with the stack ---------------------------------------------------

TEST(EditOperations, EditStackDrivesTopologyCommands) {
  RoadNetwork network;
  author_default(network, "1");
  const std::string pristine = snapshot_xodr(network);

  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack
                  .push(network,
                        roadmaker::edit::create_road(
                            {Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 70.0, .y = 55.0}},
                            LaneProfile::two_lane_default(),
                            "Branch"))
                  .has_value());
  const RoadId created = network.find_road("2");
  ASSERT_TRUE(created.is_valid());
  ASSERT_TRUE(
      stack.push(network, roadmaker::edit::rename_road(network, created, "Renamed")).has_value());
  const std::string edited = snapshot_xodr(network);

  ASSERT_TRUE(stack.undo(network).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());
  expect_network_matches(network, pristine);

  ASSERT_TRUE(stack.redo(network).has_value());
  ASSERT_TRUE(stack.redo(network).has_value());
  expect_network_matches(network, edited);
  EXPECT_EQ(network.road(created)->name, "Renamed"); // same generational id after redo
}
