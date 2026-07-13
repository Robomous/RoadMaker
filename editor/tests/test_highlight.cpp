#include <gtest/gtest.h>

#include <vector>

#include "document/highlight.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

constexpr RoadId kRoadA{.index = 1, .gen = 0};
constexpr RoadId kRoadB{.index = 2, .gen = 0};
constexpr LaneId kLaneA1{.index = 10, .gen = 0};
constexpr LaneId kLaneA2{.index = 11, .gen = 0};

TEST(HighlightState, NoneWhenNeitherSelectedNorHovered) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA1, selection, {}, {}), HighlightState::None);
}

TEST(HighlightState, RoadLevelSelectionHighlightsTheWholeRoad) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA1, selection, {}, {}), HighlightState::Selected);
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA2, selection, {}, {}), HighlightState::Selected);
  // A road-level mesh (marking / junction floor, lane invalid) matches too.
  EXPECT_EQ(highlight_state_for(kRoadA, {}, selection, {}, {}), HighlightState::Selected);
  // A different road is untouched.
  EXPECT_EQ(highlight_state_for(kRoadB, {}, selection, {}, {}), HighlightState::None);
}

TEST(HighlightState, LaneLevelSelectionHighlightsOnlyThatLane) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = kLaneA1}};
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA1, selection, {}, {}), HighlightState::Selected);
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA2, selection, {}, {}), HighlightState::None);
}

TEST(HighlightState, HoverHighlightsTheHoveredRoad) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA1, selection, kRoadA, {}), HighlightState::Hover);
  EXPECT_EQ(highlight_state_for(kRoadB, {}, selection, kRoadA, {}), HighlightState::None);
}

TEST(HighlightState, SelectionWinsOverHover) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(highlight_state_for(kRoadA, kLaneA1, selection, kRoadA, {}), HighlightState::Selected);
}

TEST(HighlightState, InvalidHoverIsInert) {
  const std::vector<SelectionEntry> selection;
  // An unset (invalid) hover id must not match a road-level (lane-invalid) mesh.
  EXPECT_EQ(highlight_state_for(kRoadA, {}, selection, {}, {}), HighlightState::None);
}

} // namespace
} // namespace roadmaker::editor
