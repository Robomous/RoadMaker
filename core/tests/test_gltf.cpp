#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <vector>

// Reader-side of tinygltf for validation (implementation is compiled in
// gltf_exporter.cpp).
#include <tiny_gltf.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path temp_glb(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

/// Throwing helper: an uncaught exception fails the calling test.
tinygltf::Model export_and_reload(const char* sample_name, const char* out_name) {
  auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample_name);
  if (!parsed) {
    throw std::runtime_error("failed to load sample");
  }
  const auto mesh = roadmaker::build_network_mesh(parsed->network);

  const auto path = temp_glb(out_name);
  const auto exported = roadmaker::export_glb(mesh, path);
  if (!exported) {
    throw std::runtime_error("export_glb failed: " + exported.error().message);
  }

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;
  const bool loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path.string());
  std::remove(path.string().c_str());
  if (!loaded) {
    throw std::runtime_error("tinygltf reload failed: " + err + " / " + warn);
  }
  return model;
}

} // namespace

TEST(Gltf, StraightRoadExportsAValidGlb) {
  const tinygltf::Model model = export_and_reload("straight_road.xodr", "rm_straight.glb");

  EXPECT_EQ(model.asset.version, "2.0");
  ASSERT_EQ(model.meshes.size(), 1U);
  EXPECT_EQ(model.meshes[0].primitives.size(), 4U + 3U); // lanes + markings
  EXPECT_FALSE(model.materials.empty());

  // Y-up: the flat road at kernel z=0 must have glTF y ~ 0 for lane
  // surfaces (markings float 2 mm above) and lateral extent along z.
  const auto& primitive = model.meshes[0].primitives[0];
  const auto& accessor =
      model.accessors[static_cast<std::size_t>(primitive.attributes.at("POSITION"))];
  ASSERT_EQ(accessor.minValues.size(), 3U);
  EXPECT_GE(accessor.minValues[1], -1e-6); // y (up) >= 0
  EXPECT_LE(accessor.maxValues[1], 0.003); // markings lift only
  EXPECT_LE(accessor.minValues[2], -1e-6); // z spans -y_kernel
}

TEST(Gltf, TJunctionExportsRoadsAndFloorNodes) {
  const tinygltf::Model model = export_and_reload("t_junction.xodr", "rm_tjunction.glb");

  // 3 arm road nodes + 1 junction floor node in the default scene — the 2
  // connecting roads' surfaces are the floor itself (issue #103, no coplanar
  // double-draw).
  ASSERT_EQ(model.scenes.size(), 1U);
  EXPECT_EQ(model.scenes[0].nodes.size(), 4U);

  // Every accessor references a valid buffer view and the single buffer.
  EXPECT_EQ(model.buffers.size(), 1U);
  for (const auto& accessor : model.accessors) {
    ASSERT_GE(accessor.bufferView, 0);
    ASSERT_LT(static_cast<std::size_t>(accessor.bufferView), model.bufferViews.size());
  }
}

TEST(Gltf, GltfIncludesJunctionDetailSubmeshes) {
  // Authored corner overlays (p4-s2, issue #226) ride alongside their floor:
  // one extra node and mesh per detail submesh. Injected directly into the
  // NetworkMesh so this pins the EXPORTER, not the corner solve.
  auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(parsed.has_value());
  roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(parsed->network);
  ASSERT_FALSE(mesh.junction_floors.empty());
  ASSERT_TRUE(mesh.junction_floors[0].details.empty());

  const auto node_count = [](const roadmaker::NetworkMesh& m, const char* out_name) {
    const auto path = temp_glb(out_name);
    const auto exported = roadmaker::export_glb(m, path);
    EXPECT_TRUE(exported.has_value());
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;
    const bool loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path.string());
    std::remove(path.string().c_str());
    EXPECT_TRUE(loaded) << err << " / " << warn;
    return model.scenes.empty() ? 0U : model.scenes[0].nodes.size();
  };
  const std::size_t before = node_count(mesh, "rm_jct_details_before.glb");

  roadmaker::SubMesh wedge;
  wedge.material = roadmaker::LaneType::Sidewalk;
  wedge.surface = "concrete";
  wedge.name = "junction corner sidewalk";
  wedge.positions = {0.0, 0.0, 0.1, 1.0, 0.0, 0.1, 0.0, 1.0, 0.1};
  wedge.normals = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
  wedge.indices = {0, 1, 2};
  mesh.junction_floors[0].details.push_back(std::move(wedge));

  EXPECT_EQ(node_count(mesh, "rm_jct_details_after.glb"), before + 1U);
}

TEST(Gltf, ExportingAnEmptyMeshFailsCleanly) {
  const roadmaker::NetworkMesh empty;
  const auto result = roadmaker::export_glb(empty, temp_glb("rm_empty.glb"));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}

TEST(Gltf, TreePropExportsASharedMeshAndInstanceNode) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                   roadmaker::Waypoint{.x = 100.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default());
  ASSERT_TRUE(road.has_value());

  roadmaker::Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = roadmaker::ObjectType::Tree;
  tree.s = 50.0;
  tree.t = 6.0;
  tree.radius = 1.2;
  tree.height = 4.2;
  network.add_object(*road, tree);

  const auto mesh = roadmaker::build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);

  const auto path = temp_glb("rm_tree.glb");
  ASSERT_TRUE(roadmaker::export_glb(mesh, path).has_value());
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;
  ASSERT_TRUE(loader.LoadBinaryFromFile(&model, &err, &warn, path.string()));
  std::remove(path.string().c_str());

  // The prop is one shared mesh (trunk + crown primitives) named for its model.
  int prop_mesh = -1;
  for (std::size_t i = 0; i < model.meshes.size(); ++i) {
    if (model.meshes[i].name == "tree_pine") {
      prop_mesh = static_cast<int>(i);
      EXPECT_EQ(model.meshes[i].primitives.size(), 2U);
    }
  }
  ASSERT_GE(prop_mesh, 0);

  // ...referenced by an instance node placed at the tree's world pose. Y-up
  // frame (x, z, -y): translation ≈ (50, 0, -6).
  bool found_node = false;
  for (const auto& node : model.nodes) {
    if (node.mesh == prop_mesh) {
      found_node = true;
      ASSERT_EQ(node.translation.size(), 3U);
      EXPECT_NEAR(node.translation[0], 50.0, 1.5);
      EXPECT_NEAR(node.translation[2], -6.0, 1.5);
    }
  }
  EXPECT_TRUE(found_node);
}
