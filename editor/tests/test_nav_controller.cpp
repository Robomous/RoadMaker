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

#include <gtest/gtest.h>

#include <QPoint>
#include <Qt>

#include "viewport/nav_controller.hpp"

namespace roadmaker::editor {
namespace {

constexpr int kSlop = NavController::kContextDragSlop;

// --- the chord table (GW-1 steps 2-3, 5-6) ----------------------------------

TEST(ChordFor, MapsEveryDocumentedChord) {
  // ⌥ alone.
  EXPECT_EQ(chord_for(Qt::LeftButton, Qt::AltModifier), NavGesture::Orbit);
  EXPECT_EQ(chord_for(Qt::RightButton, Qt::AltModifier), NavGesture::ZoomDrag);
  EXPECT_EQ(chord_for(Qt::LeftButton | Qt::RightButton, Qt::AltModifier), NavGesture::Pan);
  EXPECT_EQ(chord_for(Qt::LeftButton | Qt::RightButton, Qt::AltModifier | Qt::ShiftModifier),
            NavGesture::PivotVertical);

  // The no-⌥ alternates.
  EXPECT_EQ(chord_for(Qt::MiddleButton, Qt::NoModifier), NavGesture::Pan);
  EXPECT_EQ(chord_for(Qt::RightButton, Qt::NoModifier), NavGesture::ContextPending);

  // MMB pans whatever else is held — it needs no modifier to mean pan.
  EXPECT_EQ(chord_for(Qt::MiddleButton, Qt::AltModifier | Qt::ShiftModifier), NavGesture::Pan);
}

TEST(ChordFor, PlainLmbIsNotNavigation) {
  EXPECT_EQ(chord_for(Qt::LeftButton, Qt::NoModifier), NavGesture::None);
  EXPECT_EQ(chord_for(Qt::NoButton, Qt::AltModifier), NavGesture::None);
  // Modifiers the tools own (⇧ additive select, ⌘/Ctrl) must not steal LMB.
  EXPECT_EQ(chord_for(Qt::LeftButton, Qt::ShiftModifier), NavGesture::None);
  EXPECT_EQ(chord_for(Qt::LeftButton, Qt::ControlModifier), NavGesture::None);
}

// --- press/move/release routing ---------------------------------------------

// The whole point of the gesture-first routing: an unmodified left press must
// still reach the gizmo and the active tool untouched.
TEST(NavController, PlainLmbIsNotConsumed) {
  NavController nav;
  EXPECT_FALSE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::NoModifier, {10, 10}));
  EXPECT_EQ(nav.gesture(), NavGesture::None);
  EXPECT_FALSE(nav.navigating());
  EXPECT_FALSE(nav.move({80, 80}, Qt::LeftButton, kSlop));

  bool context_click = true; // must be cleared even on an unclaimed release
  EXPECT_FALSE(nav.release(Qt::LeftButton, Qt::NoButton, &context_click));
  EXPECT_FALSE(context_click);
}

TEST(NavController, AltLeftDragOrbits) {
  NavController nav;
  EXPECT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
  EXPECT_EQ(nav.gesture(), NavGesture::Orbit);
  EXPECT_TRUE(nav.navigating());
  EXPECT_TRUE(nav.move({12, 12}, Qt::LeftButton, kSlop)); // no slop for ⌥ chords: live at once

  bool context_click = false;
  EXPECT_TRUE(nav.release(Qt::LeftButton, Qt::NoButton, &context_click));
  EXPECT_FALSE(context_click);
  EXPECT_EQ(nav.gesture(), NavGesture::None);
}

TEST(NavController, MiddleDragPansWithoutAlt) {
  NavController nav;
  EXPECT_TRUE(nav.press(Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier, {10, 10}));
  EXPECT_EQ(nav.gesture(), NavGesture::Pan);
  EXPECT_TRUE(nav.move({11, 11}, Qt::MiddleButton, kSlop));

  bool context_click = false;
  EXPECT_TRUE(nav.release(Qt::MiddleButton, Qt::NoButton, &context_click));
  EXPECT_EQ(nav.gesture(), NavGesture::None);
}

// --- the legacy RMB binding (kept as a documented alternate) -----------------

TEST(NavController, RightClickBelowSlopPopsTheContextMenu) {
  NavController nav;
  EXPECT_TRUE(nav.press(Qt::RightButton, Qt::RightButton, Qt::NoModifier, {40, 40}));
  EXPECT_EQ(nav.gesture(), NavGesture::ContextPending);
  // A pending click consumes no moves, so hover and the active tool see the
  // wiggle exactly as they did before P1.
  EXPECT_FALSE(nav.navigating());
  EXPECT_FALSE(nav.move({42, 41}, Qt::RightButton, kSlop));
  EXPECT_EQ(nav.gesture(), NavGesture::ContextPending);

  bool context_click = false;
  EXPECT_TRUE(nav.release(Qt::RightButton, Qt::NoButton, &context_click));
  EXPECT_TRUE(context_click);
  EXPECT_EQ(nav.gesture(), NavGesture::None);
}

TEST(NavController, RightDragPastSlopOrbitsAndSuppressesTheMenu) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::RightButton, Qt::RightButton, Qt::NoModifier, {40, 40}));
  EXPECT_TRUE(nav.move({40 + kSlop + 1, 40}, Qt::RightButton, kSlop));
  EXPECT_EQ(nav.gesture(), NavGesture::LegacyOrbit);
  EXPECT_TRUE(nav.navigating());

  bool context_click = false;
  EXPECT_TRUE(nav.release(Qt::RightButton, Qt::NoButton, &context_click));
  EXPECT_FALSE(context_click) << "a drag that orbited must not also pop the menu";
}

TEST(NavController, SlopIsMeasuredFromThePressPoint) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::RightButton, Qt::RightButton, Qt::NoModifier, {100, 100}));
  // Exactly at the slop is still a click; drifting back does not re-arm it.
  EXPECT_FALSE(nav.move({100 + kSlop, 100}, Qt::RightButton, kSlop));
  EXPECT_FALSE(nav.move({100, 100}, Qt::RightButton, kSlop));
  EXPECT_TRUE(nav.move({100, 100 + kSlop + 1}, Qt::RightButton, kSlop));
  EXPECT_EQ(nav.gesture(), NavGesture::LegacyOrbit);
  // Once orbiting, returning to the press point must not revert to pending.
  EXPECT_TRUE(nav.move({100, 100}, Qt::RightButton, kSlop));
  EXPECT_EQ(nav.gesture(), NavGesture::LegacyOrbit);
}

// --- two-button chords ------------------------------------------------------

TEST(NavController, AddingRightButtonUpgradesOrbitToPan) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
  ASSERT_EQ(nav.gesture(), NavGesture::Orbit);
  EXPECT_TRUE(
      nav.press(Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::AltModifier, {10, 10}));
  EXPECT_EQ(nav.gesture(), NavGesture::Pan);
}

TEST(NavController, ShiftOnTheTwoButtonChordLiftsThePivot) {
  NavController nav;
  ASSERT_TRUE(
      nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier | Qt::ShiftModifier, {10, 10}));
  ASSERT_EQ(nav.gesture(), NavGesture::Orbit) << "⇧ alone must not change the ⌥+LMB orbit";
  EXPECT_TRUE(nav.press(Qt::RightButton,
                        Qt::LeftButton | Qt::RightButton,
                        Qt::AltModifier | Qt::ShiftModifier,
                        {10, 10}));
  EXPECT_EQ(nav.gesture(), NavGesture::PivotVertical);
}

// A chord unwinds one button at a time, in either release order, and always
// lands back at None with nothing latched.
TEST(NavController, ReleaseOrderUnwindsChordsCleanly) {
  {
    NavController nav; // release RMB first: pan → orbit → idle
    ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
    ASSERT_TRUE(
        nav.press(Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::AltModifier, {10, 10}));
    ASSERT_EQ(nav.gesture(), NavGesture::Pan);

    bool context_click = false;
    EXPECT_TRUE(nav.release(Qt::RightButton, Qt::LeftButton, &context_click));
    EXPECT_EQ(nav.gesture(), NavGesture::Orbit);
    EXPECT_FALSE(context_click) << "an RMB that was part of a chord is not a context click";
    EXPECT_TRUE(nav.release(Qt::LeftButton, Qt::NoButton, &context_click));
    EXPECT_EQ(nav.gesture(), NavGesture::None);
  }
  {
    NavController nav; // release LMB first: pan → zoom-drag → idle
    ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
    ASSERT_TRUE(
        nav.press(Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::AltModifier, {10, 10}));

    bool context_click = false;
    EXPECT_TRUE(nav.release(Qt::LeftButton, Qt::RightButton, &context_click));
    EXPECT_EQ(nav.gesture(), NavGesture::ZoomDrag);
    EXPECT_FALSE(context_click);
    EXPECT_TRUE(nav.release(Qt::RightButton, Qt::NoButton, &context_click));
    EXPECT_EQ(nav.gesture(), NavGesture::None);
    EXPECT_FALSE(context_click) << "releasing an ⌥ zoom-drag must not pop the menu";
  }
}

// Releasing ⌥ mid-drag finishes the gesture instead of killing it: the chord is
// latched at the press. This is the documented trade-off in nav_controller.hpp.
TEST(NavController, GestureIsLatchedAtThePressAndSurvivesModifierRelease) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
  EXPECT_TRUE(nav.move({60, 60}, Qt::LeftButton, kSlop)); // the widget passes no modifiers here
  EXPECT_EQ(nav.gesture(), NavGesture::Orbit);
}

// Recovery from a release that never arrived: a WM that steals ⌥+drag to move
// the window, or focus lost mid-chord. Without this the camera would keep
// orbiting a button nobody is holding.
TEST(NavController, MoveWithNoButtonsHeldDropsALatchedGesture) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
  ASSERT_TRUE(nav.move({40, 40}, Qt::LeftButton, kSlop));

  EXPECT_FALSE(nav.move({90, 90}, Qt::NoButton, kSlop)) << "no buttons — nothing to consume";
  EXPECT_EQ(nav.gesture(), NavGesture::None);
  EXPECT_FALSE(nav.navigating());

  // And the next real press starts cleanly from idle.
  EXPECT_FALSE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::NoModifier, {90, 90}));
  EXPECT_EQ(nav.gesture(), NavGesture::None);
}

TEST(NavController, ResetAbandonsAnInFlightGesture) {
  NavController nav;
  ASSERT_TRUE(nav.press(Qt::LeftButton, Qt::LeftButton, Qt::AltModifier, {10, 10}));
  nav.reset();
  EXPECT_EQ(nav.gesture(), NavGesture::None);
  EXPECT_FALSE(nav.navigating());
  EXPECT_FALSE(nav.move({60, 60}, Qt::LeftButton, kSlop));
}

} // namespace
} // namespace roadmaker::editor
