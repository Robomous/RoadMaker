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
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QSplitter>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <filesystem>

#include "app/icons.hpp"
#include "document/asset_import.hpp"
#include "panels/project_files_panel.hpp"

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

  // OS-file drop (p6-s8, #322). On the PANEL rather than the grid so a drop
  // anywhere in the dock lands, including on the file explorer half.
  setAcceptDrops(true);

  // A project overlay is adopted AFTER the panel is built, and it can introduce
  // categories the built-in catalogue has never heard of. Without this the
  // combo kept whatever it was born with and those items were unreachable by
  // category.
  connect(&model_, &QAbstractItemModel::modelReset, this, &LibraryPanel::populate_categories);

  // The catalogue half (combo + search + grid) is one splitter pane; the
  // project's asset folder is the other (p6-s7). A plain QSplitter, not a tab
  // stack: both halves stay visible and the user sets the balance.
  auto* catalogue = new QWidget(this);
  auto* catalogue_layout = new QVBoxLayout(catalogue);
  catalogue_layout->setContentsMargins(0, 0, 0, 0);
  catalogue_layout->setSpacing(6);
  catalogue_layout->addWidget(category_combo_);
  catalogue_layout->addWidget(search_);
  catalogue_layout->addWidget(view_, 1);

  files_ = new ProjectFilesPanel(this);
  files_->hide(); // no project open yet — nothing to browse

  splitter_ = new QSplitter(Qt::Vertical, this);
  splitter_->setObjectName(QStringLiteral("library_splitter"));
  splitter_->setChildrenCollapsible(true);
  splitter_->addWidget(catalogue);
  splitter_->addWidget(files_);
  splitter_->setStretchFactor(0, 3);
  splitter_->setStretchFactor(1, 2);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);
  layout->addWidget(splitter_, 1);
}

void LibraryPanel::set_project(const std::filesystem::path& project_dir) {
  files_->set_project(project_dir);
  files_->show();
}

void LibraryPanel::clear_project() {
  files_->clear_project();
  files_->hide();
}

QByteArray LibraryPanel::splitter_state() const {
  return splitter_->saveState();
}

void LibraryPanel::restore_splitter_state(const QByteArray& state) {
  if (!state.isEmpty()) {
    splitter_->restoreState(state);
  }
}

void LibraryPanel::populate_categories() {
  // Repopulating must not silently drop the filter the user is looking at, so
  // the current category is restored when the rebuilt list still offers it.
  const QString selected = category_combo_->currentData().toString();
  const QSignalBlocker block(category_combo_);
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
  const int restored = selected.isEmpty() ? 0 : category_combo_->findData(selected);
  category_combo_->setCurrentIndex(restored < 0 ? 0 : restored);
  // The combo's signal is blocked above, so push the resulting filter through
  // by hand — a category that vanished with its overlay must stop filtering.
  proxy_.set_category_filter(category_combo_->currentData().toString());
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
  // Parametric assets additionally open in the Attributes-pane editor. Prop
  // (Kind::Tree) items open it too so their Default scale is editable (p6-s11).
  if (item->kind == LibraryItem::Kind::Crosswalk || item->kind == LibraryItem::Kind::PropSet ||
      item->kind == LibraryItem::Kind::Tree) {
    emit asset_selected(item->key);
  }
}

void LibraryPanel::show_context_menu(const QPoint& pos) {
  QMenu menu(this);
  QAction* new_crosswalk = menu.addAction(tr("New crosswalk asset…"));
  connect(new_crosswalk, &QAction::triggered, this, &LibraryPanel::new_crosswalk_asset_requested);
  QAction* new_prop_set = menu.addAction(tr("New prop set…"));
  connect(new_prop_set, &QAction::triggered, this, &LibraryPanel::new_prop_set_requested);
  menu.addSeparator();
  QAction* import_asset = menu.addAction(tr("Import asset…"));
  // Empty path: no file in hand, so MainWindow opens a file dialog. Same handler
  // either way, so the menu and the drop cannot diverge.
  connect(import_asset, &QAction::triggered, this, [this]() {
    emit asset_import_requested(QString());
  });
  menu.exec(view_->viewport()->mapToGlobal(pos));
}

QStringList LibraryPanel::importable_paths(const QMimeData* mime) {
  QStringList paths;
  if (mime == nullptr || !mime->hasUrls()) {
    return paths;
  }
  for (const QUrl& url : mime->urls()) {
    if (!url.isLocalFile()) {
      continue;
    }
    const QString local = url.toLocalFile();
    // The SAME predicate the importer uses, so a file that hovers as acceptable
    // cannot then be refused — and one that would be refused shows the no-drop
    // cursor instead of accepting and toasting.
    if (asset_import_kind(std::filesystem::path(local.toStdString())).has_value()) {
      paths.push_back(local);
    }
  }
  return paths;
}

void LibraryPanel::dragEnterEvent(QDragEnterEvent* event) {
  if (importable_paths(event->mimeData()).isEmpty()) {
    return; // ignored: Qt shows the refusal cursor
  }
  event->acceptProposedAction();
}

void LibraryPanel::dragMoveEvent(QDragMoveEvent* event) {
  if (importable_paths(event->mimeData()).isEmpty()) {
    return;
  }
  event->acceptProposedAction();
}

void LibraryPanel::dropEvent(QDropEvent* event) {
  const QStringList paths = importable_paths(event->mimeData());
  if (paths.isEmpty()) {
    return;
  }
  event->acceptProposedAction();
  // One request per file: a multi-file drop imports each in turn, each with its
  // own dialog, rather than silently taking only the first.
  for (const QString& path : paths) {
    emit asset_import_requested(path);
  }
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
