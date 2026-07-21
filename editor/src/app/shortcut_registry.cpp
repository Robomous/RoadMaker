#include "app/shortcut_registry.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace roadmaker::editor::shortcuts {

namespace {

// The toolbar taxonomy (issue #317). Row 1 authors the network; row 2 holds
// the scene layers laid over it. The three reserved groups are committed now
// and render nothing until their pillar lands its first tool — so P4's
// signals and signs arrive into a category instead of re-shuffling a flat
// toolbar.
constexpr std::array kToolbarGroups{
    ToolbarGroup{"File", ToolbarRow::kAuthoring},
    ToolbarGroup{"Edit", ToolbarRow::kAuthoring},
    ToolbarGroup{"Roads", ToolbarRow::kAuthoring},
    ToolbarGroup{"Lanes", ToolbarRow::kAuthoring},
    ToolbarGroup{"Markings", ToolbarRow::kLayers},
    ToolbarGroup{"Props", ToolbarRow::kLayers},
    ToolbarGroup{"Terrain & Structures", ToolbarRow::kLayers}, // reserved
    ToolbarGroup{"Signals & Signs", ToolbarRow::kLayers},      // reserved
    ToolbarGroup{"Scenario", ToolbarRow::kLayers},             // reserved
    ToolbarGroup{"Library & View", ToolbarRow::kLayers},
};

// The map. Order here is the order on the page — NOT the toolbar's order,
// which is the independent toolbar_group/toolbar_order pair. Reordering rows
// to suit the toolbar would rewrite docs/user-guide/shortcuts.md.
constexpr std::array kTable{
    Entry{.id = Id::NewScene,
          .category = "File",
          .description = "New scene",
          .standard = QKeySequence::New,
          .documented = "Ctrl+N",
          .note = "\u2318N on macOS",
          .toolbar_group = "File",
          .toolbar_order = 10},
    Entry{.id = Id::Open,
          .category = "File",
          .description = "Open an OpenDRIVE (.xodr) file",
          .standard = QKeySequence::Open,
          .documented = "Ctrl+O",
          .note = "\u2318O on macOS",
          .toolbar_group = "File",
          .toolbar_order = 20},
    Entry{.id = Id::Save,
          .category = "File",
          .description = "Save",
          .standard = QKeySequence::Save,
          .documented = "Ctrl+S",
          .note = "\u2318S on macOS",
          .toolbar_group = "File",
          .toolbar_order = 30},
    Entry{.id = Id::SaveAs,
          .category = "File",
          .description = "Save as…",
          .standard = QKeySequence::SaveAs,
          .documented = "Ctrl+Shift+S",
          .note = "\u21e7\u2318S on macOS"},
    // Toolbar-only (p1-s5): no binding at all, so sequences() is empty and
    // markdown() skips the row — the committed shortcuts page is unchanged.
    Entry{.id = Id::ExportGlb,
          .category = "File",
          .description = "Export the network as binary glTF (.glb)",
          .toolbar_group = "File",
          .toolbar_order = 40},
    Entry{.id = Id::Quit,
          .category = "File",
          .description = "Quit",
          .standard = QKeySequence::Quit,
          .documented = "Ctrl+Q",
          .note = "\u2318Q on macOS, where the platform menu owns it"},

    Entry{.id = Id::Undo,
          .category = "Edit",
          .description = "Undo",
          .standard = QKeySequence::Undo,
          .documented = "Ctrl+Z",
          .note = "\u2318Z on macOS"},
    Entry{.id = Id::Redo,
          .category = "Edit",
          .description = "Redo",
          .standard = QKeySequence::Redo,
          .documented = "Ctrl+Shift+Z",
          .note = "Ctrl+Y also works on Windows; \u21e7\u2318Z on macOS"},

    Entry{.id = Id::ToolSelect,
          .category = "Tools",
          .description = "Select/Move tool",
          .primary = Qt::Key_Q,
          .toolbar_group = "Edit",
          .toolbar_order = 10},
    Entry{.id = Id::ToolMove,
          .category = "Tools",
          .description = "Move tool",
          .primary = Qt::Key_M,
          .toolbar_group = "Edit",
          .toolbar_order = 20},
    Entry{.id = Id::ToolCreateRoad,
          .category = "Tools",
          .description = "Create Road tool",
          .primary = Qt::Key_C,
          .toolbar_group = "Roads",
          .toolbar_order = 10},
    Entry{.id = Id::ToolEditNodes,
          .category = "Tools",
          .description = "Edit Nodes tool",
          .primary = Qt::Key_N,
          .toolbar_group = "Roads",
          .toolbar_order = 20},
    Entry{.id = Id::ToolLaneProfile,
          .category = "Tools",
          .description = "Lane tool",
          .primary = Qt::Key_L,
          .toolbar_group = "Lanes",
          .toolbar_order = 10},
    Entry{.id = Id::ToolElevation,
          .category = "Tools",
          .description = "Elevation tool",
          .primary = Qt::Key_E,
          .toolbar_group = "Roads",
          .toolbar_order = 50},
    Entry{.id = Id::ToolCreateJunction,
          .category = "Tools",
          .description = "Create Junction tool",
          .primary = Qt::Key_J,
          .toolbar_group = "Roads",
          .toolbar_order = 30},
    Entry{.id = Id::ToolSplit,
          .category = "Tools",
          .description = "Split tool",
          .primary = Qt::Key_K,
          .toolbar_group = "Edit",
          .toolbar_order = 30},
    Entry{.id = Id::ToolDelete,
          .category = "Tools",
          .description = "Delete tool",
          .primary = Qt::Key_X,
          .toolbar_group = "Edit",
          .toolbar_order = 40},
    Entry{.id = Id::ToolLaneAdd,
          .category = "Tools",
          .description = "Lane Add tool",
          .primary = Qt::Key_A,
          .toolbar_group = "Lanes",
          .toolbar_order = 20},
    Entry{.id = Id::ToolLaneForm,
          .category = "Tools",
          .description = "Lane Form tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_A),
          .toolbar_group = "Lanes",
          .toolbar_order = 30},
    Entry{.id = Id::ToolLaneCarve,
          .category = "Tools",
          .description = "Lane Carve tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_C),
          .toolbar_group = "Lanes",
          .toolbar_order = 40},
    Entry{.id = Id::ToolCrosswalk,
          .category = "Tools",
          .description = "Crosswalk & Stop Line tool",
          .primary = Qt::Key_W,
          .toolbar_group = "Markings",
          .toolbar_order = 10},
    Entry{.id = Id::ToolMarkingPoint,
          .category = "Tools",
          .description = "Marking Point tool",
          .primary = Qt::Key_S,
          .toolbar_group = "Markings",
          .toolbar_order = 20},
    Entry{.id = Id::ToolMarkingCurve,
          .category = "Tools",
          .description = "Marking Curve tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_W),
          .toolbar_group = "Markings",
          .toolbar_order = 30},
    Entry{.id = Id::ToolPropPoint,
          .category = "Tools",
          .description = "Prop Point tool",
          .primary = Qt::Key_T,
          .toolbar_group = "Props",
          .toolbar_order = 10},
    Entry{.id = Id::ToolPropCurve,
          .category = "Tools",
          .description = "Prop Curve tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_T),
          .toolbar_group = "Props",
          .toolbar_order = 20},
    Entry{.id = Id::ToolPropSpan,
          .category = "Tools",
          .description = "Prop Span tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_S),
          .toolbar_group = "Props",
          .toolbar_order = 30},
    Entry{.id = Id::ToolPropPolygon,
          .category = "Tools",
          .description = "Prop Polygon tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_P),
          .toolbar_group = "Props",
          .toolbar_order = 40},
    Entry{.id = Id::ToolCorner,
          .category = "Tools",
          .description = "Corner tool (junction fillets)",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_R),
          .note = "plain R is the Prop Polygon tool's re-scatter key",
          .toolbar_group = "Roads",
          .toolbar_order = 40},
    Entry{.id = Id::ToolStopLine,
          .category = "Tools",
          .description = "Stop Line tool (junction stop lines)",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_O),
          .note = "F flips the active line while the tool is up",
          .toolbar_group = "Roads",
          .toolbar_order = 45},
    Entry{.id = Id::ToolJunctionSpan,
          .category = "Tools",
          .description = "Junction Span tool (virtual junctions over a road)",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_J),
          .note = "plain J is the Create Junction tool",
          .toolbar_group = "Roads",
          .toolbar_order = 47},
    Entry{.id = Id::ToolJunctionSurface,
          .category = "Tools",
          .description = "Junction Surface tool (inspect and order fill spans)",
          .primary = Qt::Key_I,
          .note = "Space toggles the active span's samples; PgUp/PgDn raise or lower it",
          .toolbar_group = "Roads",
          .toolbar_order = 48},
    Entry{.id = Id::LaneWidthEditor,
          .category = "Tools",
          .description = "Lane Width editor (2D)",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_L),
          .toolbar_group = "Lanes",
          .toolbar_order = 50},
    // Toolbar-only: enabled only for a mergeable two-road selection.
    Entry{.id = Id::MergeRoads,
          .category = "Tools",
          .description = "Merge two selected roads that meet end-to-start",
          .toolbar_group = "Edit",
          .toolbar_order = 50},

    // Toolbar-only view commands.
    Entry{.id = Id::AddFromLibrary,
          .category = "View",
          .description = "Open the Library panel",
          .toolbar_group = "Library & View",
          .toolbar_order = 10},
    Entry{.id = Id::ResetCamera,
          .category = "View",
          .description = "Reset the camera to the default view",
          .toolbar_group = "Library & View",
          .toolbar_order = 20},
    Entry{.id = Id::FrameSelection,
          .category = "View",
          .description = "Frame the selection (the whole scene when nothing is selected)",
          .primary = Qt::Key_F,
          .toolbar_group = "Library & View",
          .toolbar_order = 30},
    Entry{.id = Id::FrameCursor,
          .category = "View",
          .description = "Frame on the point under the cursor (keeps the zoom)",
          .primary = Qt::Key_V},
    Entry{.id = Id::ViewportHints,
          .category = "View",
          .description = "Show or hide the active tool's hint in the viewport corner",
          .primary = Qt::Key_H},
    Entry{.id = Id::ViewPerspective,
          .category = "View",
          .description = "Perspective projection",
          .primary = Qt::Key_P},
    Entry{.id = Id::ViewOrthographic,
          .category = "View",
          .description = "Orthographic projection",
          .primary = Qt::Key_O},
    Entry{.id = Id::ViewNorth,
          .category = "View",
          .description = "Look from the north",
          .primary = Qt::KeypadModifier | Qt::Key_8,
          .alternate = Qt::Key_8,
          .note = "Numpad; the top-row digit is the alternate"},
    Entry{.id = Id::ViewSouth,
          .category = "View",
          .description = "Look from the south",
          .primary = Qt::KeypadModifier | Qt::Key_2,
          .alternate = Qt::Key_2,
          .note = "Numpad; the top-row digit is the alternate"},
    Entry{.id = Id::ViewWest,
          .category = "View",
          .description = "Look from the west",
          .primary = Qt::KeypadModifier | Qt::Key_4,
          .alternate = Qt::Key_4,
          .note = "Numpad; the top-row digit is the alternate"},
    Entry{.id = Id::ViewEast,
          .category = "View",
          .description = "Look from the east",
          .primary = Qt::KeypadModifier | Qt::Key_6,
          .alternate = Qt::Key_6,
          .note = "Numpad; the top-row digit is the alternate"},
    Entry{.id = Id::ViewTop,
          .category = "View",
          .description = "Top-down view, north up",
          .primary = Qt::KeypadModifier | Qt::Key_5,
          .alternate = Qt::Key_5,
          .note = "Numpad; the top-row digit is the alternate"},

    Entry{.id = Id::Help,
          .category = "Help",
          .description = "Open the user guide",
          .standard = QKeySequence::HelpContents,
          .documented = "F1"},
};

/// PortableText so the page is platform-stable; the StandardKey rows still
/// render their platform's convention, which is the point of using them.
QString render(const QKeySequence& sequence) {
  return sequence.toString(QKeySequence::PortableText);
}

} // namespace

std::span<const Entry> table() {
  return kTable;
}

const Entry& entry(Id id) {
  const auto it = std::ranges::find(kTable, id, &Entry::id);
  Q_ASSERT(it != kTable.end()); // every Id has a row (asserted in the tests too)
  return *it;
}

QList<QKeySequence> sequences(Id id) {
  const Entry& row = entry(id);
  if (row.standard != QKeySequence::UnknownKey) {
    return QKeySequence::keyBindings(row.standard);
  }
  // A toolbar-only row carries no binding. Returning {} (rather than a
  // QKeySequence(Qt::Key_unknown), which would be a live empty shortcut) lets
  // Actions call setShortcuts() unconditionally for every Id.
  if (row.primary.key() == Qt::Key_unknown) {
    return {};
  }
  QList<QKeySequence> out;
  out.append(QKeySequence(row.primary));
  if (row.alternate.key() != Qt::Key_unknown) {
    out.append(QKeySequence(row.alternate));
  }
  return out;
}

QString markdown() {
  QString out;
  QString category;
  for (const Entry& row : kTable) {
    // Toolbar-only rows have nothing to document. Skipped BEFORE the category
    // header so a section made up entirely of them never emits an empty table.
    if (row.documented[0] == '\0' && sequences(row.id).isEmpty()) {
      continue;
    }
    const QString row_category = QString::fromUtf8(row.category);
    if (row_category != category) {
      category = row_category;
      out +=
          QStringLiteral("\n## %1\n\n| Action | Shortcut | Notes |\n|---|---|---|\n").arg(category);
    }
    QStringList rendered;
    if (row.documented[0] != '\0') {
      // A platform-standard binding: the page commits to one spelling (see
      // Entry::documented), the Notes cell carries the platform variants.
      rendered.append(QStringLiteral("`%1`").arg(QString::fromUtf8(row.documented)));
    } else {
      const QList<QKeySequence> keys = sequences(row.id);
      rendered.reserve(keys.size());
      for (const QKeySequence& key : keys) {
        rendered.append(QStringLiteral("`%1`").arg(render(key)));
      }
    }
    out += QStringLiteral("| %1 | %2 | %3 |\n")
               .arg(QString::fromUtf8(row.description),
                    rendered.join(QStringLiteral(" or ")),
                    QString::fromUtf8(row.note));
  }
  return out;
}

std::span<const ToolbarGroup> toolbar_groups() {
  return kToolbarGroups;
}

std::vector<ToolbarGroupLayout> toolbar_layout(ToolbarRow row) {
  std::vector<ToolbarGroupLayout> layout;
  for (const ToolbarGroup& group : kToolbarGroups) {
    if (group.row != row) {
      continue;
    }
    // Reserved groups fall through with an empty `ids` — present in the
    // taxonomy, rendering nothing.
    std::vector<const Entry*> rows;
    for (const Entry& entry_row : kTable) {
      if (entry_row.toolbar_group != nullptr &&
          std::string_view(entry_row.toolbar_group) == std::string_view(group.name)) {
        rows.push_back(&entry_row);
      }
    }
    // Stable, so equal orders keep table (documentation) order as the tiebreak.
    std::ranges::stable_sort(rows, {}, [](const Entry* e) { return e->toolbar_order; });

    ToolbarGroupLayout entry_layout{.group = &group, .ids = {}};
    entry_layout.ids.reserve(rows.size());
    for (const Entry* e : rows) {
      entry_layout.ids.push_back(e->id);
    }
    layout.push_back(std::move(entry_layout));
  }
  return layout;
}

QStringList toolbar_violations(std::span<const Entry> rows, std::span<const ToolbarGroup> groups) {
  QStringList problems;
  const auto known = [groups](const char* name) {
    return std::ranges::any_of(groups, [name](const ToolbarGroup& g) {
      return std::string_view(g.name) == std::string_view(name);
    });
  };

  // Named `taken`, not `slots`: Qt's qobjectdefs.h #defines `slots` to nothing
  // (keyword macro), so that spelling silently deletes the declaration.
  std::vector<std::pair<std::string_view, int>> taken;
  for (const Entry& row : rows) {
    const QString description = QString::fromUtf8(row.description);
    if (row.toolbar_group == nullptr) {
      // THE GATE: a tool that never reaches the toolbar is a tool the user
      // cannot find. Non-tool rows (menu-only commands) are free to opt out.
      if (std::string_view(row.category) == std::string_view("Tools")) {
        problems.append(
            QStringLiteral("'%1' is a Tools entry with no toolbar_group — every tool must be "
                           "categorized (shortcut_registry.cpp kToolbarGroups)")
                .arg(description));
      }
      continue;
    }
    if (!known(row.toolbar_group)) {
      problems.append(QStringLiteral("'%1' names toolbar group '%2', which is not in the taxonomy")
                          .arg(description, QString::fromUtf8(row.toolbar_group)));
      continue;
    }
    const std::pair<std::string_view, int> slot{std::string_view(row.toolbar_group),
                                                row.toolbar_order};
    if (std::ranges::find(taken, slot) != taken.end()) {
      problems.append(QStringLiteral("'%1' collides with another action at %2 slot %3 — "
                                     "toolbar_order must be unique within a group")
                          .arg(description, QString::fromUtf8(row.toolbar_group))
                          .arg(row.toolbar_order));
      continue;
    }
    taken.push_back(slot);
  }
  return problems;
}

} // namespace roadmaker::editor::shortcuts
