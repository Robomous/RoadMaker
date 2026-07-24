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

// OpenUSD ASCII (.usda) export. THE Z-up → Y-up boundary for USD: everything
// upstream is kernel frame (Z-up); everything authored here is USD frame
// (Y-up), via the shared io_common::to_export_frame rotation.
//
// Backend: tinyusdz (USDA-only; no crate writer). Decision + spike evidence in
// docs/design/m2/04_usd_export.md. The scene is authored against our own prim
// tree so the backend can be swapped for OpenUSD later without touching
// callers.

#include "roadmaker/io/usd_exporter.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include "mesh_export_common.hpp"

// tinyusdz headers arrive through a SYSTEM include dir (rm target property) so
// their non-warning-clean code never trips our -Werror.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <prim-types.hh>
#include <string>
#include <string_view>
#include <tinyusdz.hh>
#include <unordered_map>
#include <usdGeom.hh>
#include <usdShade.hh>
#include <usda-writer.hh>
#include <vector>

namespace roadmaker {

namespace {

namespace tz = tinyusdz;

/// Turn an arbitrary mesh/road name into a valid USD prim identifier: keep
/// [A-Za-z0-9_], map everything else to '_', and never start with a digit.
/// Empty input falls back to `fallback`. add_child() still guarantees sibling
/// uniqueness on top of this.
std::string sanitize_identifier(std::string_view in, std::string_view fallback) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  if (out.empty()) {
    out = std::string(fallback);
  }
  if (out.front() >= '0' && out.front() <= '9') {
    out.insert(out.begin(), '_');
  }
  return out;
}

/// One material to author under /Looks: diffuse color (RGB) + roughness.
struct MaterialDef {
  std::array<double, 3> rgb;
  double roughness;
};

/// Authors a `Material` prim wrapping a `UsdPreviewSurface` `Shader`, connected
/// through `outputs:surface`. Path is /Looks/<name>.
tz::Prim make_material(const std::string& name, const MaterialDef& def) {
  tz::Material mat;
  mat.name = name;

  tz::Shader shader;
  shader.name = "PreviewSurface";
  tz::UsdPreviewSurface surface;
  shader.info_id = tz::kUsdPreviewSurface;
  surface.outputsSurface.set_authored(true); // author `token outputs:surface`
  surface.diffuseColor = tz::value::color3f{static_cast<float>(def.rgb[0]),
                                            static_cast<float>(def.rgb[1]),
                                            static_cast<float>(def.rgb[2])};
  surface.metallic = 0.0f;
  surface.roughness = static_cast<float>(def.roughness);
  shader.value = std::move(surface);

  mat.surface.set(tz::Path("/Looks/" + name + "/PreviewSurface", "outputs:surface"));

  tz::Prim matPrim(mat);
  tz::Prim shaderPrim(shader);
  std::string err;
  matPrim.add_child(std::move(shaderPrim), /*rename=*/true, &err);
  return matPrim;
}

/// Builds a `Mesh` prim from kernel-frame buffers. `indices` may reference a
/// shared vertex grid (lane patches) or a self-contained buffer (markings,
/// junction floors); either way the vertices actually used are compacted so
/// each prim carries only its own points. Binds `material_name` and authors the
/// MaterialBindingAPI apiSchema (usdchecker requires the schema be applied, not
/// just the relationship).
tz::Prim make_mesh(const std::string& name,
                   const std::vector<double>& positions,
                   const std::vector<double>& normals,
                   const std::vector<std::uint32_t>& indices,
                   const std::string& material_name) {
  std::vector<tz::value::point3f> pts;
  std::vector<tz::value::normal3f> nrm;
  std::vector<int> face_indices;
  face_indices.reserve(indices.size());

  // old vertex index -> compact index.
  std::unordered_map<std::uint32_t, int> remap;
  remap.reserve(indices.size());
  for (const std::uint32_t vi : indices) {
    const auto found = remap.find(vi);
    if (found != remap.end()) {
      face_indices.push_back(found->second);
      continue;
    }
    const auto base = static_cast<std::size_t>(vi) * 3;
    const int compact = static_cast<int>(pts.size());
    if (base + 2 < positions.size()) {
      const auto p =
          io_common::to_export_frame(positions[base], positions[base + 1], positions[base + 2]);
      pts.push_back({p[0], p[1], p[2]});
    } else {
      pts.push_back({0.0f, 0.0f, 0.0f});
    }
    if (base + 2 < normals.size()) {
      const auto n =
          io_common::to_export_frame(normals[base], normals[base + 1], normals[base + 2]);
      nrm.push_back({n[0], n[1], n[2]});
    } else {
      nrm.push_back({0.0f, 1.0f, 0.0f});
    }
    remap.emplace(vi, compact);
    face_indices.push_back(compact);
  }

  tz::GeomMesh mesh;
  mesh.name = name;
  mesh.points.set_value(pts);
  mesh.normals.set_value(nrm);
  mesh.normals.metas().interpolation = tz::Interpolation::Vertex;

  std::vector<int> counts(face_indices.size() / 3, 3);
  mesh.faceVertexCounts.set_value(counts);
  mesh.faceVertexIndices.set_value(face_indices);

  // tinyusdz serializes the typed `subdivisionScheme` field with a misspelled
  // token ("subdivisonScheme"), which consumers ignore -> the surface would
  // default to CatmullClark. Author the correctly-spelled uniform token via the
  // generic property map so the triangle mesh reads back as a polygon mesh.
  {
    tz::Attribute attr;
    tz::primvar::PrimVar var;
    var.set_value(tz::value::token("none"));
    attr.set_var(std::move(var));
    attr.variability() = tz::Variability::Uniform;
    mesh.props.emplace("subdivisionScheme", tz::Property(attr, /*custom=*/false));
  }

  tz::Relationship binding;
  binding.set(tz::Path("/Looks/" + material_name, ""));
  mesh.materialBinding = binding;

  tz::Prim prim(mesh);
  tz::APISchemas api;
  api.listOpQual = tz::ListEditQual::Prepend;
  api.names.push_back({tz::APISchemas::APIName::MaterialBindingAPI, ""});
  prim.metas().apiSchemas = api;
  return prim;
}

} // namespace

Expected<void> export_usda(const NetworkMesh& mesh, const std::filesystem::path& path) {
  if (mesh.roads.empty() && mesh.junction_floors.empty()) {
    return make_error(
        ErrorCode::InvalidArgument, "nothing to export: empty network mesh", path.string());
  }

  // Deduplicated materials, keyed by prim name (deterministic std::map order in
  // the /Looks scope keeps the golden output stable).
  std::map<std::string, MaterialDef> materials;
  auto lane_color3 = [](LaneType type) {
    const auto c = io_common::lane_material_color(type);
    return std::array<double, 3>{c[0], c[1], c[2]};
  };

  tz::Xform world;
  world.name = "World";
  tz::Prim worldPrim(world);
  std::string err;

  for (std::size_t ri = 0; ri < mesh.roads.size(); ++ri) {
    const RoadMesh& road = mesh.roads[ri];

    tz::Xform section;
    section.name = "lanesection0";
    tz::Prim sectionPrim(section);

    for (const RoadMesh::LanePatch& patch : road.lanes) {
      const std::string material = io_common::lane_material_name(patch.material);
      materials.emplace(material,
                        MaterialDef{lane_color3(patch.material), io_common::kLaneRoughness});
      const std::string lane_name =
          sanitize_identifier("lane_" + std::to_string(patch.odr_lane_id), "lane");
      sectionPrim.add_child(
          make_mesh(lane_name, road.positions, road.normals, patch.indices, material),
          /*rename=*/true,
          &err);
    }

    for (const SubMesh& marking : road.markings) {
      materials.emplace(io_common::kMarkingMaterialName,
                        MaterialDef{{io_common::kMarkingColor[0],
                                     io_common::kMarkingColor[1],
                                     io_common::kMarkingColor[2]},
                                    io_common::kMarkingRoughness});
      sectionPrim.add_child(make_mesh(sanitize_identifier(marking.name, "marking"),
                                      marking.positions,
                                      marking.normals,
                                      marking.indices,
                                      io_common::kMarkingMaterialName),
                            /*rename=*/true,
                            &err);
    }

    tz::Xform road_xform;
    road_xform.name = sanitize_identifier(road.name, "road_" + std::to_string(ri));
    tz::Prim roadPrim(road_xform);
    roadPrim.add_child(std::move(sectionPrim), /*rename=*/true, &err);
    worldPrim.add_child(std::move(roadPrim), /*rename=*/true, &err);
  }

  for (const JunctionFloor& floor : mesh.junction_floors) {
    // Floors carry the driving-lane material: one continuous asphalt with
    // the roads feeding them (a distinct junction color read as a patch).
    const std::string material = io_common::lane_material_name(floor.mesh.material);
    materials.emplace(material,
                      MaterialDef{lane_color3(floor.mesh.material), io_common::kLaneRoughness});
    worldPrim.add_child(make_mesh(sanitize_identifier(floor.mesh.name, "junction"),
                                  floor.mesh.positions,
                                  floor.mesh.normals,
                                  floor.mesh.indices,
                                  material),
                        /*rename=*/true,
                        &err);
    // Authored corner overlays (p4-s2): sidewalk wedges and median noses, each
    // materialled from its own lane type, laid over the floor.
    for (const SubMesh& detail : floor.details) {
      const std::string detail_material = io_common::lane_material_name(detail.material);
      materials.emplace(detail_material,
                        MaterialDef{lane_color3(detail.material), io_common::kLaneRoughness});
      worldPrim.add_child(make_mesh(sanitize_identifier(detail.name, "junction_detail"),
                                    detail.positions,
                                    detail.normals,
                                    detail.indices,
                                    detail_material),
                          /*rename=*/true,
                          &err);
    }
  }

  // Generated bridge solids (p5-s3, #233): one prim per <bridge> span. The deck
  // material carrier lives on the record; the solid renders from its lane type.
  for (const BridgeMesh& span : mesh.bridges) {
    const std::string material = io_common::lane_material_name(span.mesh.material);
    materials.emplace(material,
                      MaterialDef{lane_color3(span.mesh.material), io_common::kLaneRoughness});
    worldPrim.add_child(make_mesh(sanitize_identifier(span.mesh.name, "bridge"),
                                  span.mesh.positions,
                                  span.mesh.normals,
                                  span.mesh.indices,
                                  material),
                        /*rename=*/true,
                        &err);
  }

  // Placed props (trees/vegetation) and signals (lights/signs). tinyusdz has no
  // ergonomic prototype instancing, so each instance's geometry is baked into
  // world space (Z-up, then make_mesh rotates to Y-up) — a simulator receives
  // real per-instance meshes. Every part carries a flat model material. Props
  // and signals bake identically (both resolve through props::model).
  const auto bake_instance = [&](std::string_view prefix,
                                 std::size_t index,
                                 const std::string& model_id,
                                 const std::array<double, 3>& origin,
                                 double heading,
                                 double scale) {
    const props::PropModel* model = props::model(model_id);
    if (model == nullptr) {
      return;
    }
    // Uniform scale in model space, applied before the rotate+translate (#335).
    // Normals are unaffected: a uniform scale does not shear the surface.
    const double cos_h = std::cos(heading);
    const double sin_h = std::sin(heading);
    const std::string tag = std::string(prefix) + "_" + std::to_string(index);
    tz::Xform prop_xform;
    prop_xform.name = sanitize_identifier(tag + "_" + model_id, tag);
    tz::Prim propPrim(prop_xform);
    for (const props::PropPart& part : model->parts) {
      std::vector<double> world_pos(part.positions.size());
      std::vector<double> world_nrm(part.normals.size());
      for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
        const double x = part.positions[i] * scale;
        const double y = part.positions[i + 1] * scale;
        world_pos[i] = (cos_h * x) - (sin_h * y) + origin[0];
        world_pos[i + 1] = (sin_h * x) + (cos_h * y) + origin[1];
        world_pos[i + 2] = (part.positions[i + 2] * scale) + origin[2];
        const double nx = part.normals[i];
        const double ny = part.normals[i + 1];
        world_nrm[i] = (cos_h * nx) - (sin_h * ny);
        world_nrm[i + 1] = (sin_h * nx) + (cos_h * ny);
        world_nrm[i + 2] = part.normals[i + 2];
      }
      const std::string material =
          sanitize_identifier("propmat_" + model_id + "_" + part.name, "propmat");
      materials.emplace(
          material,
          MaterialDef{{part.color[0], part.color[1], part.color[2]}, io_common::kLaneRoughness});
      propPrim.add_child(
          make_mesh(
              sanitize_identifier(part.name, "part"), world_pos, world_nrm, part.indices, material),
          /*rename=*/true,
          &err);
    }
    worldPrim.add_child(std::move(propPrim), /*rename=*/true, &err);
  };
  for (std::size_t oi = 0; oi < mesh.objects.size(); ++oi) {
    const ObjectInstance& instance = mesh.objects[oi];
    bake_instance(
        "prop", oi, instance.model_id, instance.position, instance.heading, instance.scale);
  }
  for (std::size_t si = 0; si < mesh.signal_instances.size(); ++si) {
    const SignalInstance& instance = mesh.signal_instances[si];
    // Signals are not resizable (#335). A sign's editable text face
    // (instance.face) is deliberately NOT exported here: this backend writes
    // single-file USDA (tinyusdz cannot embed textures, and a sidecar image
    // would break the one-file contract), so a text plate exports as its flat
    // yellow plate. The @text itself is preserved in the .xodr, and the glTF
    // (.glb) exporter DOES embed the rendered face. Documented limitation
    // (#230); a textured-USD follow-up is tracked separately.
    bake_instance("signal", si, instance.model_id, instance.position, instance.heading, 1.0);
  }

  tz::Scope looks;
  looks.name = "Looks";
  tz::Prim looksPrim(looks);
  for (const auto& [name, def] : materials) {
    looksPrim.add_child(make_material(name, def), /*rename=*/true, &err);
  }

  tz::Stage stage;
  stage.metas().upAxis = tz::Axis::Y;
  stage.metas().metersPerUnit = 1.0;
  stage.metas().defaultPrim = tz::value::token("World");
  stage.add_root_prim(std::move(worldPrim));
  stage.add_root_prim(std::move(looksPrim));

  std::string warn;
  std::string werr;
  const bool ok = tz::usda::SaveAsUSDA(path.string(), stage, &warn, &werr);
  if (!ok) {
    return make_error(ErrorCode::IoFailure, "failed to write usda: " + werr, path.string());
  }
  return {};
}

} // namespace roadmaker
