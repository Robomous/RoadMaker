// Headless first-run tour state machine (UI-revamp Phase 4). The overlay only
// paints current() and forwards Next/Skip, so all step logic is tested here.

#include <gtest/gtest.h>

#include <vector>

#include "app/tour_controller.hpp"

namespace roadmaker::editor {
namespace {

std::vector<TourStep> three_steps() {
  return {TourStep{QStringLiteral("A"), QStringLiteral("body a"), QStringLiteral("Road")},
          TourStep{QStringLiteral("B"), QStringLiteral("body b"), QString()},
          TourStep{QStringLiteral("C"), QStringLiteral("body c"), QStringLiteral("Export")}};
}

TEST(TourController, InactiveUntilStarted) {
  TourController tour(three_steps());
  EXPECT_FALSE(tour.active());
  EXPECT_EQ(tour.current(), nullptr);
  EXPECT_FALSE(tour.completed());
}

TEST(TourController, StartShowsTheFirstStep) {
  TourController tour(three_steps());
  tour.start();
  ASSERT_TRUE(tour.active());
  ASSERT_NE(tour.current(), nullptr);
  EXPECT_EQ(tour.current()->title, "A");
  EXPECT_EQ(tour.index(), 0U);
  EXPECT_EQ(tour.count(), 3U);
  EXPECT_FALSE(tour.on_last_step());
}

TEST(TourController, NextWalksToTheEndThenFinishes) {
  TourController tour(three_steps());
  tour.start();
  tour.next();
  ASSERT_NE(tour.current(), nullptr);
  EXPECT_EQ(tour.current()->title, "B");
  tour.next();
  EXPECT_EQ(tour.current()->title, "C");
  EXPECT_TRUE(tour.on_last_step());

  tour.next(); // off the last step
  EXPECT_FALSE(tour.active());
  EXPECT_EQ(tour.current(), nullptr);
  EXPECT_TRUE(tour.completed());
}

TEST(TourController, SkipEndsImmediatelyAndCounts) {
  TourController tour(three_steps());
  tour.start();
  tour.next();
  tour.skip();
  EXPECT_FALSE(tour.active());
  EXPECT_TRUE(tour.completed());
  EXPECT_EQ(tour.current(), nullptr);
}

TEST(TourController, NextAndSkipAreInertWhenInactive) {
  TourController tour(three_steps());
  tour.next(); // never started
  tour.skip();
  EXPECT_FALSE(tour.active());
  EXPECT_FALSE(tour.completed());
}

TEST(TourController, EmptyTourStartsAlreadyFinished) {
  TourController tour({});
  tour.start();
  EXPECT_FALSE(tour.active());
  EXPECT_TRUE(tour.completed());
  EXPECT_EQ(tour.current(), nullptr);
}

TEST(TourController, DefaultTourHasFiveStepsTargetingRealActions) {
  const std::vector<TourStep> steps = default_tour_steps();
  ASSERT_EQ(steps.size(), 5U);
  // Every step has a title + body; the targets name toolbar action iconTexts.
  for (const TourStep& step : steps) {
    EXPECT_FALSE(step.title.isEmpty());
    EXPECT_FALSE(step.body.isEmpty());
    EXPECT_FALSE(step.target.isEmpty());
  }
  EXPECT_EQ(steps.front().target, "Road");
  EXPECT_EQ(steps.back().target, "Export");
}

} // namespace
} // namespace roadmaker::editor
