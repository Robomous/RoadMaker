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

#include "roadmaker/io/export_preview.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "mesh_export_common.hpp"

namespace roadmaker {
namespace {

/// The vendor rule namespace for preview advisories. Mirrors the convention
/// already used by robomous.ai:rm:1.0.0:objects.prop_obstruction (cascade-s4,
/// #464): a finding the ASAM catalog has no rule for still cites something, so
/// it can be referred to.
constexpr const char* kRuleChannelNotWalked = "robomous.ai:rm:1.0.0:export.channel_not_written";
constexpr const char* kRuleFormatUnsupported = "robomous.ai:rm:1.0.0:export.format_unsupported";
constexpr const char* kRuleModelNotFound = "robomous.ai:rm:1.0.0:export.model_unresolved";
constexpr const char* kRuleNothingToExport = "robomous.ai:rm:1.0.0:export.nothing_to_export";

/// Which formats walk which channel.
///
/// `member` is the literal NetworkMesh member spelling. It exists so the
/// source-scan gate in test_export_preview.cpp can assert
/// `exporter_source.contains("mesh." + member) == walked`, i.e. that this
/// table and the exporters agree — in BOTH directions, for BOTH formats,
/// without building USD.
///
/// When #390 lands (surfaces + terrain exported), the two `false`s below are
/// the edit, and the gate fails until they are made.
struct ChannelPolicy {
  MeshChannel channel;
  std::string_view label;
  std::string_view member;
  bool gltf;
  bool usd;
};

constexpr std::array<ChannelPolicy, kMeshChannelCount> kChannelPolicy{{
    {MeshChannel::Roads, "roads", "roads", true, true},
    {MeshChannel::JunctionFloors, "junction_floors", "junction_floors", true, true},
    // #390 — no exporter writes the ground channels.
    {MeshChannel::Surfaces, "surfaces", "surfaces", false, false},
    {MeshChannel::Terrain, "terrain", "terrain", false, false},
    {MeshChannel::Bridges, "bridges", "bridges", true, true},
    {MeshChannel::Objects, "objects", "objects", true, true},
    {MeshChannel::SignalInstances, "signal_instances", "signal_instances", true, true},
    // #364 — glTF embeds the rasterised face texture; tinyusdz cannot.
    {MeshChannel::SignalFaces, "signal_faces", "signal_instances", true, false},
}};

[[nodiscard]] const ChannelPolicy& policy_for(MeshChannel channel) {
  return kChannelPolicy[static_cast<std::size_t>(channel)];
}

[[nodiscard]] bool walks(const ChannelPolicy& policy, MeshExportFormat format) {
  switch (format) {
  case MeshExportFormat::Gltf:
    return policy.gltf;
  case MeshExportFormat::Usd:
    return policy.usd;
  }
  return false;
}

/// Accumulates the manifest's material table. glTF permits two materials to
/// share a name (a sign face's material is named "<model>:face" once per
/// distinct TEXT), so entries are keyed by name and their triangles summed —
/// which is exactly how the reconciliation gate aggregates the reloaded file.
class MaterialTally {
public:
  void add(const std::string& name,
           const std::array<double, 4>& color,
           double roughness,
           std::size_t triangles,
           bool textured) {
    MaterialPreview& entry = entries_[name];
    if (entry.name.empty()) {
      entry.name = name;
      entry.color = color;
      entry.roughness = roughness;
      entry.textured = textured;
    }
    entry.triangles += triangles;
    entry.textured = entry.textured || textured;
  }

  void add_lane(LaneType type, std::size_t triangles) {
    add(io_common::lane_material_name(type),
        io_common::lane_material_color(type),
        io_common::kLaneRoughness,
        triangles,
        false);
  }

  void add_marking(std::size_t triangles) {
    add(std::string(io_common::kMarkingMaterialName),
        io_common::kMarkingColor,
        io_common::kMarkingRoughness,
        triangles,
        false);
  }

  [[nodiscard]] std::vector<MaterialPreview> take() {
    std::vector<MaterialPreview> out;
    out.reserve(entries_.size());
    for (auto& [name, entry] : entries_) {
      out.push_back(std::move(entry));
    }
    return out; // std::map iterates key-sorted — deterministic by construction.
  }

private:
  std::map<std::string, MaterialPreview> entries_;
};

/// Grows an axis-aligned box by a kernel-frame vertex buffer, converting to the
/// export frame first — the frame the file is written in.
void grow(ExportBounds& bounds, const std::vector<double>& xyz) {
  for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
    const auto v = io_common::to_export_frame(xyz[i], xyz[i + 1], xyz[i + 2]);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double value = static_cast<double>(v[axis]);
      if (!bounds.valid) {
        bounds.min[axis] = value;
        bounds.max[axis] = value;
      } else {
        bounds.min[axis] = std::min(bounds.min[axis], value);
        bounds.max[axis] = std::max(bounds.max[axis], value);
      }
    }
    bounds.valid = true;
  }
}

/// Grows the box by a prop part placed at an instance pose. Mirrors
/// gltf_exporter's add_mesh_node / usd_exporter's bake_instance: uniform scale
/// in model space, then a heading rotation about kernel +Z, then a translate.
void grow_instance(ExportBounds& bounds,
                   const std::vector<double>& model_xyz,
                   const std::array<double, 3>& origin,
                   double heading,
                   double scale) {
  const double cos_h = std::cos(heading);
  const double sin_h = std::sin(heading);
  for (std::size_t i = 0; i + 2 < model_xyz.size(); i += 3) {
    const double mx = model_xyz[i] * scale;
    const double my = model_xyz[i + 1] * scale;
    const double mz = model_xyz[i + 2] * scale;
    const double wx = origin[0] + mx * cos_h - my * sin_h;
    const double wy = origin[1] + mx * sin_h + my * cos_h;
    const double wz = origin[2] + mz;
    const auto v = io_common::to_export_frame(wx, wy, wz);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double value = static_cast<double>(v[axis]);
      if (!bounds.valid) {
        bounds.min[axis] = value;
        bounds.max[axis] = value;
      } else {
        bounds.min[axis] = std::min(bounds.min[axis], value);
        bounds.max[axis] = std::max(bounds.max[axis], value);
      }
    }
    bounds.valid = true;
  }
}

[[nodiscard]] std::size_t triangles_of(const std::vector<std::uint32_t>& indices) {
  return indices.size() / 3;
}

[[nodiscard]] std::size_t vertices_of(const std::vector<double>& positions) {
  return positions.size() / 3;
}

/// How many records a channel holds in the mesh, before any export policy.
[[nodiscard]] std::size_t element_count(const NetworkMesh& mesh, MeshChannel channel) {
  switch (channel) {
  case MeshChannel::Roads:
    return mesh.roads.size();
  case MeshChannel::JunctionFloors:
    return mesh.junction_floors.size();
  case MeshChannel::Surfaces:
    return mesh.surfaces.size();
  case MeshChannel::Terrain:
    return mesh.terrain.indices.empty() ? 0U : 1U;
  case MeshChannel::Bridges:
    return mesh.bridges.size();
  case MeshChannel::Objects:
    return mesh.objects.size();
  case MeshChannel::SignalInstances:
    return mesh.signal_instances.size();
  case MeshChannel::SignalFaces: {
    std::size_t faces = 0;
    for (const SignalInstance& signal : mesh.signal_instances) {
      if (signal.face.has_value()) {
        ++faces;
      }
    }
    return faces;
  }
  }
  return 0; // unreachable — the switch above is exhaustive and default-free
}

/// Geometry a channel would contribute if the format DID walk it. Used only
/// for the omitted channels, so the report can say how much ground is being
/// left behind rather than merely that some is.
[[nodiscard]] std::pair<std::size_t, std::size_t> unwalked_geometry(const NetworkMesh& mesh,
                                                                    MeshChannel channel) {
  std::size_t vertices = 0;
  std::size_t triangles = 0;
  if (channel == MeshChannel::Surfaces) {
    for (const SurfaceMesh& surface : mesh.surfaces) {
      vertices += vertices_of(surface.mesh.positions);
      triangles += triangles_of(surface.mesh.indices);
    }
  } else if (channel == MeshChannel::Terrain) {
    vertices = vertices_of(mesh.terrain.positions);
    triangles = triangles_of(mesh.terrain.indices);
  }
  return {vertices, triangles};
}

} // namespace

bool mesh_export_available(MeshExportFormat format) {
  switch (format) {
  case MeshExportFormat::Gltf:
    return true;
  case MeshExportFormat::Usd:
#ifdef RM_HAVE_USD
    return true;
#else
    return false;
#endif
  }
  return false;
}

ScenePreview preview_mesh_export(const NetworkMesh& mesh, MeshExportFormat format) {
  ScenePreview preview;
  preview.format = format;
  preview.available = mesh_export_available(format);

  const bool gltf = format == MeshExportFormat::Gltf;

  MaterialTally materials;
  std::vector<MeshChannelPreview> rows(kMeshChannelCount);
  for (std::size_t i = 0; i < kMeshChannelCount; ++i) {
    rows[i].channel = kChannelPolicy[i].channel;
    rows[i].label = kChannelPolicy[i].label;
    rows[i].elements = element_count(mesh, kChannelPolicy[i].channel);
  }

  const auto row = [&rows](MeshChannel channel) -> MeshChannelPreview& {
    return rows[static_cast<std::size_t>(channel)];
  };

  // ---------------------------------------------------------------- roads
  {
    MeshChannelPreview& roads = row(MeshChannel::Roads);
    for (const RoadMesh& road : mesh.roads) {
      if (!road.lanes.empty()) {
        // One shared POSITION/NORMAL accessor pair per road in glTF; USD
        // repeats the same shared arrays per lane prim, but the vertex data
        // written is the road grid either way.
        roads.vertices += vertices_of(road.positions);
      }
      for (const RoadMesh::LanePatch& patch : road.lanes) {
        const std::size_t tris = triangles_of(patch.indices);
        roads.triangles += tris;
        materials.add_lane(patch.material, tris);
      }
      for (const SubMesh& marking : road.markings) {
        roads.vertices += vertices_of(marking.positions);
        const std::size_t tris = triangles_of(marking.indices);
        roads.triangles += tris;
        materials.add_marking(tris);
        // Markings carry their own geometry and sit ~2 mm proud of the surface,
        // so they extend the written bounds. Omitting them here under-reported
        // the box — caught by the reconciliation gate, not by review.
        grow(preview.bounds, marking.positions);
      }
      grow(preview.bounds, road.positions);
      preview.mesh_count += gltf ? 1 : (road.lanes.size() + road.markings.size());
      preview.node_count += 1;
    }
    roads.exported_elements = roads.elements;
  }

  // ------------------------------------------------------- junction floors
  {
    MeshChannelPreview& floors = row(MeshChannel::JunctionFloors);
    for (const JunctionFloor& floor : mesh.junction_floors) {
      floors.vertices += vertices_of(floor.mesh.positions);
      const std::size_t tris = triangles_of(floor.mesh.indices);
      floors.triangles += tris;
      materials.add_lane(floor.mesh.material, tris);
      grow(preview.bounds, floor.mesh.positions);
      preview.mesh_count += 1;
      preview.node_count += 1;
      for (const SubMesh& detail : floor.details) {
        floors.vertices += vertices_of(detail.positions);
        const std::size_t detail_tris = triangles_of(detail.indices);
        floors.triangles += detail_tris;
        materials.add_lane(detail.material, detail_tris);
        grow(preview.bounds, detail.positions);
        preview.mesh_count += 1;
        preview.node_count += 1;
      }
    }
    floors.exported_elements = floors.elements;
  }

  // -------------------------------------------------------------- bridges
  {
    MeshChannelPreview& bridges = row(MeshChannel::Bridges);
    for (const BridgeMesh& span : mesh.bridges) {
      bridges.vertices += vertices_of(span.mesh.positions);
      const std::size_t tris = triangles_of(span.mesh.indices);
      bridges.triangles += tris;
      materials.add_lane(span.mesh.material, tris);
      grow(preview.bounds, span.mesh.positions);
      preview.mesh_count += 1;
      preview.node_count += 1;
    }
    bridges.exported_elements = bridges.elements;
  }

  // ------------------------------------------------- props and signals
  //
  // glTF caches one mesh per prop model and emits a node per instance; USD
  // bakes every instance. The cache is SHARED between objects and signals
  // (both resolve through props::model), so a model used by both is counted
  // against whichever channel the exporter reaches first — objects — exactly
  // as the file stores it once. That keeps Σ(channels) == totals.
  std::set<std::string> gltf_models_seen;

  const auto account_instance = [&](MeshChannelPreview& channel_row,
                                    const std::string& model_id,
                                    const std::array<double, 3>& position,
                                    double heading,
                                    double scale) {
    const props::PropModel* model = props::model(model_id);
    if (model == nullptr) {
      // Both exporters silently skip an instance whose model does not resolve.
      // Nothing is counted for it; the shortfall against `elements` is what the
      // ModelNotFound row below is derived from.
      return;
    }
    ++channel_row.exported_elements;
    preview.node_count += 1;

    const bool first_time = gltf_models_seen.insert(model_id).second;
    const bool store_geometry = gltf ? first_time : true;
    if (store_geometry) {
      preview.mesh_count += gltf ? 1 : model->parts.size();
    }
    for (const props::PropPart& part : model->parts) {
      const std::size_t tris = triangles_of(part.indices);
      if (store_geometry) {
        channel_row.vertices += vertices_of(part.positions);
        channel_row.triangles += tris;
      }
      // A material per model part, flat-coloured, deduplicated by name in both
      // exporters — so it is counted once even when the geometry is baked N
      // times.
      if (first_time) {
        // Per-format naming, from the exporters' own shared definition — USD
        // prim identifiers cannot carry the ':' glTF uses.
        materials.add(gltf ? io_common::gltf_prop_material_name(model_id, part.name)
                           : io_common::usd_prop_material_name(model_id, part.name),
                      {static_cast<double>(part.color[0]),
                       static_cast<double>(part.color[1]),
                       static_cast<double>(part.color[2]),
                       1.0},
                      io_common::kLaneRoughness,
                      store_geometry ? tris : 0,
                      false);
      }
      grow_instance(preview.bounds, part.positions, position, heading, scale);
    }
  };

  {
    MeshChannelPreview& objects = row(MeshChannel::Objects);
    for (const ObjectInstance& instance : mesh.objects) {
      account_instance(
          objects, instance.model_id, instance.position, instance.heading, instance.scale);
    }
  }
  {
    MeshChannelPreview& signals_row = row(MeshChannel::SignalInstances);
    for (const SignalInstance& instance : mesh.signal_instances) {
      // Signals are not resizable (#335).
      account_instance(signals_row, instance.model_id, instance.position, instance.heading, 1.0);
    }
  }

  // ---------------------------------------------------------- sign faces
  {
    MeshChannelPreview& faces = row(MeshChannel::SignalFaces);
    if (gltf) {
      // One textured quad per distinct (model_id, text) — the exporter's own
      // cache key — with one embedded image and one material each.
      std::set<std::pair<std::string, std::string>> face_keys;
      for (const SignalInstance& instance : mesh.signal_instances) {
        if (!instance.face.has_value()) {
          continue;
        }
        const props::PropModel* model = props::model(instance.model_id);
        if (model == nullptr || !model->face_plate.has_value()) {
          continue;
        }
        ++faces.exported_elements;
        preview.node_count += 1;
        // The quad is model-space; the node places it at the instance pose, so
        // it extends the written bounds once per PLACEMENT even though the
        // mesh is shared per (model, text).
        grow_instance(
            preview.bounds, instance.face->positions, instance.position, instance.heading, 1.0);
        if (face_keys.emplace(instance.model_id, instance.face->text).second) {
          preview.mesh_count += 1;
          preview.image_count += 1;
          faces.vertices += vertices_of(instance.face->positions);
          const std::size_t tris = triangles_of(instance.face->indices);
          faces.triangles += tris;
          materials.add(instance.model_id + ":face",
                        {1.0, 1.0, 1.0, 1.0},
                        io_common::kLaneRoughness,
                        tris,
                        /*textured=*/true);
        }
      }
    }
  }

  // -------------------------------------------------- omissions and notes
  for (MeshChannelPreview& entry : rows) {
    const ChannelPolicy& policy = policy_for(entry.channel);
    if (entry.elements == 0) {
      entry.reason = OmissionReason::ChannelEmpty;
      continue;
    }
    if (!walks(policy, format)) {
      const auto [vertices, triangles] = unwalked_geometry(mesh, entry.channel);
      entry.vertices = 0;
      entry.triangles = 0;
      entry.exported_elements = 0;
      if (entry.channel == MeshChannel::SignalFaces) {
        entry.reason = OmissionReason::FormatUnsupported;
        entry.detail =
            "OpenUSD ASCII cannot embed the rasterised face texture, so sign text is not "
            "written (#364). The sign bodies themselves export normally.";
        preview.notes.push_back(Diagnostic{.severity = Severity::Warning,
                                           .location = std::string(entry.label),
                                           .message = entry.detail,
                                           .rule_id = kRuleFormatUnsupported,
                                           .road = {},
                                           .lane = {}});
      } else {
        entry.reason = OmissionReason::ChannelNotWalked;
        entry.detail =
            "No exporter writes the ground channels yet (#390): " + std::to_string(triangles) +
            " triangles over " + std::to_string(vertices) +
            " vertices stay in the scene and out of the file.";
        preview.notes.push_back(Diagnostic{.severity = Severity::Warning,
                                           .location = std::string(entry.label),
                                           .message = entry.detail,
                                           .rule_id = kRuleChannelNotWalked,
                                           .road = {},
                                           .lane = {}});
      }
      continue;
    }
    if (entry.exported_elements < entry.elements) {
      entry.reason = OmissionReason::ModelNotFound;
      entry.detail = std::to_string(entry.elements - entry.exported_elements) +
                     " placement(s) name a model this build does not ship, and are skipped "
                     "silently by the exporter.";
      preview.notes.push_back(Diagnostic{.severity = Severity::Warning,
                                         .location = std::string(entry.label),
                                         .message = entry.detail,
                                         .rule_id = kRuleModelNotFound,
                                         .road = {},
                                         .lane = {}});
    }
  }
  for (const MeshChannelPreview& entry : rows) {
    preview.total_vertices += entry.vertices;
    preview.total_triangles += entry.triangles;
  }
  preview.channels = std::move(rows);
  preview.materials = materials.take();

  // The empty-mesh guard, previewed rather than hit at a file dialog. Both
  // exporters carry the identical condition and the identical message; the
  // source-scan gate proves this copy still matches theirs.
  preview.would_export = !(mesh.roads.empty() && mesh.junction_floors.empty());
  if (!preview.would_export) {
    preview.refusal = Error{.code = ErrorCode::InvalidArgument,
                            .message = "nothing to export: empty network mesh",
                            .context = {}};
    preview.notes.push_back(
        Diagnostic{.severity = Severity::Error,
                   .location = "network mesh",
                   .message = "The exporters refuse a scene with no roads and no junction "
                              "floors, even when it carries terrain, ground surfaces, bridges "
                              "or props.",
                   .rule_id = kRuleNothingToExport,
                   .road = {},
                   .lane = {}});
  }

  return preview;
}

namespace {

/// Walks the produced document once, counting elements by name and collecting
/// every <userData @code>. A recursive walk rather than an XPath query: the
/// counts are simple, and this cannot depend on how pugixml was configured.
struct XmlTally {
  std::map<std::string, std::size_t> elements;
  std::map<std::string, std::size_t> user_data_codes;
  double total_road_length = 0.0;
  std::string terrain_sidecar;

  void walk(const pugi::xml_node& node) {
    for (pugi::xml_node child : node.children()) {
      if (child.type() != pugi::node_element) {
        continue;
      }
      const std::string name = child.name();
      ++elements[name];
      if (name == "road") {
        total_road_length += child.attribute("length").as_double(0.0);
      } else if (name == "userData") {
        const std::string code = child.attribute("code").as_string();
        if (!code.empty()) {
          ++user_data_codes[code];
          if (code == "rm:terrain") {
            // Read the sidecar name back out of the writer's own output rather
            // than recomputing it, so the preview and save_xodr can never name
            // different files.
            terrain_sidecar = child.attribute("value").as_string();
          }
        }
      }
      walk(child);
    }
  }

  [[nodiscard]] std::size_t count(const char* name) const {
    const auto found = elements.find(name);
    return found == elements.end() ? 0U : found->second;
  }
};

} // namespace

XodrPreview preview_xodr_export(const RoadNetwork& network,
                                std::string_view document_name,
                                const WriterOptions& options) {
  XodrPreview preview;
  preview.target_version = options.target_version;

  // ORDER IS LOAD-BEARING. write_xodr does NOT call validate_network — it calls
  // a separate, hard-failing validate() that stops at the FIRST defect and
  // collapses it into one Error. Running the advisory sweep first is what lets
  // a refused export be explained by every finding instead of by that one
  // message. Reversing these two lines silently loses diagnostics.
  preview.diagnostics = validate_network(network, options);

  auto written = write_xodr(network, document_name, options);
  if (!written) {
    preview.would_write = false;
    preview.refusal = written.error();
    return preview;
  }

  preview.would_write = true;
  preview.xml = std::move(*written);
  preview.byte_count = preview.xml.size();

  pugi::xml_document document;
  const pugi::xml_parse_result parsed =
      document.load_buffer(preview.xml.data(), preview.xml.size());
  if (!parsed) {
    // write_xodr produced bytes pugixml cannot read back. Nothing to count, but
    // the XML itself is still worth showing, so this is a note, not a refusal.
    preview.diagnostics.push_back(Diagnostic{.severity = Severity::Error,
                                             .location = "OpenDRIVE",
                                             .message = std::string("the written document did not "
                                                                    "parse back: ") +
                                                        parsed.description(),
                                             .rule_id = {},
                                             .road = {},
                                             .lane = {}});
    return preview;
  }

  XmlTally tally;
  tally.walk(document);

  preview.road_count = tally.count("road");
  preview.junction_count = tally.count("junction");
  preview.lane_section_count = tally.count("laneSection");
  preview.lane_count = tally.count("lane");
  preview.geometry_record_count = tally.count("geometry");
  preview.object_count = tally.count("object");
  preview.signal_count = tally.count("signal");
  preview.controller_count = tally.count("controller");
  preview.total_reference_length = tally.total_road_length;
  preview.terrain_sidecar = tally.terrain_sidecar;

  // Layer 1 (ADR-0008), in registry order so the report is deterministic and
  // an unregistered code cannot masquerade as one of ours.
  for (const RmCode& code : kRmCodes) {
    const auto found = tally.user_data_codes.find(std::string(code.code));
    if (found != tally.user_data_codes.end()) {
      preview.rm_records.push_back(
          XodrRecordPreview{.code = found->first, .scope = code.scope, .count = found->second});
    }
  }
  for (const auto& [code, count] : tally.user_data_codes) {
    (void)count;
    if (!is_registered_rm_code(code)) {
      preview.foreign_user_data_codes.push_back(code);
    }
  }

  return preview;
}

} // namespace roadmaker
