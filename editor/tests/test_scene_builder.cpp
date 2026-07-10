#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include "render/scene_builder.hpp"

namespace roadmaker::editor {
namespace {

TEST(LaneColor, DistinguishesCoreMaterials) {
  EXPECT_NE(lane_color(LaneType::Driving), lane_color(LaneType::Sidewalk));
  EXPECT_NE(lane_color(LaneType::Driving), lane_color(LaneType::Shoulder));
  // Unknown-ish types share the fallback gray.
  EXPECT_EQ(lane_color(LaneType::None), lane_color(LaneType::Other));
}

TEST(ToRenderData, NarrowsDoublesAndCopiesIndices) {
  const std::vector<double> positions{0.0, 1.5, -2.25};
  const std::vector<double> normals{0.0, 0.0, 1.0};
  const std::vector<std::uint32_t> indices{0, 1, 2};
  const std::array<float, 4> color{0.1F, 0.2F, 0.3F, 1.0F};

  const RenderMeshData data = to_render_data(positions, normals, indices, color);
  ASSERT_EQ(data.positions.size(), 3U);
  EXPECT_FLOAT_EQ(data.positions[1], 1.5F);
  EXPECT_FLOAT_EQ(data.positions[2], -2.25F);
  EXPECT_EQ(data.indices, indices);
  EXPECT_EQ(data.color, color);
  EXPECT_EQ(data.kind, PrimitiveKind::Triangles);
}

TEST(MakeGrid, LineCountsMatchTheSquare) {
  const RenderMeshData grid = make_grid();
  EXPECT_EQ(grid.kind, PrimitiveKind::Lines);
  // 201 stations, 2 lines each, 2 vertices per line, 3 floats per vertex.
  EXPECT_EQ(grid.positions.size(), 201U * 2U * 2U * 3U);
  EXPECT_EQ(grid.indices.size(), 201U * 4U);
  EXPECT_TRUE(grid.normals.empty());
}

TEST(BuildScene, FlattensPatchesMarkingsAndFloors) {
  RoadNetwork network;
  const RoadId road_id = network.create_road("r", "1");
  const LaneSectionId section = network.add_lane_section(road_id, 0.0);
  const LaneId lane_id = network.add_lane(section, -1, LaneType::Driving);

  NetworkMesh mesh;
  RoadMesh road;
  road.road = road_id;
  road.positions = {0, 0, 0, 10, 0, 0, 10, 5, 2, 0, 5, 2};
  road.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  road.lanes.push_back(RoadMesh::LanePatch{.lane = lane_id,
                                           .odr_lane_id = -1,
                                           .material = LaneType::Driving,
                                           .indices = {0, 1, 2, 0, 2, 3}});
  road.markings.push_back(SubMesh{.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0},
                                  .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1},
                                  .indices = {0, 1, 2}});
  mesh.roads.push_back(std::move(road));
  mesh.junction_floors.push_back(JunctionFloor{
      .junction = {},
      .mesh = SubMesh{.positions = {-5, -5, -1, 0, -5, -1, 0, 0, -1},
                      .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1},
                      .indices = {0, 1, 2}}});

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 3U);

  const SceneItem& patch = scene.items[0];
  EXPECT_EQ(patch.road, road_id);
  EXPECT_EQ(patch.lane, lane_id);
  EXPECT_EQ(patch.data.color, lane_color(LaneType::Driving));

  const SceneItem& marking = scene.items[1];
  EXPECT_EQ(marking.road, road_id);
  EXPECT_FALSE(marking.lane.is_valid());

  const SceneItem& floor = scene.items[2];
  EXPECT_FALSE(floor.road.is_valid());
  EXPECT_FALSE(floor.lane.is_valid());

  ASSERT_TRUE(scene.bounds.valid());
  EXPECT_FLOAT_EQ(scene.bounds.lo[0], -5.0F);
  EXPECT_FLOAT_EQ(scene.bounds.hi[0], 10.0F);
  EXPECT_FLOAT_EQ(scene.bounds.lo[2], -1.0F);
  EXPECT_FLOAT_EQ(scene.bounds.hi[2], 2.0F);
}

TEST(BuildScene, EmptyMeshHasInvalidBounds) {
  const Scene scene = build_scene(NetworkMesh{});
  EXPECT_TRUE(scene.items.empty());
  EXPECT_FALSE(scene.bounds.valid());
}

TEST(SceneBounds, FramingRadiusUsesPlanExtentWithFloor) {
  SceneBounds bounds;
  bounds.lo = {0.0F, 0.0F, 0.0F};
  bounds.hi = {60.0F, 20.0F, 5.0F};
  EXPECT_FLOAT_EQ(bounds.framing_radius(), 30.0F);

  SceneBounds tiny;
  tiny.lo = {0.0F, 0.0F, 0.0F};
  tiny.hi = {1.0F, 1.0F, 0.0F};
  EXPECT_FLOAT_EQ(tiny.framing_radius(), 5.0F); // 10 m floor
}

} // namespace
} // namespace roadmaker::editor
