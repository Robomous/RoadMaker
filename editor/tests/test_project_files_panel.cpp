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

// ProjectFilesPanel and the Library dock's splitter (p6-s7, #321): the file
// explorer's empty states, the glyph fallback, the view state that must
// survive a live refresh, and the proof that the catalogue half is untouched.

#include <gtest/gtest.h>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTreeView>
#include <filesystem>
#include <fstream>

#include "document/library_manifest.hpp"
#include "panels/library_panel.hpp"
#include "panels/project_files_panel.hpp"

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

LibraryListModel& catalogue_model() {
  static LibraryListModel model;
  const auto manifest =
      LibraryManifest::load(std::filesystem::path(RM_ASSETS_DIR) / "library" / "manifest.json");
  if (manifest.has_value()) {
    model.set_manifest(*manifest);
  }
  return model;
}

QModelIndex
row_named(const QAbstractItemModel& model, const QModelIndex& parent, const QString& name) {
  for (int row = 0; row < model.rowCount(parent); ++row) {
    const QModelIndex index = model.index(row, 0, parent);
    if (model.data(index, Qt::DisplayRole).toString() == name) {
      return index;
    }
  }
  return {};
}

TEST(ProjectFilesPanel, WithNoProjectThereIsNothingToBrowse) {
  ProjectFilesPanel panel;
  EXPECT_FALSE(panel.tree()->isVisibleTo(&panel));
  EXPECT_FALSE(panel.hint()->isVisibleTo(&panel));
  EXPECT_EQ(panel.model().rowCount(), 0);
}

TEST(ProjectFilesPanel, AProjectWithoutAnAssetsFolderNamesThePathToCreate) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());

  ProjectFilesPanel panel;
  panel.set_project(fs_path(project.path()));

  EXPECT_FALSE(panel.tree()->isVisibleTo(&panel));
  ASSERT_TRUE(panel.hint()->isVisibleTo(&panel));
  // The hint has to be actionable — it names the exact folder to create.
  EXPECT_TRUE(panel.hint()->text().contains(
      QString::fromStdString((fs_path(project.path()) / "assets").string())));

  // Creating it is picked up on the next scan, with no restart and nothing
  // written by the editor.
  write_file(fs_path(project.path()) / "assets" / "brick.png", "x");
  panel.model().refresh();
  EXPECT_TRUE(panel.tree()->isVisibleTo(&panel));
  EXPECT_FALSE(panel.hint()->isVisibleTo(&panel));
  EXPECT_EQ(panel.model().rowCount(), 1);
}

TEST(ProjectFilesPanel, ClosingTheProjectEmptiesTheTree) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  write_file(fs_path(project.path()) / "assets" / "brick.png", "x");

  ProjectFilesPanel panel;
  panel.set_project(fs_path(project.path()));
  ASSERT_EQ(panel.model().rowCount(), 1);

  panel.clear_project();
  EXPECT_EQ(panel.model().rowCount(), 0);
  EXPECT_TRUE(panel.model().watched_directories().isEmpty());
  EXPECT_FALSE(panel.tree()->isVisibleTo(&panel));
  EXPECT_FALSE(panel.hint()->isVisibleTo(&panel));
}

TEST(ProjectFilesPanel, EveryRowGetsAnIconEvenWithoutAThumbnail) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "models" / "bench.glb", "x");
  write_file(assets / "notes.txt", "x");

  ProjectFilesPanel panel;
  panel.set_project(fs_path(project.path()));

  // The model hands out no decoration for non-images; the proxy the view sees
  // fills in a themed glyph so no row renders blank.
  const QAbstractItemModel* view_model = panel.tree()->model();
  ASSERT_NE(view_model, nullptr);
  const QModelIndex models = row_named(*view_model, {}, QStringLiteral("models"));
  ASSERT_TRUE(models.isValid());
  EXPECT_FALSE(view_model->data(models, Qt::DecorationRole).value<QIcon>().isNull());
  EXPECT_FALSE(panel.model().data(panel.model().index(0, 0), Qt::DecorationRole).isValid());

  const QModelIndex notes = row_named(*view_model, {}, QStringLiteral("notes.txt"));
  ASSERT_TRUE(notes.isValid());
  EXPECT_FALSE(view_model->data(notes, Qt::DecorationRole).value<QIcon>().isNull());
}

TEST(ProjectFilesPanel, ExpansionAndSelectionSurviveALiveRefresh) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  const std::filesystem::path assets = fs_path(project.path()) / "assets";
  write_file(assets / "textures" / "brick.png", "x");

  ProjectFilesPanel panel;
  panel.set_project(fs_path(project.path()));

  QAbstractItemModel* view_model = panel.tree()->model();
  const QModelIndex textures = row_named(*view_model, {}, QStringLiteral("textures"));
  ASSERT_TRUE(textures.isValid());
  panel.tree()->expand(textures);
  const QModelIndex brick = row_named(*view_model, textures, QStringLiteral("brick.png"));
  ASSERT_TRUE(brick.isValid());
  panel.tree()->setCurrentIndex(brick);

  // Every filesystem event rebuilds the snapshot wholesale. A tree that
  // collapsed itself each time a file landed would be unusable while copying
  // assets in, which is exactly when the explorer is being watched.
  write_file(assets / "textures" / "asphalt.png", "x");
  panel.model().refresh();

  const QModelIndex textures_again = row_named(*view_model, {}, QStringLiteral("textures"));
  ASSERT_TRUE(textures_again.isValid());
  EXPECT_TRUE(panel.tree()->isExpanded(textures_again));
  EXPECT_EQ(panel.tree()->currentIndex().data(ProjectFilesModel::PathRole).toString(),
            QString::fromStdString((assets / "textures" / "brick.png").string()));
}

TEST(LibraryPanel, KeepsTheCatalogueGridAndGainsTheFilesPane) {
  LibraryPanel panel(catalogue_model());

  auto* splitter = panel.findChild<QSplitter*>(QStringLiteral("library_splitter"));
  ASSERT_NE(splitter, nullptr);
  EXPECT_EQ(splitter->orientation(), Qt::Vertical);
  EXPECT_EQ(splitter->count(), 2);

  // The catalogue half is untouched: same grid, same search box, same combo.
  ASSERT_NE(panel.view()->model(), nullptr);
  EXPECT_EQ(panel.view()->model()->rowCount(), 59);
  EXPECT_NE(panel.findChild<QLineEdit*>(QStringLiteral("library_search")), nullptr);
  EXPECT_NE(panel.category_combo(), nullptr);

  // No project yet — the file explorer stays out of the way entirely.
  ASSERT_NE(panel.files(), nullptr);
  EXPECT_FALSE(panel.files()->isVisibleTo(&panel));
}

TEST(LibraryPanel, ProjectFilesFollowTheOpenProject) {
  QTemporaryDir project;
  ASSERT_TRUE(project.isValid());
  write_file(fs_path(project.path()) / "assets" / "textures" / "brick.png", "x");

  LibraryPanel panel(catalogue_model());
  panel.set_project(fs_path(project.path()));
  EXPECT_TRUE(panel.files()->isVisibleTo(&panel));
  EXPECT_EQ(panel.files()->model().rowCount(), 1);

  panel.clear_project();
  EXPECT_FALSE(panel.files()->isVisibleTo(&panel));
  EXPECT_EQ(panel.files()->model().rowCount(), 0);
}

TEST(LibraryPanel, SplitterGeometryRoundTrips) {
  LibraryPanel panel(catalogue_model());
  auto* splitter = panel.findChild<QSplitter*>(QStringLiteral("library_splitter"));
  ASSERT_NE(splitter, nullptr);
  splitter->setSizes({300, 100});
  const QByteArray state = panel.splitter_state();
  ASSERT_FALSE(state.isEmpty());

  LibraryPanel restored(catalogue_model());
  restored.restore_splitter_state(state);
  EXPECT_EQ(restored.splitter_state(), state);
  // An empty state (first run) must leave the default balance alone rather
  // than collapsing a pane.
  const QByteArray before = restored.splitter_state();
  restored.restore_splitter_state({});
  EXPECT_EQ(restored.splitter_state(), before);
}

// The category combo was filled once, in the constructor. A project overlay is
// adopted AFTER the panel exists, so any category only the project defined was
// unreachable by filter.
TEST(LibraryPanel, TheCategoryComboLearnsCategoriesAnOverlayIntroduces) {
  LibraryListModel model;
  const auto base = LibraryManifest::parse(QByteArrayLiteral(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine"}}
    ]
  })"));
  ASSERT_TRUE(base.has_value());
  model.set_manifest(*base);

  LibraryPanel panel(model);
  ASSERT_EQ(panel.category_combo()->findData(QStringLiteral("Project assets")), -1);

  const auto overlay = LibraryManifest::parse(QByteArrayLiteral(R"({
    "manifest_version": 1,
    "items": [
      {"key": "project.special", "label": "Special", "category": "Project assets",
       "create": {"kind": "tree", "model": "tree_oak"}}
    ]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay);

  const int added = panel.category_combo()->findData(QStringLiteral("Project assets"));
  ASSERT_GE(added, 0);
  panel.category_combo()->setCurrentIndex(added);
  EXPECT_EQ(panel.view()->model()->rowCount(), 1);

  // A filter on a category that leaves with its project must not survive as a
  // filter that hides everything.
  model.clear_overlay();
  EXPECT_EQ(panel.category_combo()->findData(QStringLiteral("Project assets")), -1);
  EXPECT_EQ(panel.view()->model()->rowCount(), 1); // the built-in Pine, unfiltered
}

} // namespace
} // namespace roadmaker::editor
