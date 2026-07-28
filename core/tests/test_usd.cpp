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

// Golden-file structural tests for the OpenUSD (.usda) exporter. USDA is ASCII,
// so these assert on the emitted text (upAxis / metersPerUnit / defaultPrim,
// the Road→LaneSection→Lane Xform hierarchy, material bindings, and the
// MaterialBindingAPI apiSchema). Semantic validity against the OpenUSD
// reference implementation is covered by the `usdchecker` step in the dedicated
// RM_BUILD_USD CI job, which runs against the golden file this suite leaves in
// the temp directory.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/export_preview.hpp"
#include "roadmaker/io/usd_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"

// The exporter's own material vocabulary (core/tests has core/src on its
// include path), so the ground assertions name the definitions themselves.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "io/mesh_export_common.hpp"

namespace {

std::string slurp(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// Exports a sample to `out_name` in the temp dir and returns the USDA text.
/// The file is intentionally left on disk so the CI `usdchecker` step can
/// validate it against the OpenUSD reference implementation.
std::string export_sample(const char* sample_name, const char* out_name) {
  auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample_name);
  if (!parsed) {
    throw std::runtime_error("failed to load sample");
  }
  const auto mesh = roadmaker::build_network_mesh(parsed->network);

  const auto path = std::filesystem::temp_directory_path() / out_name;
  const auto exported = roadmaker::export_usda(mesh, path);
  if (!exported) {
    throw std::runtime_error("export_usda failed: " + exported.error().message);
  }
  return slurp(path);
}

/// Exports an in-memory mesh to `out_name` in the temp dir, same contract as
/// export_sample (the file is left behind for the CI compliance check).
std::string export_mesh(const roadmaker::NetworkMesh& mesh, const char* out_name) {
  const auto path = std::filesystem::temp_directory_path() / out_name;
  const auto exported = roadmaker::export_usda(mesh, path);
  if (!exported) {
    throw std::runtime_error("export_usda failed: " + exported.error().message);
  }
  return slurp(path);
}

/// A ~20 m square of four welded straights (enclosing one derivable ground
/// surface) plus a flat height field. Built in code because no committed sample
/// carries `rm:surface` or `rm:terrain` — the blind spot #390 came out of.
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

} // namespace

TEST(Usd, StraightRoadStageMetadataAndHierarchy) {
  const std::string usda = export_sample("straight_road.xodr", "rm_straight.usda");

  EXPECT_EQ(usda.rfind("#usda 1.0", 0), 0U); // magic header at byte 0
  EXPECT_NE(usda.find("upAxis = \"Y\""), std::string::npos);
  EXPECT_NE(usda.find("metersPerUnit = 1"), std::string::npos);
  EXPECT_NE(usda.find("defaultPrim = \"World\""), std::string::npos);

  // Road → LaneSection → Lane Xform/Mesh nesting.
  EXPECT_NE(usda.find("def Xform \"World\""), std::string::npos);
  EXPECT_NE(usda.find("def Xform \"lanesection0\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh "), std::string::npos);

  // Triangle surface, not a subdivision surface.
  EXPECT_NE(usda.find("subdivisionScheme = \"none\""), std::string::npos);
}

TEST(Usd, MaterialsBoundWithApiSchema) {
  const std::string usda = export_sample("straight_road.xodr", "rm_straight_mat.usda");

  // Materials live under a Looks scope and match the glTF material naming.
  EXPECT_NE(usda.find("def Scope \"Looks\""), std::string::npos);
  EXPECT_NE(usda.find("def Material \"lane_"), std::string::npos);
  EXPECT_NE(usda.find("info:id = \"UsdPreviewSurface\""), std::string::npos);

  // Every bound mesh applies MaterialBindingAPI (usdchecker requirement) and
  // carries a relationship into /Looks.
  EXPECT_NE(usda.find("prepend apiSchemas = [\"MaterialBindingAPI\"]"), std::string::npos);
  EXPECT_NE(usda.find("rel material:binding = </Looks/"), std::string::npos);
}

TEST(Usd, TJunctionExportsFloorSurfaceAndMaterial) {
  // This golden file is the one the CI usdchecker step validates.
  const std::string usda = export_sample("t_junction.xodr", "rm_usd_golden.usda");

  // The floor is one continuous asphalt with the roads feeding it: it binds
  // the driving-lane material (io_common::lane_material_name spelling,
  // "lane_<enum>"), and the legacy junction-debug material never reappears
  // (tee visual finding, follow-up to issue #103).
  const std::string driving_material =
      "lane_" + std::to_string(static_cast<int>(roadmaker::LaneType::Driving));
  EXPECT_NE(usda.find("def Mesh \"junction_"), std::string::npos);
  EXPECT_NE(usda.find("def Material \"" + driving_material + "\""), std::string::npos);
  EXPECT_EQ(usda.find("junction_floor"), std::string::npos);
}

// ------------------------------------------------- ground channels (#390)

TEST(Usd, GroundSurfacesAndTerrainAreWritten) {
  const roadmaker::RoadNetwork network = ground_network();
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(mesh.surfaces.empty()) << "fixture has no surface — the test is vacuous";
  ASSERT_FALSE(mesh.terrain.indices.empty()) << "fixture has no terrain — the test is vacuous";

  // This second golden is the one the CI usdchecker step validates for ground:
  // t_junction.xodr carries none, so without it the new prims would never reach
  // the OpenUSD reference implementation.
  const std::string usda = export_mesh(mesh, "rm_usd_ground_golden.usda");

  EXPECT_NE(usda.find("def Mesh \"surface_0\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh \"terrain\""), std::string::npos);
  EXPECT_NE(usda.find("def Material \"" + roadmaker::io_common::ground_material_name("") + "\""),
            std::string::npos);
  EXPECT_NE(
      usda.find("def Material \"" + std::string(roadmaker::io_common::kTerrainMaterialName) + "\""),
      std::string::npos);
  // Ground binds through the same /Looks relationship + apiSchema as everything
  // else — usdchecker rejects a binding without the applied schema.
  EXPECT_NE(usda.find("rel material:binding = </Looks/" +
                      std::string(roadmaker::io_common::kTerrainMaterialName)),
            std::string::npos);
  EXPECT_NE(usda.find("prepend apiSchemas = [\"MaterialBindingAPI\"]"), std::string::npos);
}

TEST(Usd, APavedSurfaceBindsItsOwnGroundMaterial) {
  roadmaker::RoadNetwork network = ground_network();
  roadmaker::SurfaceId id;
  network.for_each_surface([&id](roadmaker::SurfaceId candidate, const roadmaker::Surface&) {
    if (!id.is_valid()) {
      id = candidate;
    }
  });
  ASSERT_TRUE(id.is_valid());
  auto paint = roadmaker::edit::set_surface_material(network, id, "asphalt");
  ASSERT_TRUE(paint != nullptr);
  ASSERT_TRUE(paint->apply(network).has_value());

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const std::string usda = export_mesh(mesh, "rm_usd_ground_paved.usda");

  EXPECT_NE(
      usda.find("def Material \"" + roadmaker::io_common::ground_material_name("asphalt") + "\""),
      std::string::npos);
  // A paved surface is no longer grass — the terrain still is, so this asserts
  // on the surface's own binding, not on the material's mere absence.
  EXPECT_EQ(usda.find("</Looks/" + roadmaker::io_common::ground_material_name("") + ">"),
            std::string::npos);
}

TEST(Usd, ASceneOfNothingButGroundStillExports) {
  const roadmaker::RoadNetwork network = ground_network();
  roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  mesh.roads.clear();
  mesh.junction_floors.clear();
  ASSERT_FALSE(mesh.terrain.indices.empty());

  const std::string usda = export_mesh(mesh, "rm_usd_ground_only.usda");
  EXPECT_NE(usda.find("def Mesh \"terrain\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh \"surface_0\""), std::string::npos);
}

TEST(Usd, ExportingAnEmptyMeshFailsCleanly) {
  const roadmaker::NetworkMesh empty;
  const auto result =
      roadmaker::export_usda(empty, std::filesystem::temp_directory_path() / "rm_empty.usda");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}

TEST(Usd, TreePropEmitsAnXformWithPartMeshesAndMaterials) {
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
  const auto path = std::filesystem::temp_directory_path() / "rm_tree.usda";
  ASSERT_TRUE(roadmaker::export_usda(mesh, path).has_value());
  const std::string usda = slurp(path);

  // A prop Xform with the trunk + crown part meshes and their flat materials.
  EXPECT_NE(usda.find("def Xform \"prop_0_tree_pine\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh \"trunk\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh \"crown\""), std::string::npos);
  EXPECT_NE(usda.find("def Material \"propmat_tree_pine_crown\""), std::string::npos);
}

// USD bakes each instance's geometry into world space, so a resized prop (#335)
// must come out with its vertices scaled — the exported stage matches what the
// viewport draws rather than the model's authored size.
TEST(Usd, ScaledPropBakesLargerVertices) {
  const auto extent_z = [](const std::string& usda) {
    // The tallest z in the stage's baked points, read out of the USDA text:
    // every point is "(x, y, z)" and the kernel Z-up frame is rotated to Y-up
    // at export, so the model's height axis lands in the SECOND component.
    double max_y = 0.0;
    for (std::size_t i = usda.find('('); i != std::string::npos; i = usda.find('(', i + 1)) {
      double x = 0.0;
      double y = 0.0;
      double z = 0.0;
      if (std::sscanf(usda.c_str() + i, "(%lf, %lf, %lf)", &x, &y, &z) == 3) {
        max_y = std::max(max_y, y);
      }
    }
    return max_y;
  };

  const auto export_tree = [&](double scale_factor) {
    roadmaker::RoadNetwork network;
    const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                     roadmaker::Waypoint{.x = 100.0, .y = 0.0}};
    auto road = roadmaker::author_clothoid_road(
        network, waypoints, roadmaker::LaneProfile::two_lane_default());
    if (!road.has_value()) {
      throw std::runtime_error("author failed");
    }
    const roadmaker::props::PropModel* model = roadmaker::props::model("tree_pine");
    roadmaker::Object tree;
    tree.odr_id = "1";
    tree.name = "tree_pine";
    tree.type = roadmaker::ObjectType::Tree;
    tree.s = 50.0;
    tree.t = 6.0;
    tree.height = model->height * scale_factor;
    network.add_object(*road, tree);

    const auto mesh = roadmaker::build_network_mesh(network, {});
    const auto path = std::filesystem::temp_directory_path() /
                      ("rm_scaled_tree_" + std::to_string(scale_factor) + ".usda");
    if (!roadmaker::export_usda(mesh, path).has_value()) {
      throw std::runtime_error("export failed");
    }
    return slurp(path);
  };

  const double unit_top = extent_z(export_tree(1.0));
  const double doubled_top = extent_z(export_tree(2.0));
  EXPECT_GT(unit_top, 0.0);
  // The road surface contributes points too, so compare the two stages against
  // each other rather than against an absolute model height.
  EXPECT_NEAR(doubled_top, unit_top * 2.0, 1e-6);
}

// --------------------------------------------------------------------------
// Export-manifest reconciliation (p7-s1, #241).
//
// The USD half of preview_mesh_export re-states this exporter's policy, so it
// must be checked against emitted USDA the way the glTF half is checked against
// a reloaded .glb. These live HERE, in the `Usd` suite, because the usd-export
// CI job runs `ctest -R '^Usd\.'` — a gate in any other suite would never run
// against a real USD build.
//
// Coverage posture, stated rather than implied: the USD manifest's POLICY (which
// channels, which materials, which omissions) is gated everywhere by the
// source-scan and totality tests in test_export_preview.cpp, which need no USD
// build at all. Only the numeric fidelity below is gated exclusively here.

namespace {

std::size_t occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

roadmaker::NetworkMesh mesh_of(const char* sample_name) {
  auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample_name);
  if (!parsed) {
    throw std::runtime_error("failed to load sample");
  }
  return roadmaker::build_network_mesh(parsed->network);
}

} // namespace

TEST(Usd, ManifestReconcilesWithTheWrittenStage) {
  for (const char* sample : {"t_junction.xodr", "props_scale.xodr"}) {
    SCOPED_TRACE(sample);
    const roadmaker::NetworkMesh mesh = mesh_of(sample);
    const roadmaker::ScenePreview preview =
        roadmaker::preview_mesh_export(mesh, roadmaker::MeshExportFormat::Usd);

    const auto path = std::filesystem::temp_directory_path() / "rm_usd_manifest.usda";
    ASSERT_TRUE(roadmaker::export_usda(mesh, path).has_value());
    const std::string usda = slurp(path);
    std::remove(path.string().c_str());

    EXPECT_EQ(preview.mesh_count, occurrences(usda, "def Mesh "));
    EXPECT_EQ(preview.materials.size(), occurrences(usda, "def Material "));

    // Every manifest material must actually be a prim in the stage.
    for (const roadmaker::MaterialPreview& material : preview.materials) {
      EXPECT_NE(usda.find("def Material \"" + material.name + "\""), std::string::npos)
          << material.name << " is in the manifest but not in the stage";
    }
  }
}

TEST(Usd, PropsAreBakedPerInstanceAsTheManifestClaims) {
  const roadmaker::NetworkMesh mesh = mesh_of("tree_avenue.xodr");
  ASSERT_GT(mesh.objects.size(), 1U) << "sample has too few props — the test is vacuous";

  const roadmaker::ScenePreview usd =
      roadmaker::preview_mesh_export(mesh, roadmaker::MeshExportFormat::Usd);
  const roadmaker::ScenePreview gltf =
      roadmaker::preview_mesh_export(mesh, roadmaker::MeshExportFormat::Gltf);

  // The asymmetry the manifest exists to report: USD stores every instance's
  // geometry, glTF stores one copy and instances it with nodes.
  EXPECT_GT(usd.total_triangles, gltf.total_triangles);
}

TEST(Usd, SignFaceTexturesAreAbsentAndTheManifestSaysSo) {
  const roadmaker::NetworkMesh mesh = mesh_of("sign_pack.xodr");
  const roadmaker::ScenePreview preview =
      roadmaker::preview_mesh_export(mesh, roadmaker::MeshExportFormat::Usd);
  const auto& faces =
      preview.channels[static_cast<std::size_t>(roadmaker::MeshChannel::SignalFaces)];
  ASSERT_GT(faces.elements, 0U) << "sample carries no sign faces — the test is vacuous";
  EXPECT_EQ(faces.exported_elements, 0U);
  EXPECT_EQ(faces.reason, roadmaker::OmissionReason::FormatUnsupported);
  EXPECT_EQ(preview.image_count, 0U);

  const auto path = std::filesystem::temp_directory_path() / "rm_usd_faces.usda";
  ASSERT_TRUE(roadmaker::export_usda(mesh, path).has_value());
  const std::string usda = slurp(path);
  std::remove(path.string().c_str());

  // The negative the manifest promises: no face prim, no texture, anywhere.
  EXPECT_EQ(usda.find(":face"), std::string::npos);
  EXPECT_EQ(usda.find("inputs:file"), std::string::npos);
}
