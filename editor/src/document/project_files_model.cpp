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

#include "document/project_files_model.hpp"

#include <spdlog/spdlog.h>

#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <algorithm>
#include <system_error>

namespace roadmaker::editor {

namespace {

constexpr quintptr kNoNode = static_cast<quintptr>(-1);

/// Thumbnail edge in device-independent pixels — a tree row, not the icon grid.
constexpr int kThumbnailEdge = 32;

QString qpath(const std::filesystem::path& path) {
  return QString::fromStdString(path.string());
}

ProjectFilesModel::FileType type_for(const std::filesystem::path& path) {
  const QString suffix = QString::fromStdString(path.extension().string()).toLower();
  if (suffix == QStringLiteral(".png") || suffix == QStringLiteral(".jpg") ||
      suffix == QStringLiteral(".jpeg") || suffix == QStringLiteral(".bmp") ||
      suffix == QStringLiteral(".webp")) {
    return ProjectFilesModel::FileType::Image;
  }
  if (suffix == QStringLiteral(".xodr")) {
    return ProjectFilesModel::FileType::Scene;
  }
  if (suffix == QStringLiteral(".gltf") || suffix == QStringLiteral(".glb") ||
      suffix == QStringLiteral(".obj")) {
    return ProjectFilesModel::FileType::Model;
  }
  if (suffix == QStringLiteral(".json")) {
    return ProjectFilesModel::FileType::Manifest;
  }
  return ProjectFilesModel::FileType::Other;
}

/// Decodes `path` to a thumbnail, or a null icon when it is not a readable
/// image. QImageReader::setScaledSize decodes AT the target size, so a 4K
/// texture never costs a full-resolution QImage for a 32 px row.
QIcon load_thumbnail(const QString& path) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QSize source = reader.size();
  if (source.isValid() && !source.isEmpty()) {
    const QSize target = source.scaled(kThumbnailEdge, kThumbnailEdge, Qt::KeepAspectRatio);
    if (!target.isEmpty()) {
      reader.setScaledSize(target);
    }
  }
  const QImage image = reader.read();
  if (image.isNull()) {
    return {};
  }
  return QIcon(QPixmap::fromImage(image));
}

} // namespace

ProjectFilesModel::ProjectFilesModel(QObject* parent) : QAbstractItemModel(parent) {
  nodes_.push_back(Node{}); // the hidden root; its children are the top-level rows

  debounce_.setSingleShot(true);
  debounce_.setInterval(kDefaultDebounceMs);
  connect(&debounce_, &QTimer::timeout, this, &ProjectFilesModel::refresh);
  // Directory granularity only: create/delete/rename inside a watched folder is
  // exactly what a directoryChanged reports, and one watch per FILE would run
  // into the platform watch limits on any real asset folder.
  connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
    debounce_.start();
  });
}

void ProjectFilesModel::set_root(std::filesystem::path root, std::filesystem::path watch_fallback) {
  if (root.empty()) {
    clear_root();
    return;
  }
  root_ = std::move(root);
  watch_fallback_ = std::move(watch_fallback);
  refresh();
}

void ProjectFilesModel::clear_root() {
  root_.clear();
  watch_fallback_.clear();
  refresh();
}

void ProjectFilesModel::set_debounce_interval_ms(int ms) {
  debounce_.setInterval(std::max(0, ms));
}

bool ProjectFilesModel::root_exists() const {
  if (root_.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_directory(root_, ec);
}

void ProjectFilesModel::refresh() {
  beginResetModel();
  rebuild();
  endResetModel();
  sync_watches();
  emit refreshed();
}

void ProjectFilesModel::rebuild() {
  nodes_.clear();
  nodes_by_path_.clear();
  visited_.clear();
  icon_cache_.clear();
  truncated_ = false;
  nodes_.push_back(Node{});

  if (!root_exists()) {
    return;
  }
  scan(root_, 0, 0);
  if (truncated_) {
    spdlog::warn("project files: the tree under {} was truncated at {} entries / depth {} — "
                 "what the Library shows is not the whole folder",
                 root_.string(),
                 kMaxNodes,
                 kMaxDepth);
  }
}

void ProjectFilesModel::scan(const std::filesystem::path& dir, int parent_node, int depth) {
  if (depth >= kMaxDepth) {
    truncated_ = true;
    return;
  }
  // A symlink pointing at one of its own ancestors would otherwise walk
  // forever; remember every directory this scan has already entered.
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
  const std::filesystem::path& marker = ec ? dir : canonical;
  if (std::find(visited_.begin(), visited_.end(), marker) != visited_.end()) {
    return;
  }
  visited_.push_back(marker);

  std::vector<std::filesystem::directory_entry> entries;
  std::filesystem::directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return; // unreadable directory: show it empty rather than failing the scan
  }
  for (const std::filesystem::directory_entry& entry : it) {
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.front() == '.') {
      continue; // dotfiles are tooling, not assets
    }
    entries.push_back(entry);
  }

  // Directories first, then files; each by name, case-insensitively — what
  // every file manager does, and what Project::scenes() aims at with QDir::Name.
  std::sort(
      entries.begin(),
      entries.end(),
      [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b) {
        std::error_code dir_ec;
        const bool a_dir = a.is_directory(dir_ec);
        const bool b_dir = b.is_directory(dir_ec);
        if (a_dir != b_dir) {
          return a_dir;
        }
        const QString a_name = QString::fromStdString(a.path().filename().string());
        const QString b_name = QString::fromStdString(b.path().filename().string());
        const int cased = QString::compare(a_name, b_name, Qt::CaseInsensitive);
        // Tie-break case-sensitively so "Tree.png" and "tree.png" get a
        // stable, reproducible order instead of whatever the FS returned.
        return cased != 0 ? cased < 0 : a_name < b_name;
      });

  for (const std::filesystem::directory_entry& entry : entries) {
    if (static_cast<int>(nodes_.size()) >= kMaxNodes) {
      truncated_ = true;
      return;
    }
    std::error_code entry_ec;
    const bool is_dir = entry.is_directory(entry_ec);
    if (entry_ec) {
      continue; // vanished between the listing and the stat — a normal race
    }
    const std::filesystem::path relative = std::filesystem::relative(entry.path(), root_, entry_ec);
    const int node = static_cast<int>(nodes_.size());
    nodes_.push_back(
        Node{.name = QString::fromStdString(entry.path().filename().string()),
             .path = entry.path(),
             .absolute = qpath(entry.path()),
             .relative = entry_ec ? qpath(entry.path().filename())
                                  : QString::fromStdString(relative.generic_string()),
             .parent = parent_node,
             .row = static_cast<int>(nodes_[static_cast<std::size_t>(parent_node)].children.size()),
             .type = is_dir ? FileType::Directory : type_for(entry.path())});
    nodes_[static_cast<std::size_t>(parent_node)].children.push_back(node);
    nodes_by_path_.insert(nodes_[static_cast<std::size_t>(node)].absolute, node);
    if (is_dir) {
      scan(entry.path(), node, depth + 1);
    }
  }
}

void ProjectFilesModel::sync_watches() {
  QStringList desired;
  if (root_exists()) {
    desired.append(qpath(root_));
    for (const Node& node : nodes_) {
      if (node.type == FileType::Directory) {
        desired.append(node.absolute);
      }
    }
  } else if (!watch_fallback_.empty()) {
    // The assets folder does not exist yet: watch the project directory so its
    // creation lands in the tree without a restart.
    std::error_code ec;
    if (std::filesystem::is_directory(watch_fallback_, ec)) {
      desired.append(qpath(watch_fallback_));
    }
  }

  const QStringList current = watcher_.directories();
  QStringList stale;
  for (const QString& path : current) {
    if (!desired.contains(path)) {
      stale.append(path);
    }
  }
  QStringList added;
  for (const QString& path : desired) {
    if (!current.contains(path)) {
      added.append(path);
    }
  }
  if (!stale.isEmpty()) {
    watcher_.removePaths(stale);
  }
  if (!added.isEmpty()) {
    watcher_.addPaths(added);
  }
}

QStringList ProjectFilesModel::watched_directories() const {
  QStringList paths = watcher_.directories();
  paths.sort();
  return paths;
}

const ProjectFilesModel::Node* ProjectFilesModel::node_for(const QModelIndex& index) const {
  if (!index.isValid() || index.internalId() == kNoNode) {
    return nullptr;
  }
  const auto node = static_cast<std::size_t>(index.internalId());
  return node < nodes_.size() ? &nodes_[node] : nullptr;
}

QModelIndex ProjectFilesModel::index_for_node(int node) const {
  if (node <= 0 || static_cast<std::size_t>(node) >= nodes_.size()) {
    return {}; // node 0 is the hidden root — its index is the invalid one
  }
  return createIndex(nodes_[static_cast<std::size_t>(node)].row, 0, static_cast<quintptr>(node));
}

QModelIndex ProjectFilesModel::index_for_path(const QString& path) const {
  const auto it = nodes_by_path_.constFind(path);
  return it == nodes_by_path_.constEnd() ? QModelIndex{} : index_for_node(it.value());
}

QModelIndex ProjectFilesModel::index(int row, int column, const QModelIndex& parent) const {
  if (!hasIndex(row, column, parent)) {
    return {};
  }
  const Node* parent_node = parent.isValid() ? node_for(parent) : &nodes_[0];
  if (parent_node == nullptr || row >= static_cast<int>(parent_node->children.size())) {
    return {};
  }
  return createIndex(
      row, column, static_cast<quintptr>(parent_node->children[static_cast<std::size_t>(row)]));
}

QModelIndex ProjectFilesModel::parent(const QModelIndex& child) const {
  const Node* node = node_for(child);
  if (node == nullptr || node->parent <= 0) {
    return {};
  }
  return index_for_node(node->parent);
}

int ProjectFilesModel::rowCount(const QModelIndex& parent) const {
  if (parent.column() > 0) {
    return 0;
  }
  const Node* node = parent.isValid() ? node_for(parent) : &nodes_[0];
  return node == nullptr ? 0 : static_cast<int>(node->children.size());
}

int ProjectFilesModel::columnCount(const QModelIndex& /*parent*/) const {
  return 1;
}

QVariant ProjectFilesModel::data(const QModelIndex& index, int role) const {
  const Node* node = node_for(index);
  if (node == nullptr || index.column() != 0) {
    return {};
  }
  switch (role) {
  case Qt::DisplayRole:
    return node->name;
  case Qt::ToolTipRole:
    return node->relative;
  case Qt::DecorationRole: {
    // Only real images produce a picture here. Everything else returns an
    // INVALID variant on purpose: the per-type glyph is the view's job
    // (ProjectFilesProxy), which keeps the themed Icons out of document/.
    if (node->type != FileType::Image) {
      return {};
    }
    auto it = icon_cache_.constFind(node->absolute);
    if (it == icon_cache_.constEnd()) {
      it = icon_cache_.insert(node->absolute, load_thumbnail(node->absolute));
    }
    // A negative cache entry (null icon) must read as "no thumbnail", not as a
    // blank one — otherwise the proxy cannot fall back to a glyph.
    return it.value().isNull() ? QVariant{} : QVariant::fromValue(it.value());
  }
  case PathRole:
    return node->absolute;
  case RelativePathRole:
    return node->relative;
  case IsDirRole:
    return node->type == FileType::Directory;
  case FileTypeRole:
    return static_cast<int>(node->type);
  default:
    return {};
  }
}

QHash<int, QByteArray> ProjectFilesModel::roleNames() const {
  QHash<int, QByteArray> names = QAbstractItemModel::roleNames();
  names.insert(PathRole, "path");
  names.insert(RelativePathRole, "relativePath");
  names.insert(IsDirRole, "isDir");
  names.insert(FileTypeRole, "fileType");
  return names;
}

} // namespace roadmaker::editor
