// glTF 2.0 (.glb) export. THE Z-up → Y-up boundary: everything upstream of
// this file is kernel frame (Z-up); everything in the written file is glTF
// frame (Y-up). Conversion: (x, y, z) → (x, z, −y).

#include "roadmaker/io/gltf_exporter.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include <cmath>

#include "mesh_export_common.hpp"

// tinygltf implementation lives in this TU only (its NO_STB/NO_EXTERNAL
// config macros come from the rm_tinygltf CMake target so every consumer
// agrees). RoadMaker writes fully self-contained binary glTF.
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace roadmaker {

namespace {

using io_common::kMarkingColor;

class GlbWriter {
public:
  Expected<void> write(const NetworkMesh& mesh, const std::filesystem::path& path) {
    model_.asset.version = "2.0";
    model_.asset.generator = "RoadMaker";
    model_.buffers.emplace_back();

    tinygltf::Scene scene;
    for (const RoadMesh& road : mesh.roads) {
      scene.nodes.push_back(add_road_node(road));
    }
    for (const JunctionFloor& floor : mesh.junction_floors) {
      // Floors carry the driving-lane material: one continuous asphalt with
      // the roads feeding them (a distinct junction color read as a patch).
      scene.nodes.push_back(add_submesh_node(floor.mesh, material_for(floor.mesh.material)));
    }
    // Placed props: one shared mesh per prop model, one node per instance —
    // idiomatic glTF instancing, so many trees stay one mesh in the file.
    for (const ObjectInstance& instance : mesh.objects) {
      const int node = add_prop_node(instance.model_id, instance.position, instance.heading);
      if (node >= 0) {
        scene.nodes.push_back(node);
      }
    }
    // Placed signals share the identical instancing path (one mesh per signal
    // model, one node per placement).
    for (const SignalInstance& instance : mesh.signal_instances) {
      const int node = add_prop_node(instance.model_id, instance.position, instance.heading);
      if (node >= 0) {
        scene.nodes.push_back(node);
      }
    }
    model_.scenes.push_back(std::move(scene));
    model_.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    const bool ok = writer.WriteGltfSceneToFile(&model_,
                                                path.string(),
                                                /*embedImages=*/true,
                                                /*embedBuffers=*/true,
                                                /*prettyPrint=*/false,
                                                /*writeBinary=*/true);
    if (!ok) {
      return make_error(ErrorCode::IoFailure, "failed to write glb", path.string());
    }
    return {};
  }

private:
  /// Kernel Z-up → glTF Y-up, doubles → floats (shared boundary rotation).
  static std::vector<float> to_gltf_frame(const std::vector<double>& xyz) {
    std::vector<float> out;
    out.reserve(xyz.size());
    for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
      const auto v = io_common::to_export_frame(xyz[i], xyz[i + 1], xyz[i + 2]);
      out.insert(out.end(), v.begin(), v.end());
    }
    return out;
  }

  int append_to_buffer(const void* data, std::size_t bytes, int target) {
    std::vector<unsigned char>& buffer = model_.buffers[0].data;
    // glTF requires 4-byte alignment for accessor offsets.
    while (buffer.size() % 4 != 0) {
      buffer.push_back(0);
    }
    const std::size_t offset = buffer.size();
    buffer.resize(offset + bytes);
    std::memcpy(buffer.data() + offset, data, bytes);

    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = offset;
    view.byteLength = bytes;
    view.target = target;
    model_.bufferViews.push_back(view);
    return static_cast<int>(model_.bufferViews.size() - 1);
  }

  int add_vec3_accessor(const std::vector<float>& data, bool with_min_max) {
    const int view =
        append_to_buffer(data.data(), data.size() * sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
    tinygltf::Accessor accessor;
    accessor.bufferView = view;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = TINYGLTF_TYPE_VEC3;
    accessor.count = data.size() / 3;
    if (with_min_max && !data.empty()) {
      std::array<double, 3> lo{std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::max()};
      std::array<double, 3> hi{std::numeric_limits<double>::lowest(),
                               std::numeric_limits<double>::lowest(),
                               std::numeric_limits<double>::lowest()};
      for (std::size_t i = 0; i < data.size(); i += 3) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
          lo[axis] = std::min(lo[axis], static_cast<double>(data[i + axis]));
          hi[axis] = std::max(hi[axis], static_cast<double>(data[i + axis]));
        }
      }
      accessor.minValues.assign(lo.begin(), lo.end());
      accessor.maxValues.assign(hi.begin(), hi.end());
    }
    model_.accessors.push_back(std::move(accessor));
    return static_cast<int>(model_.accessors.size() - 1);
  }

  int add_index_accessor(const std::vector<std::uint32_t>& indices) {
    const int view = append_to_buffer(indices.data(),
                                      indices.size() * sizeof(std::uint32_t),
                                      TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    tinygltf::Accessor accessor;
    accessor.bufferView = view;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    accessor.type = TINYGLTF_TYPE_SCALAR;
    accessor.count = indices.size();
    model_.accessors.push_back(std::move(accessor));
    return static_cast<int>(model_.accessors.size() - 1);
  }

  int material_for(LaneType type) {
    const auto found = lane_materials_.find(type);
    if (found != lane_materials_.end()) {
      return found->second;
    }
    const auto color = io_common::lane_material_color(type);
    const int index =
        add_material(io_common::lane_material_name(type), color, io_common::kLaneRoughness);
    lane_materials_.emplace(type, index);
    return index;
  }

  int marking_material() {
    if (marking_material_ < 0) {
      marking_material_ = add_material(
          io_common::kMarkingMaterialName, kMarkingColor, io_common::kMarkingRoughness);
    }
    return marking_material_;
  }

  int add_material(const std::string& name, const std::array<double, 4>& color, double roughness) {
    tinygltf::Material material;
    material.name = name;
    material.pbrMetallicRoughness.baseColorFactor.assign(color.begin(), color.end());
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = roughness;
    material.doubleSided = false;
    model_.materials.push_back(std::move(material));
    return static_cast<int>(model_.materials.size() - 1);
  }

  int add_road_node(const RoadMesh& road) {
    tinygltf::Mesh gltf_mesh;
    gltf_mesh.name = road.name;

    // Shared grid: one POSITION/NORMAL accessor pair, one primitive per lane
    // patch — watertightness survives into the file.
    if (!road.lanes.empty()) {
      const int positions = add_vec3_accessor(to_gltf_frame(road.positions), true);
      const int normals = add_vec3_accessor(to_gltf_frame(road.normals), false);
      for (const RoadMesh::LanePatch& patch : road.lanes) {
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.attributes["POSITION"] = positions;
        primitive.attributes["NORMAL"] = normals;
        primitive.indices = add_index_accessor(patch.indices);
        primitive.material = material_for(patch.material);
        gltf_mesh.primitives.push_back(std::move(primitive));
      }
    }
    for (const SubMesh& marking : road.markings) {
      gltf_mesh.primitives.push_back(make_primitive(marking, marking_material()));
    }

    model_.meshes.push_back(std::move(gltf_mesh));
    tinygltf::Node node;
    node.name = road.name;
    node.mesh = static_cast<int>(model_.meshes.size() - 1);
    model_.nodes.push_back(std::move(node));
    return static_cast<int>(model_.nodes.size() - 1);
  }

  tinygltf::Primitive make_primitive(const SubMesh& sub, int material) {
    tinygltf::Primitive primitive;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.attributes["POSITION"] = add_vec3_accessor(to_gltf_frame(sub.positions), true);
    primitive.attributes["NORMAL"] = add_vec3_accessor(to_gltf_frame(sub.normals), false);
    primitive.indices = add_index_accessor(sub.indices);
    primitive.material = material;
    return primitive;
  }

  int add_submesh_node(const SubMesh& sub, int material) {
    tinygltf::Mesh gltf_mesh;
    gltf_mesh.name = sub.name;
    gltf_mesh.primitives.push_back(make_primitive(sub, material));
    model_.meshes.push_back(std::move(gltf_mesh));

    tinygltf::Node node;
    node.name = sub.name;
    node.mesh = static_cast<int>(model_.meshes.size() - 1);
    model_.nodes.push_back(std::move(node));
    return static_cast<int>(model_.nodes.size() - 1);
  }

  /// The shared glTF mesh for a prop model (trunk + crown primitives, one flat
  /// material per part), built once and cached so every instance references it.
  /// Returns -1 for an unknown model id.
  int prop_mesh_for(const std::string& model_id) {
    const auto cached = prop_meshes_.find(model_id);
    if (cached != prop_meshes_.end()) {
      return cached->second;
    }
    const props::PropModel* model = props::model(model_id);
    if (model == nullptr) {
      return -1;
    }
    tinygltf::Mesh gltf_mesh;
    gltf_mesh.name = model_id;
    for (const props::PropPart& part : model->parts) {
      tinygltf::Primitive primitive;
      primitive.mode = TINYGLTF_MODE_TRIANGLES;
      primitive.attributes["POSITION"] = add_vec3_accessor(to_gltf_frame(part.positions), true);
      primitive.attributes["NORMAL"] = add_vec3_accessor(to_gltf_frame(part.normals), false);
      primitive.indices = add_index_accessor(part.indices);
      primitive.material = add_material(model_id + ":" + part.name,
                                        {part.color[0], part.color[1], part.color[2], 1.0},
                                        io_common::kLaneRoughness);
      gltf_mesh.primitives.push_back(std::move(primitive));
    }
    model_.meshes.push_back(std::move(gltf_mesh));
    const int index = static_cast<int>(model_.meshes.size() - 1);
    prop_meshes_.emplace(model_id, index);
    return index;
  }

  /// One instance node placing a shared prop mesh at its world pose. The
  /// kernel Z-up → glTF Y-up map (x,y,z)→(x,z,−y) is Rx(−90°), so a Z-up
  /// heading θ becomes a rotation about +Y by θ. Returns -1 for an unknown
  /// prop model (skipped).
  int add_prop_node(const std::string& model_id,
                    const std::array<double, 3>& position,
                    double heading) {
    const int mesh_index = prop_mesh_for(model_id);
    if (mesh_index < 0) {
      return -1;
    }
    tinygltf::Node node;
    node.name = model_id;
    node.mesh = mesh_index;
    node.translation = {position[0], position[2], -position[1]};
    const double half = heading * 0.5;
    node.rotation = {0.0, std::sin(half), 0.0, std::cos(half)};
    model_.nodes.push_back(std::move(node));
    return static_cast<int>(model_.nodes.size() - 1);
  }

  tinygltf::Model model_;
  std::map<LaneType, int> lane_materials_;
  std::map<std::string, int> prop_meshes_;
  int marking_material_ = -1;
};

} // namespace

Expected<void> export_glb(const NetworkMesh& mesh, const std::filesystem::path& path) {
  if (mesh.roads.empty() && mesh.junction_floors.empty()) {
    return make_error(
        ErrorCode::InvalidArgument, "nothing to export: empty network mesh", path.string());
  }
  return GlbWriter{}.write(mesh, path);
}

} // namespace roadmaker
