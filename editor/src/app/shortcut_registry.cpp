#include "app/shortcut_registry.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <array>

namespace roadmaker::editor::shortcuts {

namespace {

// The map. Order here is the order on the page.
constexpr std::array kTable{
    Entry{.id = Id::NewScene,
          .category = "File",
          .description = "New scene",
          .standard = QKeySequence::New,
          .documented = "Ctrl+N",
          .note = "\u2318N on macOS"},
    Entry{.id = Id::Open,
          .category = "File",
          .description = "Open an OpenDRIVE (.xodr) file",
          .standard = QKeySequence::Open,
          .documented = "Ctrl+O",
          .note = "\u2318O on macOS"},
    Entry{.id = Id::Save,
          .category = "File",
          .description = "Save",
          .standard = QKeySequence::Save,
          .documented = "Ctrl+S",
          .note = "\u2318S on macOS"},
    Entry{.id = Id::SaveAs,
          .category = "File",
          .description = "Save as…",
          .standard = QKeySequence::SaveAs,
          .documented = "Ctrl+Shift+S",
          .note = "\u21e7\u2318S on macOS"},
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
          .primary = Qt::Key_Q},
    Entry{
        .id = Id::ToolMove, .category = "Tools", .description = "Move tool", .primary = Qt::Key_M},
    Entry{.id = Id::ToolCreateRoad,
          .category = "Tools",
          .description = "Create Road tool",
          .primary = Qt::Key_C},
    Entry{.id = Id::ToolEditNodes,
          .category = "Tools",
          .description = "Edit Nodes tool",
          .primary = Qt::Key_N},
    Entry{.id = Id::ToolLaneProfile,
          .category = "Tools",
          .description = "Lane tool",
          .primary = Qt::Key_L},
    Entry{.id = Id::ToolElevation,
          .category = "Tools",
          .description = "Elevation tool",
          .primary = Qt::Key_E},
    Entry{.id = Id::ToolCreateJunction,
          .category = "Tools",
          .description = "Create Junction tool",
          .primary = Qt::Key_J},
    Entry{.id = Id::ToolSplit,
          .category = "Tools",
          .description = "Split tool",
          .primary = Qt::Key_K},
    Entry{.id = Id::ToolDelete,
          .category = "Tools",
          .description = "Delete tool",
          .primary = Qt::Key_X},
    Entry{.id = Id::ToolLaneAdd,
          .category = "Tools",
          .description = "Lane Add tool",
          .primary = Qt::Key_A},
    Entry{.id = Id::ToolLaneForm,
          .category = "Tools",
          .description = "Lane Form tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_A)},
    Entry{.id = Id::ToolLaneCarve,
          .category = "Tools",
          .description = "Lane Carve tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_C)},
    Entry{.id = Id::ToolCrosswalk,
          .category = "Tools",
          .description = "Crosswalk & Stop Line tool",
          .primary = Qt::Key_W},
    Entry{.id = Id::ToolMarkingPoint,
          .category = "Tools",
          .description = "Marking Point tool",
          .primary = Qt::Key_S},
    Entry{.id = Id::ToolMarkingCurve,
          .category = "Tools",
          .description = "Marking Curve tool",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_W)},
    Entry{.id = Id::LaneWidthEditor,
          .category = "Tools",
          .description = "Lane Width editor (2D)",
          .primary = QKeyCombination(Qt::ShiftModifier, Qt::Key_L)},

    Entry{.id = Id::FrameSelection,
          .category = "View",
          .description = "Frame the selection (the whole scene when nothing is selected)",
          .primary = Qt::Key_F},
    Entry{.id = Id::FrameCursor,
          .category = "View",
          .description = "Frame on the point under the cursor (keeps the zoom)",
          .primary = Qt::Key_V},
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

} // namespace roadmaker::editor::shortcuts
