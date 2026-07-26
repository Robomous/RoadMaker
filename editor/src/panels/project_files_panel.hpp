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

#pragma once

// The Library dock's lower half (p6-s7, #321): a live tree over the open
// project's `assets` folder. Thin view over ProjectFilesModel — a QTreeView, a
// section label, and an empty-state hint for a project that has no assets
// folder yet. Read-only by design: nothing here drags, imports, renames or
// deletes; turning a file into a material or a prop is p6-s8.

#include <QIdentityProxyModel>
#include <QStringList>
#include <QWidget>
#include <filesystem>

#include "document/project_files_model.hpp"

class QLabel;
class QTreeView;

namespace roadmaker::editor {

/// Supplies the per-type themed glyph when the model has no thumbnail — the
/// same model/view split LibraryFilterProxy uses for the catalogue grid, which
/// is what keeps `Icons` (an app-layer widget concern) out of `document/`.
class ProjectFilesProxy : public QIdentityProxyModel {
  Q_OBJECT

public:
  explicit ProjectFilesProxy(QObject* parent = nullptr);
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
};

class ProjectFilesPanel : public QWidget {
  Q_OBJECT

public:
  explicit ProjectFilesPanel(QWidget* parent = nullptr);

  /// Browses `project_dir`'s asset folder. The model watches the project
  /// directory too while `assets` does not exist, so creating it from the OS
  /// file manager fills the tree in without a restart.
  void set_project(const std::filesystem::path& project_dir);

  /// No project open — the tree empties and every watch is dropped. The Library
  /// hides the whole section.
  void clear_project();

  [[nodiscard]] QTreeView* tree() { return tree_; }

  [[nodiscard]] QLabel* hint() { return hint_; }

  [[nodiscard]] ProjectFilesModel& model() { return model_; }

private:
  /// Shows the tree or the "no assets folder yet" hint, whichever the root's
  /// existence calls for.
  void update_empty_state();

  /// Expansion and current row are captured BEFORE a reset and restored after:
  /// the model rebuilds wholesale on every filesystem event, and a tree that
  /// collapsed itself each time a file landed would be unusable.
  void remember_view_state();
  void restore_view_state();

  ProjectFilesModel model_;
  ProjectFilesProxy proxy_;
  QLabel* title_;
  QLabel* hint_;
  QTreeView* tree_;

  QStringList expanded_paths_;
  QString current_path_;
};

} // namespace roadmaker::editor
