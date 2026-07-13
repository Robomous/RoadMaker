#pragma once

// Flat item model over a LibraryManifest for the Library panel's icon grid.
// Read-only: one row per catalogue item, exposing the label, thumbnail path,
// stable key, and grouping category through roles. The panel (a QListView in
// icon mode) and a category proxy consume it; the drop handler resolves the
// dragged key back to its LibraryItem via item().

#include <QAbstractListModel>

#include "document/library_manifest.hpp"

namespace roadmaker::editor {

/// MIME type of a dragged library item; the payload is the item's `key`
/// (UTF-8). The panel is the drag source, the viewport the drop target.
inline constexpr char kLibraryItemMimeType[] = "application/x-roadmaker-library-item";

class LibraryListModel : public QAbstractListModel {
  Q_OBJECT

public:
  enum Roles {
    KeyRole = Qt::UserRole + 1, ///< stable id (QString) — the drag payload
    CategoryRole,               ///< grouping header (QString)
    ThumbnailRole,              ///< manifest-relative image path (QString)
  };

  explicit LibraryListModel(QObject* parent = nullptr);

  /// Replaces the catalogue (full model reset). The panel calls this once the
  /// manifest is loaded; passing an empty manifest clears the model.
  void set_manifest(LibraryManifest manifest);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // Drag source: one dragged item carries its key as kLibraryItemMimeType.
  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
  [[nodiscard]] QStringList mimeTypes() const override;
  [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;

  /// The item at `row`, or nullptr when out of range — the drop handler's
  /// key→item lookup goes through here.
  [[nodiscard]] const LibraryItem* item(int row) const;

  /// Lookup by stable key (the drop handler resolves the dragged key). Null
  /// when unknown.
  [[nodiscard]] const LibraryItem* item_for_key(const QString& key) const;

private:
  std::vector<LibraryItem> items_;
};

} // namespace roadmaker::editor
