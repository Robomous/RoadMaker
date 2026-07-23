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

// The Library dock (UI-revamp Phase 2): a searchable icon grid over the
// LibraryListModel — road templates and parametric assemblies the user drags
// onto the viewport to create geometry. Thin view: a search box + a QListView
// in icon mode over a filter/sort proxy; all catalogue data lives in the
// model. The drag source and the viewport drop handler land in a follow-up
// (P2.4); this PR is the browsable panel.

#include <QSortFilterProxyModel>
#include <QWidget>

#include "document/library_list_model.hpp"

class QComboBox;
class QLineEdit;
class QListView;

namespace roadmaker::editor {

/// Filters the catalogue by the search box (case-insensitive, on the label),
/// sorts items by category then label so classes cluster, and injects a
/// themed class icon for the grid's DecorationRole (the model stays pure).
class LibraryFilterProxy : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit LibraryFilterProxy(QObject* parent = nullptr);
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  /// Restricts the grid to one category header (the manifest's spelling, e.g.
  /// "Props"); an empty string shows every category. Combines with the search
  /// box — an item must pass both to appear (#367).
  void set_category_filter(const QString& category);

protected:
  [[nodiscard]] bool filterAcceptsRow(int source_row,
                                      const QModelIndex& source_parent) const override;

private:
  QString category_filter_;
};

class LibraryPanel : public QWidget {
  Q_OBJECT

public:
  explicit LibraryPanel(LibraryListModel& model, QWidget* parent = nullptr);

  [[nodiscard]] QListView* view() { return view_; }

  /// Scrolls to and selects the first item of `category` (the manifest's
  /// spelling, e.g. "Props") — what an engaged Attributes-pane slot asks for.
  /// Clears the search box first, so a stale filter cannot hide the target.
  /// Unknown categories leave the panel alone.
  void focus_category(const QString& category);

  /// Selects the item with `key` and emits asset_selected for it (used after a
  /// "New crosswalk asset…" so its editor opens straight away). No-op if the
  /// key is not present.
  void select_asset(const QString& key);

  /// The category filter combo, exposed for headless tests.
  [[nodiscard]] QComboBox* category_combo() { return category_combo_; }

signals:
  /// A parametric asset (currently Kind::Crosswalk) was selected — MainWindow
  /// routes it to the Attributes-pane asset editor (PropertiesPanel::edit_asset).
  /// Fires only for editable-asset kinds, never for droppable geometry items.
  void asset_selected(const QString& key);

  /// The current catalogue item changed to `key` (fires for EVERY kind, on any
  /// valid selection). MainWindow tracks it as the "current Library asset" and
  /// arms the matching placement tool (Library-first interaction, #367): a prop
  /// arms Prop Point, a stencil Marking Point, a crosswalk the Crosswalk tool, a
  /// prop set Prop Curve, a text sign the Sign tool. Kinds with no placement
  /// mode leave the active tool untouched.
  void asset_current_changed(const QString& key);

  /// The context menu's "New crosswalk asset…" was chosen — MainWindow creates
  /// a project-overlay crosswalk asset and opens its editor.
  void new_crosswalk_asset_requested();

  /// The context menu's "New prop set…" was chosen — MainWindow creates a
  /// project-overlay prop-set asset and opens its editor.
  void new_prop_set_requested();

private:
  /// Emits asset_selected when `index` (proxy space) is a parametric asset.
  void handle_current_changed(const QModelIndex& index);
  void show_context_menu(const QPoint& pos);

  /// Fills category_combo_ with "All categories" + each distinct category in the
  /// merged model, in first-seen order.
  void populate_categories();

  LibraryListModel& model_;
  LibraryFilterProxy proxy_;
  QComboBox* category_combo_;
  QLineEdit* search_;
  QListView* view_;
};

} // namespace roadmaker::editor
