// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Per-kind selection bounds (P1/GW-1 steps 7-9). These are the regression
// tests for the framing bugs the P1 discovery pass found: framing used to copy
// whole roads into a temp mesh, so a signal framed its entire road, a lane
// framed its whole road, and a junction framed nothing at all.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "viewport/framing.hpp"

namespace roadmaker::editor {
namespace {

/// A road mesh spanning [0,size]² at height z, with TWO lane patches: lane −1
/// on the near half (y ≤ size/2) and lane −2 on the far half. The split is what
/// makes "a lane frames only its own patch" testable.
RoadMesh make_two_lane_road(RoadNetwork& network, std::string odr_id, double size) {
  const RoadId road_id = network.create_road("", std::move(odr_id));
  const LaneSectionId section = network.add_lane_section(road_id, 0.0);
  const LaneId near_lane = network.add_lane(section, -1, LaneType::Driving);
  const LaneId far_lane = network.add_lane(section, -2, LaneType::Driving);

  const double half = size / 2.0;
  RoadMesh mesh;
  mesh.road = road_id;
  // 0..3 = near half (y in [0, half]), 4..7 = far half (y in [half, size]).
  mesh.positions = {0.0, 0.0,  0.0, size, 0.0,  0.0, size, half, 0.0, 0.0, half, 0.0,
                    0.0, half, 0.0, size, half, 0.0, size, size, 0.0, 0.0, size, 0.0};
  mesh.normals.assign(mesh.positions.size(), 0.0);
  mesh.lanes.push_back(RoadMesh::LanePatch{.lane = near_lane,
                                           .odr_lane_id = -1,
                                           .material = LaneType::Driving,
                                           .indices = {0, 1, 2, 0, 2, 3}});
  mesh.lanes.push_back(RoadMesh::LanePatch{.lane = far_lane,
                                           .odr_lane_id = -2,
                                           .material = LaneType::Driving,
                                           .indices = {4, 5, 6, 4, 6, 7}});
  return mesh;
}

JunctionFloor make_floor(RoadNetwork& network, std::string odr_id, double origin, double size) {
  const JunctionId junction_id = network.create_junction(std::move(odr_id), "");
  JunctionFloor floor;
  floor.junction = junction_id;
  floor.mesh.positions = {origin,
                          origin,
                          0.0,
                          origin + size,
                          origin,
                          0.0,
                          origin + size,
                          origin + size,
                          0.0,
                          origin,
                          origin + size,
                          0.0};
  return floor;
}

/// A ground surface (#215) quad [origin, origin+size]² at z=0, keyed to a fresh
/// surface id. Only positions matter to framing (it grows over them).
SurfaceMesh make_surface(RoadNetwork& network, double origin, double size) {
  const SurfaceId surface_id = network.create_surface(Surface{});
  SurfaceMesh surface;
  surface.surface = surface_id;
  surface.mesh.positions = {origin,
                            origin,
                            0.0,
                            origin + size,
                            origin,
                            0.0,
                            origin + size,
                            origin + size,
                            0.0,
                            origin,
                            origin + size,
                            0.0};
  return surface;
}

TEST(SelectionBounds, RoadSelectionFramesTheWholeRoad) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  const RoadId road = mesh.roads[0].road;

  const std::vector<SelectionEntry> entries{{.road = road}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  EXPECT_FLOAT_EQ(bounds.lo[0], 0.0F);
  EXPECT_FLOAT_EQ(bounds.hi[0], 20.0F);
  EXPECT_FLOAT_EQ(bounds.lo[1], 0.0F);
  EXPECT_FLOAT_EQ(bounds.hi[1], 20.0F);
}

// The bug: selecting a lane framed its whole road. A lane patch indexes into
// the road's shared vertex array, so only the vertices its triangles touch
// belong to it.
TEST(SelectionBounds, LaneSelectionFramesOnlyThatLanesPatch) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  const RoadId road = mesh.roads[0].road;
  const LaneId near_lane = mesh.roads[0].lanes[0].lane;
  const LaneId far_lane = mesh.roads[0].lanes[1].lane;

  const std::vector<SelectionEntry> near_entry{{.road = road, .lane = near_lane}};
  const SceneBounds near_bounds = selection_bounds(mesh, near_entry);
  ASSERT_TRUE(near_bounds.valid());
  EXPECT_FLOAT_EQ(near_bounds.lo[1], 0.0F);
  EXPECT_FLOAT_EQ(near_bounds.hi[1], 10.0F) << "the near lane must not reach the road's far edge";

  const std::vector<SelectionEntry> far_entry{{.road = road, .lane = far_lane}};
  const SceneBounds far_bounds = selection_bounds(mesh, far_entry);
  ASSERT_TRUE(far_bounds.valid());
  EXPECT_FLOAT_EQ(far_bounds.lo[1], 10.0F);
  EXPECT_FLOAT_EQ(far_bounds.hi[1], 20.0F);
}

// The bug: junction selections were ignored outright.
TEST(SelectionBounds, JunctionSelectionFramesItsFloor) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  mesh.junction_floors.push_back(make_floor(network, "j1", 100.0, 8.0));
  const JunctionId junction = mesh.junction_floors[0].junction;

  const std::vector<SelectionEntry> entries{{.junction = junction}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  EXPECT_FLOAT_EQ(bounds.lo[0], 100.0F) << "the junction floor, not the road at the origin";
  EXPECT_FLOAT_EQ(bounds.hi[0], 108.0F);
}

TEST(SelectionBounds, SurfaceSelectionBounds) {
  // #215: framing a ground surface grows the bounds over its mesh — the
  // surface analog of framing a junction floor, and it must win over the road
  // at the origin the same way.
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  mesh.surfaces.push_back(make_surface(network, 100.0, 8.0));
  const SurfaceId surface = mesh.surfaces[0].surface;

  const std::vector<SelectionEntry> entries{{.surface = surface}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  EXPECT_FLOAT_EQ(bounds.lo[0], 100.0F) << "the surface, not the road at the origin";
  EXPECT_FLOAT_EQ(bounds.hi[0], 108.0F);
}

// The bug: selecting a signal framed its whole owning road. The entry carries
// that road, so the signal kind must win.
TEST(SelectionBounds, SignalSelectionFramesTheSignalNotItsRoad) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 200.0));
  const RoadId road = mesh.roads[0].road;

  Signal sign;
  sign.odr_id = "s1";
  const SignalId signal = network.add_signal(road, sign);
  mesh.signal_instances.push_back(SignalInstance{.signal = signal,
                                                 .road = road,
                                                 .model_id = "signal_light",
                                                 .position = {150.0, 150.0, 0.0},
                                                 .heading = 0.0});

  const std::vector<SelectionEntry> entries{{.road = road, .signal = signal}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  // Tight around the signal — nowhere near the 200 m road's extent.
  EXPECT_LT(bounds.hi[0] - bounds.lo[0], 20.0F);
  EXPECT_NEAR((bounds.lo[0] + bounds.hi[0]) / 2.0F, 150.0F, 1e-3F);
  EXPECT_NEAR((bounds.lo[1] + bounds.hi[1]) / 2.0F, 150.0F, 1e-3F);
}

TEST(SelectionBounds, ObjectSelectionFramesThePropWithItsModelHeight) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 200.0));
  const RoadId road = mesh.roads[0].road;

  Object tree;
  tree.odr_id = "o1";
  tree.name = "tree_pine";
  const ObjectId object = network.add_object(road, tree);
  mesh.objects.push_back(ObjectInstance{.object = object,
                                        .road = road,
                                        .model_id = "tree_pine",
                                        .position = {40.0, 60.0, 0.0},
                                        .heading = 0.0});

  const std::vector<SelectionEntry> entries{{.road = road, .object = object}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  // The prop's declared volume, not a zero-size point the camera would slam into.
  EXPECT_NEAR(bounds.lo[0], 40.0F - static_cast<float>(model->radius), 1e-3F);
  EXPECT_NEAR(bounds.hi[2], static_cast<float>(model->height), 1e-3F);
}

TEST(SelectionBounds, ObjectSelectionFramesScaledProp) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 200.0));
  const RoadId road = mesh.roads[0].road;

  Object tree;
  tree.odr_id = "o1";
  tree.name = "tree_pine";
  const ObjectId object = network.add_object(road, tree);
  mesh.objects.push_back(ObjectInstance{.object = object,
                                        .road = road,
                                        .model_id = "tree_pine",
                                        .position = {40.0, 60.0, 0.0},
                                        .heading = 0.0,
                                        .scale = 2.0});

  const std::vector<SelectionEntry> entries{{.road = road, .object = object}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  // Framing follows the RENDERED size — zooming to a resized prop must not cut
  // it off at model height (#335).
  EXPECT_NEAR(bounds.lo[0], 40.0F - static_cast<float>(model->radius * 2.0), 1e-3F);
  EXPECT_NEAR(bounds.hi[2], static_cast<float>(model->height * 2.0), 1e-3F);
}

TEST(SelectionBounds, UnknownPropModelStillFramesSomething) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 50.0));
  const RoadId road = mesh.roads[0].road;
  Object thing;
  thing.odr_id = "o1";
  const ObjectId object = network.add_object(road, thing);
  mesh.objects.push_back(ObjectInstance{
      .object = object, .road = road, .model_id = "no_such_model", .position = {5.0, 5.0, 0.0}});

  const std::vector<SelectionEntry> entries{{.road = road, .object = object}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  EXPECT_GT(bounds.hi[0] - bounds.lo[0], 0.0F) << "never a zero-size box";
}

TEST(SelectionBounds, MixedSelectionReturnsTheUnion) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  mesh.junction_floors.push_back(make_floor(network, "j1", 100.0, 8.0));
  const RoadId road = mesh.roads[0].road;
  const JunctionId junction = mesh.junction_floors[0].junction;

  const std::vector<SelectionEntry> entries{{.road = road}, {.junction = junction}};
  const SceneBounds bounds = selection_bounds(mesh, entries);
  ASSERT_TRUE(bounds.valid());
  EXPECT_FLOAT_EQ(bounds.lo[0], 0.0F);   // the road at the origin...
  EXPECT_FLOAT_EQ(bounds.hi[0], 108.0F); // ...through the far junction
}

TEST(SelectionBounds, EmptySelectionAndEmptyMeshAreNotValid) {
  RoadNetwork network;
  NetworkMesh mesh;
  EXPECT_FALSE(selection_bounds(mesh, {}).valid());

  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));
  EXPECT_FALSE(selection_bounds(mesh, {}).valid()) << "no entries, no bounds";
}

TEST(SelectionBounds, StaleIdsContributeNothing) {
  RoadNetwork network;
  NetworkMesh mesh;
  mesh.roads.push_back(make_two_lane_road(network, "1", 20.0));

  const std::vector<SelectionEntry> entries{{.road = RoadId{}}};
  EXPECT_FALSE(selection_bounds(mesh, entries).valid())
      << "an id the mesh does not know must not frame the whole scene";
}

} // namespace
} // namespace roadmaker::editor
