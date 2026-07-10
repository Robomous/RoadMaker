// Document preview session (docs/design/m2/01_editing_framework.md §3, gate
// issue #37): live mutation + re-mesh with NOTHING on the undo stack until
// commit pushes exactly one entry; cancel leaves write_xodr byte-identical.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QObject>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"

using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::editor::Document;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Fresh document with one junction-free three-node road; the undo stack
/// holds the create, so tests measure stack growth relative to base_count.
/// Document is a QObject (pinned in place), hence setup in the constructor.
struct Scene {
  Document document;
  RoadId road;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    auto created = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                      Waypoint{.x = 80.0, .y = 20.0},
                                      Waypoint{.x = 160.0, .y = 0.0}},
                                     roadmaker::LaneProfile::two_lane_default(),
                                     "Editable"));
    if (!created) {
      throw std::runtime_error(created.error().message);
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
      if (r.name == "Editable") {
        road = id;
      }
    });
    if (!road.is_valid()) {
      throw std::runtime_error("created road not found");
    }
    base_count = document.undo_stack()->count();
    base_xodr = xodr(document);
  }
};

std::unique_ptr<roadmaker::edit::Command>
move_middle(const RoadNetwork& network, RoadId road, Waypoint to) {
  return roadmaker::edit::move_waypoint(network, road, 1, to);
}

Waypoint middle_node(const Document& document, RoadId road) {
  const roadmaker::Road* road_ptr = document.network().road(road);
  if (road_ptr == nullptr || !road_ptr->authoring_waypoints) {
    throw std::runtime_error("road lost its waypoints");
  }
  return (*road_ptr->authoring_waypoints)[1];
}

} // namespace

TEST(PreviewSession, BeginAppliesAndRemeshesWithoutStackEntry) {
  Scene scene;
  std::vector<std::vector<RoadId>> payloads;
  QObject::connect(&scene.document,
                   &Document::mesh_changed,
                   &scene.document,
                   [&payloads](const std::vector<RoadId>& roads) { payloads.push_back(roads); });

  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());

  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_EQ(middle_node(scene.document, scene.road), (Waypoint{.x = 80.0, .y = 60.0}));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  // Geometry-only edit on a junction-free road rides the incremental path.
  ASSERT_EQ(payloads.size(), 1U);
  EXPECT_EQ(payloads[0], (std::vector<RoadId>{scene.road}));

  scene.document.cancel_preview();
}

TEST(PreviewSession, UntouchedRoadKeepsItsBuffersDuringPreview) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .push_command(roadmaker::edit::create_road(
                      {Waypoint{.x = 0.0, .y = 200.0}, Waypoint{.x = 160.0, .y = 200.0}},
                      roadmaker::LaneProfile::two_lane_default(),
                      "Bystander"))
                  .has_value());
  const double* bystander_positions = nullptr;
  for (const roadmaker::RoadMesh& mesh : scene.document.mesh().roads) {
    if (mesh.road != scene.road) {
      bystander_positions = mesh.positions.data();
    }
  }
  ASSERT_NE(bystander_positions, nullptr);

  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());

  for (const roadmaker::RoadMesh& mesh : scene.document.mesh().roads) {
    if (mesh.road != scene.road) {
      EXPECT_EQ(mesh.positions.data(), bystander_positions);
    }
  }
  scene.document.cancel_preview();
}

TEST(PreviewSession, CancelledSessionIsByteIdentical) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());
  ASSERT_TRUE(scene.document
                  .update_preview([&](const RoadNetwork& base) {
                    return move_middle(base, scene.road, Waypoint{.x = 90.0, .y = 45.0});
                  })
                  .has_value());

  scene.document.cancel_preview();

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(PreviewSession, CommitPushesExactlyOneEntryAndUndoRestoresBase) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 40.0}))
                  .has_value());
  for (const double y : {50.0, 60.0, 70.0}) {
    ASSERT_TRUE(scene.document
                    .update_preview([&](const RoadNetwork& base) {
                      return move_middle(base, scene.road, Waypoint{.x = 80.0, .y = y});
                    })
                    .has_value());
  }

  scene.document.commit_preview();

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(middle_node(scene.document, scene.road), (Waypoint{.x = 80.0, .y = 70.0}));
  const std::string committed = xodr(scene.document);

  // The regression the factory-based update_preview exists for: the pushed
  // command's "before" state must be the BASE, not a mid-drag frame.
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  scene.document.undo_stack()->redo();
  EXPECT_EQ(xodr(scene.document), committed);
}

TEST(PreviewSession, PushCommandIsRefusedWhileActive) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());

  EXPECT_FALSE(scene.document
                   .push_command(roadmaker::edit::rename_road(
                       scene.document.network(), scene.road, "Renamed"))
                   .has_value());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);

  scene.document.cancel_preview();
  EXPECT_TRUE(scene.document
                  .push_command(
                      roadmaker::edit::rename_road(scene.document.network(), scene.road, "Renamed"))
                  .has_value());
}

TEST(PreviewSession, BeginRejectsInvalidNullAndNested) {
  Scene scene;

  EXPECT_FALSE(scene.document.begin_preview(nullptr).has_value());
  EXPECT_FALSE(scene.document
                   .begin_preview(roadmaker::edit::move_waypoint(
                       scene.document.network(), RoadId{}, 1, Waypoint{.x = 0.0, .y = 0.0}))
                   .has_value());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);

  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());
  EXPECT_FALSE(scene.document
                   .begin_preview(move_middle(
                       scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 61.0}))
                   .has_value());
  EXPECT_TRUE(scene.document.preview_active()); // first session survives
  scene.document.cancel_preview();
}

TEST(PreviewSession, FailedUpdateKeepsLastGoodStateAndSession) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());

  EXPECT_FALSE(scene.document
                   .update_preview([](const RoadNetwork& base) {
                     return roadmaker::edit::move_waypoint(
                         base, RoadId{}, 1, Waypoint{.x = 0.0, .y = 0.0});
                   })
                   .has_value());
  EXPECT_FALSE(scene.document
                   .update_preview([](const RoadNetwork&) {
                     return std::unique_ptr<roadmaker::edit::Command>{};
                   })
                   .has_value());

  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_EQ(middle_node(scene.document, scene.road), (Waypoint{.x = 80.0, .y = 60.0}));

  scene.document.commit_preview();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(PreviewSession, SessionlessCallsAreSafe) {
  Scene scene;

  EXPECT_FALSE(scene.document
                   .update_preview([&](const RoadNetwork& base) {
                     return move_middle(base, scene.road, Waypoint{.x = 80.0, .y = 60.0});
                   })
                   .has_value());
  scene.document.commit_preview();
  scene.document.cancel_preview();

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(PreviewSession, LoadCancelsTheSession) {
  Scene scene;
  ASSERT_TRUE(scene.document
                  .begin_preview(move_middle(
                      scene.document.network(), scene.road, Waypoint{.x = 80.0, .y = 60.0}))
                  .has_value());

  const std::filesystem::path sample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";
  ASSERT_TRUE(scene.document.load(sample).has_value());

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), 0);
}
