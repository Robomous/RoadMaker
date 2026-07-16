#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <QImage>

#include "render/scene_builder.hpp"

namespace roadmaker::editor {
namespace {

// The textured render mode uploads these bundled CC0 surface textures at GL
// init; guard that they decode from the qrc (the JPEG image handler is present
// and the /textures alias resolves) so a broken bundle fails here, not silently
// at render time as untextured surfaces.
TEST(SurfaceTextures, BundledJpegsDecodeFromQrc) {
  for (const char* path : {":/textures/asphalt.jpg", ":/textures/concrete.jpg"}) {
    const QImage image{QString::fromLatin1(path)};
    ASSERT_FALSE(image.isNull()) << path;
    EXPECT_EQ(image.width(), 512);
    EXPECT_EQ(image.height(), 512);
  }
}

TEST(LaneColor, DistinguishesCoreMaterials) {
  EXPECT_NE(lane_color(LaneType::Driving), lane_color(LaneType::Sidewalk));
  EXPECT_NE(lane_color(LaneType::Driving), lane_color(LaneType::Shoulder));
  // Unknown-ish types share the fallback gray.
  EXPECT_EQ(lane_color(LaneType::None), lane_color(LaneType::Other));
}

TEST(MarkPaint, YellowIsDistinctFromTheWhiteDefault) {
  // A yellow centre line painted white is the bug this closes: every marking
  // used to take one hardcoded off-white regardless of its roadMark colour.
  const auto white = mark_paint(RoadMarkColor::Standard);
  EXPECT_NE(mark_paint(RoadMarkColor::Yellow), white);
  EXPECT_GT(mark_paint(RoadMarkColor::Yellow)[0], mark_paint(RoadMarkColor::Yellow)[2]);

  // Standard means "the standard colour for this mark type" — white for
  // everything RoadMaker authors — so it and White agree, and an unknown
  // colour falls back rather than rendering invisible.
  EXPECT_EQ(mark_paint(RoadMarkColor::White), white);
  EXPECT_EQ(mark_paint(RoadMarkColor::Other), white);
}

TEST(BuildScene, MarkingItemsTakeTheirRoadMarkColor) {
  RoadNetwork network;
  NetworkMesh mesh;
  RoadMesh road;
  road.road = network.create_road("r", "1");
  SubMesh yellow;
  yellow.positions = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0};
  yellow.normals = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
  yellow.indices = {0, 1, 2};
  yellow.mark_color = RoadMarkColor::Yellow;
  yellow.name = "road 1 lane 0 marking";
  road.markings.push_back(yellow);
  mesh.roads.push_back(std::move(road));

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 1U);
  EXPECT_EQ(scene.items.front().data.color, mark_paint(RoadMarkColor::Yellow));
  EXPECT_EQ(scene.items.front().surface, SurfaceKind::Paint);
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
  EXPECT_TRUE(data.uvs.empty()); // no uvs passed → untextured
}

TEST(ToRenderData, NarrowsUVsWhenProvided) {
  const std::vector<double> positions{0.0, 0.0, 0.0, 4.0, 0.0, 0.0};
  const std::vector<double> normals{0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
  const std::vector<std::uint32_t> indices{0, 1};
  const std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
  const std::vector<double> uvs{0.0, -1.5, 4.0, -1.5}; // u=s, v=t

  const RenderMeshData data = to_render_data(positions, normals, indices, color, uvs);
  ASSERT_EQ(data.uvs.size(), 4U);
  EXPECT_FLOAT_EQ(data.uvs[0], 0.0F);
  EXPECT_FLOAT_EQ(data.uvs[1], -1.5F);
  EXPECT_FLOAT_EQ(data.uvs[2], 4.0F);
}

// A default-constructed Material must reproduce the flat look: no base-color
// texture and no instance transforms, so the GL backend falls back to the
// per-mesh flat color exactly as the pre-material renderer did.
TEST(Material, DefaultsAreFlatAndUninstanced) {
  const Material mat;
  EXPECT_FALSE(mat.base_color.valid());
  EXPECT_FLOAT_EQ(mat.uv_scale, 0.25F); // 4 m tile
  EXPECT_FALSE(mat.unlit);

  const DrawItem item;
  EXPECT_FALSE(item.material.base_color.valid());
  EXPECT_TRUE(item.instances.empty());
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
  mesh.junction_floors.push_back(
      JunctionFloor{.junction = {},
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

TEST(BuildScene, EmitsGroundSurfaceItemTaggedWithSurfaceId) {
  // #215: a ground surface becomes one render item carrying its SurfaceId (for
  // pick-back) and the Grass material class, with road/lane/junction invalid.
  RoadNetwork network;
  const SurfaceId surface_id = network.create_surface(Surface{});

  NetworkMesh mesh;
  mesh.surfaces.push_back(
      SurfaceMesh{.surface = surface_id,
                  .mesh = SubMesh{.positions = {0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0},
                                  .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1},
                                  .indices = {0, 1, 2, 0, 2, 3}}});

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 1U);
  const SceneItem& ground = scene.items[0];
  EXPECT_EQ(ground.surface_id, surface_id);
  EXPECT_EQ(ground.surface, SurfaceKind::Grass);
  EXPECT_FALSE(ground.road.is_valid());
  EXPECT_FALSE(ground.lane.is_valid());
  EXPECT_FALSE(ground.junction.is_valid());
  ASSERT_TRUE(scene.bounds.valid());
  EXPECT_FLOAT_EQ(scene.bounds.hi[0], 8.0F);
}

// The Sober preset must reproduce the M2 `0.35 + 0.65*lambert` grey shading:
// a white, normal-independent ambient (sky == ground) at 0.35 plus a white
// directional term at 0.65 along the original hardcoded light direction.
TEST(Lighting, SoberPresetReproducesFlatM2Shading) {
  const Environment sober = sober_lighting();
  EXPECT_EQ(sober.sky_color, sober.ground_color); // no hemisphere tint
  EXPECT_FLOAT_EQ(sober.sky_color[0], 1.0F);
  EXPECT_FLOAT_EQ(sober.ambient, 0.35F);
  EXPECT_FLOAT_EQ(sober.sun_color[0], 1.0F);
  EXPECT_FLOAT_EQ(sober.sun_intensity, 0.65F);
  EXPECT_FALSE(sober.procedural_sky);
  // Same sun direction the pre-Environment shader hardcoded.
  EXPECT_FLOAT_EQ(sober.sun_dir[0], 0.35F);
  EXPECT_FLOAT_EQ(sober.sun_dir[1], 0.25F);
  EXPECT_FLOAT_EQ(sober.sun_dir[2], 0.90F);
}

TEST(Lighting, TexturedPresetIsTheDaytimeDefault) {
  const Environment textured = textured_lighting();
  const Environment defaults; // struct defaults == the textured preset
  EXPECT_EQ(textured.sky_color, defaults.sky_color);
  EXPECT_TRUE(textured.procedural_sky);
  // Textured sky is a tinted hemisphere, unlike Sober's flat white.
  EXPECT_NE(textured.sky_color, sober_lighting().sky_color);
}

// The procedural ground plane sits just below the network floor so a road or
// junction surface at the floor draws over the opaque grass without z-fighting.
TEST(GroundBaseZ, SitsJustBelowTheNetworkFloor) {
  SceneBounds bounds;
  bounds.lo = {-10.0F, -10.0F, 2.0F};
  bounds.hi = {10.0F, 10.0F, 8.0F};
  EXPECT_FLOAT_EQ(ground_base_z(bounds), 2.0F - 0.05F);
}

TEST(GroundBaseZ, DefaultsBelowTheZeroDatumWithoutGeometry) {
  EXPECT_FLOAT_EQ(ground_base_z(SceneBounds{}), -0.05F);
}

TEST(SurfaceFor, MapsLaneTypesToTexturedClasses) {
  EXPECT_EQ(surface_for(LaneType::Driving), SurfaceKind::Asphalt);
  EXPECT_EQ(surface_for(LaneType::Shoulder), SurfaceKind::Asphalt);
  EXPECT_EQ(surface_for(LaneType::Sidewalk), SurfaceKind::Concrete);
  EXPECT_EQ(surface_for(LaneType::Curb), SurfaceKind::Concrete);
}

TEST(BuildScene, TagsSurfaceClassesForTexturedMode) {
  NetworkMesh mesh;
  RoadMesh road;
  road.road = RoadId{.index = 1, .gen = 0};
  road.positions = {0, 0, 0, 10, 0, 0, 10, 5, 0, 0, 5, 0};
  road.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  road.lanes.push_back(RoadMesh::LanePatch{.lane = {},
                                           .odr_lane_id = -1,
                                           .material = LaneType::Sidewalk,
                                           .indices = {0, 1, 2, 0, 2, 3}});
  road.markings.push_back(SubMesh{.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0},
                                  .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1},
                                  .indices = {0, 1, 2}});
  mesh.roads.push_back(std::move(road));
  mesh.junction_floors.push_back(
      JunctionFloor{.junction = {},
                    .mesh = SubMesh{.positions = {-5, -5, -1, 0, -5, -1, 0, 0, -1},
                                    .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1},
                                    .indices = {0, 1, 2}}});

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 3U);
  EXPECT_EQ(scene.items[0].surface, SurfaceKind::Concrete); // sidewalk lane
  EXPECT_EQ(scene.items[1].surface, SurfaceKind::Paint);    // lane marking
  EXPECT_EQ(scene.items[2].surface, SurfaceKind::Asphalt);  // junction floor
}

TEST(BuildScene, RoadPatchesInheritSharedGridUVs) {
  NetworkMesh mesh;
  RoadMesh road;
  road.road = RoadId{.index = 1, .gen = 0};
  road.positions = {0, 0, 0, 10, 0, 0, 10, 5, 0, 0, 5, 0};
  road.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  road.uvs = {0, 0, 10, 0, 10, 5, 0, 5}; // u=s, v=t per grid vertex
  road.lanes.push_back(RoadMesh::LanePatch{
      .lane = {}, .odr_lane_id = -1, .material = LaneType::Driving, .indices = {0, 1, 2, 0, 2, 3}});
  mesh.roads.push_back(std::move(road));

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 1U);
  // The whole shared grid's UVs travel with the patch (indices select into it).
  ASSERT_EQ(scene.items[0].data.uvs.size(), 8U);
  EXPECT_FLOAT_EQ(scene.items[0].data.uvs[2], 10.0F);
  EXPECT_FLOAT_EQ(scene.items[0].data.uvs[5], 5.0F);
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

TEST(BuildScene, EmitsPropItemsTaggedWithObjectAndRoad) {
  constexpr ObjectId kObject{.index = 5, .gen = 0};
  constexpr RoadId kRoad{.index = 2, .gen = 0};
  NetworkMesh mesh;
  mesh.objects.push_back(ObjectInstance{.object = kObject,
                                        .road = kRoad,
                                        .model_id = "tree_pine",
                                        .position = {10.0, 20.0, 0.0},
                                        .heading = 0.0});

  const Scene scene = build_scene(mesh);
  ASSERT_FALSE(scene.items.empty()); // trunk + crown parts
  for (const SceneItem& item : scene.items) {
    EXPECT_EQ(item.object, kObject);
    EXPECT_EQ(item.road, kRoad);
    EXPECT_FALSE(item.lane.is_valid());
  }
  ASSERT_TRUE(scene.bounds.valid());
  EXPECT_NEAR(scene.bounds.center()[0], 10.0F, 2.0F);
  EXPECT_NEAR(scene.bounds.center()[1], 20.0F, 2.0F);
}

TEST(BuildScene, UnknownPropModelEmitsNothing) {
  NetworkMesh mesh;
  mesh.objects.push_back(ObjectInstance{.object = ObjectId{.index = 1, .gen = 0},
                                        .road = RoadId{.index = 1, .gen = 0},
                                        .model_id = "not_a_prop",
                                        .position = {0.0, 0.0, 0.0},
                                        .heading = 0.0});
  EXPECT_TRUE(build_scene(mesh).items.empty());
}

} // namespace
} // namespace roadmaker::editor
