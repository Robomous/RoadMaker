#include "panels/library_panel.hpp"

#include <QLineEdit>
#include <QListView>
#include <QVBoxLayout>

#include "app/icons.hpp"

namespace roadmaker::editor {

namespace {

/// Maps a catalogue key to a bundled themed icon (Icons::get tints it to the
/// palette). Reuses the Create-Road template glyphs and the junction glyph;
/// unknown keys fall back to a generic box.
[[nodiscard]] QString icon_name_for(const QString& key) {
  if (key == QStringLiteral("road.rural")) {
    return QStringLiteral("template-rural");
  }
  if (key == QStringLiteral("road.urban")) {
    return QStringLiteral("template-urban");
  }
  if (key == QStringLiteral("road.highway")) {
    return QStringLiteral("template-highway");
  }
  if (key.startsWith(QStringLiteral("assembly."))) {
    return QStringLiteral("junction-connect");
  }
  if (key.startsWith(QStringLiteral("prop."))) {
    return QStringLiteral("trees");
  }
  return QStringLiteral("box");
}

} // namespace

LibraryFilterProxy::LibraryFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
  setFilterCaseSensitivity(Qt::CaseInsensitive);
  setFilterRole(Qt::DisplayRole); // the label
  setSortRole(LibraryListModel::CategoryRole);
  setDynamicSortFilter(true);
}

QVariant LibraryFilterProxy::data(const QModelIndex& index, int role) const {
  if (role == Qt::DecorationRole) {
    const QString key = QSortFilterProxyModel::data(index, LibraryListModel::KeyRole).toString();
    return Icons::get(icon_name_for(key));
  }
  return QSortFilterProxyModel::data(index, role);
}

LibraryPanel::LibraryPanel(LibraryListModel& model, QWidget* parent) : QWidget(parent) {
  proxy_.setSourceModel(&model);
  proxy_.sort(0);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("library_search"));
  search_->setPlaceholderText(tr("Search the library…"));
  search_->setClearButtonEnabled(true);

  view_ = new QListView(this);
  view_->setModel(&proxy_);
  view_->setViewMode(QListView::IconMode);
  view_->setResizeMode(QListView::Adjust); // reflow on resize
  view_->setMovement(QListView::Static);   // read-only grid, no reordering
  view_->setUniformItemSizes(true);
  view_->setWordWrap(true);
  view_->setIconSize(QSize(48, 48));
  view_->setGridSize(QSize(96, 84));
  view_->setSpacing(6);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->setDragEnabled(true); // drag an item onto the viewport to create it
  view_->setDragDropMode(QAbstractItemView::DragOnly);

  connect(search_, &QLineEdit::textChanged, &proxy_, &QSortFilterProxyModel::setFilterFixedString);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);
  layout->addWidget(search_);
  layout->addWidget(view_, 1);
}

} // namespace roadmaker::editor
