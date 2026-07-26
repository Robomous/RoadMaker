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

#include "panels/project_files_panel.hpp"

#include <QLabel>
#include <QTreeView>
#include <QVBoxLayout>

#include "app/icons.hpp"

namespace roadmaker::editor {

namespace {

/// The bundled glyph for a row the model gave no thumbnail — a folder, a
/// non-image file, or an image that failed to decode.
QString glyph_for(ProjectFilesModel::FileType type) {
  switch (type) {
  case ProjectFilesModel::FileType::Directory:
    return QStringLiteral("folder");
  case ProjectFilesModel::FileType::Image:
    return QStringLiteral("image");
  case ProjectFilesModel::FileType::Scene:
    return QStringLiteral("clothoid-road");
  case ProjectFilesModel::FileType::Model:
    return QStringLiteral("box");
  case ProjectFilesModel::FileType::Manifest:
  case ProjectFilesModel::FileType::Other:
    break;
  }
  return QStringLiteral("file");
}

/// Appends the paths of every expanded row under `parent`, depth-first.
void collect_expanded(const QTreeView& tree, const QModelIndex& parent, QStringList& out) {
  const QAbstractItemModel* model = tree.model();
  for (int row = 0; row < model->rowCount(parent); ++row) {
    const QModelIndex index = model->index(row, 0, parent);
    if (!tree.isExpanded(index)) {
      continue;
    }
    out.append(model->data(index, ProjectFilesModel::PathRole).toString());
    collect_expanded(tree, index, out);
  }
}

} // namespace

ProjectFilesProxy::ProjectFilesProxy(QObject* parent) : QIdentityProxyModel(parent) {}

QVariant ProjectFilesProxy::data(const QModelIndex& index, int role) const {
  if (role == Qt::DecorationRole) {
    // Prefer the model's decoded thumbnail; only a row without one falls back
    // to a themed glyph, which Icons::get retints on a palette change.
    const QVariant thumbnail = QIdentityProxyModel::data(index, Qt::DecorationRole);
    if (thumbnail.isValid()) {
      return thumbnail;
    }
    const auto type = static_cast<ProjectFilesModel::FileType>(
        QIdentityProxyModel::data(index, ProjectFilesModel::FileTypeRole).toInt());
    return Icons::get(glyph_for(type));
  }
  return QIdentityProxyModel::data(index, role);
}

ProjectFilesPanel::ProjectFilesPanel(QWidget* parent) : QWidget(parent) {
  proxy_.setSourceModel(&model_);

  title_ = new QLabel(tr("Project files"), this);
  title_->setObjectName(QStringLiteral("project_files_title"));

  hint_ = new QLabel(this);
  hint_->setObjectName(QStringLiteral("project_files_hint"));
  hint_->setWordWrap(true);
  hint_->setTextInteractionFlags(Qt::TextSelectableByMouse); // the path is meant to be copied
  hint_->hide();

  tree_ = new QTreeView(this);
  tree_->setObjectName(QStringLiteral("project_files_tree"));
  tree_->setModel(&proxy_);
  tree_->setHeaderHidden(true);
  tree_->setUniformRowHeights(true);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  // Read-only browser: no drag source this sprint. Importing a browsed file is
  // p6-s8, and a drag that produced nothing would be a promise the editor
  // cannot keep.
  tree_->setDragEnabled(false);
  tree_->setIconSize(QSize(20, 20));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  layout->addWidget(title_);
  layout->addWidget(hint_);
  layout->addWidget(tree_, 1);

  connect(
      &model_, &QAbstractItemModel::modelAboutToBeReset, this, [this] { remember_view_state(); });
  connect(&model_, &ProjectFilesModel::refreshed, this, [this] {
    update_empty_state();
    restore_view_state();
  });
  update_empty_state();
}

void ProjectFilesPanel::set_project(const std::filesystem::path& project_dir) {
  // Remembered paths are absolute, so another project's simply fail to resolve
  // and re-opening THIS one comes back with its folders still expanded.
  model_.set_root(project_dir / "assets", project_dir);
}

void ProjectFilesPanel::clear_project() {
  model_.clear_root();
  expanded_paths_.clear();
  current_path_.clear();
}

void ProjectFilesPanel::update_empty_state() {
  const bool has_root = !model_.root().empty();
  const bool exists = model_.root_exists();
  tree_->setVisible(has_root && exists);
  hint_->setVisible(has_root && !exists);
  if (has_root && !exists) {
    hint_->setText(tr("No assets folder yet. Create %1 and its contents appear here.")
                       .arg(QString::fromStdString(model_.root().string())));
  }
}

void ProjectFilesPanel::remember_view_state() {
  // A reset invalidates every index, so the state is kept as paths.
  QStringList expanded;
  collect_expanded(*tree_, QModelIndex(), expanded);
  expanded_paths_ = expanded;
  const QModelIndex current = tree_->currentIndex();
  current_path_ =
      current.isValid() ? proxy_.data(current, ProjectFilesModel::PathRole).toString() : QString();
}

void ProjectFilesPanel::restore_view_state() {
  for (const QString& path : expanded_paths_) {
    const QModelIndex index = proxy_.mapFromSource(model_.index_for_path(path));
    if (index.isValid()) {
      tree_->expand(index);
    }
  }
  if (!current_path_.isEmpty()) {
    const QModelIndex index = proxy_.mapFromSource(model_.index_for_path(current_path_));
    if (index.isValid()) {
      tree_->setCurrentIndex(index);
    }
  }
}

} // namespace roadmaker::editor
