// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// The scrub gesture in isolation: ScrubLabel knows pixels and modifiers, never
// units or values. Its consumer's side of the contract (one preview session per
// gesture = one undo entry) is covered in test_panels.cpp.

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTest>

#include "panels/scrub_label.hpp"

namespace roadmaker::editor {
namespace {

/// Drives a synthetic drag: press at x, move through each x in `xs`, and
/// (unless `hold` is set) release at the last one.
void drag(ScrubLabel& label,
          int from_x,
          const std::vector<int>& xs,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier,
          bool hold = false) {
  QTest::mousePress(&label, Qt::LeftButton, Qt::NoModifier, QPoint(from_x, 5));
  for (const int x : xs) {
    // QTest::mouseMove does not carry modifiers, so post the move directly.
    QMouseEvent move(
        QEvent::MouseMove, QPointF(x, 5), QPointF(x, 5), Qt::NoButton, Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&label, &move);
  }
  if (!hold) {
    QTest::mouseRelease(
        &label, Qt::LeftButton, Qt::NoModifier, QPoint(xs.empty() ? from_x : xs.back(), 5));
  }
}

TEST(ScrubLabel, ClickWithinTheSlopEmitsNothing) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy started(&label, &ScrubLabel::scrub_started);
  QSignalSpy moved(&label, &ScrubLabel::scrub_moved);
  QSignalSpy finished(&label, &ScrubLabel::scrub_finished);

  drag(label, 20, {20 + ScrubLabel::kDragSlop});

  EXPECT_EQ(started.count(), 0) << "a click on a label must not open a session";
  EXPECT_EQ(moved.count(), 0);
  EXPECT_EQ(finished.count(), 0);
  EXPECT_FALSE(label.scrubbing());
}

TEST(ScrubLabel, HorizontalDragEmitsARunningPixelDelta) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy started(&label, &ScrubLabel::scrub_started);
  QSignalSpy moved(&label, &ScrubLabel::scrub_moved);
  QSignalSpy finished(&label, &ScrubLabel::scrub_finished);

  drag(label, 20, {60, 80});

  EXPECT_EQ(started.count(), 1);
  ASSERT_EQ(moved.count(), 2);
  // Deltas are measured from the PRESS, not the previous move: 60−20, 80−20.
  EXPECT_DOUBLE_EQ(moved.at(0).at(0).toDouble(), 40.0);
  EXPECT_DOUBLE_EQ(moved.at(1).at(0).toDouble(), 60.0);
  EXPECT_EQ(finished.count(), 1);
  EXPECT_FALSE(label.scrubbing());
}

TEST(ScrubLabel, DraggingLeftGivesANegativeDelta) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy moved(&label, &ScrubLabel::scrub_moved);
  drag(label, 100, {40});
  ASSERT_EQ(moved.count(), 1);
  EXPECT_DOUBLE_EQ(moved.at(0).at(0).toDouble(), -60.0);
}

TEST(ScrubLabel, ShiftIsFineAndControlIsCoarse) {
  {
    ScrubLabel label(QStringLiteral("Width"));
    QSignalSpy moved(&label, &ScrubLabel::scrub_moved);
    drag(label, 20, {120}, Qt::ShiftModifier);
    ASSERT_EQ(moved.count(), 1);
    EXPECT_DOUBLE_EQ(moved.at(0).at(0).toDouble(), 100.0 * ScrubLabel::kFineMultiplier);
  }
  {
    ScrubLabel label(QStringLiteral("Width"));
    QSignalSpy moved(&label, &ScrubLabel::scrub_moved);
    drag(label, 20, {120}, Qt::ControlModifier);
    ASSERT_EQ(moved.count(), 1);
    EXPECT_DOUBLE_EQ(moved.at(0).at(0).toDouble(), 100.0 * ScrubLabel::kCoarseMultiplier);
  }
}

// The multiplier applies to the motion made WHILE it is held. Scaling the
// running total instead would make the value jump the instant ⇧ goes down.
TEST(ScrubLabel, ModifierAppliesOnlyToTheMotionMadeWhileItIsHeld) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy moved(&label, &ScrubLabel::scrub_moved);

  QTest::mousePress(&label, Qt::LeftButton, Qt::NoModifier, QPoint(20, 5));
  const auto move_to = [&label](int x, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(
        QEvent::MouseMove, QPointF(x, 5), QPointF(x, 5), Qt::NoButton, Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&label, &event);
  };
  move_to(70, Qt::NoModifier);     // +50 at 1.0  → 50
  move_to(120, Qt::ShiftModifier); // +50 at 0.1  → 55
  move_to(70, Qt::NoModifier);     // −50 at 1.0  → 5

  ASSERT_EQ(moved.count(), 3);
  EXPECT_DOUBLE_EQ(moved.at(0).at(0).toDouble(), 50.0);
  EXPECT_DOUBLE_EQ(moved.at(1).at(0).toDouble(), 55.0);
  EXPECT_DOUBLE_EQ(moved.at(2).at(0).toDouble(), 5.0);
}

TEST(ScrubLabel, EscapeCancelsTheGestureInsteadOfFinishingIt) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy finished(&label, &ScrubLabel::scrub_finished);
  QSignalSpy cancelled(&label, &ScrubLabel::scrub_cancelled);

  drag(label, 20, {90}, Qt::NoModifier, /*hold=*/true);
  ASSERT_TRUE(label.scrubbing());
  QTest::keyClick(&label, Qt::Key_Escape);

  EXPECT_EQ(cancelled.count(), 1);
  EXPECT_EQ(finished.count(), 0) << "a cancelled scrub must never also commit";
  EXPECT_FALSE(label.scrubbing());

  // The release that follows the Esc is inert — the gesture is already over.
  QTest::mouseRelease(&label, Qt::LeftButton, Qt::NoModifier, QPoint(90, 5));
  EXPECT_EQ(finished.count(), 0);
  EXPECT_EQ(cancelled.count(), 1);
}

TEST(ScrubLabel, EscapeWithoutADragDoesNothing) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy cancelled(&label, &ScrubLabel::scrub_cancelled);
  QTest::keyClick(&label, Qt::Key_Escape);
  EXPECT_EQ(cancelled.count(), 0);
}

TEST(ScrubLabel, RightButtonIsNotAScrub) {
  ScrubLabel label(QStringLiteral("Width"));
  QSignalSpy started(&label, &ScrubLabel::scrub_started);
  QTest::mousePress(&label, Qt::RightButton, Qt::NoModifier, QPoint(20, 5));
  QTest::mouseRelease(&label, Qt::RightButton, Qt::NoModifier, QPoint(90, 5));
  EXPECT_EQ(started.count(), 0);
}

} // namespace
} // namespace roadmaker::editor
