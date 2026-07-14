#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "viewport/camera.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// One-quad road at height `z` covering [0,size]x[0,size], registered in
/// `network` so the mesh carries valid ids.
RoadMesh make_quad_road(RoadNetwork& network, std::string odr_id, double z, double size) {
  const RoadId road_id = network.create_road("", odr_id);
  const LaneSectionId section = network.add_lane_section(road_id, 0.0);
  const LaneId lane_id = network.add_lane(section, -1, LaneType::Driving);

  RoadMesh mesh;
  mesh.road = road_id;
  mesh.positions = {0.0, 0.0, z, size, 0.0, z, size, size, z, 0.0, size, z};
  mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  mesh.lanes.push_back(RoadMesh::LanePatch{.lane = lane_id,
                                           .odr_lane_id = -1,
                                           .material = LaneType::Driving,
                                           .indices = {0, 1, 2, 0, 2, 3}});
  return mesh;
}

/// One-quad junction floor at height `z` covering [0,size]x[0,size], keyed to a
/// fresh junction id in `network`.
JunctionFloor make_quad_floor(RoadNetwork& network, std::string odr_id, double z, double size) {
  const JunctionId junction_id = network.create_junction(odr_id, "");
  JunctionFloor floor;
  floor.junction = junction_id;
  floor.mesh.positions = {0.0, 0.0, z, size, 0.0, z, size, size, z, 0.0, size, z};
  floor.mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  floor.mesh.indices = {0, 1, 2, 0, 2, 3};
  return floor;
}

Ray straight_down(double x, double y, double from_z = 10.0) {
  return Ray{.origin = {x, y, from_z}, .direction = {0.0, 0.0, -1.0}};
}

ReferenceLine straight_line(double length) {
  ReferenceLine line;
  line.append(
      GeometryRecord{.x = 0.0, .y = 0.0, .hdg = 0.0, .length = length, .shape = LineGeom{}});
  return line;
}

TEST(PickRay, CenterPixelPassesThroughCameraTarget) {
  OrbitCamera camera; // default target (0,0,0)
  const CameraMatrices matrices = camera.matrices(16.0F / 9.0F);
  const Ray ray = make_pick_ray(matrices, 800.0, 450.0, 1600.0, 900.0);

  // Distance from the ray to the origin (camera target).
  const std::array<double, 3>& o = ray.origin;
  const std::array<double, 3>& d = ray.direction;
  const double t = -((o[0] * d[0]) + (o[1] * d[1]) + (o[2] * d[2]));
  const double px = o[0] + (t * d[0]);
  const double py = o[1] + (t * d[1]);
  const double pz = o[2] + (t * d[2]);
  EXPECT_NEAR(std::sqrt((px * px) + (py * py) + (pz * pz)), 0.0, 1e-3);
}

TEST(PickRay, PixelYAxisPointsDown) {
  OrbitCamera camera;
  const CameraMatrices matrices = camera.matrices(1.0F);
  const Ray top = make_pick_ray(matrices, 500.0, 0.0, 1000.0, 1000.0);
  const Ray bottom = make_pick_ray(matrices, 500.0, 1000.0, 1000.0, 1000.0);
  // Orbit camera looks down at the scene: the top-of-screen ray must tilt
  // upward (greater world +Z component) relative to the bottom-of-screen ray.
  EXPECT_GT(top.direction[2], bottom.direction[2]);
}

TEST(PickRay, DirectionIsNormalized) {
  OrbitCamera camera;
  const CameraMatrices matrices = camera.matrices(2.0F);
  const Ray ray = make_pick_ray(matrices, 13.0, 977.0, 1600.0, 1000.0);
  const std::array<double, 3>& d = ray.direction;
  EXPECT_NEAR((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]), 1.0, 1e-12);
}

TEST(Pick, HitInsideQuadReturnsLaneAndPoint) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  const auto hit = pick(mesh, aabbs, straight_down(4.0, 6.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->road, mesh.roads[0].road);
  EXPECT_EQ(hit->lane, mesh.roads[0].lanes[0].lane);
  EXPECT_NEAR(hit->position[0], 4.0, 1e-9);
  EXPECT_NEAR(hit->position[1], 6.0, 1e-9);
  EXPECT_NEAR(hit->position[2], 0.0, 1e-9);
  EXPECT_NEAR(hit->distance, 10.0, 1e-9);
}

TEST(Pick, JunctionFloorIsSelectable) {
  // Gate finding 4: a junction floor is its own pickable entity — a ray into
  // the open floor interior (no road covers it) reports the JunctionId with
  // road/lane/object left invalid.
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.junction_floors.push_back(make_quad_floor(network, "j", 0.0, 10.0));

  const auto hit = pick(mesh, {}, straight_down(5.0, 5.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->junction, mesh.junction_floors[0].junction);
  EXPECT_FALSE(hit->road.is_valid());
  EXPECT_FALSE(hit->lane.is_valid());
  EXPECT_FALSE(hit->object.is_valid());
  EXPECT_NEAR(hit->position[2], 0.0, 1e-9);
}

TEST(Pick, RoadOverFloorWinsOnTie) {
  // A road patch coplanar with (or above) the floor still wins the pick so the
  // arms stay grabbable; the floor only claims the interior no road covers.
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.junction_floors.push_back(make_quad_floor(network, "j", 0.0, 10.0));
  mesh.roads.push_back(make_quad_road(network, "1", 0.1, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  const auto hit = pick(mesh, aabbs, straight_down(5.0, 5.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->road, mesh.roads[0].road);
  EXPECT_FALSE(hit->junction.is_valid());
}

TEST(Pick, MissOutsideQuad) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  EXPECT_FALSE(pick(mesh, aabbs, straight_down(11.0, 5.0)).has_value());
  EXPECT_FALSE(pick(mesh, aabbs, straight_down(-0.5, 5.0)).has_value());
}

TEST(Pick, BackfaceIsNotCulled) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  // From below, looking up: the quad's back side must still report a hit
  // (roads are thin sheets; downhill-facing patches stay pickable).
  const Ray ray{.origin = {5.0, 5.0, -10.0}, .direction = {0.0, 0.0, 1.0}};
  const auto hit = pick(mesh, aabbs, ray);
  ASSERT_TRUE(hit.has_value());
  EXPECT_NEAR(hit->distance, 10.0, 1e-9);
}

TEST(Pick, RayParallelToPlaneMisses) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  const Ray ray{.origin = {-5.0, 5.0, 0.5}, .direction = {1.0, 0.0, 0.0}};
  EXPECT_FALSE(pick(mesh, aabbs, ray).has_value());
}

TEST(Pick, NearestOfStackedQuadsWins) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "low", 0.0, 10.0));
  mesh.roads.push_back(make_quad_road(network, "high", 1.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  const auto hit = pick(mesh, aabbs, straight_down(5.0, 5.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->road, mesh.roads[1].road);
  EXPECT_NEAR(hit->position[2], 1.0, 1e-9);
}

TEST(Pick, AabbPrefilterMatchesBruteForce) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  mesh.roads.push_back(make_quad_road(network, "2", 2.0, 4.0));
  const auto aabbs = compute_road_aabbs(mesh);

  for (const double x : {1.0, 3.0, 5.0, 9.0, 12.0}) {
    const Ray ray = straight_down(x, 3.0);
    const auto with = pick(mesh, aabbs, ray);
    const auto without = pick(mesh, {}, ray); // empty span disables the prefilter
    ASSERT_EQ(with.has_value(), without.has_value());
    if (with) {
      EXPECT_EQ(with->road, without->road);
      EXPECT_NEAR(with->distance, without->distance, 1e-12);
    }
  }
}

TEST(FindStation, StraightLineAnalytic) {
  const ReferenceLine line = straight_line(100.0);

  const StationCoord right = find_station(line, 50.0, -3.0);
  EXPECT_NEAR(right.s, 50.0, 1e-3);
  EXPECT_NEAR(right.t, -3.0, 1e-6); // right of travel direction -> negative

  const StationCoord left = find_station(line, 25.0, 4.0);
  EXPECT_NEAR(left.s, 25.0, 1e-3);
  EXPECT_NEAR(left.t, 4.0, 1e-6); // left of travel direction -> positive
}

TEST(StationToWorld, InvertsFindStationOnAStraightLine) {
  const ReferenceLine line = straight_line(100.0);
  // station_to_world is the exact inverse of find_station: projecting a point to
  // (s, t) and back must return the original point (the ghost==commit contract).
  for (const auto& p : {std::array<double, 2>{50.0, -3.0},
                        std::array<double, 2>{25.0, 4.0},
                        std::array<double, 2>{10.0, 0.0}}) {
    const StationCoord st = find_station(line, p[0], p[1]);
    const auto world = station_to_world(line, st.s, st.t);
    EXPECT_NEAR(world[0], p[0], 1e-6);
    EXPECT_NEAR(world[1], p[1], 1e-6);
  }
}

TEST(StationToWorld, EmptyLineYieldsOrigin) {
  const ReferenceLine line;
  const auto world = station_to_world(line, 5.0, 2.0);
  EXPECT_DOUBLE_EQ(world[0], 0.0);
  EXPECT_DOUBLE_EQ(world[1], 0.0);
}

TEST(FindStation, ClampsToLineEnds) {
  const ReferenceLine line = straight_line(100.0);

  const StationCoord past_end = find_station(line, 150.0, 2.0);
  EXPECT_NEAR(past_end.s, 100.0, 1e-3);
  EXPECT_NEAR(past_end.t, 2.0, 1e-6);

  const StationCoord before_start = find_station(line, -10.0, -1.0);
  EXPECT_NEAR(before_start.s, 0.0, 1e-3);
  EXPECT_NEAR(before_start.t, -1.0, 1e-6);
}

TEST(FindStation, ArcAnalytic) {
  // Left-turning arc: curvature 0.02 (R = 50 m), start at origin heading +X,
  // center at (0, 50). Query 2 m left of the curve point at theta = 1 rad.
  constexpr double kRadius = 50.0;
  constexpr double kTheta = 1.0;
  ReferenceLine line;
  line.append(GeometryRecord{.x = 0.0,
                             .y = 0.0,
                             .hdg = 0.0,
                             .length = kPi * kRadius, // half circle
                             .shape = ArcGeom{.curvature = 1.0 / kRadius}});

  // Point at station R*theta, offset t=+2 (left = toward the arc center).
  const double qx = (kRadius - 2.0) * std::sin(kTheta);
  const double qy = kRadius - ((kRadius - 2.0) * std::cos(kTheta));
  const StationCoord coord = find_station(line, qx, qy);
  EXPECT_NEAR(coord.s, kRadius * kTheta, 1e-3);
  EXPECT_NEAR(coord.t, 2.0, 1e-4);
}

TEST(FindStation, HalfCircleQueryNearEndResolvesToEnd) {
  // Half circle ends at (0, 100): a query next to the endpoint must map to
  // s ~ length, not to the s = 0 side of the multimodal distance field.
  constexpr double kRadius = 50.0;
  ReferenceLine line;
  line.append(GeometryRecord{.x = 0.0,
                             .y = 0.0,
                             .hdg = 0.0,
                             .length = kPi * kRadius,
                             .shape = ArcGeom{.curvature = 1.0 / kRadius}});

  const StationCoord coord = find_station(line, -0.5, 99.0);
  EXPECT_GT(coord.s, 0.9 * kPi * kRadius);
}

TEST(FindStation, EmptyLineYieldsZero) {
  const ReferenceLine line;
  const StationCoord coord = find_station(line, 5.0, 5.0);
  EXPECT_EQ(coord.s, 0.0);
  EXPECT_EQ(coord.t, 0.0);
}

TEST(PickEndToEnd, AuthoredRoadPickAndStationRoundTrip) {
  RoadNetwork network;
  const std::array<Waypoint, 2> waypoints{Waypoint{0.0, 0.0}, Waypoint{100.0, 0.0}};
  const auto road_id = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default());
  ASSERT_TRUE(road_id.has_value());

  const NetworkMesh mesh = build_network_mesh(network);
  const auto aabbs = compute_road_aabbs(mesh);

  // Middle of the right driving lane (odr lane -1, 3.5 m wide): t = -1.75.
  const auto hit = pick(mesh, aabbs, straight_down(50.0, -1.75));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->road, *road_id);
  const Lane* lane = network.lane(hit->lane);
  ASSERT_NE(lane, nullptr);
  EXPECT_EQ(lane->odr_id, -1);

  const Road* road = network.road(*road_id);
  ASSERT_NE(road, nullptr);
  const StationCoord coord = find_station(road->plan_view, hit->position[0], hit->position[1]);
  EXPECT_NEAR(coord.s, 50.0, 0.1);
  EXPECT_NEAR(coord.t, -1.75, 0.1);
}

TEST(Pick, HitsAPlacedPropInFrontOfTheRoad) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 50.0));
  const ObjectId tree{.index = 7, .gen = 0};
  mesh.objects.push_back(ObjectInstance{.object = tree,
                                        .road = mesh.roads[0].road,
                                        .model_id = "tree_pine",
                                        .position = {25.0, 25.0, 0.0},
                                        .heading = 0.0});
  const auto aabbs = compute_road_aabbs(mesh);

  // Straight down through the tree: its bounding sphere is nearer than the
  // road surface below it, so the prop wins the pick.
  const auto on_tree = pick(mesh, aabbs, straight_down(25.0, 25.0));
  ASSERT_TRUE(on_tree.has_value());
  EXPECT_EQ(on_tree->object, tree);
  EXPECT_EQ(on_tree->road, mesh.roads[0].road);

  // Away from the tree the road is still picked (object invalid).
  const auto on_road = pick(mesh, aabbs, straight_down(5.0, 5.0));
  ASSERT_TRUE(on_road.has_value());
  EXPECT_FALSE(on_road->object.is_valid());
}

TEST(Pick, MissesPropWhenRayIsWideOfIt) {
  RoadNetwork network;
  NetworkMesh mesh;
  const ObjectId tree{.index = 7, .gen = 0};
  mesh.objects.push_back(ObjectInstance{.object = tree,
                                        .road = RoadId{.index = 1, .gen = 0},
                                        .model_id = "tree_pine",
                                        .position = {25.0, 25.0, 0.0},
                                        .heading = 0.0});
  // No road, ray 20 m away from the tree — nothing hit.
  EXPECT_FALSE(pick(mesh, {}, straight_down(45.0, 45.0)).has_value());
}

} // namespace
} // namespace roadmaker::editor
