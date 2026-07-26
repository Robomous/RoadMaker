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

// ProjectFilesModel (p6-s7, #321): the Library's file explorer over a
// project's `assets` folder. These tests pin the snapshot contract (hierarchy,
// ordering, roles, thumbnails), the refresh deltas that back the "no restart"
// acceptance, the watcher wiring, and the caps that keep a pathological folder
// from taking the editor with it.

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "document/project_files_model.hpp"

namespace roadmaker::editor {
namespace {

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << contents;
}

/// A real PNG on disk — the bundled thumbnails are the only images the test
/// tree is guaranteed to have, and they are what the editor decodes for real.
void copy_png(const std::filesystem::path& destination) {
  const std::filesystem::path source =
      std::filesystem::path(RM_ASSETS_DIR) / "library" / "thumbnails" / "assembly_t.png";
  std::filesystem::create_directories(destination.parent_path());
  std::error_code ec;
  std::filesystem::copy_file(
      source, destination, std::filesystem::copy_options::overwrite_existing, ec);
  ASSERT_FALSE(ec) << "cannot stage " << source.string();
}

QStringList top_level_names(const ProjectFilesModel& model) {
  QStringList names;
  for (int row = 0; row < model.rowCount(); ++row) {
    names.append(model.data(model.index(row, 0), Qt::DisplayRole).toString());
  }
  return names;
}

QModelIndex
child_named(const ProjectFilesModel& model, const QModelIndex& parent, const QString& name) {
  for (int row = 0; row < model.rowCount(parent); ++row) {
    const QModelIndex index = model.index(row, 0, parent);
    if (model.data(index, Qt::DisplayRole).toString() == name) {
      return index;
    }
  }
  return {};
}

TEST(ProjectFilesModel, PassesQtModelSanityChecksEmptyAndPopulated) {
  ProjectFilesModel model;
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  EXPECT_EQ(model.rowCount(), 0);
  EXPECT_FALSE(model.root_exists());

  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.png", "not really a png");
  write_file(assets / "models" / "bench.glb", "glb");
  model.set_root(assets, fs_path(project.path()));

  EXPECT_TRUE(model.root_exists());
  EXPECT_EQ(model.rowCount(), 2); // models/, textures/
}

TEST(ProjectFilesModel, MirrorsTheHierarchyWithFoldersFirstAndACaseInsensitiveSort) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "Bravo.txt", "b");
  write_file(assets / "alpha.txt", "a");
  write_file(assets / "zulu" / "nested.txt", "n");
  write_file(assets / ".hidden", "dot");
  std::filesystem::create_directories(assets / ".git");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));

  // Directories lead, then files; each sorted case-insensitively, so "alpha"
  // precedes "Bravo" rather than sorting after it by ASCII.
  EXPECT_EQ(
      top_level_names(model),
      QStringList(
          {QStringLiteral("zulu"), QStringLiteral("alpha.txt"), QStringLiteral("Bravo.txt")}));

  const QModelIndex zulu = model.index(0, 0);
  EXPECT_TRUE(model.data(zulu, ProjectFilesModel::IsDirRole).toBool());
  ASSERT_EQ(model.rowCount(zulu), 1);
  EXPECT_EQ(model.data(model.index(0, 0, zulu), Qt::DisplayRole).toString(),
            QStringLiteral("nested.txt"));
  EXPECT_EQ(model.parent(model.index(0, 0, zulu)), zulu);
}

TEST(ProjectFilesModel, RolesReportThePathTypeAndRelativeLocation) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.PNG", "x");
  write_file(assets / "scene.xodr", "x");
  write_file(assets / "library" / "manifest.json", "{}");
  write_file(assets / "models" / "bench.glb", "x");
  write_file(assets / "notes.md", "x");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));

  const QModelIndex textures = child_named(model, {}, QStringLiteral("textures"));
  ASSERT_TRUE(textures.isValid());
  const QModelIndex brick = child_named(model, textures, QStringLiteral("brick.PNG"));
  ASSERT_TRUE(brick.isValid());

  EXPECT_EQ(model.data(brick, ProjectFilesModel::PathRole).toString(),
            QString::fromStdString((assets / "textures" / "brick.PNG").string()));
  // Relative paths are '/'-separated on every platform — they are shown to the
  // user, not handed to the filesystem.
  EXPECT_EQ(model.data(brick, ProjectFilesModel::RelativePathRole).toString(),
            QStringLiteral("textures/brick.PNG"));
  EXPECT_EQ(model.data(brick, Qt::ToolTipRole).toString(), QStringLiteral("textures/brick.PNG"));
  EXPECT_FALSE(model.data(brick, ProjectFilesModel::IsDirRole).toBool());

  // The extension decides the type, case-insensitively; nothing is opened.
  const auto type_of = [&model](const QModelIndex& index) {
    return static_cast<ProjectFilesModel::FileType>(
        model.data(index, ProjectFilesModel::FileTypeRole).toInt());
  };
  EXPECT_EQ(type_of(brick), ProjectFilesModel::FileType::Image);
  EXPECT_EQ(type_of(textures), ProjectFilesModel::FileType::Directory);
  EXPECT_EQ(type_of(child_named(model, {}, QStringLiteral("scene.xodr"))),
            ProjectFilesModel::FileType::Scene);
  EXPECT_EQ(type_of(child_named(model, {}, QStringLiteral("notes.md"))),
            ProjectFilesModel::FileType::Other);
  const QModelIndex models = child_named(model, {}, QStringLiteral("models"));
  ASSERT_TRUE(models.isValid());
  EXPECT_EQ(type_of(child_named(model, models, QStringLiteral("bench.glb"))),
            ProjectFilesModel::FileType::Model);
  const QModelIndex library = child_named(model, {}, QStringLiteral("library"));
  ASSERT_TRUE(library.isValid());
  EXPECT_EQ(type_of(child_named(model, library, QStringLiteral("manifest.json"))),
            ProjectFilesModel::FileType::Manifest);

  EXPECT_EQ(model.index_for_path(model.data(brick, ProjectFilesModel::PathRole).toString()), brick);
  EXPECT_FALSE(model.index_for_path(QStringLiteral("/nowhere/at/all.png")).isValid());
}

TEST(ProjectFilesModel, DecodesImageThumbnailsAndLeavesEverythingElseToTheView) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  copy_png(assets / "good.png");
  write_file(assets / "notes.txt", "plain text");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));

  const QModelIndex good = child_named(model, {}, QStringLiteral("good.png"));
  ASSERT_TRUE(good.isValid());
  const QVariant thumbnail = model.data(good, Qt::DecorationRole);
  ASSERT_TRUE(thumbnail.isValid());
  EXPECT_FALSE(thumbnail.value<QIcon>().isNull());

  // A non-image carries NO decoration on purpose: the per-type glyph is the
  // proxy's job, and a blank icon here would leave it nothing to fall back to.
  const QModelIndex notes = child_named(model, {}, QStringLiteral("notes.txt"));
  ASSERT_TRUE(notes.isValid());
  EXPECT_FALSE(model.data(notes, Qt::DecorationRole).isValid());
}

TEST(ProjectFilesModel, AFailedDecodeIsCachedAndOnlyRetriedAfterARefresh) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  // Half-copied or corrupt: the extension says image, the bytes do not.
  write_file(assets / "broken.png", "\x89PNG truncated");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));
  const QModelIndex broken = child_named(model, {}, QStringLiteral("broken.png"));
  ASSERT_TRUE(broken.isValid());
  EXPECT_FALSE(model.data(broken, Qt::DecorationRole).isValid());

  // Repair the file WITHOUT telling the model. A second read must still come
  // back empty — that is the negative cache doing its job instead of re-probing
  // a bad file on every paint.
  copy_png(assets / "broken.png");
  EXPECT_FALSE(model.data(broken, Qt::DecorationRole).isValid());

  // A refresh clears the cache, so the repaired file decodes.
  model.refresh();
  const QModelIndex repaired = child_named(model, {}, QStringLiteral("broken.png"));
  ASSERT_TRUE(repaired.isValid());
  EXPECT_TRUE(model.data(repaired, Qt::DecorationRole).isValid());
}

TEST(ProjectFilesModel, RefreshReflectsCreateRenameAndDelete) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.png", "x");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));
  const QModelIndex textures = child_named(model, {}, QStringLiteral("textures"));
  ASSERT_TRUE(textures.isValid());
  ASSERT_EQ(model.rowCount(textures), 1);

  write_file(assets / "textures" / "asphalt.png", "x");
  model.refresh();
  EXPECT_EQ(model.rowCount(child_named(model, {}, QStringLiteral("textures"))), 2);

  std::filesystem::rename(assets / "textures" / "asphalt.png",
                          assets / "textures" / "asphalt_worn.png");
  model.refresh();
  const QModelIndex after_rename = child_named(model, {}, QStringLiteral("textures"));
  EXPECT_TRUE(child_named(model, after_rename, QStringLiteral("asphalt_worn.png")).isValid());
  EXPECT_FALSE(child_named(model, after_rename, QStringLiteral("asphalt.png")).isValid());

  std::filesystem::remove(assets / "textures" / "brick.png");
  model.refresh();
  EXPECT_EQ(model.rowCount(child_named(model, {}, QStringLiteral("textures"))), 1);
}

TEST(ProjectFilesModel, AMissingAssetsFolderIsEmptyUntilItAppears) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));
  EXPECT_FALSE(model.root_exists());
  EXPECT_EQ(model.rowCount(), 0);
  // With no assets folder the PROJECT directory is what gets watched, so its
  // creation is noticed without a restart.
  EXPECT_EQ(model.watched_directories(), QStringList({project.path()}));

  write_file(assets / "brick.png", "x");
  model.refresh();
  EXPECT_TRUE(model.root_exists());
  EXPECT_EQ(model.rowCount(), 1);
  EXPECT_FALSE(model.watched_directories().contains(project.path()));
}

TEST(ProjectFilesModel, WatchesEveryDirectoryOfTheSnapshotAndDropsThemOnClear) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "sub" / "brick.png", "x");
  write_file(assets / "models" / "bench.glb", "x");

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));

  QStringList expected{QString::fromStdString(assets.string()),
                       QString::fromStdString((assets / "models").string()),
                       QString::fromStdString((assets / "textures").string()),
                       QString::fromStdString((assets / "textures" / "sub").string())};
  expected.sort();
  EXPECT_EQ(model.watched_directories(), expected);

  model.clear_root();
  EXPECT_TRUE(model.watched_directories().isEmpty());
  EXPECT_EQ(model.rowCount(), 0);
}

TEST(ProjectFilesModel, TheWatcherRefreshesTheTreeWithoutARestart) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.png", "x");

  ProjectFilesModel model;
  model.set_debounce_interval_ms(0);
  model.set_root(assets, fs_path(project.path()));
  ASSERT_EQ(model.rowCount(child_named(model, {}, QStringLiteral("textures"))), 1);

  QSignalSpy spy(&model, &ProjectFilesModel::refreshed);
  write_file(assets / "textures" / "asphalt.png", "x");
  // Filesystem notifications are asynchronous and their latency is the
  // platform's business (FSEvents on macOS, inotify on Linux) — wait for the
  // event, never for a duration. The deterministic content assertions live in
  // RefreshReflectsCreateRenameAndDelete.
  ASSERT_TRUE(spy.wait(15000)) << "no filesystem notification arrived";
  EXPECT_EQ(model.rowCount(child_named(model, {}, QStringLiteral("textures"))), 2);
}

TEST(ProjectFilesModel, ADeepTreeIsTruncatedRatherThanWalkedForever) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  std::filesystem::path deep = fs_path(project.path()) / "assets";
  for (int level = 0; level < ProjectFilesModel::kMaxDepth + 4; ++level) {
    deep /= "level";
  }
  write_file(deep / "brick.png", "x");

  ProjectFilesModel model;
  model.set_root(fs_path(project.path()) / "assets", fs_path(project.path()));
  EXPECT_TRUE(model.truncated());

  // Exactly kMaxDepth levels are present, and the scan stopped there.
  QModelIndex index = model.index(0, 0);
  int levels = 0;
  while (index.isValid()) {
    ++levels;
    index = model.index(0, 0, index);
  }
  EXPECT_EQ(levels, ProjectFilesModel::kMaxDepth);
}

TEST(ProjectFilesModel, ASymlinkPointingAtItsOwnAncestorDoesNotLoop) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.png", "x");

  std::error_code ec;
  std::filesystem::create_directory_symlink(assets, assets / "textures" / "loop", ec);
  if (ec) {
    GTEST_SKIP() << "this platform/user cannot create directory symlinks";
  }

  ProjectFilesModel model;
  model.set_root(assets, fs_path(project.path()));
  // The scan terminates; the loop entry is present but never re-entered.
  const QModelIndex textures = child_named(model, {}, QStringLiteral("textures"));
  ASSERT_TRUE(textures.isValid());
  const QModelIndex loop = child_named(model, textures, QStringLiteral("loop"));
  ASSERT_TRUE(loop.isValid());
  EXPECT_EQ(model.rowCount(loop), 0);
  EXPECT_FALSE(model.truncated());
}

} // namespace
} // namespace roadmaker::editor
