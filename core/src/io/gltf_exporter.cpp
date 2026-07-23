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

// glTF 2.0 (.glb) export. THE Z-up → Y-up boundary: everything upstream of
// this file is kernel frame (Z-up); everything in the written file is glTF
// frame (Y-up). Conversion: (x, y, z) → (x, z, −y).

#include "roadmaker/io/gltf_exporter.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/assets/sign_face.hpp"

#include <cmath>

#include "mesh_export_common.hpp"

// tinygltf implementation lives in this TU only (its NO_STB/NO_EXTERNAL
// config macros come from the rm_tinygltf CMake target so every consumer
// agrees). RoadMaker writes fully self-contained binary glTF.
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

// stb_image_write encodes sign-face bitmaps to PNG for embedding — tinygltf's
// own PNG writer is disabled (TINYGLTF_NO_STB_IMAGE_WRITE), so we drive stb
// directly. STATIC keeps its symbols internal (shared-kernel export check);
// SYSTEM include (angle brackets) keeps it clean under -Werror.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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
      // Authored corner overlays (p4-s2): sidewalk wedges and median noses,
      // each carrying its own lane-type material, laid over the floor.
      for (const SubMesh& detail : floor.details) {
        scene.nodes.push_back(add_submesh_node(detail, material_for(detail.material)));
      }
    }
    // Placed props: one shared mesh per prop model, one node per instance —
    // idiomatic glTF instancing, so many trees stay one mesh in the file.
    for (const ObjectInstance& instance : mesh.objects) {
      const int node =
          add_prop_node(instance.model_id, instance.position, instance.heading, instance.scale);
      if (node >= 0) {
        scene.nodes.push_back(node);
      }
    }
    // Placed signals share the identical instancing path (one mesh per signal
    // model, one node per placement).
    for (const SignalInstance& instance : mesh.signal_instances) {
      // Signals are not resizable (#335).
      const int node = add_prop_node(instance.model_id, instance.position, instance.heading, 1.0);
      if (node >= 0) {
        scene.nodes.push_back(node);
      }
      // Editable text face: a textured quad sharing the signal's transform. The
      // face mesh (with its embedded bitmap texture) is cached per (model_id,
      // text), so two identical plates share one image.
      if (instance.face.has_value()) {
        const int face_mesh = face_mesh_for(instance.model_id, *instance.face);
        if (face_mesh >= 0) {
          scene.nodes.push_back(add_mesh_node(
              face_mesh, instance.position, instance.heading, 1.0, instance.model_id + ":face"));
        }
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
    // target 0 (image/PNG data) must be omitted — glTF only allows 34962/34963.
    if (target != 0) {
      view.target = target;
    }
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

  int add_vec2_accessor(const std::vector<float>& data) {
    const int view =
        append_to_buffer(data.data(), data.size() * sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
    tinygltf::Accessor accessor;
    accessor.bufferView = view;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = TINYGLTF_TYPE_VEC2;
    accessor.count = data.size() / 2;
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

  /// Places a shared mesh at an instance world pose. The kernel Z-up → glTF Y-up
  /// map (x,y,z)→(x,z,−y) is Rx(−90°), so a Z-up heading θ becomes a rotation
  /// about +Y by θ. `scale` is the instance's uniform size factor (#335) —
  /// uniform scale commutes with the Y-up rotation, so it maps across unchanged.
  int add_mesh_node(int mesh_index,
                    const std::array<double, 3>& position,
                    double heading,
                    double scale,
                    const std::string& name) {
    tinygltf::Node node;
    node.name = name;
    node.mesh = mesh_index;
    node.translation = {position[0], position[2], -position[1]};
    const double half = heading * 0.5;
    node.rotation = {0.0, std::sin(half), 0.0, std::cos(half)};
    node.scale = {scale, scale, scale};
    model_.nodes.push_back(std::move(node));
    return static_cast<int>(model_.nodes.size() - 1);
  }

  /// One instance node placing a shared prop mesh at its world pose. Returns -1
  /// for an unknown prop model (skipped).
  int add_prop_node(const std::string& model_id,
                    const std::array<double, 3>& position,
                    double heading,
                    double scale) {
    const int mesh_index = prop_mesh_for(model_id);
    if (mesh_index < 0) {
      return -1;
    }
    return add_mesh_node(mesh_index, position, heading, scale, model_id);
  }

  /// The single CLAMP_TO_EDGE sampler shared by every sign-face texture (the
  /// baked 4-texel margin plus edge clamping keeps text from bleeding).
  int clamp_sampler() {
    if (clamp_sampler_ < 0) {
      tinygltf::Sampler sampler;
      sampler.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
      sampler.wrapT = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
      sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
      sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
      model_.samplers.push_back(std::move(sampler));
      clamp_sampler_ = static_cast<int>(model_.samplers.size() - 1);
    }
    return clamp_sampler_;
  }

  /// Embeds an RGBA sign-face bitmap as a PNG image referenced through a
  /// bufferView (the bytes live in the single glb binary buffer). Returns the
  /// image index.
  int add_face_image(const signs::FaceBitmap& bmp) {
    std::vector<unsigned char> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
          auto* out = static_cast<std::vector<unsigned char>*>(ctx);
          const auto* bytes = static_cast<const unsigned char*>(data);
          out->insert(out->end(), bytes, bytes + size);
        },
        &png,
        bmp.width,
        bmp.height,
        4,
        bmp.rgba.data(),
        bmp.width * 4);
    // Image data must NOT carry an ARRAY/ELEMENT target (target 0 → omitted).
    const int view = append_to_buffer(png.data(), png.size(), 0);
    tinygltf::Image image;
    image.mimeType = "image/png";
    image.bufferView = view;
    image.as_is = true; // already encoded — tinygltf must not re-decode
    model_.images.push_back(std::move(image));
    return static_cast<int>(model_.images.size() - 1);
  }

  /// The shared glTF mesh for a sign face — a textured quad — built once per
  /// (model_id, text) so two identical plates reference one image/material/mesh.
  /// Returns -1 when the model has no face plate.
  int face_mesh_for(const std::string& model_id, const SignalFaceOverlay& overlay) {
    const std::string key = model_id + '\x1f' + overlay.text;
    const auto cached = face_meshes_.find(key);
    if (cached != face_meshes_.end()) {
      return cached->second;
    }
    const props::PropModel* model = props::model(model_id);
    if (model == nullptr || !model->face_plate.has_value()) {
      return -1;
    }
    const signs::FaceBitmap bmp = signs::render_face(overlay.text, *model->face_plate);
    const int image = add_face_image(bmp);

    tinygltf::Texture texture;
    texture.source = image;
    texture.sampler = clamp_sampler();
    model_.textures.push_back(std::move(texture));
    const int texture_index = static_cast<int>(model_.textures.size() - 1);

    tinygltf::Material material;
    material.name = model_id + ":face";
    material.pbrMetallicRoughness.baseColorTexture.index = texture_index;
    material.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = io_common::kLaneRoughness;
    material.doubleSided = false;
    model_.materials.push_back(std::move(material));
    const int material_index = static_cast<int>(model_.materials.size() - 1);

    tinygltf::Mesh gltf_mesh;
    gltf_mesh.name = model_id + ":face";
    tinygltf::Primitive primitive;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.attributes["POSITION"] = add_vec3_accessor(to_gltf_frame(overlay.positions), true);
    primitive.attributes["NORMAL"] = add_vec3_accessor(to_gltf_frame(overlay.normals), false);
    std::vector<float> uvs;
    uvs.reserve(overlay.uvs.size());
    for (const double uv : overlay.uvs) {
      uvs.push_back(static_cast<float>(uv));
    }
    primitive.attributes["TEXCOORD_0"] = add_vec2_accessor(uvs);
    primitive.indices = add_index_accessor(overlay.indices);
    primitive.material = material_index;
    gltf_mesh.primitives.push_back(std::move(primitive));
    model_.meshes.push_back(std::move(gltf_mesh));
    const int index = static_cast<int>(model_.meshes.size() - 1);
    face_meshes_.emplace(key, index);
    return index;
  }

  tinygltf::Model model_;
  std::map<LaneType, int> lane_materials_;
  std::map<std::string, int> prop_meshes_;
  std::map<std::string, int> face_meshes_;
  int marking_material_ = -1;
  int clamp_sampler_ = -1;
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
