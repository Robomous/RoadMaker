#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

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

  // 5 road nodes + 1 junction floor node in the default scene.
  ASSERT_EQ(model.scenes.size(), 1U);
  EXPECT_EQ(model.scenes[0].nodes.size(), 6U);

  // Every accessor references a valid buffer view and the single buffer.
  EXPECT_EQ(model.buffers.size(), 1U);
  for (const auto& accessor : model.accessors) {
    ASSERT_GE(accessor.bufferView, 0);
    ASSERT_LT(static_cast<std::size_t>(accessor.bufferView), model.bufferViews.size());
  }
}

TEST(Gltf, ExportingAnEmptyMeshFailsCleanly) {
  const roadmaker::NetworkMesh empty;
  const auto result = roadmaker::export_glb(empty, temp_glb("rm_empty.glb"));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}
