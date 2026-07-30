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

// Importing a user's own glTF/GLB as a project prop (p6-s8, #322 · ADR-0013).
//
// The claim these tests exist to hold is that NOTHING downstream of the importer
// changed: once a model is registered, the mesh builder instances it, the prop
// tools place it and the exporters draw it exactly as they do a bundled prop.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "document/asset_import.hpp"
#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"

namespace roadmaker::editor {
namespace {

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(RM_GLTF_FIXTURES_DIR) / name;
}

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

AssetImportRequest request_for(const std::filesystem::path& source, const QString& label) {
  AssetImportRequest request;
  request.source = source;
  request.label = label;
  request.category = QStringLiteral("Props");
  request.license = QStringLiteral("CC0 / own work");
  return request;
}

/// The overlay is process-wide, so every case here has to put it back.
class PropAssetImport : public testing::Test {
protected:
  void TearDown() override { props::clear_project_models(); }
};

} // namespace

TEST_F(PropAssetImport, AModelIsClassifiedAsAPropRatherThanAMaterial) {
  EXPECT_EQ(asset_import_kind("chair.glb"), AssetImportKind::Prop);
  EXPECT_EQ(asset_import_kind("chair.gltf"), AssetImportKind::Prop);
  EXPECT_EQ(asset_import_kind("brick.png"), AssetImportKind::Material);
  // Still refused by name, per ADR-0013's closed list.
  EXPECT_FALSE(asset_import_kind("chair.obj").has_value());
  EXPECT_FALSE(asset_import_kind("chair.fbx").has_value());
  // And the dialog filter offers both kinds now.
  const QString filter = asset_import_filter();
  EXPECT_TRUE(filter.contains(QStringLiteral("*.glb")));
  EXPECT_TRUE(filter.contains(QStringLiteral("*.png")));
}

TEST_F(PropAssetImport, CopiesTheModelIntoTheProjectAndDescribesTheAsset) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported =
      import_prop_asset(fs_path(project.path()),
                        request_for(fixture("textured_box.glb"), QStringLiteral("Wooden Crate")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;
  const auto& [item, result, diagnostics] = *imported;

  EXPECT_EQ(result.slug, QStringLiteral("wooden_crate"));
  EXPECT_EQ(result.key, QStringLiteral("prop.wooden_crate"));
  EXPECT_EQ(result.copied_to, fs_path(project.path()) / "assets" / "props" / "wooden_crate.glb");
  EXPECT_TRUE(std::filesystem::is_regular_file(result.copied_to));

  // `kind: tree` is the create-intent tag for EVERY point prop; the OpenDRIVE
  // class comes from the model, not this tag.
  EXPECT_EQ(item.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(item.model, QStringLiteral("wooden_crate"));
  EXPECT_EQ(item.category, QStringLiteral("Props"));
  // No thumbnail — #322 puts render-to-thumbnail out of scope (#509); the panel
  // falls back to a themed glyph.
  EXPECT_TRUE(item.thumbnail.isEmpty());

  // Provenance and the model file travel in create_raw, which to_json re-emits
  // verbatim — the same mechanism #336 used for default_scale.
  EXPECT_EQ(item.create_raw.value(QStringLiteral("model_file")).toString(),
            QStringLiteral("assets/props/wooden_crate.glb"));
  EXPECT_EQ(item.create_raw.value(QStringLiteral("license")).toString(),
            QStringLiteral("CC0 / own work"));
  EXPECT_FALSE(item.create_raw.value(QStringLiteral("source")).toString().isEmpty());

  // The flatten warning reaches the caller rather than being swallowed.
  const bool flattened =
      std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
        return d.message.find("flattened") != std::string::npos;
      });
  EXPECT_TRUE(flattened) << "the textured fixture must report its flattening";
}

TEST_F(PropAssetImport, AModelIsReadBeforeItIsCopied) {
  // ★ A file that cannot become a prop must not leave bytes inside the project.
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported = import_prop_asset(
      fs_path(project.path()), request_for(fixture("truncated.glb"), QStringLiteral("Broken")));
  ASSERT_FALSE(imported.has_value());
  EXPECT_FALSE(
      std::filesystem::exists(fs_path(project.path()) / "assets" / "props" / "broken.glb"));
}

TEST_F(PropAssetImport, ACollidingNameIsSuffixedRatherThanOverwritten) {
  // Stronger reason than for materials: overwriting a slug already placed in a
  // scene would silently swap the geometry under it.
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto one = import_prop_asset(
      fs_path(project.path()), request_for(fixture("textured_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(one.has_value()) << one.error().message;
  const auto two = import_prop_asset(
      fs_path(project.path()), request_for(fixture("factor_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(two.has_value()) << two.error().message;
  EXPECT_EQ(std::get<1>(*one).slug, QStringLiteral("crate"));
  EXPECT_EQ(std::get<1>(*two).slug, QStringLiteral("crate_2"));
  EXPECT_TRUE(std::get<1>(*two).renamed);
  EXPECT_TRUE(std::filesystem::is_regular_file(std::get<1>(*one).copied_to));
}

TEST_F(PropAssetImport, AProjectsModelsAreRegisteredFromItsManifest) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported = import_prop_asset(
      fs_path(project.path()), request_for(fixture("textured_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;

  LibraryManifest manifest;
  manifest.upsert(std::get<0>(*imported));

  // Before registration the model does not resolve at all.
  EXPECT_EQ(props::model("crate"), nullptr);

  const std::vector<Diagnostic> diagnostics =
      register_project_prop_models(fs_path(project.path()), manifest);

  const props::PropModel* model = props::model("crate");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->id, "crate");
  EXPECT_TRUE(props::is_project_model("crate"));
  EXPECT_GT(model->height, 0.0);
  // The bundled catalogue is untouched.
  EXPECT_NE(props::model("tree_pine"), nullptr);
  // The reader's warnings are re-reported on every project open, so a user who
  // reopens a project still learns why their crate is flat.
  EXPECT_FALSE(diagnostics.empty());
}

TEST_F(PropAssetImport, AMissingModelFileIsReportedAndSkippedRatherThanFatal) {
  // ★ The project still opens. A broken asset is a diagnostic, not a reason to
  // refuse the whole library.
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  LibraryManifest manifest;
  LibraryItem present;
  present.key = QStringLiteral("prop.gone");
  present.kind = LibraryItem::Kind::Tree;
  present.model = QStringLiteral("gone");
  QJsonObject create;
  create[QStringLiteral("kind")] = QStringLiteral("tree");
  create[QStringLiteral("model")] = QStringLiteral("gone");
  create[QStringLiteral("model_file")] = QStringLiteral("assets/props/gone.glb");
  present.create_raw = create;
  manifest.upsert(present);

  const std::vector<Diagnostic> diagnostics =
      register_project_prop_models(fs_path(project.path()), manifest);

  EXPECT_EQ(props::model("gone"), nullptr);
  ASSERT_FALSE(diagnostics.empty());
  const bool named = std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
    return d.message.find("gone") != std::string::npos &&
           d.message.find("will not") != std::string::npos;
  });
  EXPECT_TRUE(named) << "the diagnostic has to name the asset that will not draw";
}

TEST_F(PropAssetImport, ABundledPropItemIsLeftToTheCompiledInCatalogue) {
  // An items[] row with no model_file is a bundled prop, and must not be treated
  // as a missing project asset.
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  LibraryManifest manifest;
  LibraryItem bundled;
  bundled.key = QStringLiteral("prop.tree.pine");
  bundled.kind = LibraryItem::Kind::Tree;
  bundled.model = QStringLiteral("tree_pine");
  manifest.upsert(bundled);

  const std::vector<Diagnostic> diagnostics =
      register_project_prop_models(fs_path(project.path()), manifest);
  EXPECT_TRUE(diagnostics.empty());
  EXPECT_NE(props::model("tree_pine"), nullptr);
  EXPECT_FALSE(props::is_project_model("tree_pine"));
}

TEST_F(PropAssetImport, AnImportedPropPlacesAndMeshesLikeABundledOne) {
  // ★ THE CLAIM OF THE WHOLE SPRINT: nothing downstream changed. This goes through
  // the ordinary placement path (make_prop_object, the same call every prop tool
  // and the Library drop make) and then the ordinary mesh builder.
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported = import_prop_asset(
      fs_path(project.path()), request_for(fixture("textured_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;
  const LibraryItem& item = std::get<0>(*imported);

  LibraryManifest manifest;
  manifest.upsert(item);
  // The returned diagnostics carry the flatten warning, which its own case
  // asserts; here only the registration matters.
  const std::vector<Diagnostic> ignored =
      register_project_prop_models(fs_path(project.path()), manifest);
  (void)ignored;
  const props::PropModel* model = props::model("crate");
  ASSERT_NE(model, nullptr);

  // The placement path gates on the model resolving, so this is also the check
  // that an imported asset is placeable at all.
  EXPECT_TRUE(is_prop_asset(item));

  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 60.0, .y = 0.0}};
  auto authored = author_clothoid_road(network, waypoints, LaneProfile::urban_sidewalk());
  ASSERT_TRUE(authored.has_value()) << authored.error().message;
  const RoadId road = *authored;
  Object placed = make_prop_object(item, "1", 30.0, -6.0);
  EXPECT_EQ(placed.name, "crate") << "the object's @name IS the model id";
  EXPECT_TRUE(placed.height.has_value());
  EXPECT_NEAR(*placed.height, model->height, 1e-9);
  (void)network.add_object(road, placed);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_EQ(mesh.objects.front().model_id, "crate");
  // @height / model height, the same rule a bundled prop obeys (#335).
  EXPECT_NEAR(mesh.objects.front().scale, 1.0, 1e-9);

  // And when the project closes, the same scene meshes to nothing — the gap #508
  // tracks, pinned so the follow-up has a test to flip.
  props::clear_project_models();
  EXPECT_TRUE(build_network_mesh(network).objects.empty());
}

TEST_F(PropAssetImport, ScalingAnImportedPropUsesTheDeclaredHeight) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported = import_prop_asset(
      fs_path(project.path()), request_for(fixture("factor_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;
  LibraryManifest manifest;
  manifest.upsert(std::get<0>(*imported));
  // The returned diagnostics carry the flatten warning, which its own case
  // asserts; here only the registration matters.
  const std::vector<Diagnostic> ignored =
      register_project_prop_models(fs_path(project.path()), manifest);
  (void)ignored;
  const props::PropModel* model = props::model("crate");
  ASSERT_NE(model, nullptr);

  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 60.0, .y = 0.0}};
  auto authored = author_clothoid_road(network, waypoints, LaneProfile::urban_sidewalk());
  ASSERT_TRUE(authored.has_value()) << authored.error().message;
  const RoadId road = *authored;
  Object placed = make_prop_object(std::get<0>(*imported), "1", 30.0, -6.0);
  placed.height = model->height * 2.5;
  (void)network.add_object(road, placed);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_NEAR(mesh.objects.front().scale, 2.5, 1e-9);
}

TEST_F(PropAssetImport, TheItemRoundTripsThroughTheManifest) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported = import_prop_asset(
      fs_path(project.path()), request_for(fixture("textured_box.glb"), QStringLiteral("Crate")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;

  LibraryManifest manifest;
  manifest.upsert(std::get<0>(*imported));
  const auto path = fs_path(project.path()) / "assets" / "library" / "manifest.json";
  std::filesystem::create_directories(path.parent_path());
  ASSERT_TRUE(manifest.save(path).has_value());

  const auto reloaded = LibraryManifest::load(path);
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
  ASSERT_EQ(reloaded->items().size(), 1U);
  const LibraryItem& item = reloaded->items().front();
  EXPECT_EQ(item.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(item.model, QStringLiteral("crate"));
  EXPECT_EQ(item.create_raw.value(QStringLiteral("model_file")).toString(),
            QStringLiteral("assets/props/crate.glb"));
  EXPECT_EQ(item.create_raw.value(QStringLiteral("license")).toString(),
            QStringLiteral("CC0 / own work"));

  // And the reloaded item still registers, which is what "survives close and
  // reopen" actually means.
  const std::vector<Diagnostic> diagnostics =
      register_project_prop_models(fs_path(project.path()), *reloaded);
  EXPECT_NE(props::model("crate"), nullptr);
  const bool broken = std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
    return d.message.find("could not be loaded") != std::string::npos;
  });
  EXPECT_FALSE(broken);
}

} // namespace roadmaker::editor
