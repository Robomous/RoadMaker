// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "document/highlight.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

constexpr RoadId kRoadA{.index = 1, .gen = 0};
constexpr RoadId kRoadB{.index = 2, .gen = 0};
constexpr LaneId kLaneA1{.index = 10, .gen = 0};
constexpr LaneId kLaneA2{.index = 11, .gen = 0};
constexpr ObjectId kTreeA{.index = 20, .gen = 0};
constexpr ObjectId kTreeB{.index = 21, .gen = 0};
constexpr JunctionId kJctA{.index = 30, .gen = 0};
constexpr JunctionId kJctB{.index = 31, .gen = 0};
constexpr SignalId kSignalA{.index = 40, .gen = 0};
constexpr SignalId kSignalB{.index = 41, .gen = 0};
constexpr SurfaceId kSurfaceA{.index = 50, .gen = 0};
constexpr SurfaceId kSurfaceB{.index = 51, .gen = 0};

// Convenience wrappers so each test reads at the level it cares about — a road/
// lane/prop mesh (no junction) or a junction floor (road/lane/object invalid).
HighlightState road_state(RoadId road,
                          LaneId lane,
                          ObjectId object,
                          std::span<const SelectionEntry> selection,
                          RoadId hovered_road = {},
                          LaneId hovered_lane = {},
                          ObjectId hovered_object = {}) {
  return highlight_state_for(road,
                             lane,
                             object,
                             {},
                             {},
                             {},
                             selection,
                             hovered_road,
                             hovered_lane,
                             hovered_object,
                             {},
                             {},
                             {});
}

HighlightState floor_state(JunctionId junction,
                           std::span<const SelectionEntry> selection,
                           JunctionId hovered_junction = {}) {
  return highlight_state_for(
      {}, {}, {}, {}, junction, {}, selection, {}, {}, {}, {}, hovered_junction, {});
}

HighlightState signal_state(SignalId signal,
                            std::span<const SelectionEntry> selection,
                            SignalId hovered_signal = {}) {
  return highlight_state_for(
      {}, {}, {}, signal, {}, {}, selection, {}, {}, {}, hovered_signal, {}, {});
}

HighlightState surface_state(SurfaceId surface,
                             std::span<const SelectionEntry> selection,
                             SurfaceId hovered_surface = {}) {
  return highlight_state_for(
      {}, {}, {}, {}, {}, surface, selection, {}, {}, {}, {}, {}, hovered_surface);
}

TEST(HighlightState, NoneWhenNeitherSelectedNorHovered) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(road_state(kRoadA, kLaneA1, {}, selection), HighlightState::None);
}

TEST(HighlightState, RoadLevelSelectionHighlightsTheWholeRoad) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(road_state(kRoadA, kLaneA1, {}, selection), HighlightState::Selected);
  EXPECT_EQ(road_state(kRoadA, kLaneA2, {}, selection), HighlightState::Selected);
  // A road-level mesh (marking, lane invalid) matches too.
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::Selected);
  // A different road is untouched.
  EXPECT_EQ(road_state(kRoadB, {}, {}, selection), HighlightState::None);
}

TEST(HighlightState, RoadSelectionDoesNotHighlightItsProps) {
  // A prop part belongs to road A but is a distinct entity — selecting the road
  // must not light its trees.
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(road_state(kRoadA, {}, kTreeA, selection), HighlightState::None);
}

TEST(HighlightState, LaneLevelSelectionHighlightsOnlyThatLane) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = kLaneA1}};
  EXPECT_EQ(road_state(kRoadA, kLaneA1, {}, selection), HighlightState::Selected);
  EXPECT_EQ(road_state(kRoadA, kLaneA2, {}, selection), HighlightState::None);
}

TEST(HighlightState, ObjectSelectionHighlightsOnlyThatProp) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}, .object = kTreeA}};
  EXPECT_EQ(road_state(kRoadA, {}, kTreeA, selection), HighlightState::Selected);
  // A different prop, and the owning road's surface, stay unlit.
  EXPECT_EQ(road_state(kRoadA, {}, kTreeB, selection), HighlightState::None);
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::None);
}

TEST(HighlightState, JunctionSelectionHighlightsOnlyThatFloor) {
  // Gate finding 4: a selected junction floor lights up, and only that floor —
  // never a road or a different junction.
  const std::vector<SelectionEntry> selection{{.junction = kJctA}};
  EXPECT_EQ(floor_state(kJctA, selection), HighlightState::Selected);
  EXPECT_EQ(floor_state(kJctB, selection), HighlightState::None);
  // A junction selection does not light a road-level mesh, and vice versa.
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::None);
  const std::vector<SelectionEntry> road_selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(floor_state(kJctA, road_selection), HighlightState::None);
}

TEST(HighlightState, SignalSelectionHighlightsOnlyThatSignal) {
  // A selected signal lights only its own pole — never another signal, and a
  // road/prop selection never lights a signal (distinct entity id spaces).
  const std::vector<SelectionEntry> selection{{.signal = kSignalA}};
  EXPECT_EQ(signal_state(kSignalA, selection), HighlightState::Selected);
  EXPECT_EQ(signal_state(kSignalB, selection), HighlightState::None);
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::None);
  const std::vector<SelectionEntry> road_selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(signal_state(kSignalA, road_selection), HighlightState::None);
}

TEST(HighlightState, HoverHighlightsTheHoveredRoad) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(road_state(kRoadA, kLaneA1, {}, selection, kRoadA), HighlightState::Hover);
  EXPECT_EQ(road_state(kRoadB, {}, {}, selection, kRoadA), HighlightState::None);
}

TEST(HighlightState, HoverHighlightsTheHoveredProp) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(road_state(kRoadA, {}, kTreeA, selection, {}, {}, kTreeA), HighlightState::Hover);
  // Hovering the road does not hover its prop, and vice versa.
  EXPECT_EQ(road_state(kRoadA, {}, kTreeA, selection, kRoadA), HighlightState::None);
}

TEST(HighlightState, HoverHighlightsTheHoveredFloor) {
  const std::vector<SelectionEntry> selection;
  EXPECT_EQ(floor_state(kJctA, selection, kJctA), HighlightState::Hover);
  EXPECT_EQ(floor_state(kJctB, selection, kJctA), HighlightState::None);
}

TEST(HighlightState, SurfaceSelectionAndHover) {
  // A ground surface (#215) matches only by surface id — never a road or floor,
  // and vice versa. Selected beats Hover, same as every other kind.
  const std::vector<SelectionEntry> selection{{.surface = kSurfaceA}};
  EXPECT_EQ(surface_state(kSurfaceA, selection), HighlightState::Selected);
  EXPECT_EQ(surface_state(kSurfaceB, selection), HighlightState::None);
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::None);

  const std::vector<SelectionEntry> none;
  EXPECT_EQ(surface_state(kSurfaceA, none, kSurfaceA), HighlightState::Hover);
  EXPECT_EQ(surface_state(kSurfaceB, none, kSurfaceA), HighlightState::None);
}

TEST(HighlightState, SelectionWinsOverHover) {
  const std::vector<SelectionEntry> selection{{.road = kRoadA, .lane = {}}};
  EXPECT_EQ(road_state(kRoadA, kLaneA1, {}, selection, kRoadA), HighlightState::Selected);
}

TEST(HighlightState, InvalidHoverIsInert) {
  const std::vector<SelectionEntry> selection;
  // An unset (invalid) hover id must not match a road-level (lane-invalid) mesh.
  EXPECT_EQ(road_state(kRoadA, {}, {}, selection), HighlightState::None);
}

} // namespace
} // namespace roadmaker::editor
