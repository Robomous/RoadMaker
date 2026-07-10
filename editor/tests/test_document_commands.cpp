// Document::push_command bridge: apply-once semantics, undo/redo through
// the kernel, failed applies never pushed, stack cleared on load.

#include "roadmaker/edit/operations.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <filesystem>
#include <string>

#include "document/document.hpp"

using roadmaker::RoadId;
using roadmaker::editor::Document;

namespace {

std::filesystem::path sample() {
  return std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";
}

RoadId any_road(const Document& document) {
  RoadId found;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) {
    if (!found.is_valid()) {
      found = id;
    }
  });
  return found;
}

} // namespace

TEST(DocumentCommands, PushAppliesOnceAndEnablesUndo) {
  Document document;
  ASSERT_TRUE(document.load(sample()).has_value());
  const RoadId road = any_road(document);
  const std::string original = document.network().road(road)->name;

  QSignalSpy mesh_spy(&document, &Document::mesh_changed);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::rename_road(document.network(), road, "Renamed"))
          .has_value());

  EXPECT_EQ(document.network().road(road)->name, "Renamed"); // applied exactly once
  EXPECT_EQ(mesh_spy.count(), 1);
  EXPECT_TRUE(document.undo_stack()->canUndo());
  EXPECT_FALSE(document.undo_stack()->canRedo());

  document.undo_stack()->undo();
  EXPECT_EQ(document.network().road(road)->name, original);
  document.undo_stack()->redo();
  EXPECT_EQ(document.network().road(road)->name, "Renamed");
}

TEST(DocumentCommands, FailedApplyIsNotPushedAndDiagnosed) {
  Document document;
  ASSERT_TRUE(document.load(sample()).has_value());
  const std::size_t diagnostics_before = document.diagnostics().size();

  QSignalSpy diag_spy(&document, &Document::diagnostics_changed);
  const auto result =
      document.push_command(roadmaker::edit::rename_road(document.network(), RoadId{}, "x"));

  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(document.undo_stack()->canUndo());
  EXPECT_EQ(document.diagnostics().size(), diagnostics_before + 1);
  EXPECT_EQ(diag_spy.count(), 1);
  EXPECT_FALSE(document.push_command(nullptr).has_value());
}

TEST(DocumentCommands, TopologyChangedFiresOnlyForTopologyEdits) {
  Document document;
  ASSERT_TRUE(document.load(sample()).has_value());
  const RoadId road = any_road(document);

  QSignalSpy topology_spy(&document, &Document::topology_changed);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::rename_road(document.network(), road, "Renamed"))
          .has_value());
  EXPECT_EQ(topology_spy.count(), 0);

  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::create_road({roadmaker::Waypoint{.x = 500.0, .y = 500.0},
                                                      roadmaker::Waypoint{.x = 580.0, .y = 510.0}},
                                                     roadmaker::LaneProfile::two_lane_default(),
                                                     "New Road"))
          .has_value());
  EXPECT_EQ(topology_spy.count(), 1);

  document.undo_stack()->undo(); // undoing the create is also a topology change
  EXPECT_EQ(topology_spy.count(), 2);
}

TEST(DocumentCommands, UndoStackClearsOnLoad) {
  Document document;
  ASSERT_TRUE(document.load(sample()).has_value());
  const RoadId road = any_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::rename_road(document.network(), road, "Renamed"))
          .has_value());
  ASSERT_TRUE(document.undo_stack()->canUndo());

  ASSERT_TRUE(document.load(sample()).has_value());
  EXPECT_FALSE(document.undo_stack()->canUndo());
  EXPECT_FALSE(document.undo_stack()->canRedo());
}
