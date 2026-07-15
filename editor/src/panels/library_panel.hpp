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

private:
  LibraryFilterProxy proxy_;
  QLineEdit* search_;
  QListView* view_;
};

} // namespace roadmaker::editor
