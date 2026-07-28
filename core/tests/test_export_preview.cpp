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

// The anti-drift gates for the export manifests (p7-s1, #241).
//
// preview_mesh_export re-states the exporters' policy rather than running
// them (they are path-only, and the acceptance forbids writing files), so it
// is a second implementation — and a second implementation is a lie waiting
// to happen. Four tiers keep it honest:
//
//   1. RECONCILIATION — export for real, reload, and compare every number.
//      Catches any divergence in the glTF half. (USD's equivalent lives in
//      test_usd.cpp, in the `Usd` suite, because the usd-export CI job runs
//      `ctest -R '^Usd\.'` and would never see it anywhere else.)
//   2. SOURCE SCAN — read the exporters off disk and assert the channel table
//      agrees with which `mesh.<member>` each one actually walks, in both
//      directions, for both formats. This covers the WHOLE USD half with no
//      USD build, so it runs on every machine and in every job.
//   3. TOTALITY — the manifest must cover every NetworkMesh member. This is
//      what stops a future channel repeating #390's silent omission.
//   4. REFUSAL PARITY — the previewed verdict equals the real one.
//
// preview_xodr_export needs no such gate: it counts the bytes write_xodr
// produced, and a summary derived from the output cannot disagree with the
// output. Its tests below pin the ordering and the ADR-0008 layering instead.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/export_preview.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <tiny_gltf.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The exporters' own material vocabulary. core/tests has core/src on its
// include path (core/tests/CMakeLists.txt) — the same move test_fill_predicates
// makes for fill_backend.hpp — so the gate can assert against the definitions
// themselves, not against a copy of them.
#include "io/mesh_export_common.hpp"

namespace roadmaker {
namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "missing " << path.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// Removes // and /* */ comments. Load-bearing for the source scan: a comment
/// reading "we do not export mesh.surfaces" would otherwise satisfy the scan
/// and fake the gate green. (test_rm_registry.cpp accepts that hazard for the
/// rm: codes and documents it; here it would invert the assertion's meaning.)
std::string strip_comments(const std::string& source) {
  std::string out;
  out.reserve(source.size());
  for (std::size_t i = 0; i < source.size();) {
    if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n') {
        ++i;
      }
    } else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      i += 2;
      while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
        ++i;
      }
      i = std::min(i + 2, source.size());
    } else {
      out.push_back(source[i]);
      ++i;
    }
  }
  return out;
}

NetworkMesh mesh_of_sample(const char* sample) {
  auto parsed = load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample);
  EXPECT_TRUE(parsed.has_value()) << "failed to load " << sample;
  return build_network_mesh(parsed->network);
}

/// Exports for real and reads the file back. The manifest is checked against
/// THIS, never against another copy of the exporter's rules.
tinygltf::Model export_and_reload(const NetworkMesh& mesh, const char* out_name) {
  const auto path = std::filesystem::temp_directory_path() / out_name;
  const auto exported = export_glb(mesh, path);
  EXPECT_TRUE(exported.has_value()) << (exported ? std::string{} : exported.error().message);

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  // A no-op image loader keeps the reload STRUCTURAL: tinygltf is built here
  // without stb_image, so it must not try to decode the embedded face PNGs.
  // The image ENTRIES still land in model.images, which is all the manifest
  // claims a count of. (Same move as test_gltf.cpp's export_text_signs.)
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
  EXPECT_TRUE(loaded) << err << " / " << warn;
  return model;
}

std::size_t triangles_in(const tinygltf::Model& model, const tinygltf::Primitive& primitive) {
  if (primitive.indices < 0) {
    return 0;
  }
  return model.accessors[static_cast<std::size_t>(primitive.indices)].count / 3;
}

/// Triangles the FILE stores, summed over every mesh — the same file-weighted
/// rule the manifest documents (a shared prop mesh counts once, however many
/// nodes instance it).
std::size_t file_triangles(const tinygltf::Model& model) {
  std::size_t total = 0;
  for (const tinygltf::Mesh& mesh : model.meshes) {
    for (const tinygltf::Primitive& primitive : mesh.primitives) {
      total += triangles_in(model, primitive);
    }
  }
  return total;
}

/// Per-material-NAME triangle totals. Keyed by name, not index, because glTF
/// permits two materials to share a name — a sign face's material is named
/// "<model>:face" once per distinct text — and the manifest aggregates the
/// same way.
std::map<std::string, std::size_t> triangles_by_material(const tinygltf::Model& model) {
  std::map<std::string, std::size_t> totals;
  for (const tinygltf::Mesh& mesh : model.meshes) {
    for (const tinygltf::Primitive& primitive : mesh.primitives) {
      if (primitive.material < 0) {
        continue;
      }
      const std::string& name = model.materials[static_cast<std::size_t>(primitive.material)].name;
      totals[name] += triangles_in(model, primitive);
    }
  }
  return totals;
}

std::map<std::string, std::size_t> triangles_by_material(const ScenePreview& preview) {
  std::map<std::string, std::size_t> totals;
  for (const MaterialPreview& material : preview.materials) {
    totals[material.name] += material.triangles;
  }
  return totals;
}

const MeshChannelPreview& channel(const ScenePreview& preview, MeshChannel which) {
  return preview.channels[static_cast<std::size_t>(which)];
}

RoadId
segment(RoadNetwork& network, const char* odr_id, double x0, double y0, double x1, double y1) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(RoadId{});
}

/// A scene carrying EVERY channel, built in code.
///
/// It has to be built rather than loaded: not one committed sample carries a
/// ground surface or a height field (`grep -l "rm:surface\|rm:terrain"
/// assets/samples/*.xodr` matches none of the twelve), so a sample-based
/// fixture would make every omission assertion below vacuously true — which is
/// precisely how #390 stayed invisible for two pillars.
RoadNetwork full_channel_network() {
  RoadNetwork network;
  // A ~20 m square loop of four welded straights — the same fixture shape
  // test_surface_mesh uses, large enough to enclose a derivable face.
  segment(network, "a", 0.0, 0.0, 20.0, 0.0);
  segment(network, "b", 20.0, 0.0, 20.0, 20.0);
  segment(network, "c", 20.0, 20.0, 0.0, 20.0);
  segment(network, "d", 0.0, 20.0, 0.0, 0.0);
  derive_surfaces(network);
  EXPECT_GT(network.surface_count(), 0U) << "fixture derived no surface";

  // A flat height field through the command layer, so the fixture cannot drift
  // from how the product actually creates one.
  auto create = edit::create_terrain_field(network);
  EXPECT_TRUE(create->apply(network).has_value());

  return network;
}

} // namespace

// ---------------------------------------------------------------- tier 1

TEST(ExportPreview, ManifestReconcilesWithTheWrittenGlb) {
  for (const char* sample : {"t_junction.xodr", "props_scale.xodr", "sign_pack.xodr"}) {
    SCOPED_TRACE(sample);
    const NetworkMesh mesh = mesh_of_sample(sample);
    const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
    const tinygltf::Model model = export_and_reload(mesh, "rm_preview_reconcile.glb");

    EXPECT_EQ(preview.mesh_count, model.meshes.size());
    EXPECT_EQ(preview.image_count, model.images.size());
    ASSERT_FALSE(model.scenes.empty());
    EXPECT_EQ(preview.node_count, model.scenes[0].nodes.size());
    EXPECT_EQ(preview.total_triangles, file_triangles(model));

    // Material NAMES, both directions — an extra material in the file and a
    // phantom one in the manifest are different bugs and both must fail.
    std::set<std::string> file_names;
    for (const tinygltf::Material& material : model.materials) {
      file_names.insert(material.name);
    }
    std::set<std::string> manifest_names;
    for (const MaterialPreview& material : preview.materials) {
      manifest_names.insert(material.name);
    }
    EXPECT_EQ(manifest_names, file_names);

    EXPECT_EQ(triangles_by_material(preview), triangles_by_material(model));

    // Base colour and roughness as actually written.
    for (const MaterialPreview& material : preview.materials) {
      const auto found = std::find_if(
          model.materials.begin(), model.materials.end(), [&](const tinygltf::Material& candidate) {
            return candidate.name == material.name;
          });
      ASSERT_NE(found, model.materials.end()) << material.name;
      const auto& factor = found->pbrMetallicRoughness.baseColorFactor;
      ASSERT_EQ(factor.size(), 4U) << material.name;
      for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(material.color[i], factor[i], 1e-9) << material.name << " channel " << i;
      }
      EXPECT_NEAR(material.roughness, found->pbrMetallicRoughness.roughnessFactor, 1e-9)
          << material.name;
      EXPECT_EQ(material.textured, found->pbrMetallicRoughness.baseColorTexture.index >= 0)
          << material.name;
    }
  }
}

TEST(ExportPreview, VertexTotalMatchesTheUniqueAccessorsInTheFile) {
  const NetworkMesh mesh = mesh_of_sample("props_scale.xodr");
  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const tinygltf::Model model = export_and_reload(mesh, "rm_preview_verts.glb");

  // POSITION accessors are shared across a road's lane primitives, so count
  // each accessor once — the same file-weighted rule the manifest uses.
  std::set<int> position_accessors;
  for (const tinygltf::Mesh& gltf_mesh : model.meshes) {
    for (const tinygltf::Primitive& primitive : gltf_mesh.primitives) {
      const auto found = primitive.attributes.find("POSITION");
      if (found != primitive.attributes.end()) {
        position_accessors.insert(found->second);
      }
    }
  }
  std::size_t vertices = 0;
  for (const int index : position_accessors) {
    vertices += model.accessors[static_cast<std::size_t>(index)].count;
  }
  EXPECT_EQ(preview.total_vertices, vertices);
}

TEST(ExportPreview, BoundsMatchTheWrittenGeometry) {
  const NetworkMesh mesh = mesh_of_sample("t_junction.xodr");
  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const tinygltf::Model model = export_and_reload(mesh, "rm_preview_bounds.glb");

  ASSERT_TRUE(preview.bounds.valid);
  std::array<double, 3> lo{1e300, 1e300, 1e300};
  std::array<double, 3> hi{-1e300, -1e300, -1e300};
  for (const tinygltf::Accessor& accessor : model.accessors) {
    if (accessor.minValues.size() != 3 || accessor.maxValues.size() != 3) {
      continue;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
      lo[axis] = std::min(lo[axis], accessor.minValues[axis]);
      hi[axis] = std::max(hi[axis], accessor.maxValues[axis]);
    }
  }
  // Float storage in the file, doubles in the manifest — compare loosely.
  for (std::size_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(preview.bounds.min[axis], lo[axis], 1e-3) << "axis " << axis;
    EXPECT_NEAR(preview.bounds.max[axis], hi[axis], 1e-3) << "axis " << axis;
  }
}

// ---------------------------------------------------------------- tier 2

TEST(ExportPreview, ChannelTableAgreesWithWhatEachExporterWalks) {
  const std::filesystem::path src(RM_CORE_SRC_DIR);
  const std::string gltf = strip_comments(read_file(src / "io" / "gltf_exporter.cpp"));
  const std::string usd = strip_comments(read_file(src / "io" / "usd_exporter.cpp"));

  struct Row {
    MeshChannel channel;
    const char* member;
    bool gltf;
    bool usd;
  };

  // Mirrors kChannelPolicy in export_preview.cpp. SignalFaces has no member of
  // its own — it rides on signal_instances — so it is checked by tier 1 and by
  // the USD negative test, not here.
  const Row rows[] = {
      {MeshChannel::Roads, "mesh.roads", true, true},
      {MeshChannel::JunctionFloors, "mesh.junction_floors", true, true},
      {MeshChannel::Surfaces, "mesh.surfaces", true, true},
      {MeshChannel::Terrain, "mesh.terrain", true, true},
      {MeshChannel::Bridges, "mesh.bridges", true, true},
      {MeshChannel::Objects, "mesh.objects", true, true},
      {MeshChannel::SignalInstances, "mesh.signal_instances", true, true},
  };

  for (const Row& row : rows) {
    EXPECT_EQ(gltf.find(row.member) != std::string::npos, row.gltf)
        << row.member
        << ": gltf_exporter.cpp and the channel table in export_preview.cpp disagree. If you "
           "just taught the exporter this channel (e.g. #390), flip its row in kChannelPolicy.";
    EXPECT_EQ(usd.find(row.member) != std::string::npos, row.usd)
        << row.member
        << ": usd_exporter.cpp and the channel table in export_preview.cpp disagree. If you "
           "just taught the exporter this channel (e.g. #390), flip its row in kChannelPolicy.";
  }
}

// The refusal used to be three copies of one condition — two exporters and the
// manifest — held together by a scan for the literal text. Since #390 it is ONE
// function that all three call, which is strictly stronger: a shared definition
// cannot drift from itself. What the scan still buys is that an exporter cannot
// quietly go back to rolling its own guard, which is how a terrain-only scene
// came to be refused in the first place.
TEST(ExportPreview, PreviewedRefusalConditionMatchesBothExporters) {
  const std::filesystem::path src(RM_CORE_SRC_DIR);
  const std::string guard = "has_exportable_geometry(mesh)";
  for (const char* file : {"gltf_exporter.cpp", "usd_exporter.cpp"}) {
    const std::string source = strip_comments(read_file(src / "io" / file));
    EXPECT_NE(source.find(guard), std::string::npos)
        << file << " no longer calls the shared guard the manifest previews (" << guard
        << "). Both exporters and preview_mesh_export must use the one definition in "
           "mesh_export_common.hpp.";
  }
  // And the manifest previews it by CALLING it, not by restating it.
  const std::string preview = strip_comments(read_file(src / "io" / "export_preview.cpp"));
  EXPECT_NE(preview.find(guard), std::string::npos);
}

// ---------------------------------------------------------------- tier 3

TEST(ExportPreview, EveryNetworkMeshChannelIsAccountedFor) {
  const ScenePreview preview =
      preview_mesh_export(mesh_of_sample("t_junction.xodr"), MeshExportFormat::Gltf);
  ASSERT_EQ(preview.channels.size(), kMeshChannelCount);
  std::set<int> seen;
  for (const MeshChannelPreview& row : preview.channels) {
    EXPECT_TRUE(seen.insert(static_cast<int>(row.channel)).second) << "duplicate channel row";
    EXPECT_FALSE(row.label.empty());
  }
  EXPECT_EQ(seen.size(), kMeshChannelCount);

  // The structural half: count NetworkMesh's members and require a channel for
  // each. SignalFaces is the one channel with no member of its own.
  const std::string header = read_file(std::filesystem::path(RM_CORE_SRC_DIR).parent_path() /
                                       "include" / "roadmaker" / "mesh" / "mesh.hpp");
  const std::size_t begin = header.find("struct NetworkMesh {");
  ASSERT_NE(begin, std::string::npos);
  const std::size_t end = header.find("\n};", begin);
  ASSERT_NE(end, std::string::npos);
  const std::string body = strip_comments(header.substr(begin, end - begin));

  std::size_t members = 0;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    // A member declaration is the only thing left that ends in ';' once the
    // comments are gone.
    if (line.find(';') != std::string::npos) {
      ++members;
    }
  }
  EXPECT_EQ(members, kMeshChannelCount - 1)
      << "NetworkMesh gained or lost a member. Add (or remove) a MeshChannel row and decide "
         "whether each exporter walks it — that decision is exactly what #390 records having "
         "been made silently.";
}

// ---------------------------------------------------------------- tier 4

TEST(ExportPreview, TerrainOnlySceneExportsInsteadOfBeingRefused) {
  // Terrain and ground, and no carriageway: real geometry, which both exporters
  // used to refuse because their guard tested only roads and junction floors
  // (#390). (create_terrain_field sizes the grid from the network's extent, so
  // the roads have to exist to author the field — they are dropped from the
  // MESH afterwards, which is exactly the shape the guard sees.)
  const NetworkMesh authored = build_network_mesh(full_channel_network());
  ASSERT_FALSE(authored.terrain.indices.empty());

  NetworkMesh mesh = authored;
  mesh.roads.clear();
  mesh.junction_floors.clear();
  ASSERT_TRUE(mesh.roads.empty());
  ASSERT_TRUE(mesh.junction_floors.empty());
  ASSERT_FALSE(mesh.terrain.indices.empty()) << "fixture has no terrain — the test is vacuous";

  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  EXPECT_TRUE(preview.would_export);
  EXPECT_FALSE(preview.refusal.has_value());
  EXPECT_GT(preview.total_triangles, 0U);

  // Previewed verdict == real verdict: the file is actually written.
  const auto path = std::filesystem::temp_directory_path() / "rm_preview_terrain_only.glb";
  const auto real = export_glb(mesh, path);
  ASSERT_TRUE(real.has_value()) << (real ? std::string{} : real.error().message);
  EXPECT_TRUE(std::filesystem::exists(path));
  std::remove(path.string().c_str());
}

TEST(ExportPreview, AnEntirelyEmptyMeshIsStillRefusedIdentically) {
  // The guard did not go away, it got honest: nothing in ANY channel.
  const NetworkMesh mesh;
  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  EXPECT_FALSE(preview.would_export);
  ASSERT_TRUE(preview.refusal.has_value());

  const auto path = std::filesystem::temp_directory_path() / "rm_preview_refusal.glb";
  const auto real = export_glb(mesh, path);
  std::remove(path.string().c_str());
  ASSERT_FALSE(real.has_value());
  EXPECT_EQ(preview.refusal->code, real.error().code);
  EXPECT_EQ(preview.refusal->message, real.error().message);
}

// ------------------------------------------------- omissions and policy

TEST(ExportPreview, GroundChannelsAreExportedForBothFormats) {
  const NetworkMesh mesh = build_network_mesh(full_channel_network());
  ASSERT_FALSE(mesh.surfaces.empty()) << "fixture has no surfaces — the test is vacuous";
  ASSERT_FALSE(mesh.terrain.indices.empty()) << "fixture has no terrain — the test is vacuous";

  for (const MeshExportFormat format : {MeshExportFormat::Gltf, MeshExportFormat::Usd}) {
    SCOPED_TRACE(format == MeshExportFormat::Gltf ? "gltf" : "usd");
    const ScenePreview preview = preview_mesh_export(mesh, format);

    for (const MeshChannel ground : {MeshChannel::Surfaces, MeshChannel::Terrain}) {
      const MeshChannelPreview& row = channel(preview, ground);
      EXPECT_GT(row.elements, 0U);
      EXPECT_EQ(row.exported_elements, row.elements);
      EXPECT_EQ(row.reason, OmissionReason::None);
      EXPECT_GT(row.triangles, 0U);
      EXPECT_GT(row.vertices, 0U);
      EXPECT_TRUE(row.detail.empty()) << "an exported channel has nothing to explain";
    }
  }

  // Neither format shares nor bakes the ground — one mesh per surface plus one
  // for the field, either way — so unlike props the two agree exactly.
  const ScenePreview gltf = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const ScenePreview usd = preview_mesh_export(mesh, MeshExportFormat::Usd);
  EXPECT_EQ(channel(gltf, MeshChannel::Surfaces).triangles,
            channel(usd, MeshChannel::Surfaces).triangles);
  EXPECT_EQ(channel(gltf, MeshChannel::Terrain).triangles,
            channel(usd, MeshChannel::Terrain).triangles);
}

TEST(ExportPreview, GroundIsIncludedInTheTotalsAndReconcilesWithTheFile) {
  const NetworkMesh mesh = build_network_mesh(full_channel_network());
  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const tinygltf::Model model = export_and_reload(mesh, "rm_preview_ground.glb");

  // Non-vacuity first: without this the reconciliation below would hold just as
  // well on the pre-#390 code, where neither side carried any ground at all.
  const std::size_t ground_triangles = channel(preview, MeshChannel::Surfaces).triangles +
                                       channel(preview, MeshChannel::Terrain).triangles;
  ASSERT_GT(ground_triangles, 0U) << "no ground in the manifest — the test is vacuous";

  // THE gate: what the manifest promises is what the file stores, ground and
  // all. One mesh per surface and one for the terrain, counted on both sides.
  EXPECT_EQ(preview.total_triangles, file_triangles(model));
  EXPECT_EQ(preview.mesh_count, model.meshes.size());
  EXPECT_EQ(triangles_by_material(preview), triangles_by_material(model));

  // And the ground materials are the exporters' own spelling, not a second
  // palette invented for the report.
  const auto materials = triangles_by_material(model);
  EXPECT_TRUE(materials.contains(io_common::ground_material_name("")));
  EXPECT_TRUE(materials.contains(io_common::kTerrainMaterialName));
}

TEST(ExportPreview, SignFacesExportToGltfAndNotToUsd) {
  const NetworkMesh mesh = mesh_of_sample("sign_pack.xodr");
  // NOTE the local: binding a const& to a member of a temporary ScenePreview
  // dangles — lifetime extension does not reach through a function returning a
  // reference to a member.
  const ScenePreview gltf_preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const MeshChannelPreview& gltf = channel(gltf_preview, MeshChannel::SignalFaces);
  ASSERT_GT(gltf.elements, 0U) << "sample carries no sign faces — the test is vacuous";
  EXPECT_GT(gltf.exported_elements, 0U);
  EXPECT_EQ(gltf.reason, OmissionReason::None);

  const ScenePreview usd = preview_mesh_export(mesh, MeshExportFormat::Usd);
  const MeshChannelPreview& faces = channel(usd, MeshChannel::SignalFaces);
  EXPECT_EQ(faces.exported_elements, 0U);
  EXPECT_EQ(faces.reason, OmissionReason::FormatUnsupported);
  EXPECT_NE(faces.detail.find("#364"), std::string::npos);
  EXPECT_EQ(usd.image_count, 0U);
}

TEST(ExportPreview, PropsAreSharedInGltfAndBakedInUsd) {
  const NetworkMesh mesh = mesh_of_sample("tree_avenue.xodr");
  ASSERT_GT(mesh.objects.size(), 1U) << "sample has too few props — the test is vacuous";

  const ScenePreview gltf = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const ScenePreview usd = preview_mesh_export(mesh, MeshExportFormat::Usd);
  const MeshChannelPreview& gltf_objects = channel(gltf, MeshChannel::Objects);
  const MeshChannelPreview& usd_objects = channel(usd, MeshChannel::Objects);

  EXPECT_EQ(gltf_objects.exported_elements, usd_objects.exported_elements);
  // The file-weighted difference the two formats create, which is why a
  // format-agnostic triangle count would be wrong in one of them.
  EXPECT_LT(gltf_objects.triangles, usd_objects.triangles);
}

TEST(ExportPreview, UnresolvableModelIdIsReportedNotSilentlyDropped) {
  NetworkMesh mesh = mesh_of_sample("t_junction.xodr");
  ObjectInstance ghost;
  ghost.model_id = "no_such_model_ships_with_this_build";
  ghost.position = {0.0, 0.0, 0.0};
  mesh.objects.push_back(ghost);

  const ScenePreview preview = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const MeshChannelPreview& objects = channel(preview, MeshChannel::Objects);
  EXPECT_EQ(objects.exported_elements, objects.elements - 1);
  EXPECT_EQ(objects.reason, OmissionReason::ModelNotFound);

  // And the file agrees — the exporter really did skip it.
  const tinygltf::Model model = export_and_reload(mesh, "rm_preview_ghost.glb");
  ASSERT_FALSE(model.scenes.empty());
  EXPECT_EQ(preview.node_count, model.scenes[0].nodes.size());
}

TEST(ExportPreview, PreviewIsPureAndRepeatable) {
  const NetworkMesh mesh = mesh_of_sample("t_junction.xodr");
  const ScenePreview first = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  const ScenePreview second = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  EXPECT_EQ(first.total_triangles, second.total_triangles);
  EXPECT_EQ(first.total_vertices, second.total_vertices);
  EXPECT_EQ(first.mesh_count, second.mesh_count);
  ASSERT_EQ(first.materials.size(), second.materials.size());
  for (std::size_t i = 0; i < first.materials.size(); ++i) {
    EXPECT_EQ(first.materials[i].name, second.materials[i].name) << "material order is unstable";
  }
}

TEST(ExportPreview, UsdAvailabilityReflectsTheBuild) {
  EXPECT_TRUE(mesh_export_available(MeshExportFormat::Gltf));
#ifdef RM_HAVE_USD
  EXPECT_TRUE(mesh_export_available(MeshExportFormat::Usd));
#else
  EXPECT_FALSE(mesh_export_available(MeshExportFormat::Usd));
#endif
  // Availability gates WRITING, never the manifest: a USD-off build still
  // reports what a USD build would produce.
  const ScenePreview preview =
      preview_mesh_export(mesh_of_sample("t_junction.xodr"), MeshExportFormat::Usd);
  EXPECT_GT(preview.total_triangles, 0U);
  EXPECT_FALSE(preview.materials.empty());
}

// -------------------------------------------------------------- OpenDRIVE

TEST(ExportPreview, XodrPreviewXmlIsByteIdenticalToWriteXodr) {
  auto parsed = load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(parsed.has_value());

  const XodrPreview preview = preview_xodr_export(parsed->network, "t_junction");
  const auto written = write_xodr(parsed->network, "t_junction");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(preview.xml, *written);
  EXPECT_EQ(preview.byte_count, written->size());
  EXPECT_TRUE(preview.would_write);
}

TEST(ExportPreview, XodrCountsComeFromTheOutputNotTheNetwork) {
  auto parsed = load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(parsed.has_value());
  const XodrPreview preview = preview_xodr_export(parsed->network, "t_junction");

  // Every count must equal the number of that element in the emitted bytes —
  // which is the property that makes this half undriftable.
  const auto occurrences = [&preview](const std::string& token) {
    std::size_t count = 0;
    for (std::size_t at = preview.xml.find(token); at != std::string::npos;
         at = preview.xml.find(token, at + token.size())) {
      ++count;
    }
    return count;
  };
  EXPECT_EQ(preview.road_count, occurrences("<road "));
  EXPECT_EQ(preview.junction_count, occurrences("<junction "));
  EXPECT_EQ(preview.lane_section_count, occurrences("<laneSection"));
  EXPECT_GT(preview.lane_count, 0U);
  EXPECT_GT(preview.geometry_record_count, 0U);
  EXPECT_GT(preview.total_reference_length, 0.0);
}

TEST(ExportPreview, RefusedWriteStillCarriesTheWholeFindingList) {
  // A road with plan-view geometry but no lane section: the writer's internal
  // validate() refuses it, collapsing to ONE message.
  RoadNetwork network;
  const RoadId road = segment(network, "r0", 0.0, 0.0, 100.0, 0.0);
  ASSERT_TRUE(road.is_valid());
  network.road(road)->sections.clear();

  const XodrPreview preview = preview_xodr_export(network, "broken");
  EXPECT_FALSE(preview.would_write);
  ASSERT_TRUE(preview.refusal.has_value());
  EXPECT_TRUE(preview.xml.empty());

  // The point of the ordering: validate_network ran FIRST, so the findings
  // survive the refusal instead of being replaced by the single collapsed
  // message. Sabotage by moving write_xodr above validate_network and this is
  // the assertion that fails.
  EXPECT_FALSE(preview.diagnostics.empty())
      << "a refused export must still explain itself with the full finding list";
  const bool cites_a_rule = std::any_of(preview.diagnostics.begin(),
                                        preview.diagnostics.end(),
                                        [](const Diagnostic& d) { return !d.rule_id.empty(); });
  EXPECT_TRUE(cites_a_rule) << "at least one finding should cite a normative rule";
}

TEST(ExportPreview, Layer1RecordsAreCountedFromTheOutputAndAllRegistered) {
  const XodrPreview preview = preview_xodr_export(full_channel_network(), "full");
  ASSERT_TRUE(preview.would_write);
  ASSERT_FALSE(preview.rm_records.empty()) << "fixture emits no rm: records — test is vacuous";

  for (const XodrRecordPreview& record : preview.rm_records) {
    EXPECT_TRUE(is_registered_rm_code(record.code)) << record.code;
    // Counted out of the bytes: the scan token carries the closing quote, or
    // rm:signal would be satisfied by rm:signalmount (the fmt-s2 #326 defect).
    const std::string token = "code=\"" + record.code + "\"";
    std::size_t occurrences = 0;
    for (std::size_t at = preview.xml.find(token); at != std::string::npos;
         at = preview.xml.find(token, at + token.size())) {
      ++occurrences;
    }
    EXPECT_EQ(record.count, occurrences) << record.code;
  }
}

TEST(ExportPreview, TerrainSidecarIsAnnouncedFromTheWritersOwnOutput) {
  const XodrPreview with_terrain = preview_xodr_export(full_channel_network(), "full");
  EXPECT_FALSE(with_terrain.terrain_sidecar.empty())
      << "save_xodr would write a .asc beside the .xodr and the preview must say so";
  // Read back out of the emitted rm:terrain value, never recomputed — so the
  // preview and save_xodr cannot name different files.
  EXPECT_NE(with_terrain.xml.find(with_terrain.terrain_sidecar), std::string::npos);

  auto parsed = load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(preview_xodr_export(parsed->network, "t_junction").terrain_sidecar.empty());
}

TEST(ExportPreview, NoLayer2StateEverAppears) {
  // ADR-0008: the preview shows Layers 0 and 1 only. The .rmscene.json
  // companion is editor state and must not leak into either.
  const XodrPreview preview = preview_xodr_export(full_channel_network(), "full");
  EXPECT_EQ(preview.xml.find("rmscene"), std::string::npos);
  EXPECT_EQ(preview.xml.find("\"camera\""), std::string::npos);
  for (const XodrRecordPreview& record : preview.rm_records) {
    EXPECT_TRUE(is_registered_rm_code(record.code)) << record.code;
  }
}

} // namespace roadmaker
