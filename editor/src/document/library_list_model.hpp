#pragma once

// Flat item model over a LibraryManifest for the Library panel's icon grid.
// Read-only: one row per catalogue item, exposing the label, thumbnail path,
// stable key, and grouping category through roles. The panel (a QListView in
// icon mode) and a category proxy consume it; the drop handler resolves the
// dragged key back to its LibraryItem via item().

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QString>

#include "document/library_manifest.hpp"
#include "render/material_catalog.hpp"

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
    ThumbnailRole,              ///< RESOLVED image path (qrc for built-ins, absolute
                                ///< for overlay items); empty when none on disk
  };

  explicit LibraryListModel(QObject* parent = nullptr);

  /// Replaces the built-in catalogue (full model reset). The panel calls this
  /// once the manifest is loaded; passing an empty manifest clears the base
  /// items. Any project overlay in effect is re-merged on top.
  void set_manifest(LibraryManifest manifest);

  /// Applies a per-project overlay on top of the built-in catalogue (p6-s1):
  /// an overlay item whose key matches a built-in item REPLACES it in place
  /// (the project wins, the row keeps its position); keys the base doesn't
  /// have are appended, so new project categories appear in the panel. Full
  /// model reset. `base_dir` is the project directory a project-relative
  /// overlay thumbnail path resolves against (p6-s2); empty leaves overlay
  /// thumbnails unresolved.
  void set_overlay(LibraryManifest manifest, const QString& base_dir = {});

  /// Removes the project overlay (project close/switch) — the model returns
  /// to the built-in catalogue alone. No-op when no overlay is set.
  void clear_overlay();

  /// True while a project overlay is merged into the catalogue.
  [[nodiscard]] bool has_overlay() const { return !overlay_items_.empty(); }

  /// True when the project overlay defines an item with `key` — i.e. the item
  /// is project-owned and editable, not a read-only built-in (p3-s2 asset
  /// editor).
  [[nodiscard]] bool has_overlay_item(const QString& key) const;

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
  /// Rebuilds items_ = base + overlay (overlay wins on key collision) under a
  /// model reset.
  void rebuild();

  std::vector<LibraryItem> base_items_;
  std::vector<LibraryItem> overlay_items_;
  std::vector<LibraryItem> items_; ///< the merged rows the view sees

  /// Lazy path -> QIcon cache for DecorationRole, including a NEGATIVE cache (a
  /// null QIcon for a path that failed to load) so a missing thumbnail is not
  /// re-probed every paint. Cleared on every rebuild(). `mutable` because it is
  /// a pure read-through cache populated from the const data().
  mutable QHash<QString, QIcon> icon_cache_;

  /// Crosswalk assets carry no PNG thumbnail — their DecorationRole icon is
  /// painted at runtime from the item's stripe/material params (crosswalk_item).
  /// Cached by key; cleared on rebuild() so an edited asset re-renders.
  mutable QHash<QString, QIcon> crosswalk_icon_cache_;
  mutable QHash<QString, QIcon> stencil_icon_cache_;

  /// Resolves a crosswalk asset's material code to a paint tint for the preview.
  MaterialCatalog materials_;
};

} // namespace roadmaker::editor
