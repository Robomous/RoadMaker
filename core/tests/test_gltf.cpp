#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <catch2/catch_test_macros.hpp>

// Reader-side of tinygltf for validation (implementation is compiled in
// gltf_exporter.cpp).
#include <tiny_gltf.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path temp_glb(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

tinygltf::Model export_and_reload(const char* sample_name, const char* out_name) {
  const auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample_name);
  REQUIRE(parsed.has_value());
  const auto mesh = roadmaker::build_network_mesh(parsed->network);

  const auto path = temp_glb(out_name);
  const auto exported = roadmaker::export_glb(mesh, path);
  REQUIRE(exported.has_value());

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;
  const bool loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path.string());
  INFO("tinygltf error: " << err << " warn: " << warn);
  REQUIRE(loaded);
  std::remove(path.string().c_str());
  return model;
}

} // namespace

TEST_CASE("straight road exports a valid glb", "[gltf]") {
  const tinygltf::Model model = export_and_reload("straight_road.xodr", "rm_straight.glb");

  REQUIRE(model.asset.version == "2.0");
  REQUIRE(model.meshes.size() == 1);
  REQUIRE(model.meshes[0].primitives.size() == 4 + 3); // lanes + markings
  REQUIRE_FALSE(model.materials.empty());

  // Y-up: the flat road at kernel z=0 must have glTF y ~ 0 for lane
  // surfaces (markings float 2 mm above) and lateral extent along z.
  const auto& primitive = model.meshes[0].primitives[0];
  const auto& accessor =
      model.accessors[static_cast<std::size_t>(primitive.attributes.at("POSITION"))];
  REQUIRE(accessor.minValues.size() == 3);
  REQUIRE(accessor.minValues[1] >= -1e-6); // y (up) >= 0
  REQUIRE(accessor.maxValues[1] <= 0.003); // markings lift only
  REQUIRE(accessor.minValues[2] <= -1e-6); // z spans -y_kernel
}

TEST_CASE("t_junction exports roads and floor nodes", "[gltf]") {
  const tinygltf::Model model = export_and_reload("t_junction.xodr", "rm_tjunction.glb");

  // 5 road nodes + 1 junction floor node in the default scene.
  REQUIRE(model.scenes.size() == 1);
  REQUIRE(model.scenes[0].nodes.size() == 6);

  // Every accessor references a valid buffer view and the single buffer.
  REQUIRE(model.buffers.size() == 1);
  for (const auto& accessor : model.accessors) {
    REQUIRE(accessor.bufferView >= 0);
    REQUIRE(static_cast<std::size_t>(accessor.bufferView) < model.bufferViews.size());
  }
}

TEST_CASE("exporting an empty mesh fails cleanly", "[gltf]") {
  const roadmaker::NetworkMesh empty;
  const auto result = roadmaker::export_glb(empty, temp_glb("rm_empty.glb"));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == roadmaker::ErrorCode::InvalidArgument);
}
