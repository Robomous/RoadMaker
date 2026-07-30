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

// Importing a user's own image as a project material (p6-s8, #322 · ADR-0013).
//
// Headless: asset_import is a pure function over a project directory, so the
// whole gesture — collision, copy, thumbnail, manifest entry — is checked here
// without a dialog or a MainWindow.

#include <gtest/gtest.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "document/asset_import.hpp"
#include "document/library_manifest.hpp"
#include "document/project.hpp"

namespace roadmaker::editor {
namespace {

/// A small real PNG on disk. Real rather than fabricated bytes because the
/// importer decodes it — a fake would test the error path by accident.
QString write_image(const QDir& dir, const QString& name, QColor color = Qt::red, int size = 64) {
  QImage image(size, size, QImage::Format_RGB888);
  image.fill(color);
  const QString path = dir.filePath(name);
  EXPECT_TRUE(image.save(path, "PNG")) << path.toStdString();
  return path;
}

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

AssetImportRequest request_for(const QString& source, const QString& label) {
  AssetImportRequest request;
  request.source = fs_path(source);
  request.label = label;
  request.category = QStringLiteral("Materials");
  request.license = QStringLiteral("CC0 / own work");
  return request;
}

} // namespace

TEST(AssetImportKindTest, ImagesAreMaterialsAndModelsAreProps) {
  EXPECT_EQ(asset_import_kind("brick.png"), AssetImportKind::Material);
  EXPECT_EQ(asset_import_kind("brick.JPG"), AssetImportKind::Material);
  EXPECT_EQ(asset_import_kind("brick.jpeg"), AssetImportKind::Material);
  // A model imports as a prop, not a material (the prop half of p6-s8).
  EXPECT_EQ(asset_import_kind("chair.glb"), AssetImportKind::Prop);
  EXPECT_EQ(asset_import_kind("chair.gltf"), AssetImportKind::Prop);
  // Neither, and refused by name per ADR-0013's closed list.
  EXPECT_FALSE(asset_import_kind("scene.xodr").has_value());
  EXPECT_FALSE(asset_import_kind("notes.txt").has_value());
  EXPECT_FALSE(asset_import_kind("noextension").has_value());
}

TEST(AssetImportKindTest, TheDialogFilterAndThePredicateCannotDisagree) {
  // Both are built from one list, which is the whole point of the predicate.
  const QString filter = asset_import_filter();
  EXPECT_TRUE(filter.contains(QStringLiteral("*.png")));
  EXPECT_TRUE(filter.contains(QStringLiteral("*.jpg")));
  EXPECT_TRUE(filter.contains(QStringLiteral("*.glb")));
  EXPECT_TRUE(filter.contains(QStringLiteral("*.gltf")));
}

TEST(AssetSlug, DerivesAFilesystemSafeName) {
  EXPECT_EQ(asset_slug(QStringLiteral("Worn Asphalt")), QStringLiteral("worn_asphalt"));
  EXPECT_EQ(asset_slug(QStringLiteral("brick-wall_02")), QStringLiteral("brick_wall_02"));
  // Runs of punctuation collapse to a single separator, and edges are trimmed.
  EXPECT_EQ(asset_slug(QStringLiteral("  Red // Brick!  ")), QStringLiteral("red_brick"));
  // Nothing usable at all is reported rather than silently becoming "_".
  EXPECT_TRUE(asset_slug(QStringLiteral("!!!")).isEmpty());
  EXPECT_TRUE(asset_slug(QString()).isEmpty());
}

TEST(AssetImport, CopiesTheFileIntoTheProjectAndWritesAThumbnail) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  QTemporaryDir downloads;
  ASSERT_TRUE(downloads.isValid());
  const QString source = write_image(QDir(downloads.path()), QStringLiteral("brick.png"));

  const auto imported = import_material_asset(fs_path(project.path()),
                                              request_for(source, QStringLiteral("Red Brick")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;
  const auto& [material, result] = *imported;

  EXPECT_EQ(result.slug, QStringLiteral("red_brick"));
  EXPECT_EQ(result.key, QStringLiteral("material.red_brick"));
  EXPECT_FALSE(result.renamed);

  // The bytes are now INSIDE the project, which is what makes it survive the
  // user tidying their Downloads folder.
  EXPECT_TRUE(std::filesystem::is_regular_file(result.copied_to));
  EXPECT_EQ(result.copied_to, fs_path(project.path()) / "assets" / "textures" / "red_brick.png");

  // A real thumbnail, not a placeholder.
  const QString thumbnail =
      QDir(project.path()).filePath(QStringLiteral("assets/library/thumbnails/red_brick.png"));
  ASSERT_TRUE(QFile::exists(thumbnail)) << thumbnail.toStdString();
  const QImage image(thumbnail);
  ASSERT_FALSE(image.isNull());
  EXPECT_LE(image.width(), 96);
  EXPECT_LE(image.height(), 96);
  EXPECT_GT(image.width(), 0);

  // The definition points at the project's COPY, by a portable relative path.
  EXPECT_EQ(material.id, QStringLiteral("rm:red_brick"));
  EXPECT_EQ(material.label, QStringLiteral("Red Brick"));
  EXPECT_EQ(material.albedo, QStringLiteral("assets/textures/red_brick.png"));
  EXPECT_EQ(material.thumbnail, QStringLiteral("assets/library/thumbnails/red_brick.png"));
  EXPECT_FALSE(material.albedo.contains(QLatin1Char('\\')));

  // Provenance, per #322's acceptance: where it came from and what the user said
  // about its licence. Neither is read to load the asset.
  EXPECT_EQ(material.source, source);
  EXPECT_EQ(material.license, QStringLiteral("CC0 / own work"));
}

TEST(AssetImport, ASecondImportOfTheSameNameIsSuffixedRatherThanOverwritten) {
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  const QString first = write_image(QDir(downloads.path()), QStringLiteral("a.png"), Qt::red);
  const QString second = write_image(QDir(downloads.path()), QStringLiteral("b.png"), Qt::blue);

  const auto one =
      import_material_asset(fs_path(project.path()), request_for(first, QStringLiteral("Brick")));
  ASSERT_TRUE(one.has_value()) << one.error().message;
  const auto two =
      import_material_asset(fs_path(project.path()), request_for(second, QStringLiteral("Brick")));
  ASSERT_TRUE(two.has_value()) << two.error().message;

  EXPECT_EQ(one->second.slug, QStringLiteral("brick"));
  EXPECT_EQ(two->second.slug, QStringLiteral("brick_2"));
  EXPECT_FALSE(one->second.renamed);
  EXPECT_TRUE(two->second.renamed) << "the caller has to be able to say the name changed";

  // The FIRST import's bytes are still the first import's bytes. Overwriting
  // would silently replace an asset already used in a scene — and would also
  // strand ViewportWidget::texture_for on its cached copy of the old image.
  EXPECT_TRUE(std::filesystem::is_regular_file(one->second.copied_to));
  EXPECT_TRUE(std::filesystem::is_regular_file(two->second.copied_to));
  EXPECT_NE(one->second.copied_to, two->second.copied_to);
  const QImage kept(QString::fromStdString(one->second.copied_to.string()));
  ASSERT_FALSE(kept.isNull());
  EXPECT_EQ(kept.pixelColor(0, 0).red(), 255) << "the first asset was overwritten";
}

TEST(AssetImport, AnUnsupportedFileIsRefusedByName) {
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  const QString path = QDir(downloads.path()).filePath(QStringLiteral("notes.txt"));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("not an image");
  file.close();

  const auto imported =
      import_material_asset(fs_path(project.path()), request_for(path, QStringLiteral("Notes")));
  ASSERT_FALSE(imported.has_value());
  EXPECT_EQ(imported.error().code, ErrorCode::InvalidArgument);
}

TEST(AssetImport, TheMaterialImporterRefusesAModelEvenThoughItIsImportable) {
  // A .glb is importable — as a PROP. Handing it to the material importer has to
  // be refused rather than treated as an undecodable image.
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  const QString path = QDir(downloads.path()).filePath(QStringLiteral("chair.glb"));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("glTF");
  file.close();

  const auto imported =
      import_material_asset(fs_path(project.path()), request_for(path, QStringLiteral("Chair")));
  ASSERT_FALSE(imported.has_value());
  EXPECT_EQ(imported.error().code, ErrorCode::InvalidArgument);
}

TEST(AssetImport, AMissingFileIsNotFound) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const auto imported =
      import_material_asset(fs_path(project.path()),
                            request_for(QDir(project.path()).filePath(QStringLiteral("gone.png")),
                                        QStringLiteral("Gone")));
  ASSERT_FALSE(imported.has_value());
  EXPECT_EQ(imported.error().code, ErrorCode::FileNotFound);
}

TEST(AssetImport, ACorruptImageLeavesNothingBehind) {
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  // A .png extension over bytes that are not a PNG — what a mis-renamed file is.
  const QString path = QDir(downloads.path()).filePath(QStringLiteral("broken.png"));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("\x89PNG\r\n\x1a\n"
             "garbage");
  file.close();

  const auto imported =
      import_material_asset(fs_path(project.path()), request_for(path, QStringLiteral("Broken")));
  ASSERT_FALSE(imported.has_value());
  // ★ AND THE COPY IS GONE. A failed import that leaves a file inside the project
  // with no manifest entry is litter the user cannot see or remove.
  EXPECT_FALSE(
      std::filesystem::exists(fs_path(project.path()) / "assets" / "textures" / "broken.png"));
}

TEST(AssetImport, AnUnnameableAssetIsRefusedBeforeAnythingIsCopied) {
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  const QString source = write_image(QDir(downloads.path()), QStringLiteral("x.png"));
  const auto imported =
      import_material_asset(fs_path(project.path()), request_for(source, QStringLiteral("!!!")));
  ASSERT_FALSE(imported.has_value());
  EXPECT_EQ(imported.error().code, ErrorCode::InvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(fs_path(project.path()) / "assets" / "textures"));
}

TEST(AssetImport, TheDefinitionRoundTripsThroughTheManifest) {
  QTemporaryDir project;
  QTemporaryDir downloads;
  ASSERT_TRUE(project.isValid());
  ASSERT_TRUE(downloads.isValid());
  const QString source = write_image(QDir(downloads.path()), QStringLiteral("brick.png"));
  const auto imported = import_material_asset(fs_path(project.path()),
                                              request_for(source, QStringLiteral("Red Brick")));
  ASSERT_TRUE(imported.has_value()) << imported.error().message;

  // The gesture the editor performs: upsert, save, reload.
  LibraryManifest manifest;
  manifest.upsert_material(imported->first);
  const auto path = fs_path(project.path()) / "assets" / "library" / "manifest.json";
  ASSERT_TRUE(manifest.save(path).has_value());

  const auto reloaded = LibraryManifest::load(path);
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
  ASSERT_EQ(reloaded->materials().size(), 1U);
  const LibraryMaterial& material = reloaded->materials().front();
  EXPECT_EQ(material.id, QStringLiteral("rm:red_brick"));
  EXPECT_EQ(material.label, QStringLiteral("Red Brick"));
  EXPECT_EQ(material.albedo, QStringLiteral("assets/textures/red_brick.png"));
  EXPECT_EQ(material.license, QStringLiteral("CC0 / own work"));
  EXPECT_EQ(material.source, source);

  // And it presents itself in the Library without needing a hand-written
  // items[] row.
  const auto& items = reloaded->items();
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items.front().key, QStringLiteral("material.red_brick"));
  EXPECT_EQ(items.front().kind, LibraryItem::Kind::Material);
  EXPECT_EQ(items.front().material, QStringLiteral("rm:red_brick"));
  EXPECT_TRUE(items.front().synthesized);
}

TEST(ProjectLibraryWritePath, IsCreatableSoAFreshProjectCanAuthorItsFirstAsset) {
  // ★ THE BUG THIS FIXES. library_manifest_path() returns nullopt until the file
  // exists, and every asset-commit path in MainWindow bailed on that — so a fresh
  // project could never author its FIRST asset of any kind, crosswalks and prop
  // sets included.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto project = Project::create(fs_path(dir.path()), QStringLiteral("Demo"));
  ASSERT_TRUE(project.has_value()) << project.error().message;

  EXPECT_FALSE(project->library_manifest_path().has_value())
      << "read side must still report 'no overlay yet'";

  const auto writable = project->library_manifest_path_for_write();
  ASSERT_TRUE(writable.has_value()) << writable.error().message;
  EXPECT_EQ(*writable, project->assets_dir() / "library" / "manifest.json");
  EXPECT_TRUE(std::filesystem::is_directory(project->assets_dir() / "library"));
  // Creating the folder must not conjure the file itself: the read side still
  // reports no overlay until something is actually saved.
  EXPECT_FALSE(project->library_manifest_path().has_value());

  LibraryManifest manifest;
  ASSERT_TRUE(manifest.save(*writable).has_value());
  EXPECT_TRUE(project->library_manifest_path().has_value());
}

} // namespace roadmaker::editor
