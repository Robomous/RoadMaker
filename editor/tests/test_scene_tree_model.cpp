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

#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QUndoStack>
#include <string>

#include "document/document.hpp"
#include "document/scene_tree_model.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

/// Places `name` on the first road's lane -1 as ONE scenario command — what the
/// Actor tool pushes for a single click.
void place_actor(Document& document, const std::string& name) {
  std::string road_odr_id;
  document.network().for_each_road([&](RoadId, const Road& road) {
    if (road_odr_id.empty()) {
      road_odr_id = road.odr_id;
    }
  });
  ASSERT_FALSE(road_odr_id.empty());
  osc::LanePosition lane;
  lane.road_id = road_odr_id;
  lane.lane_id = "-1";
  lane.s = 5.0;
  ASSERT_TRUE(
      document
          .push_scenario_command(osc::edit::place_scenario_object(
              document.scenario(), osc::make_actor(osc::ActorKind::Car, name), osc::Position{lane}))
          .has_value());
}

TEST(SceneTreeModel, PassesQtModelSanityChecksEmpty) {
  Document document;
  SceneTreeModel model(document);
  // Fatal mode: any index/parent/rowCount contract violation aborts the test.
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  EXPECT_EQ(model.rowCount(), 3); // Roads + Junctions + Scenario groups
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
  EXPECT_EQ(model.index(2, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Scenario"));
  const QModelIndex first_road = model.index(0, 0, model.index(0, 0));
  EXPECT_FALSE(first_road.data(Qt::DisplayRole).toString().isEmpty());
}

// --- the Scenario branch (#246, GW-6 step 3) ---------------------------------

// ★ THE TESTER RUNS ACROSS THE SCENARIO MUTATIONS, not merely before and after
// them. rebuild() is a full reset, and a reset emitted without its
// begin/endResetModel pair is exactly the contract violation
// QAbstractItemModelTester exists to catch — it cannot be caught by inspecting
// the finished model.
TEST(SceneTreeModel, PassesQtModelSanityChecksAcrossScenarioEdits) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);

  const QModelIndex scenario_group = model.index(2, 0);
  ASSERT_TRUE(scenario_group.isValid());
  EXPECT_EQ(model.rowCount(scenario_group), 0);

  place_actor(document, "Car1");
  place_actor(document, "Car2");
  EXPECT_EQ(model.rowCount(model.index(2, 0)), 2);

  ASSERT_TRUE(
      document.push_scenario_command(osc::edit::remove_scenario_object(document.scenario(), "Car1"))
          .has_value());
  EXPECT_EQ(model.rowCount(model.index(2, 0)), 1);

  // An undo runs the reset path again, from the other direction.
  document.undo_stack()->undo();
  EXPECT_EQ(model.rowCount(model.index(2, 0)), 2);
}

// ★ THE RESET SIGNAL IS ITS OWN GATE, because QAbstractItemModelTester cannot
// be one here. The tester only observes SIGNALS; a rebuild() that clears and
// repopulates `nodes_` while emitting nothing is invisible to it, and every
// rowCount assertion still reads the correct new value. What breaks is the
// VIEW, which keeps QModelIndexes whose internalId now names a different node.
// Measured: dropping the begin/endResetModel pair leaves every other test in
// this file green.
TEST(SceneTreeModel, AScenarioEditEmitsExactlyOneModelReset) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);
  QSignalSpy resets(&model, &QAbstractItemModel::modelReset);

  place_actor(document, "Car1");
  EXPECT_EQ(resets.count(), 1) << "the scenario branch changed without a model reset";

  ASSERT_TRUE(
      document.push_scenario_command(osc::edit::remove_scenario_object(document.scenario(), "Car1"))
          .has_value());
  EXPECT_EQ(resets.count(), 2);

  document.undo_stack()->undo();
  EXPECT_EQ(resets.count(), 3);
}

TEST(SceneTreeModel, AnActorRowCarriesItsNameAndNoIdsAtAll) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  place_actor(document, "Car1");
  SceneTreeModel model(document);

  const QModelIndex index = model.index_for_actor("Car1");
  ASSERT_TRUE(index.isValid());
  const SceneTreeModel::Target target = model.target_for(index);
  EXPECT_EQ(target.actor, "Car1");
  // ★ GW-6 step 4, at the model layer: an actor row names no road, so a tree
  // click on it cannot select the road it stands on however the panel maps it.
  EXPECT_FALSE(target.road.is_valid());
  EXPECT_FALSE(target.lane.is_valid());
  EXPECT_FALSE(target.junction.is_valid());
  EXPECT_EQ(index.data(Qt::DisplayRole).toString(), QStringLiteral("Car1"));
}

TEST(SceneTreeModel, TheScenarioBranchIsPresentOnASceneWithNoScenario) {
  // The two existing groups are unconditional; a third that came and went would
  // make every top-level row index depend on document state.
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SceneTreeModel model(document);

  ASSERT_EQ(model.rowCount(), 3);
  EXPECT_EQ(model.rowCount(model.index(2, 0)), 0);
  const SceneTreeModel::Target group = model.target_for(model.index(2, 0));
  EXPECT_TRUE(group.actor.empty()) << "the group header resolved to an entity";
}

TEST(SceneTreeModel, AnUnknownActorNameYieldsNoIndex) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  place_actor(document, "Car1");
  SceneTreeModel model(document);

  EXPECT_FALSE(model.index_for_actor("NoSuchActor").isValid());
  EXPECT_FALSE(model.index_for_actor("").isValid());
}

} // namespace
} // namespace roadmaker::editor
