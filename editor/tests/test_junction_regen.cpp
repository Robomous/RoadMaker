// Junction regeneration after a node drag (gate finding 2): dragging a node of
// a junction's incoming arm through the real preview→commit path must leave the
// connecting roads coincident with the moved arm, not frozen. Drives the
// Document preview API (the same path node_drag/EditNodesTool commit through)
// and checks the weld report the generator itself uses.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
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

/// Three arms pointing at the origin, joined into a junction, in a Document.
struct JunctionScene {
  Document document;
  RoadId west;
  RoadId east;
  RoadId south;
  JunctionId junction;

  JunctionScene() {
    west = make(-40.0, 0.0, -6.0, 0.0, "1");
    east = make(40.0, 0.0, 6.0, 0.0, "2");
    south = make(0.0, -40.0, 0.0, -6.0, "3");
    const std::array<RoadEnd, 3> ends{RoadEnd{west, ContactPoint::End},
                                      RoadEnd{east, ContactPoint::End},
                                      RoadEnd{south, ContactPoint::End}};
    if (!document.push_command(roadmaker::edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction create failed");
    }
    document.network().for_each_junction(
        [&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  }

  RoadId make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                         LaneProfile::two_lane_default(),
                                         odr))) {
      throw std::runtime_error("road create failed");
    }
    RoadId id;
    document.network().for_each_road([&](RoadId rid, const roadmaker::Road& road) {
      if (road.odr_id == odr) {
        id = rid;
      }
    });
    return id;
  }
};

} // namespace

TEST(JunctionRegen, DraggingAnArmNodeKeepsTheConnectingRoadsCoincident) {
  JunctionScene scene;
  const std::size_t connections =
      scene.document.network().junction(scene.junction)->connections.size();
  ASSERT_GT(connections, 0U);
  QSignalSpy skipped(&scene.document, &Document::regeneration_skipped);

  // Drag the west arm's far node (index 0) through the preview→commit path,
  // changing the arm's approach angle at the junction.
  ASSERT_TRUE(scene.document
                  .begin_preview(roadmaker::edit::move_waypoint(
                      scene.document.network(), scene.west, 0, Waypoint{.x = -40.0, .y = 6.0}))
                  .has_value());
  ASSERT_TRUE(scene.document
                  .update_preview([&](const roadmaker::RoadNetwork& base) {
                    return roadmaker::edit::move_waypoint(
                        base, scene.west, 0, Waypoint{.x = -40.0, .y = 14.0});
                  })
                  .has_value());
  scene.document.commit_preview();

  // The junction followed: no skip, every connection kept, welds coincide.
  EXPECT_EQ(skipped.count(), 0);
  EXPECT_EQ(scene.document.network().junction(scene.junction)->connections.size(), connections);
  const auto welds =
      roadmaker::edit::verify_junction_welds(scene.document.network(), scene.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
  EXPECT_LE(welds->max_position_gap, roadmaker::tol::kWeldPosition);
}

// Dragging node 0 swings the WEST arm's far end, so its junction-facing contact
// point barely moves and its approach heading changes a lot. A connector that
// does not follow therefore leaves a heading gap at the weld — which is exactly
// what verify_junction_welds reports, and what a user sees as a kinked joint.
TEST(JunctionRegen, ConnectingRoadsFollowTheArmMidDragNotOnlyOnRelease) {
  JunctionScene scene;
  QSignalSpy skipped(&scene.document, &Document::regeneration_skipped);
  const Waypoint swung{.x = -40.0, .y = 14.0};

  // The old behaviour, for contrast: a plain move previews the arm and leaves
  // the connectors where they were, so mid-drag the joint is visibly broken.
  ASSERT_TRUE(scene.document
                  .begin_preview(roadmaker::edit::move_waypoint(
                      scene.document.network(), scene.west, 0, swung))
                  .has_value());
  const auto stale =
      roadmaker::edit::verify_junction_welds(scene.document.network(), scene.junction);
  ASSERT_TRUE(stale.has_value());
  EXPECT_TRUE(stale->breaches) << "the drag-time weld should breach without live follow — "
                                  "if it does not, this test proves nothing";
  const double stale_heading_gap = stale->max_heading_gap;
  scene.document.cancel_preview();

  // The new behaviour: one preview frame, no commit anywhere below this line.
  ASSERT_TRUE(scene.document
                  .begin_preview(roadmaker::edit::move_waypoint_following_junctions(
                      scene.document.network(), scene.west, 0, swung))
                  .has_value());

  const auto followed =
      roadmaker::edit::verify_junction_welds(scene.document.network(), scene.junction);
  ASSERT_TRUE(followed.has_value());
  EXPECT_FALSE(followed->breaches);
  EXPECT_LE(followed->max_position_gap, roadmaker::tol::kWeldPosition);
  EXPECT_LT(followed->max_heading_gap, stale_heading_gap);
  EXPECT_EQ(skipped.count(), 0);
  EXPECT_TRUE(scene.document.preview_active()); // still mid-drag

  // Still a session: cancelling restores the connectors too, byte-identically.
  const std::string base = xodr(scene.document);
  scene.document.cancel_preview();
  EXPECT_NE(xodr(scene.document), base);
}

TEST(JunctionRegen, LiveFollowCommitsExactlyOneUndoEntry) {
  JunctionScene scene;
  const int base_count = scene.document.undo_stack()->count();
  const std::string base_xodr = xodr(scene.document);

  ASSERT_TRUE(scene.document
                  .begin_preview(roadmaker::edit::move_waypoint_following_junctions(
                      scene.document.network(), scene.west, 0, Waypoint{.x = -40.0, .y = 6.0}))
                  .has_value());
  ASSERT_TRUE(scene.document
                  .update_preview([&](const roadmaker::RoadNetwork& base) {
                    return roadmaker::edit::move_waypoint_following_junctions(
                        base, scene.west, 0, Waypoint{.x = -40.0, .y = 14.0});
                  })
                  .has_value());
  scene.document.commit_preview(/*already_regenerated=*/true);

  // The move and every regeneration are ONE entry, and undo is byte-identical
  // — the commit must not have regenerated a second time on top.
  EXPECT_EQ(scene.document.undo_stack()->count(), base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), base_xodr);
}

TEST(JunctionRegen, DirectCommitPathMatchesTheDragResult) {
  JunctionScene scene;
  QSignalSpy skipped(&scene.document, &Document::regeneration_skipped);
  // The same move via push_command (gw1's direct path) regenerates identically.
  ASSERT_TRUE(scene.document
                  .push_command(roadmaker::edit::move_waypoint(
                      scene.document.network(), scene.west, 0, Waypoint{.x = -40.0, .y = 14.0}))
                  .has_value());
  EXPECT_EQ(skipped.count(), 0);
  const auto welds =
      roadmaker::edit::verify_junction_welds(scene.document.network(), scene.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}

// The P2 case: a lane added to a junction arm used to make the regeneration
// refuse ("changed the connection count") and the editor swallow it into a
// warning toast, freezing the junction. It must now regenerate silently, and
// the junction must genuinely grow.
TEST(JunctionRegen, AddingADrivingLaneToAnArmRegeneratesWithoutAToast) {
  JunctionScene scene;
  const std::size_t before =
      scene.document.network().junction(scene.junction)->connections.size();
  QSignalSpy skipped(&scene.document, &Document::regeneration_skipped);

  // One extra outgoing lane on east and one extra incoming lane on west open a
  // second west->east movement; the min(incoming, outgoing) pairing needs both.
  const auto add = [&](RoadId road, int side) {
    return scene.document.push_command(
        roadmaker::edit::add_lane(scene.document.network(),
                                  scene.document.network().road(road)->sections.front(),
                                  side,
                                  roadmaker::LaneType::Driving));
  };
  ASSERT_TRUE(add(scene.west, -1).has_value());
  ASSERT_TRUE(add(scene.east, 1).has_value());

  EXPECT_EQ(skipped.count(), 0) << "no 'junction not updated' toast";
  EXPECT_GT(scene.document.network().junction(scene.junction)->connections.size(), before);
  const auto welds =
      roadmaker::edit::verify_junction_welds(scene.document.network(), scene.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}
