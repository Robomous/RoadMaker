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

#include "document/library_list_model.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QSize>
#include <QStringList>
#include <algorithm>

#include "document/crosswalk_item.hpp"
#include "document/stencil_item.hpp"

namespace roadmaker::editor {

namespace {

/// A built-in item's bundled thumbnail lives in the qresource under
/// `:/library/thumbnails/<basename>`; the manifest stores a repo-relative path
/// (e.g. `assets/library/thumbnails/road_rural.png`) whose basename picks the
/// alias. Empty in → empty out (no thumbnail).
QString resolve_builtin_thumbnail(const QString& manifest_path) {
  if (manifest_path.isEmpty()) {
    return {};
  }
  return QStringLiteral(":/library/thumbnails/") + QFileInfo(manifest_path).fileName();
}

/// A project overlay's thumbnail path is resolved on disk, relative to the
/// project directory. Empty path or no project dir → unresolved (empty).
QString resolve_overlay_thumbnail(const QString& manifest_path, const QString& base_dir) {
  if (manifest_path.isEmpty() || base_dir.isEmpty()) {
    return {};
  }
  return QDir(base_dir).filePath(manifest_path);
}

} // namespace

LibraryListModel::LibraryListModel(QObject* parent) : QAbstractListModel(parent) {}

void LibraryListModel::set_manifest(LibraryManifest manifest) {
  base_items_ = manifest.items();
  for (LibraryItem& item : base_items_) {
    item.thumbnail = resolve_builtin_thumbnail(item.thumbnail);
  }
  rebuild();
}

void LibraryListModel::set_overlay(LibraryManifest manifest, const QString& base_dir) {
  overlay_items_ = manifest.items();
  for (LibraryItem& item : overlay_items_) {
    item.thumbnail = resolve_overlay_thumbnail(item.thumbnail, base_dir);
  }
  rebuild();
}

void LibraryListModel::clear_overlay() {
  if (overlay_items_.empty()) {
    return; // avoid a spurious full reset when nothing was overlaid
  }
  overlay_items_.clear();
  rebuild();
}

void LibraryListModel::rebuild() {
  beginResetModel();
  icon_cache_.clear();           // resolved paths may change; drop stale (incl. negative) icons
  crosswalk_icon_cache_.clear(); // params may change; re-render edited asset previews
  stencil_icon_cache_.clear();   // glyph params may change; re-render edited previews
  items_ = base_items_;
  for (const LibraryItem& overlay : overlay_items_) {
    const auto match =
        std::find_if(items_.begin(), items_.end(), [&overlay](const LibraryItem& existing) {
          return existing.key == overlay.key;
        });
    if (match != items_.end()) {
      *match = overlay; // project wins on key collision; the row keeps its place
    } else {
      items_.push_back(overlay); // new keys (and categories) append
    }
  }
  endResetModel();
}

int LibraryListModel::rowCount(const QModelIndex& parent) const {
  // Flat list: children of a valid index have no rows (Qt model contract).
  return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

bool LibraryListModel::has_overlay_item(const QString& key) const {
  return std::any_of(overlay_items_.begin(), overlay_items_.end(), [&](const LibraryItem& item) {
    return item.key == key;
  });
}

QVariant LibraryListModel::data(const QModelIndex& index, int role) const {
  const LibraryItem* entry = item(index.row());
  if (entry == nullptr || index.column() != 0) {
    return {};
  }
  switch (role) {
  case Qt::DisplayRole:
  case Qt::ToolTipRole:
    return entry->label;
  case Qt::DecorationRole: {
    // Crosswalk assets have no PNG thumbnail — paint their preview at runtime
    // from the stripe/material params, cached by key (cleared on rebuild()).
    if (entry->kind == LibraryItem::Kind::Crosswalk) {
      auto it = crosswalk_icon_cache_.constFind(entry->key);
      if (it == crosswalk_icon_cache_.constEnd()) {
        it = crosswalk_icon_cache_.insert(
            entry->key, QIcon(render_crosswalk_preview(*entry, QSize(64, 48), materials_)));
      }
      return it.value();
    }
    // Stencil (arrow) assets likewise carry no PNG — paint the glyph at runtime.
    if (entry->kind == LibraryItem::Kind::Stencil) {
      auto it = stencil_icon_cache_.constFind(entry->key);
      if (it == stencil_icon_cache_.constEnd()) {
        it = stencil_icon_cache_.insert(
            entry->key, QIcon(render_stencil_preview(*entry, QSize(64, 48), materials_)));
      }
      return it.value();
    }
    // The resolved thumbnail as a QIcon, read through a lazy cache (a null icon
    // for a path that failed to load is cached too, so a missing thumbnail is
    // not re-probed each paint). An item with no thumbnail returns a null
    // variant; the panel's proxy falls back to a themed glyph there.
    if (entry->thumbnail.isEmpty()) {
      return {};
    }
    auto it = icon_cache_.constFind(entry->thumbnail);
    if (it == icon_cache_.constEnd()) {
      it = icon_cache_.insert(entry->thumbnail, QIcon(entry->thumbnail));
    }
    if (it.value().isNull()) {
      return {};
    }
    return it.value();
  }
  case KeyRole:
    return entry->key;
  case CategoryRole:
    return entry->category;
  case ThumbnailRole:
    return entry->thumbnail;
  default:
    return {};
  }
}

QHash<int, QByteArray> LibraryListModel::roleNames() const {
  QHash<int, QByteArray> names = QAbstractListModel::roleNames();
  names[KeyRole] = "key";
  names[CategoryRole] = "category";
  names[ThumbnailRole] = "thumbnail";
  return names;
}

Qt::ItemFlags LibraryListModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags base = QAbstractListModel::flags(index);
  if (index.isValid()) {
    base |= Qt::ItemIsDragEnabled;
  }
  return base;
}

QStringList LibraryListModel::mimeTypes() const {
  return {QString::fromLatin1(kLibraryItemMimeType)};
}

QMimeData* LibraryListModel::mimeData(const QModelIndexList& indexes) const {
  for (const QModelIndex& index : indexes) {
    if (const LibraryItem* entry = item(index.row())) {
      auto* mime = new QMimeData;
      mime->setData(QString::fromLatin1(kLibraryItemMimeType), entry->key.toUtf8());
      return mime; // one item per drag
    }
  }
  return nullptr;
}

const LibraryItem* LibraryListModel::item_for_key(const QString& key) const {
  for (const LibraryItem& entry : items_) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

double LibraryListModel::default_scale_for_model(const QString& model) const {
  // items_ is already base+overlay merged with the overlay winning on key
  // collision, so the first Kind::Tree match reflects the project's shadowing.
  for (const LibraryItem& entry : items_) {
    if (entry.kind == LibraryItem::Kind::Tree && entry.model == model) {
      return entry.default_scale;
    }
  }
  return 1.0;
}

const LibraryItem* LibraryListModel::item(int row) const {
  if (row < 0 || row >= static_cast<int>(items_.size())) {
    return nullptr;
  }
  return &items_[static_cast<std::size_t>(row)];
}

} // namespace roadmaker::editor
