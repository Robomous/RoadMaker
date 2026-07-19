#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <QImage>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/prop_batching.hpp"
#include "render/scene_builder.hpp"

namespace roadmaker::editor {
namespace {

// The textured render mode uploads these bundled CC0 surface textures at GL
// init; guard that they decode from the qrc (the JPEG image handler is present
// and the /textures alias resolves) so a broken bundle fails here, not silently
// at render time as untextured surfaces.
TEST(SurfaceTextures, BundledJpegsDecodeFromQrc) {
  // Albedo + normal (PNG, lossless) + roughness for each catalog material,
  // including the new worn-asphalt variant. A broken bundle fails here, not
  // silently at render time as untextured surfaces.
  for (const char* path : {":/textures/asphalt.jpg",
                           ":/textures/asphalt_nor.png",
                           ":/textures/asphalt_rough.jpg",
                           ":/textures/asphalt_worn.jpg",
                           ":/textures/asphalt_worn_nor.png",
                           ":/textures/asphalt_worn_rough.jpg",
                           ":/textures/concrete.jpg",
                           ":/textures/concrete_nor.png",
                           ":/textures/concrete_rough.jpg"}) {
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
  // PBR-lite compatibility gate: no maps and full roughness, so the sun
  // specular lobe (scaled by 1 - roughness) contributes nothing — a default
  // Material renders pixel-identically to the pre-material shader.
  EXPECT_FALSE(mat.normal.valid());
  EXPECT_FALSE(mat.roughness.valid());
  EXPECT_FLOAT_EQ(mat.roughness_value, 1.0F);

  const DrawItem item;
  EXPECT_FALSE(item.material.base_color.valid());
  EXPECT_FALSE(item.material.normal.valid());
  EXPECT_FALSE(item.material.roughness.valid());
  EXPECT_FLOAT_EQ(item.material.roughness_value, 1.0F);
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

// WP6 (#237): a LanePatch's surface code flows onto the SceneItem so the
// viewport can resolve it through the MaterialCatalog; a code-less patch leaves
// the field empty (the SurfaceKind fallback then applies).
TEST(BuildScene, LanePatchSurfaceCodeBecomesSceneItemMaterial) {
  RoadNetwork network;
  const RoadId road_id = network.create_road("r", "1");
  const LaneSectionId section = network.add_lane_section(road_id, 0.0);
  const LaneId paved = network.add_lane(section, -1, LaneType::Driving);
  const LaneId bare = network.add_lane(section, -2, LaneType::Driving);

  NetworkMesh mesh;
  RoadMesh road;
  road.road = road_id;
  road.positions = {0, 0, 0, 10, 0, 0, 10, 5, 0, 0, 5, 0};
  road.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  road.lanes.push_back(RoadMesh::LanePatch{.lane = paved,
                                           .odr_lane_id = -1,
                                           .material = LaneType::Driving,
                                           .surface = "rm:asphalt_worn",
                                           .indices = {0, 1, 2}});
  road.lanes.push_back(RoadMesh::LanePatch{.lane = bare,
                                           .odr_lane_id = -2,
                                           .material = LaneType::Driving,
                                           .surface = "",
                                           .indices = {0, 2, 3}});
  mesh.roads.push_back(std::move(road));

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.items.size(), 2U);
  EXPECT_EQ(scene.items[0].material, "rm:asphalt_worn");
  EXPECT_TRUE(scene.items[1].material.empty());
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

TEST(BuildScene, SurfaceMaterialSelectsTheRenderClass) {
  // p6-s2: a surface with a stored material takes the matching SurfaceKind when
  // build_scene is given the network; a null network keeps the default Grass.
  RoadNetwork network;
  const SurfaceId surface_id = network.create_surface(Surface{});
  network.surface(surface_id)->material = "asphalt";

  NetworkMesh mesh;
  mesh.surfaces.push_back(
      SurfaceMesh{.surface = surface_id,
                  .mesh = SubMesh{.positions = {0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0},
                                  .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1},
                                  .indices = {0, 1, 2, 0, 2, 3}}});

  EXPECT_EQ(build_scene(mesh).items.at(0).surface, SurfaceKind::Grass); // no network
  EXPECT_EQ(build_scene(mesh, &network).items.at(0).surface, SurfaceKind::Asphalt);

  network.surface(surface_id)->material = "concrete";
  EXPECT_EQ(build_scene(mesh, &network).items.at(0).surface, SurfaceKind::Concrete);

  network.surface(surface_id)->material = "unknown_stuff";
  EXPECT_EQ(build_scene(mesh, &network).items.at(0).surface, SurfaceKind::Grass); // paved fallback
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

// A placed prop no longer bakes per-part SceneItems; it becomes an instance in a
// shared model batch (the instanced fast path). The batch carries the model
// parts ONCE and one ScenePropInstance per placement tagged with object + road.
TEST(BuildScene, EmitsPropInstanceTaggedWithObjectAndRoad) {
  constexpr ObjectId kObject{.index = 5, .gen = 0};
  constexpr RoadId kRoad{.index = 2, .gen = 0};
  NetworkMesh mesh;
  mesh.objects.push_back(ObjectInstance{.object = kObject,
                                        .road = kRoad,
                                        .model_id = "tree_pine",
                                        .position = {10.0, 20.0, 0.0},
                                        .heading = 0.0});

  const Scene scene = build_scene(mesh);
  EXPECT_TRUE(scene.items.empty()); // props travel in prop_batches, not items
  ASSERT_EQ(scene.prop_batches.size(), 1U);
  const ScenePropBatch& batch = scene.prop_batches.front();
  EXPECT_EQ(batch.model_id, "tree_pine");
  ASSERT_FALSE(batch.parts.empty()); // trunk + crown parts, baked once
  ASSERT_EQ(batch.instances.size(), 1U);
  const ScenePropInstance& inst = batch.instances.front();
  EXPECT_EQ(inst.object, kObject);
  EXPECT_EQ(inst.road, kRoad);
  EXPECT_FALSE(inst.signal.is_valid());
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
  const Scene scene = build_scene(mesh);
  EXPECT_TRUE(scene.items.empty());
  EXPECT_TRUE(scene.prop_batches.empty());
}

// Perf proxy for "1k props interactive": 1000 placements of one model collapse
// to a SINGLE batch (2 uploads + 2 instanced draws, not 2000/2000). No per-prop
// SceneItem leaks into items, and the model parts are baked exactly once.
TEST(BuildScene, ManyInstancesOfOneModelShareOneBatch) {
  NetworkMesh mesh;
  for (std::uint32_t i = 0; i < 1000U; ++i) {
    mesh.objects.push_back(ObjectInstance{.object = ObjectId{.index = i + 1U, .gen = 0},
                                          .road = RoadId{.index = 1U, .gen = 0},
                                          .model_id = "tree_pine",
                                          .position = {static_cast<double>(i), 0.0, 0.0},
                                          .heading = 0.0});
  }

  const Scene scene = build_scene(mesh);
  EXPECT_TRUE(scene.items.empty()); // zero per-prop SceneItems
  ASSERT_EQ(scene.prop_batches.size(), 1U);
  const ScenePropBatch& batch = scene.prop_batches.front();
  EXPECT_EQ(batch.instances.size(), 1000U);
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(batch.parts.size(), model->parts.size()); // parts baked once, not per prop
}

// prop_transform must reproduce the pre-instancing world-space bake exactly for a
// nonzero (x, y, z, heading): apply the column-major matrix to a known model
// vertex and compare against x' = c*x - s*y + px, y' = s*x + c*y + py, z' = z+pz.
TEST(PropTransform, MatchesWorldSpaceBake) {
  const std::array<double, 3> origin{3.0, -4.0, 1.5};
  const double heading = 0.7;
  const InstanceData m = prop_transform(origin, heading);

  const double vx = 2.0;
  const double vy = 1.0;
  const double vz = 0.5;
  const double c = std::cos(heading);
  const double s = std::sin(heading);
  const double wx = (c * vx) - (s * vy) + origin[0];
  const double wy = (s * vx) + (c * vy) + origin[1];
  const double wz = vz + origin[2];

  // Column-major: element(row r, col k) = model[k*4 + r].
  const auto& a = m.model;
  const double rx = (a[0] * vx) + (a[4] * vy) + (a[8] * vz) + a[12];
  const double ry = (a[1] * vx) + (a[5] * vy) + (a[9] * vz) + a[13];
  const double rz = (a[2] * vx) + (a[6] * vy) + (a[10] * vz) + a[14];
  EXPECT_NEAR(rx, wx, 1e-4);
  EXPECT_NEAR(ry, wy, 1e-4);
  EXPECT_NEAR(rz, wz, 1e-4);
  // Bottom row of an affine transform is (0,0,0,1).
  EXPECT_FLOAT_EQ(a[3], 0.0F);
  EXPECT_FLOAT_EQ(a[7], 0.0F);
  EXPECT_FLOAT_EQ(a[11], 0.0F);
  EXPECT_FLOAT_EQ(a[15], 1.0F);
}

// Distinct models form separate batches in first-encounter order; an
// object-backed instance carries an ObjectId, a signal-backed one a SignalId; and
// the scene bounds cover every instance.
TEST(BuildScene, DistinctModelsFormSeparateBatches) {
  NetworkMesh mesh;
  mesh.objects.push_back(ObjectInstance{.object = ObjectId{.index = 1, .gen = 0},
                                        .road = RoadId{.index = 1, .gen = 0},
                                        .model_id = "tree_pine",
                                        .position = {0.0, 0.0, 0.0},
                                        .heading = 0.0});
  mesh.objects.push_back(ObjectInstance{.object = ObjectId{.index = 2, .gen = 0},
                                        .road = RoadId{.index = 1, .gen = 0},
                                        .model_id = "tree_oak",
                                        .position = {40.0, 0.0, 0.0},
                                        .heading = 0.0});
  mesh.signal_instances.push_back(SignalInstance{.signal = SignalId{.index = 7, .gen = 0},
                                                 .road = RoadId{.index = 1, .gen = 0},
                                                 .model_id = "signal_light",
                                                 .position = {0.0, 40.0, 0.0},
                                                 .heading = 0.0});

  const Scene scene = build_scene(mesh);
  ASSERT_EQ(scene.prop_batches.size(), 3U);
  EXPECT_EQ(scene.prop_batches[0].model_id, "tree_pine"); // first encounter
  EXPECT_EQ(scene.prop_batches[1].model_id, "tree_oak");
  EXPECT_EQ(scene.prop_batches[2].model_id, "signal_light");

  // Object-backed instance carries an ObjectId, not a SignalId.
  const ScenePropInstance& tree = scene.prop_batches[0].instances.front();
  EXPECT_TRUE(tree.object.is_valid());
  EXPECT_FALSE(tree.signal.is_valid());
  // Signal-backed instance carries a SignalId, not an ObjectId.
  const ScenePropInstance& light = scene.prop_batches[2].instances.front();
  EXPECT_TRUE(light.signal.is_valid());
  EXPECT_FALSE(light.object.is_valid());

  // Bounds cover the spread of instance origins (0..40 in x and y).
  ASSERT_TRUE(scene.bounds.valid());
  EXPECT_LE(scene.bounds.lo[0], 0.0F);
  EXPECT_GE(scene.bounds.hi[0], 40.0F);
  EXPECT_LE(scene.bounds.lo[1], 0.0F);
  EXPECT_GE(scene.bounds.hi[1], 40.0F);
}

// The pure partition helper (prop_batching.hpp) splits a batch by highlight
// state: None → kept transforms (drawn together), anything else → highlighted
// indices (drawn individually), including the all- and none-highlighted edges.
TEST(PartitionPropBatch, SplitsByHighlightState) {
  std::vector<InstanceData> transforms(4);
  transforms[0].model[12] = 0.0F;
  transforms[1].model[12] = 1.0F;
  transforms[2].model[12] = 2.0F;
  transforms[3].model[12] = 3.0F;
  const std::vector<HighlightState> states{
      HighlightState::None, HighlightState::Hover, HighlightState::None, HighlightState::Selected};

  const PropPartition split = partition_prop_batch(states, transforms);
  ASSERT_EQ(split.kept.size(), 2U);
  EXPECT_FLOAT_EQ(split.kept[0].model[12], 0.0F); // instance 0
  EXPECT_FLOAT_EQ(split.kept[1].model[12], 2.0F); // instance 2
  ASSERT_EQ(split.highlighted.size(), 2U);
  EXPECT_EQ(split.highlighted[0], 1U);
  EXPECT_EQ(split.highlighted[1], 3U);
}

TEST(PartitionPropBatch, NoneHighlightedKeepsAll) {
  std::vector<InstanceData> transforms(3);
  const std::vector<HighlightState> states(3, HighlightState::None);
  const PropPartition split = partition_prop_batch(states, transforms);
  EXPECT_EQ(split.kept.size(), 3U);
  EXPECT_TRUE(split.highlighted.empty());
}

TEST(PartitionPropBatch, AllHighlightedKeepsNone) {
  std::vector<InstanceData> transforms(3);
  const std::vector<HighlightState> states(3, HighlightState::Selected);
  const PropPartition split = partition_prop_batch(states, transforms);
  EXPECT_TRUE(split.kept.empty());
  ASSERT_EQ(split.highlighted.size(), 3U);
  EXPECT_EQ(split.highlighted[0], 0U);
  EXPECT_EQ(split.highlighted[2], 2U);
}

} // namespace
} // namespace roadmaker::editor
