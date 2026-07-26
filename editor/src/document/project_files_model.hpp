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

// Read-only tree model over a project's asset folder (p6-s7, #321) — the
// Library dock's file explorer. Mirrors the on-disk hierarchy under
// `<project>/assets` (Project::assets_dir) as a flat node snapshot rebuilt
// inside a model reset, exactly like SceneTreeModel: node indices in
// QModelIndex::internalId(), never pointers.
//
// The model is a READER. It never creates, moves, or deletes anything on disk
// — a project without an `assets` folder simply shows nothing, and the folder
// appearing later is picked up by the watcher. Turning a browsed file into a
// material or a prop is p6-s8.
//
// Live refresh is a QFileSystemWatcher over every directory of the snapshot
// (the repo's first — nothing here polls), debounced through a single-shot
// timer so a bulk copy or an unzip coalesces into one rebuild. Headless: Qt
// Core/Gui only, no widget, unit-testable offscreen. The image decode for
// thumbnails is deliberate; a per-type GLYPH fallback is the view's job (see
// ProjectFilesProxy), so `Icons` stays out of the document layer.

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace roadmaker::editor {

class ProjectFilesModel : public QAbstractItemModel {
  Q_OBJECT

public:
  /// What a row is, for the view's glyph fallback and for p6-s8's import
  /// affordances. Decided by the lower-cased file extension alone — the model
  /// never opens a file to classify it.
  enum class FileType : std::uint8_t {
    Directory,
    Image,    ///< png/jpg/jpeg/bmp/webp — the kinds that get a real thumbnail
    Scene,    ///< xodr
    Model,    ///< gltf/glb/obj
    Manifest, ///< json
    Other
  };

  enum Roles {
    PathRole = Qt::UserRole + 1, ///< absolute path (QString)
    RelativePathRole,            ///< path relative to root(), '/'-separated
    IsDirRole,                   ///< bool
    FileTypeRole,                ///< FileType, as an int
  };

  /// A tree deeper than this is not descended into. Beyond a handful of levels
  /// a project asset folder is a build output, not something to browse.
  static constexpr int kMaxDepth = 8;

  /// Hard ceiling on snapshot size. A scan that hits it stops and warns — a
  /// truncated tree must never be mistaken for a small one.
  static constexpr int kMaxNodes = 5000;

  /// Coalescing window for filesystem events, in milliseconds. Tests set 0.
  static constexpr int kDefaultDebounceMs = 150;

  explicit ProjectFilesModel(QObject* parent = nullptr);

  /// Points the model at `root` (typically `Project::assets_dir()`) and starts
  /// watching. `watch_fallback` — the project directory — is watched too while
  /// `root` does not exist yet, so the folder being created shows up without a
  /// restart. Passing an empty root is the same as clear_root().
  void set_root(std::filesystem::path root, std::filesystem::path watch_fallback = {});

  /// Drops the root and every watch (project closed). The model empties.
  void clear_root();

  /// Rescans the root from disk under a model reset and re-syncs the watches.
  /// The watcher calls this; tests call it directly for a deterministic result
  /// with no event loop involved.
  void refresh();

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  /// True when the root is set AND is a directory on disk right now. False
  /// drives the panel's "no assets folder yet" hint.
  [[nodiscard]] bool root_exists() const;

  /// True when the last scan stopped early on kMaxDepth/kMaxNodes — the tree
  /// on screen is not the whole folder.
  [[nodiscard]] bool truncated() const { return truncated_; }

  void set_debounce_interval_ms(int ms);

  /// The directories currently watched, sorted — the watcher-wiring assertion
  /// tests read this rather than racing the filesystem.
  [[nodiscard]] QStringList watched_directories() const;

  /// The index of the row whose absolute path is `path`, or an invalid index.
  /// The panel restores expansion and selection by path across a reset.
  [[nodiscard]] QModelIndex index_for_path(const QString& path) const;

  [[nodiscard]] QModelIndex
  index(int row, int column, const QModelIndex& parent = {}) const override;
  [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

signals:
  /// Emitted after every rescan (whether or not anything changed) — the panel
  /// restores its expansion state on it, tests wait on it.
  void refreshed();

private:
  struct Node {
    QString name;
    std::filesystem::path path;
    QString absolute; ///< cached QString form of `path`, the role payload
    QString relative; ///< path relative to root_
    int parent = -1;  ///< index into nodes_; -1 only for the hidden root
    int row = 0;      ///< row within parent
    std::vector<int> children;
    FileType type = FileType::Other;
  };

  /// Rebuilds nodes_ from disk. nodes_[0] is a hidden root whose children are
  /// the top-level rows.
  void rebuild();

  /// Depth-first scan of `dir` appending to nodes_ under `parent_node`.
  void scan(const std::filesystem::path& dir, int parent_node, int depth);

  /// addPaths/removePaths so the watcher matches the snapshot's directories.
  void sync_watches();

  [[nodiscard]] const Node* node_for(const QModelIndex& index) const;
  [[nodiscard]] QModelIndex index_for_node(int node) const;

  std::filesystem::path root_;
  std::filesystem::path watch_fallback_;
  std::vector<Node> nodes_;
  QHash<QString, int> nodes_by_path_;

  /// Canonical directories already visited by the running scan — a symlink
  /// pointing at an ancestor must not send the scan around forever.
  std::vector<std::filesystem::path> visited_;

  bool truncated_ = false;

  QFileSystemWatcher watcher_;
  QTimer debounce_;

  /// Lazy absolute-path -> QIcon cache for image thumbnails, INCLUDING the
  /// negative entry (a null QIcon for a file that failed to decode) so a
  /// corrupt or half-copied image is not re-read on every paint. Cleared on
  /// every rebuild. `mutable` because it is a pure read-through cache filled
  /// from the const data().
  mutable QHash<QString, QIcon> icon_cache_;
};

} // namespace roadmaker::editor
