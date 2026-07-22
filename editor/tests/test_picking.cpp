// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>

#include "viewport/camera.hpp"
#include "viewport/picking.hpp"
#include "viewport/projection.hpp"

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

/// One-quad ground surface (#215) at height `z` covering [0,size]x[0,size],
/// keyed to a fresh surface id in `network`.
SurfaceMesh make_quad_surface(RoadNetwork& network, double z, double size) {
  const SurfaceId surface_id = network.create_surface(Surface{});
  SurfaceMesh surface;
  surface.surface = surface_id;
  surface.mesh.positions = {0.0, 0.0, z, size, 0.0, z, size, size, z, 0.0, size, z};
  surface.mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  surface.mesh.indices = {0, 1, 2, 0, 2, 3};
  return surface;
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
  OrbitCamera camera;
  const std::array<float, 3> t3 = camera.target(); // the pivot, wherever it sits
  const CameraMatrices matrices = camera.matrices(16.0F / 9.0F);
  const Ray ray = make_pick_ray(matrices, 800.0, 450.0, 1600.0, 900.0);

  // Distance from the ray to the camera target: project (target − origin) onto
  // the ray and measure the residual.
  const std::array<double, 3>& o = ray.origin;
  const std::array<double, 3>& d = ray.direction;
  const std::array<double, 3> to_target{t3[0] - o[0], t3[1] - o[1], t3[2] - o[2]};
  const double t = (to_target[0] * d[0]) + (to_target[1] * d[1]) + (to_target[2] * d[2]);
  const double px = o[0] + (t * d[0]) - t3[0];
  const double py = o[1] + (t * d[1]) - t3[1];
  const double pz = o[2] + (t * d[2]) - t3[2];
  EXPECT_NEAR(std::sqrt((px * px) + (py * py) + (pz * pz)), 0.0, 1e-3);
}

// --- orthographic pick rays (P1/GW-1 step 11) --------------------------------
// Unprojection goes through inverse(proj·view), so it is projection-agnostic —
// but ortho is the case where the rays become PARALLEL instead of fanning from
// the eye, so it gets its own guard.

TEST(PickRay, OrthoRaysAreParallelInsteadOfFanningFromTheEye) {
  OrbitCamera camera;
  camera.set_projection(ProjectionMode::Orthographic);
  const CameraMatrices matrices = camera.matrices(16.0F / 9.0F);

  const Ray left = make_pick_ray(matrices, 100.0, 450.0, 1600.0, 900.0);
  const Ray right = make_pick_ray(matrices, 1500.0, 450.0, 1600.0, 900.0);

  // Same direction everywhere on screen...
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(left.direction[i], right.direction[i], 1e-6)
        << "ortho rays must not converge on an eye point";
  }
  // ...but different origins, which is what makes them able to hit anything.
  const double origin_gap = std::abs(left.origin[0] - right.origin[0]) +
                            std::abs(left.origin[1] - right.origin[1]) +
                            std::abs(left.origin[2] - right.origin[2]);
  EXPECT_GT(origin_gap, 1.0);
}

TEST(PickRay, OrthoCenterPixelStillPassesThroughTheTarget) {
  OrbitCamera camera;
  camera.set_projection(ProjectionMode::Orthographic);
  const std::array<float, 3> t3 = camera.target();
  const Ray ray = make_pick_ray(camera.matrices(16.0F / 9.0F), 800.0, 450.0, 1600.0, 900.0);

  const std::array<double, 3>& o = ray.origin;
  const std::array<double, 3>& d = ray.direction;
  const std::array<double, 3> to_target{t3[0] - o[0], t3[1] - o[1], t3[2] - o[2]};
  const double t = (to_target[0] * d[0]) + (to_target[1] * d[1]) + (to_target[2] * d[2]);
  const double px = o[0] + (t * d[0]) - t3[0];
  const double py = o[1] + (t * d[1]) - t3[1];
  const double pz = o[2] + (t * d[2]) - t3[2];
  EXPECT_NEAR(std::sqrt((px * px) + (py * py) + (pz * pz)), 0.0, 1e-3);
}

// Picking must resolve the same lane under the same pixel in either projection
// — the O/P toggle changes how the scene looks, never what a click hits.
TEST(Pick, OrthoHitsTheSameQuadAsPerspective) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  OrbitCamera camera;
  camera.frame({5.0F, 5.0F, 0.0F}, 8.0F); // centred on the quad
  camera.set_view(0.0F, OrbitCamera::kTopDownPitch);

  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);
  const Ray perspective_ray = make_pick_ray(camera.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  const auto perspective_hit = pick(mesh, aabbs, perspective_ray);
  ASSERT_TRUE(perspective_hit.has_value());

  camera.set_projection(ProjectionMode::Orthographic);
  const Ray ortho_ray = make_pick_ray(camera.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  const auto ortho_hit = pick(mesh, aabbs, ortho_ray);
  ASSERT_TRUE(ortho_hit.has_value()) << "the same pixel must still hit the quad";
  EXPECT_EQ(ortho_hit->lane, perspective_hit->lane);
  EXPECT_NEAR(ortho_hit->position[0], perspective_hit->position[0], 1e-2);
  EXPECT_NEAR(ortho_hit->position[1], perspective_hit->position[1], 1e-2);
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

TEST(Pick, SurfaceIsSelectable) {
  // #215: an enclosed-area ground surface is its own pickable entity — a ray
  // into the interior (no road covers it) reports the SurfaceId with
  // road/lane/junction left invalid.
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.surfaces.push_back(make_quad_surface(network, 0.0, 10.0));

  const auto hit = pick(mesh, {}, straight_down(5.0, 5.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->surface, mesh.surfaces[0].surface);
  EXPECT_FALSE(hit->road.is_valid());
  EXPECT_FALSE(hit->lane.is_valid());
  EXPECT_FALSE(hit->junction.is_valid());
  EXPECT_FALSE(hit->object.is_valid());
  EXPECT_NEAR(hit->position[2], 0.0, 1e-9);
}

TEST(Pick, RoadOverSurfaceWinsOnTie) {
  // A road patch coplanar with (or above) the surface still wins the pick, so
  // the bounding roads stay grabbable; the surface only claims the open
  // interior no road covers (mirrors the junction-floor tie rule).
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.surfaces.push_back(make_quad_surface(network, 0.0, 10.0));
  mesh.roads.push_back(make_quad_road(network, "1", 0.1, 10.0));
  const auto aabbs = compute_road_aabbs(mesh);

  const auto hit = pick(mesh, aabbs, straight_down(5.0, 5.0));
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->road, mesh.roads[0].road);
  EXPECT_FALSE(hit->surface.is_valid());
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

TEST(Pick, HitsAPlacedSignalInFrontOfTheRoad) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_quad_road(network, "1", 0.0, 50.0));
  const SignalId light{.index = 9, .gen = 0};
  mesh.signal_instances.push_back(SignalInstance{.signal = light,
                                                 .road = mesh.roads[0].road,
                                                 .model_id = "signal_light",
                                                 .position = {25.0, 25.0, 0.0},
                                                 .heading = 0.0});
  const auto aabbs = compute_road_aabbs(mesh);

  // Straight down through the signal: its bounding sphere wins over the road.
  const auto on_signal = pick(mesh, aabbs, straight_down(25.0, 25.0));
  ASSERT_TRUE(on_signal.has_value());
  EXPECT_EQ(on_signal->signal, light);
  EXPECT_EQ(on_signal->road, mesh.roads[0].road);
  EXPECT_FALSE(on_signal->object.is_valid());

  // Away from the signal the road is still picked (signal invalid).
  const auto on_road = pick(mesh, aabbs, straight_down(5.0, 5.0));
  ASSERT_TRUE(on_road.has_value());
  EXPECT_FALSE(on_road->signal.is_valid());
}

TEST(NearestLaneBoundary, ResolvesTheEdgeAndInsertPositionFromCursorT) {
  RoadNetwork network;
  const std::array<Waypoint, 2> waypoints{Waypoint{0.0, 0.0}, Waypoint{100.0, 0.0}};
  const auto road_id = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default());
  ASSERT_TRUE(road_id.has_value());
  // two_lane_default boundaries at any s: [+3.5, 0, -3.5, -4.5] (+1 | centre |
  // -1 | -2 shoulder), lane offset 0.

  // Just inside the -1 | -2 edge: picks that edge, carve inserts at -1 (right).
  auto edge = nearest_lane_boundary(network, *road_id, 50.0, -3.4);
  ASSERT_TRUE(edge.has_value());
  EXPECT_EQ(edge->side, -1);
  EXPECT_EQ(edge->at_odr_id, -1);
  EXPECT_NEAR(edge->t, -3.5, 1e-9);

  // Near the outer shoulder edge: carve inserts at -2.
  auto shoulder = nearest_lane_boundary(network, *road_id, 50.0, -4.4);
  ASSERT_TRUE(shoulder.has_value());
  EXPECT_EQ(shoulder->side, -1);
  EXPECT_EQ(shoulder->at_odr_id, -2);
  EXPECT_NEAR(shoulder->t, -4.5, 1e-9);

  // Outer edge of the +1 lane: left side, insert at +1.
  auto left = nearest_lane_boundary(network, *road_id, 50.0, 3.4);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->side, 1);
  EXPECT_EQ(left->at_odr_id, 1);
  EXPECT_NEAR(left->t, 3.5, 1e-9);

  // Nearest the centre line, cursor slightly right: carve on the right.
  auto centre = nearest_lane_boundary(network, *road_id, 50.0, -0.2);
  ASSERT_TRUE(centre.has_value());
  EXPECT_EQ(centre->side, -1);
  EXPECT_EQ(centre->at_odr_id, -1);

  // A stale road yields nothing, not a crash.
  EXPECT_FALSE(nearest_lane_boundary(network, RoadId{}, 50.0, -3.4).has_value());
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

namespace {

/// A lone tree at (25, 25) rendered at `scale` (#335) — no road, so the pick
/// result is entirely the prop's bounding sphere.
NetworkMesh lone_tree(double scale) {
  NetworkMesh mesh;
  mesh.objects.push_back(ObjectInstance{.object = ObjectId{.index = 7, .gen = 0},
                                        .road = RoadId{.index = 1, .gen = 0},
                                        .model_id = "tree_pine",
                                        .position = {25.0, 25.0, 0.0},
                                        .heading = 0.0,
                                        .scale = scale});
  return mesh;
}

} // namespace

// tree_pine is 4.2 m tall / 1.2 m in radius, so its unit hit sphere has radius
// max(1.2, 2.1) = 2.1 m. A ray 3 m off-axis clears it at model size but must hit
// once the prop is drawn at twice that size.
TEST(Pick, ScaledPropGrowsHitSphere) {
  EXPECT_FALSE(pick(lone_tree(1.0), {}, straight_down(28.0, 25.0)).has_value());
  EXPECT_TRUE(pick(lone_tree(2.0), {}, straight_down(28.0, 25.0)).has_value());
}

TEST(Pick, ShrunkPropMisses) {
  EXPECT_TRUE(pick(lone_tree(1.0), {}, straight_down(26.0, 25.0)).has_value());
  EXPECT_FALSE(pick(lone_tree(0.25), {}, straight_down(26.0, 25.0)).has_value());
}

// --- screen-space polyline distance (p4-s6, issue #227) ----------------------
// The hit test for geometry with NO mesh proxy: a junction's connecting-road
// paths are deliberately not tessellated, so the Maneuver tool measures the
// cursor against the PROJECTED path instead of ray-casting it.

TEST(ScreenDistanceToPolyline, MeasuresToTheNearestPointOnTheNearestSegment) {
  OrbitCamera camera;
  constexpr double kW = 1600.0;
  constexpr double kH = 900.0;
  const CameraMatrices matrices = camera.matrices(static_cast<float>(kW / kH));

  // A ground-plane segment through the camera target, and the pixels its two
  // ends land on — the ground truth the distance is measured against.
  const std::array<float, 3> t3 = camera.target();
  const std::array<double, 3> a{t3[0] - 10.0, t3[1], 0.0};
  const std::array<double, 3> b{t3[0] + 10.0, t3[1], 0.0};
  const auto screen_a = project_to_screen(matrices, a[0], a[1], a[2], kW, kH);
  const auto screen_b = project_to_screen(matrices, b[0], b[1], b[2], kW, kH);
  ASSERT_TRUE(screen_a.has_value());
  ASSERT_TRUE(screen_b.has_value());

  const std::array<std::array<double, 3>, 2> path{a, b};
  const auto centre = ScreenContext{.camera = matrices,
                                    .px = ((*screen_a)[0] + (*screen_b)[0]) / 2.0,
                                    .py = ((*screen_a)[1] + (*screen_b)[1]) / 2.0,
                                    .width = kW,
                                    .height = kH};
  const std::optional<PolylineScreenHit> on_it = screen_distance_to_polyline(centre, path);
  ASSERT_TRUE(on_it.has_value());
  EXPECT_NEAR(on_it->distance, 0.0, 1e-6) << "the cursor is ON the projected segment";
  EXPECT_EQ(on_it->segment, 0U);
  EXPECT_NEAR(on_it->t, 0.5, 1e-6);

  // 30 px off the segment, measured along the segment's own screen NORMAL (the
  // projected segment is not axis-aligned — the camera has yaw).
  const double sx = (*screen_b)[0] - (*screen_a)[0];
  const double sy = (*screen_b)[1] - (*screen_a)[1];
  const double slen = std::hypot(sx, sy);
  ASSERT_GT(slen, 1.0);
  const double nx = -sy / slen;
  const double ny = sx / slen;
  ScreenContext above = centre;
  above.px += nx * 30.0;
  above.py += ny * 30.0;
  const std::optional<PolylineScreenHit> off = screen_distance_to_polyline(above, path);
  ASSERT_TRUE(off.has_value());
  EXPECT_NEAR(off->distance, 30.0, 1e-6);

  // Past an END the nearest point is that endpoint, not the infinite line.
  ScreenContext beyond = centre;
  beyond.px = (*screen_b)[0] + ((sx / slen) * 40.0);
  beyond.py = (*screen_b)[1] + ((sy / slen) * 40.0);
  const std::optional<PolylineScreenHit> capped = screen_distance_to_polyline(beyond, path);
  ASSERT_TRUE(capped.has_value());
  EXPECT_NEAR(capped->distance, 40.0, 1e-6);
  EXPECT_NEAR(capped->t, 1.0, 1e-9);
}

TEST(ScreenDistanceToPolyline, EmptyPolylineHasNoAnswer) {
  OrbitCamera camera;
  const ScreenContext screen{.camera = camera.matrices(16.0F / 9.0F),
                             .px = 800.0,
                             .py = 450.0,
                             .width = 1600.0,
                             .height = 900.0};
  EXPECT_FALSE(screen_distance_to_polyline(screen, {}).has_value());

  // A single sample is still something to hover: it answers as a point.
  const std::array<std::array<double, 3>, 1> dot{std::array<double, 3>{0.0, 0.0, 0.0}};
  EXPECT_TRUE(screen_distance_to_polyline(screen, dot).has_value());
}

} // namespace
} // namespace roadmaker::editor
