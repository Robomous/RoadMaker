/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/assets/sign_catalog.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"

// The exporter's own material vocabulary — core/tests has core/src on its
// include path, so the ground assertions below name the definitions rather
// than a copy of them.
#include <gtest/gtest.h>

#include <vector>

#include "io/mesh_export_common.hpp"

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

/// Writes `mesh` and reads it straight back — for the fixtures that are built
/// in code rather than loaded from a sample.
tinygltf::Model write_and_reload(const roadmaker::NetworkMesh& mesh, const char* out_name) {
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

/// A ~20 m square of four welded straights, which encloses one derivable
/// ground surface, plus a flat height field authored through the command layer.
/// Built in code because not one committed sample carries `rm:surface` or
/// `rm:terrain` — which is exactly how the missing ground (#390) stayed
/// invisible for two pillars.
roadmaker::RoadNetwork ground_network() {
  roadmaker::RoadNetwork network;
  const auto segment = [&network](double x0, double y0, double x1, double y1, const char* id) {
    const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{.x = x0, .y = y0},
                                                     roadmaker::Waypoint{.x = x1, .y = y1}};
    auto road = roadmaker::author_clothoid_road(
        network, waypoints, roadmaker::LaneProfile::two_lane_default(), "", id);
    EXPECT_TRUE(road.has_value());
  };
  segment(0.0, 0.0, 20.0, 0.0, "a");
  segment(20.0, 0.0, 20.0, 20.0, "b");
  segment(20.0, 20.0, 0.0, 20.0, "c");
  segment(0.0, 20.0, 0.0, 0.0, "d");
  roadmaker::derive_surfaces(network);
  EXPECT_GT(network.surface_count(), 0U) << "fixture derived no surface";

  auto create = roadmaker::edit::create_terrain_field(network);
  EXPECT_TRUE(create->apply(network).has_value());
  return network;
}

/// Material name → triangles, as the FILE stores it.
std::map<std::string, std::size_t> triangles_by_material(const tinygltf::Model& model) {
  std::map<std::string, std::size_t> totals;
  for (const tinygltf::Mesh& mesh : model.meshes) {
    for (const tinygltf::Primitive& primitive : mesh.primitives) {
      if (primitive.material < 0 || primitive.indices < 0) {
        continue;
      }
      const std::string& name = model.materials[static_cast<std::size_t>(primitive.material)].name;
      totals[name] += model.accessors[static_cast<std::size_t>(primitive.indices)].count / 3;
    }
  }
  return totals;
}

const tinygltf::Material* material_named(const tinygltf::Model& model, const std::string& name) {
  for (const tinygltf::Material& material : model.materials) {
    if (material.name == name) {
      return &material;
    }
  }
  return nullptr;
}

bool has_mesh_named(const tinygltf::Model& model, const std::string& name) {
  for (const tinygltf::Mesh& mesh : model.meshes) {
    if (mesh.name == name) {
      return true;
    }
  }
  return false;
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

// ------------------------------------------------- ground channels (#390)

TEST(Gltf, GroundSurfacesAndTerrainAreWritten) {
  const roadmaker::RoadNetwork network = ground_network();
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(mesh.surfaces.empty()) << "fixture has no surface — the test is vacuous";
  ASSERT_FALSE(mesh.terrain.indices.empty()) << "fixture has no terrain — the test is vacuous";

  const tinygltf::Model model = write_and_reload(mesh, "rm_ground.glb");

  // One node and one mesh per surface, plus one for the field. Named, so the
  // file is legible: the submeshes themselves are all called "surface".
  EXPECT_TRUE(has_mesh_named(model, "surface_0"));
  EXPECT_TRUE(has_mesh_named(model, "terrain"));

  const auto totals = triangles_by_material(model);
  const std::string grass = roadmaker::io_common::ground_material_name("");
  ASSERT_TRUE(totals.contains(grass)) << "no grass material in the file";
  ASSERT_TRUE(totals.contains(roadmaker::io_common::kTerrainMaterialName));
  EXPECT_GT(totals.at(grass), 0U);
  EXPECT_GT(totals.at(roadmaker::io_common::kTerrainMaterialName), 0U);

  // The colours are the ones the viewport draws, not a second palette.
  const tinygltf::Material* material = material_named(model, grass);
  ASSERT_NE(material, nullptr);
  const auto& base = material->pbrMetallicRoughness.baseColorFactor;
  ASSERT_EQ(base.size(), 4U);
  EXPECT_NEAR(base[0], roadmaker::io_common::kGrassColor[0], 1e-9);
  EXPECT_NEAR(base[1], roadmaker::io_common::kGrassColor[1], 1e-9);
  EXPECT_NEAR(base[2], roadmaker::io_common::kGrassColor[2], 1e-9);
}

TEST(Gltf, ASurfaceMaterialReachesTheFileAndSurvivesAChange) {
  // The material a ground surface wears lives on the ARENA record, which the
  // exporter never sees — it travels on the mesh (SubMesh::surface). This is
  // the test that fails if that stamp is dropped, or if a material edit stops
  // re-meshing the surface and the export goes stale behind the viewport.
  roadmaker::RoadNetwork network = ground_network();
  roadmaker::SurfaceId id;
  network.for_each_surface([&id](roadmaker::SurfaceId candidate, const roadmaker::Surface&) {
    if (!id.is_valid()) {
      id = candidate;
    }
  });
  ASSERT_TRUE(id.is_valid());

  const roadmaker::NetworkMesh grassy = roadmaker::build_network_mesh(network);
  const auto before = triangles_by_material(write_and_reload(grassy, "rm_ground_grass.glb"));
  EXPECT_TRUE(before.contains(roadmaker::io_common::ground_material_name("")));
  EXPECT_FALSE(before.contains(roadmaker::io_common::ground_material_name("asphalt")));

  auto paint = roadmaker::edit::set_surface_material(network, id, "asphalt");
  ASSERT_TRUE(paint != nullptr);
  ASSERT_TRUE(paint->apply(network).has_value());

  roadmaker::NetworkMesh painted = grassy;
  const std::vector<roadmaker::SurfaceId> dirty{id};
  roadmaker::remesh_surfaces(network, painted, dirty);

  const tinygltf::Model model = write_and_reload(painted, "rm_ground_paved.glb");
  const auto after = triangles_by_material(model);
  const std::string paved = roadmaker::io_common::ground_material_name("asphalt");
  ASSERT_TRUE(after.contains(paved)) << "the surface's material never reached the file";
  EXPECT_FALSE(after.contains(roadmaker::io_common::ground_material_name("")))
      << "the only surface is paved now, so no grass may remain";

  // Paved ground is written in the neutral pavement grey, not the grass green.
  const tinygltf::Material* material = material_named(model, paved);
  ASSERT_NE(material, nullptr);
  EXPECT_NEAR(material->pbrMetallicRoughness.baseColorFactor[0],
              roadmaker::io_common::kPavedGroundColor[0],
              1e-9);
}

TEST(Gltf, TwoUnpaintedSurfacesShareOneMaterial) {
  // Ground materials are cached by NAME. Without that, every surface would
  // create its own identical `ground_grass` entry.
  const roadmaker::RoadNetwork network = ground_network();
  roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  mesh.surfaces.push_back(mesh.surfaces.front()); // a second, identical ground

  const tinygltf::Model model = write_and_reload(mesh, "rm_ground_twice.glb");
  const std::string grass = roadmaker::io_common::ground_material_name("");
  std::size_t entries = 0;
  for (const tinygltf::Material& material : model.materials) {
    entries += material.name == grass ? 1U : 0U;
  }
  EXPECT_EQ(entries, 1U);
  EXPECT_TRUE(has_mesh_named(model, "surface_1"));
}

TEST(Gltf, ASceneOfNothingButGroundStillExports) {
  // The empty-mesh guard used to test only roads and junction floors, so real
  // geometry was refused (#390).
  const roadmaker::RoadNetwork network = ground_network();
  roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  mesh.roads.clear();
  mesh.junction_floors.clear();
  ASSERT_FALSE(mesh.terrain.indices.empty());

  const tinygltf::Model model = write_and_reload(mesh, "rm_ground_only.glb");
  EXPECT_TRUE(has_mesh_named(model, "terrain"));
  EXPECT_TRUE(has_mesh_named(model, "surface_0"));
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
  // Declared at the bundled model's own size, so it exports at unit scale.
  const roadmaker::props::PropModel* pine = roadmaker::props::model("tree_pine");
  ASSERT_NE(pine, nullptr);
  tree.radius = pine->radius;
  tree.height = pine->height;
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
// glTF consumer sees the double-height tree the viewport shows, not the model's.
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
    // A D3-1 street-name blade: the shipped pack's editable-legend sign, and
    // the only kind whose face is a texture rather than flat geometry.
    const roadmaker::signs::SignDef* def = roadmaker::signs::find_by_key("us.d3_1");
    if (def == nullptr) {
      throw std::runtime_error("no us.d3_1 catalogue entry");
    }
    roadmaker::Signal sign;
    sign.odr_id = "t" + std::to_string(i);
    sign.type = std::string(def->type);
    sign.subtype = std::string(def->subtype);
    sign.country = std::string(def->country);
    sign.dynamic = def->dynamic;
    sign.s = 20.0 + 10.0 * i;
    sign.t = 6.0;
    sign.text = text;
    network.add_signal(*road, sign);
  }
  const auto mesh = roadmaker::build_network_mesh(network, {});

  // Unique per call: two tests exercise this helper with different counts, and a
  // fixed temp name would let their export/read/remove race under `ctest -j`
  // (parallel-safety is a test contract — docs/testing/audit-2026-07.md).
  const auto path = temp_glb(("rm_text_sign_" + std::to_string(count) + ".glb").c_str());
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
