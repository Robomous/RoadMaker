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

#include "panels/library_panel.hpp"

#include <QComboBox>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QStringList>
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
  if (key.startsWith(QStringLiteral("style."))) {
    // Road styles reuse the road-template glyph family (a dedicated style icon
    // is a Library-polish follow-up, P6).
    return QStringLiteral("template-urban");
  }
  if (key.startsWith(QStringLiteral("assembly."))) {
    return QStringLiteral("junction-connect");
  }
  if (key.startsWith(QStringLiteral("prop_set."))) {
    // Prop sets reuse the vegetation glyph (Icons::get falls back to it — no
    // per-set thumbnail this sprint).
    return QStringLiteral("trees");
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
    // Prefer the bundled/overlay thumbnail (p6-s2); fall back to a themed glyph
    // only when the item has none (an overlay item without a thumbnail, or an
    // Unknown kind). Thumbnail icons are static — unlike the glyphs, they are
    // NOT retinted on a palette change, so nothing hooks them into cache
    // clearing.
    const QVariant thumbnail = QSortFilterProxyModel::data(index, Qt::DecorationRole);
    if (thumbnail.isValid()) {
      return thumbnail;
    }
    const QString key = QSortFilterProxyModel::data(index, LibraryListModel::KeyRole).toString();
    return Icons::get(icon_name_for(key));
  }
  return QSortFilterProxyModel::data(index, role);
}

void LibraryFilterProxy::set_category_filter(const QString& category) {
  if (category_filter_ == category) {
    return;
  }
  category_filter_ = category;
  invalidateFilter();
}

bool LibraryFilterProxy::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
  // Search box first (label match, handled by the base), then the category combo.
  if (!QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent)) {
    return false;
  }
  if (category_filter_.isEmpty()) {
    return true;
  }
  const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);
  return sourceModel()->data(index, LibraryListModel::CategoryRole).toString() == category_filter_;
}

LibraryPanel::LibraryPanel(LibraryListModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
  proxy_.setSourceModel(&model);
  proxy_.sort(0);

  category_combo_ = new QComboBox(this);
  category_combo_->setObjectName(QStringLiteral("library_category"));
  populate_categories();

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
  connect(category_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
    // Index 0 ("All categories") carries an empty userData → no category filter.
    proxy_.set_category_filter(category_combo_->currentData().toString());
  });
  connect(
      view_->selectionModel(),
      &QItemSelectionModel::currentChanged,
      this,
      [this](const QModelIndex& current, const QModelIndex&) { handle_current_changed(current); });

  view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view_, &QListView::customContextMenuRequested, this, &LibraryPanel::show_context_menu);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);
  layout->addWidget(category_combo_);
  layout->addWidget(search_);
  layout->addWidget(view_, 1);
}

void LibraryPanel::populate_categories() {
  category_combo_->clear();
  category_combo_->addItem(tr("All categories"), QString());
  // First-seen order over the merged model — the manifest already lists items
  // grouped by category, so this reads top-to-bottom as authored.
  QStringList seen;
  for (int row = 0; row < model_.rowCount(); ++row) {
    const LibraryItem* item = model_.item(row);
    if (item == nullptr || item->category.isEmpty() || seen.contains(item->category)) {
      continue;
    }
    seen.append(item->category);
    category_combo_->addItem(item->category, item->category);
  }
}

void LibraryPanel::handle_current_changed(const QModelIndex& index) {
  if (!index.isValid()) {
    return;
  }
  const LibraryItem* item = model_.item(proxy_.mapToSource(index).row());
  if (item == nullptr) {
    return;
  }
  // Every valid selection updates the "current Library asset" so MainWindow can
  // arm the matching placement tool (Library-first, #367).
  emit asset_current_changed(item->key);
  // Parametric assets additionally open in the Attributes-pane editor.
  if (item->kind == LibraryItem::Kind::Crosswalk || item->kind == LibraryItem::Kind::PropSet) {
    emit asset_selected(item->key);
  }
}

void LibraryPanel::show_context_menu(const QPoint& pos) {
  QMenu menu(this);
  QAction* new_crosswalk = menu.addAction(tr("New crosswalk asset…"));
  connect(new_crosswalk, &QAction::triggered, this, &LibraryPanel::new_crosswalk_asset_requested);
  QAction* new_prop_set = menu.addAction(tr("New prop set…"));
  connect(new_prop_set, &QAction::triggered, this, &LibraryPanel::new_prop_set_requested);
  menu.exec(view_->viewport()->mapToGlobal(pos));
}

void LibraryPanel::select_asset(const QString& key) {
  for (int row = 0; row < proxy_.rowCount(); ++row) {
    const QModelIndex index = proxy_.index(row, 0);
    if (proxy_.data(index, LibraryListModel::KeyRole).toString() == key) {
      search_->clear();
      view_->setCurrentIndex(index);
      view_->scrollTo(index, QAbstractItemView::PositionAtCenter);
      emit asset_selected(key);
      return;
    }
  }
}

void LibraryPanel::focus_category(const QString& category) {
  search_->clear(); // the jump must not land behind a filter that hides it
  for (int row = 0; row < proxy_.rowCount(); ++row) {
    const QModelIndex index = proxy_.index(row, 0);
    if (proxy_.data(index, LibraryListModel::CategoryRole).toString() == category) {
      view_->setCurrentIndex(index);
      view_->scrollTo(index, QAbstractItemView::PositionAtTop);
      return;
    }
  }
}

} // namespace roadmaker::editor
