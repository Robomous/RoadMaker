#include "document/library_list_model.hpp"

namespace roadmaker::editor {

LibraryListModel::LibraryListModel(QObject* parent) : QAbstractListModel(parent) {}

void LibraryListModel::set_manifest(LibraryManifest manifest) {
  beginResetModel();
  items_ = manifest.items();
  endResetModel();
}

int LibraryListModel::rowCount(const QModelIndex& parent) const {
  // Flat list: children of a valid index have no rows (Qt model contract).
  return parent.isValid() ? 0 : static_cast<int>(items_.size());
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

const LibraryItem* LibraryListModel::item(int row) const {
  if (row < 0 || row >= static_cast<int>(items_.size())) {
    return nullptr;
  }
  return &items_[static_cast<std::size_t>(row)];
}

} // namespace roadmaker::editor
