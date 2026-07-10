#include <gtest/gtest.h>

#include <QSignalSpy>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

RoadId first_road(const Document& document) {
  RoadId first;
  document.network().for_each_road([&](RoadId id, const Road&) {
    if (!first.is_valid()) {
      first = id;
    }
  });
  return first;
}

LaneId first_lane_of(const Document& document, RoadId road_id) {
  const Road* road = document.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return {};
  }
  const LaneSection* section = document.network().lane_section(road->sections.front());
  return (section == nullptr || section->lanes.empty()) ? LaneId{} : section->lanes.front();
}

TEST(SelectionModel, EmitsOnlyOnActualChange) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);

  const RoadId road = first_road(document);
  selection.select_road(road);
  EXPECT_EQ(spy.count(), 1);
  selection.select_road(road); // no-op: same selection
  EXPECT_EQ(spy.count(), 1);

  const LaneId lane = first_lane_of(document, road);
  ASSERT_TRUE(lane.is_valid());
  selection.select_lane(road, lane);
  EXPECT_EQ(spy.count(), 2);
  EXPECT_EQ(selection.road(), road);
  EXPECT_EQ(selection.lane(), lane);

  selection.clear();
  EXPECT_EQ(spy.count(), 3);
  selection.clear(); // already empty
  EXPECT_EQ(spy.count(), 3);
  EXPECT_TRUE(selection.empty());
}

TEST(SelectionModel, InvalidIdsClearSelection) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  selection.select_road(first_road(document));
  EXPECT_FALSE(selection.empty());

  selection.select_road(RoadId{}); // stale/invalid -> clear
  EXPECT_TRUE(selection.empty());
}

TEST(SelectionModel, ReloadClearsSelectionEvenWhenIdsAlias) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  selection.select_road(first_road(document));
  ASSERT_FALSE(selection.empty());

  // Reloading rebuilds the arenas from scratch: the old id would alias the
  // new file's first road, which lookups alone cannot detect. The model must
  // clear on loaded().
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_TRUE(selection.empty());
  EXPECT_EQ(spy.count(), 1);
}

} // namespace
} // namespace roadmaker::editor
