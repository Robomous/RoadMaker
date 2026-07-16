#pragma once

// The 2D Editor pane (P1/GW-2 step 7): a tabbed host for the editors that work
// in a flat, non-perspective view of one entity — the vertical profile today,
// the cross-section and the Signal Phase Editor (GW-4 step 4, p4-s5) later.
//
// The host is deliberately thin. A page owns its own selection subscription and
// its own commands exactly as it did standing alone; the host only decides
// which tab is worth looking at. That keeps pages testable on their own and
// means adding one is a page class plus a register_page() call.

#include <QTabWidget>
#include <QWidget>
#include <memory>
#include <vector>

#include "panels/profile_panel.hpp"
#include "panels/width_panel.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// One pluggable editor in the 2D Editor pane.
class Editor2DPage {
public:
  Editor2DPage() = default;
  virtual ~Editor2DPage() = default;

  Editor2DPage(const Editor2DPage&) = delete;
  Editor2DPage& operator=(const Editor2DPage&) = delete;
  Editor2DPage(Editor2DPage&&) = delete;
  Editor2DPage& operator=(Editor2DPage&&) = delete;

  /// The tab label.
  [[nodiscard]] virtual QString title() const = 0;

  /// The widget to host. Owned by the page; the host reparents it into the
  /// tab bar and never deletes it out from under the page.
  [[nodiscard]] virtual QWidget* widget() = 0;

  /// Whether this page has anything to say about the current selection. The
  /// host raises a relevant tab when the selection changes; it never hides an
  /// irrelevant one, because a user who chose a tab should keep it.
  [[nodiscard]] virtual bool relevant(const SelectionModel& selection) const = 0;
};

/// Hosts the existing ProfilePanel unchanged. The adapter lives here rather
/// than in ProfilePanel so the panel stays unaware it is hosted at all — which
/// is why editor/tests/test_profile_panel.cpp needed no changes.
class ProfileEditorPage : public Editor2DPage {
public:
  ProfileEditorPage(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  [[nodiscard]] QString title() const override;
  [[nodiscard]] QWidget* widget() override;

  /// Relevant whenever a road is selected — the profile edits z(s) of a road.
  [[nodiscard]] bool relevant(const SelectionModel& selection) const override;

  [[nodiscard]] ProfilePanel* panel() { return panel_; }

private:
  ProfilePanel* panel_;
};

/// Hosts a WidthPanel — the per-lane width-along-s editor (p2-s4). Like the
/// profile page, the panel keeps its own selection subscription; this adapter
/// only supplies the tab label and relevance.
class WidthEditorPage : public Editor2DPage {
public:
  WidthEditorPage(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  [[nodiscard]] QString title() const override;
  [[nodiscard]] QWidget* widget() override;

  /// Relevant whenever a lane is selected — the panel edits w(s) of a lane.
  [[nodiscard]] bool relevant(const SelectionModel& selection) const override;

  [[nodiscard]] WidthPanel* panel() { return panel_; }

private:
  WidthPanel* panel_;
};

/// Tabbed container for Editor2DPages.
class Editor2DHost : public QWidget {
  Q_OBJECT

public:
  explicit Editor2DHost(const SelectionModel& selection, QWidget* parent = nullptr);

  /// Adds `page` as a tab (the host takes ownership). Registration order is
  /// tab order.
  void register_page(std::unique_ptr<Editor2DPage> page);

  [[nodiscard]] int page_count() const { return tabs_->count(); }

  [[nodiscard]] QString current_title() const { return tabs_->tabText(tabs_->currentIndex()); }

  /// Raises the first registered page that is relevant to the selection. A
  /// no-op when the current tab is already relevant — switching tabs out from
  /// under someone mid-edit would be worse than showing a stale one.
  void raise_relevant_page();

  /// Raises the page whose title() equals `title`, if any (the dedicated
  /// shortcut path — e.g. ⇧L jumps straight to Lane Width). Returns whether a
  /// matching tab was found.
  bool show_page(const QString& title);

private:
  const SelectionModel& selection_;
  QTabWidget* tabs_;
  std::vector<std::unique_ptr<Editor2DPage>> pages_;
};

} // namespace roadmaker::editor
