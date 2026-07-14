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

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <array>
#include <stdexcept>
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
