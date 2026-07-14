#include <gtest/gtest.h>

#include <QAbstractItemModelTester>

#include "document/document.hpp"
#include "document/scene_tree_model.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

TEST(SceneTreeModel, PassesQtModelSanityChecksEmpty) {
  Document document;
  SceneTreeModel model(document);
  // Fatal mode: any index/parent/rowCount contract violation aborts the test.
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  EXPECT_EQ(model.rowCount(), 2); // Roads + Junctions groups
  EXPECT_EQ(model.rowCount(model.index(0, 0)), 0);
}

TEST(SceneTreeModel, PassesQtModelSanityChecksLoadedAndReloaded) {
  Document document;
  SceneTreeModel model(document);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);

  ASSERT_TRUE(document.load(kSample).has_value());
  const QModelIndex roads_group = model.index(0, 0);
  EXPECT_EQ(model.rowCount(roads_group), static_cast<int>(document.network().road_count()));

  // Reload runs the full reset path under the tester too.
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_EQ(model.rowCount(model.index(0, 0)), static_cast<int>(document.network().road_count()));
}

TEST(SceneTreeModel, TargetRoundTripsThroughIndexLookup) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);

  document.network().for_each_road([&](RoadId road_id, const Road&) {
    const QModelIndex index = model.index_for_road(road_id);
    ASSERT_TRUE(index.isValid());
    EXPECT_EQ(model.target_for(index).road, road_id);
    EXPECT_FALSE(model.target_for(index).lane.is_valid());
  });

  // Every lane index round-trips with both ids set.
  const QModelIndex roads_group = model.index(0, 0);
  for (int r = 0; r < model.rowCount(roads_group); ++r) {
    const QModelIndex road_index = model.index(r, 0, roads_group);
    for (int s = 0; s < model.rowCount(road_index); ++s) {
      const QModelIndex section_index = model.index(s, 0, road_index);
      for (int l = 0; l < model.rowCount(section_index); ++l) {
        const QModelIndex lane_index = model.index(l, 0, section_index);
        const SceneTreeModel::Target target = model.target_for(lane_index);
        ASSERT_TRUE(target.lane.is_valid());
        EXPECT_EQ(model.index_for_lane(target.lane), lane_index);
      }
    }
  }
}

TEST(SceneTreeModel, JunctionNodeRoundTripsToASelectableTarget) {
  // Gate finding 4: a Junctions-tree node resolves to a junction target (road/
  // lane invalid) and round-trips through index_for_junction, so a tree click
  // selects the junction the same entity a floor pick does.
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);

  document.network().for_each_junction([&](JunctionId junction_id, const Junction&) {
    const QModelIndex index = model.index_for_junction(junction_id);
    ASSERT_TRUE(index.isValid());
    const SceneTreeModel::Target target = model.target_for(index);
    EXPECT_EQ(target.junction, junction_id);
    EXPECT_FALSE(target.road.is_valid());
    EXPECT_FALSE(target.lane.is_valid());
  });
}

TEST(SceneTreeModel, GroupHeadersYieldEmptyTargets) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);

  const SceneTreeModel::Target roads = model.target_for(model.index(0, 0));
  EXPECT_FALSE(roads.road.is_valid());
  EXPECT_FALSE(roads.lane.is_valid());
}

TEST(SceneTreeModel, LabelsAreHumanReadable) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);

  EXPECT_EQ(model.index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Roads"));
  EXPECT_EQ(model.index(1, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Junctions"));
  const QModelIndex first_road = model.index(0, 0, model.index(0, 0));
  EXPECT_FALSE(first_road.data(Qt::DisplayRole).toString().isEmpty());
}

} // namespace
} // namespace roadmaker::editor
