// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/assets/prop_library.hpp"
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
      // The tree declares exactly the model height, so it exports at unit size.
      ASSERT_EQ(node.scale.size(), 3U);
      EXPECT_DOUBLE_EQ(node.scale[0], 1.0);
    }
  }
  EXPECT_TRUE(found_node);
}

// A prop resized in the editor must export at its rendered size (#335) — a
// glTF consumer sees the 8.4 m tree the viewport shows, not a 4.2 m one.
// Signals are not resizable, so they stay unit in the same file.
TEST(Gltf, ScaledPropExportsAUniformNodeScale) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                   roadmaker::Waypoint{.x = 100.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default());
  ASSERT_TRUE(road.has_value());
  const roadmaker::props::PropModel* model = roadmaker::props::model("tree_pine");
  ASSERT_NE(model, nullptr);

  roadmaker::Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = roadmaker::ObjectType::Tree;
  tree.s = 50.0;
  tree.t = 6.0;
  tree.height = model->height * 2.0;
  network.add_object(*road, tree);

  roadmaker::Signal sign;
  sign.odr_id = "s1";
  sign.type = "274";
  sign.country = "DE";
  sign.dynamic = false;
  sign.s = 20.0;
  sign.t = -6.0;
  network.add_signal(*road, sign);

  const auto mesh = roadmaker::build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);

  const auto path = temp_glb("rm_scaled_tree.glb");
  ASSERT_TRUE(roadmaker::export_glb(mesh, path).has_value());
  tinygltf::Model gltf;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;
  ASSERT_TRUE(loader.LoadBinaryFromFile(&gltf, &err, &warn, path.string()));
  std::remove(path.string().c_str());

  bool checked_prop = false;
  bool checked_signal = false;
  for (const auto& node : gltf.nodes) {
    if (node.mesh < 0) {
      continue;
    }
    const std::string& mesh_name = gltf.meshes[static_cast<std::size_t>(node.mesh)].name;
    if (mesh_name == "tree_pine") {
      checked_prop = true;
      ASSERT_EQ(node.scale.size(), 3U);
      EXPECT_DOUBLE_EQ(node.scale[0], 2.0);
      EXPECT_DOUBLE_EQ(node.scale[1], 2.0);
      EXPECT_DOUBLE_EQ(node.scale[2], 2.0);
    } else if (mesh_name == mesh.signal_instances.front().model_id) {
      checked_signal = true;
      ASSERT_EQ(node.scale.size(), 3U);
      EXPECT_DOUBLE_EQ(node.scale[0], 1.0);
    }
  }
  EXPECT_TRUE(checked_prop);
  EXPECT_TRUE(checked_signal);
}

// --- editable sign-face textures (p4-s9, #230) ------------------------------

namespace {

// Authors a straight road with `count` StVO 310 text plates carrying `text`,
// exports, reloads, and returns the glTF model. A no-op image loader keeps the
// reload structural: tinygltf is built without stb_image, so it must not try to
// decode the embedded PNG (we only inspect the model graph).
tinygltf::Model export_text_signs(int count, const std::string& text) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{{.x = 0.0, .y = 0.0}, {.x = 120.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default());
  if (!road.has_value()) {
    throw std::runtime_error("author failed");
  }
  for (int i = 0; i < count; ++i) {
    roadmaker::Signal sign;
    sign.odr_id = "t" + std::to_string(i);
    sign.type = "310";
    sign.subtype = "-1";
    sign.country = "DE";
    sign.dynamic = false;
    sign.s = 20.0 + 10.0 * i;
    sign.t = 6.0;
    sign.text = text;
    network.add_signal(*road, sign);
  }
  const auto mesh = roadmaker::build_network_mesh(network, {});

  const auto path = temp_glb("rm_text_sign.glb");
  const auto exported = roadmaker::export_glb(mesh, path);
  if (!exported) {
    throw std::runtime_error("export_glb failed: " + exported.error().message);
  }
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  loader.SetImageLoader([](tinygltf::Image*,
                           const int,
                           std::string*,
                           std::string*,
                           int,
                           int,
                           const unsigned char*,
                           int,
                           void*) { return true; },
                        nullptr);
  std::string err;
  std::string warn;
  const bool loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path.string());
  std::remove(path.string().c_str());
  if (!loaded) {
    throw std::runtime_error("reload failed: " + err + " / " + warn);
  }
  return model;
}

} // namespace

TEST(GltfExport, TextSignEmbedsFaceTexture) {
  const tinygltf::Model model = export_text_signs(1, "City");

  // Exactly one embedded PNG image, referenced by a bufferView.
  ASSERT_EQ(model.images.size(), 1U);
  EXPECT_EQ(model.images[0].mimeType, "image/png");
  EXPECT_GE(model.images[0].bufferView, 0);

  // A CLAMP_TO_EDGE sampler and a texture pointing at the image.
  ASSERT_EQ(model.samplers.size(), 1U);
  EXPECT_EQ(model.samplers[0].wrapS, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
  EXPECT_EQ(model.samplers[0].wrapT, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
  ASSERT_EQ(model.textures.size(), 1U);
  EXPECT_EQ(model.textures[0].source, 0);
  EXPECT_EQ(model.textures[0].sampler, 0);

  // A material whose base colour is the texture, on a face mesh with TEXCOORD_0.
  bool found_face = false;
  for (const auto& gmesh : model.meshes) {
    for (const auto& prim : gmesh.primitives) {
      if (prim.attributes.count("TEXCOORD_0") == 0) {
        continue;
      }
      found_face = true;
      ASSERT_GE(prim.material, 0);
      const auto& mat = model.materials[static_cast<std::size_t>(prim.material)];
      EXPECT_EQ(mat.pbrMetallicRoughness.baseColorTexture.index, 0);
    }
  }
  EXPECT_TRUE(found_face) << "a face mesh with TEXCOORD_0 must be present";
}

TEST(GltfExport, SameTextSharesOneImage) {
  // Two identical plates reuse the same cached image/texture/material/mesh.
  const tinygltf::Model model = export_text_signs(2, "City");
  EXPECT_EQ(model.images.size(), 1U);
  EXPECT_EQ(model.textures.size(), 1U);
}

TEST(GltfExport, SignWithoutTextEmitsNoImages) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{{.x = 0.0, .y = 0.0}, {.x = 120.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default());
  ASSERT_TRUE(road.has_value());
  roadmaker::Signal sign; // a plain 274 disc — no face plate, no text
  sign.odr_id = "s1";
  sign.type = "274";
  sign.subtype = "50";
  sign.country = "DE";
  sign.dynamic = false;
  sign.s = 20.0;
  sign.t = -6.0;
  network.add_signal(*road, sign);
  const auto mesh = roadmaker::build_network_mesh(network, {});

  const auto path = temp_glb("rm_plain_sign.glb");
  ASSERT_TRUE(roadmaker::export_glb(mesh, path).has_value());
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;
  ASSERT_TRUE(loader.LoadBinaryFromFile(&model, &err, &warn, path.string()));
  std::remove(path.string().c_str());
  EXPECT_TRUE(model.images.empty());
  EXPECT_TRUE(model.textures.empty());
}
