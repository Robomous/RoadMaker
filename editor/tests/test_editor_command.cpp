#include <gtest/gtest.h>

#include <QUndoStack>

#include "document/editor_command.hpp"

namespace {

using roadmaker::editor::EditorCommand;

// Counts apply/revert calls so tests can assert exact dispatch behavior.
class CountingCommand final : public EditorCommand {
public:
  CountingCommand(int& applies, int& reverts, bool already_applied)
      : EditorCommand(QStringLiteral("counting"), already_applied), applies_(applies),
        reverts_(reverts) {}

private:
  void apply() override { ++applies_; }

  void revert() override { ++reverts_; }

  int& applies_;
  int& reverts_;
};

TEST(EditorCommand, PushAppliesExactlyOnce) {
  QUndoStack stack;
  int applies = 0;
  int reverts = 0;
  stack.push(new CountingCommand(applies, reverts, /*already_applied=*/false));
  EXPECT_EQ(applies, 1);
  EXPECT_EQ(reverts, 0);
}

TEST(EditorCommand, AlreadyAppliedSkipsOnlyTheFirstRedo) {
  QUndoStack stack;
  int applies = 0;
  int reverts = 0;
  // Preview-session commit: the work already happened before the push.
  stack.push(new CountingCommand(applies, reverts, /*already_applied=*/true));
  EXPECT_EQ(applies, 0);
  EXPECT_EQ(reverts, 0);

  stack.undo();
  EXPECT_EQ(reverts, 1);
  stack.redo(); // later redos must execute normally
  EXPECT_EQ(applies, 1);
  stack.undo();
  EXPECT_EQ(reverts, 2);
}

TEST(EditorCommand, UndoRedoSequenceBalances) {
  QUndoStack stack;
  int applies = 0;
  int reverts = 0;
  stack.push(new CountingCommand(applies, reverts, false));
  stack.undo();
  stack.redo();
  stack.undo();
  stack.redo();
  EXPECT_EQ(applies, 3);
  EXPECT_EQ(reverts, 2);
  EXPECT_TRUE(stack.canUndo());
  EXPECT_FALSE(stack.canRedo());
}

TEST(EditorCommand, MacroGroupsChildrenIntoOneUndoStep) {
  QUndoStack stack;
  int applies = 0;
  int reverts = 0;
  stack.beginMacro(QStringLiteral("delete selection"));
  stack.push(new CountingCommand(applies, reverts, false));
  stack.push(new CountingCommand(applies, reverts, false));
  stack.endMacro();
  ASSERT_EQ(stack.count(), 1);
  EXPECT_EQ(applies, 2);

  stack.undo();
  EXPECT_EQ(reverts, 2);
}

} // namespace
