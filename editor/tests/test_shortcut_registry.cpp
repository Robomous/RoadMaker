// The shortcut map is a single source of truth: Actions binds from it and
// docs/user-guide/shortcuts.md is rendered from it. These tests are what keeps
// those three in step — before P1, key letters were hand-copied into tooltips
// and nothing noticed when they drifted.

#include <gtest/gtest.h>

#include <QFile>
#include <QSet>
#include <QUndoStack>
#include <fstream>
#include <sstream>

#include "app/actions.hpp"
#include "app/shortcut_registry.hpp"

namespace roadmaker::editor {
namespace {

using shortcuts::Id;

TEST(ShortcutRegistry, EveryIdHasExactlyOneRow) {
  QSet<int> seen;
  for (const shortcuts::Entry& row : shortcuts::table()) {
    EXPECT_FALSE(seen.contains(static_cast<int>(row.id)))
        << "duplicate row for id " << static_cast<int>(row.id);
    seen.insert(static_cast<int>(row.id));
    EXPECT_STRNE(row.description, "") << "every row must describe itself for the page";
  }
  // Every value below the sentinel must be covered. Bounded by kIdCount, not
  // by whichever Id happens to be declared last.
  for (int i = 0; i < static_cast<int>(Id::kIdCount); ++i) {
    EXPECT_TRUE(seen.contains(i)) << "shortcuts::Id " << i << " has no table row";
  }
}

// p1-s5 admitted toolbar-only rows (no key binding) so the toolbar could be
// generated from one table. They must stay INVISIBLE to the shortcuts page —
// that is what keeps the committed page byte-identical across this change.
TEST(ShortcutRegistry, UnboundEntriesStayOffThePage) {
  const QString page = shortcuts::markdown();
  for (const Id id : {Id::ExportGlb, Id::MergeRoads, Id::AddFromLibrary, Id::ResetCamera}) {
    const shortcuts::Entry& row = shortcuts::entry(id);
    EXPECT_TRUE(shortcuts::sequences(id).isEmpty())
        << "'" << row.description << "' has no binding, so sequences() must be empty";
    EXPECT_FALSE(page.contains(QString::fromUtf8(row.description)))
        << "'" << row.description << "' is unbound but reached the rendered page";
  }
}

TEST(ShortcutRegistry, SequencesPutThePrimaryFirstAndKeepTheAlternate) {
  const QList<QKeySequence> cardinals = shortcuts::sequences(Id::ViewNorth);
  ASSERT_EQ(cardinals.size(), 2) << "the numpad-less alternate must not be dropped";
  EXPECT_EQ(cardinals.at(0), QKeySequence(Qt::KeypadModifier | Qt::Key_8));
  EXPECT_EQ(cardinals.at(1), QKeySequence(Qt::Key_8));

  const QList<QKeySequence> select = shortcuts::sequences(Id::ToolSelect);
  ASSERT_EQ(select.size(), 1);
  EXPECT_EQ(select.at(0), QKeySequence(Qt::Key_Q)) << "Select rebound to Q in p1-s2";
}

// The Lane Width editor (p2-s4) is ⇧L — a QKeyCombination, never an int (that
// would hit QKeyCombination::operator int() and only MSVC/​/WX would notice).
TEST(ShortcutRegistry, LaneWidthEditorIsShiftL) {
  EXPECT_EQ(shortcuts::entry(Id::LaneWidthEditor).primary,
            QKeyCombination(Qt::ShiftModifier, Qt::Key_L));
  const QList<QKeySequence> keys = shortcuts::sequences(Id::LaneWidthEditor);
  ASSERT_EQ(keys.size(), 1);
  EXPECT_EQ(keys.at(0), QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_L)));
}

// Two actions sharing a key would leave one silently dead. The numpad/top-row
// pairs are the same ACTION's two bindings, so they are not a conflict.
TEST(ShortcutRegistry, NoDuplicateActiveBindings) {
  QSet<QString> seen;
  for (const shortcuts::Entry& row : shortcuts::table()) {
    for (const QKeySequence& key : shortcuts::sequences(row.id)) {
      const QString text = key.toString(QKeySequence::PortableText);
      EXPECT_FALSE(seen.contains(text))
          << "'" << text.toStdString() << "' is bound twice (" << row.description << ")";
      seen.insert(text);
    }
  }
}

// The rebind that made room for frame-on-cursor. Worth pinning by name: it is
// the one binding change a user would notice.
TEST(ShortcutRegistry, VIsFrameOnCursorAndSelectIsQ) {
  EXPECT_EQ(shortcuts::sequences(Id::FrameCursor).at(0), QKeySequence(Qt::Key_V));
  EXPECT_EQ(shortcuts::sequences(Id::ToolSelect).at(0), QKeySequence(Qt::Key_Q));
}

// Actions must BIND what the table says — otherwise the page documents one
// thing and the app does another.
TEST(ShortcutRegistry, ActionsBindWhatTheTableDocuments) {
  QUndoStack stack;
  Actions actions(stack);

  EXPECT_EQ(actions.tool_select->shortcuts(), shortcuts::sequences(Id::ToolSelect));
  EXPECT_EQ(actions.frame_cursor->shortcuts(), shortcuts::sequences(Id::FrameCursor));
  EXPECT_EQ(actions.frame_selection->shortcuts(), shortcuts::sequences(Id::FrameSelection));
  EXPECT_EQ(actions.view_orthographic->shortcuts(), shortcuts::sequences(Id::ViewOrthographic));
  EXPECT_EQ(actions.view_top->shortcuts(), shortcuts::sequences(Id::ViewTop));
  EXPECT_EQ(actions.tool_delete->shortcuts(), shortcuts::sequences(Id::ToolDelete));
  EXPECT_EQ(actions.tool_prop_point->shortcuts(), shortcuts::sequences(Id::ToolPropPoint));
  EXPECT_EQ(actions.tool_prop_curve->shortcuts(), shortcuts::sequences(Id::ToolPropCurve));
  EXPECT_EQ(actions.tool_prop_span->shortcuts(), shortcuts::sequences(Id::ToolPropSpan));
  EXPECT_EQ(actions.tool_prop_polygon->shortcuts(), shortcuts::sequences(Id::ToolPropPolygon));
  EXPECT_EQ(actions.tool_corner->shortcuts(), shortcuts::sequences(Id::ToolCorner));
  EXPECT_EQ(actions.help_contents->shortcuts(), shortcuts::sequences(Id::Help));
  EXPECT_EQ(actions.viewport_hints->shortcuts(), shortcuts::sequences(Id::ViewportHints));
}

// #333: the toggle is only useful if it reads as a toggle — checkable, and
// discoverable on the generated shortcuts page (which skips unbound rows).
TEST(ShortcutRegistry, ViewportHintsIsACheckableBoundViewRow) {
  QUndoStack stack;
  const Actions actions(stack);
  EXPECT_TRUE(actions.viewport_hints->isCheckable());
  EXPECT_FALSE(shortcuts::sequences(Id::ViewportHints).isEmpty());
  EXPECT_STREQ(shortcuts::entry(Id::ViewportHints).category, "View");
  EXPECT_EQ(shortcuts::entry(Id::ViewportHints).toolbar_group, nullptr); // menu-only
}

// The gate: the committed page must be what the table renders. A binding
// changed without regenerating the page fails HERE rather than shipping a lie.
// RM_DOCS_DIR is a compile define (editor/tests/CMakeLists.txt).
TEST(ShortcutRegistry, MarkdownMatchesCommittedShortcutsPage) {
  const std::filesystem::path page =
      std::filesystem::path(RM_DOCS_DIR) / "user-guide" / "shortcuts.md";
  std::ifstream file(page);
  ASSERT_TRUE(file.is_open()) << "missing " << page.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  const QString committed = QString::fromStdString(buffer.str());

  const QString generated = shortcuts::markdown();
  EXPECT_TRUE(committed.contains(generated))
      << "docs/user-guide/shortcuts.md is out of date with the shortcut table.\n"
         "Regenerate its tables from shortcuts::markdown():\n\n"
      << generated.toStdString();
}

} // namespace
} // namespace roadmaker::editor
