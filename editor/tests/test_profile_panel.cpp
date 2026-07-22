// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Profile panel (hardening sprint WS-C): elevation node/grade edits as
// commands through Document (drags = one preview session + one undo entry),
// insert/delete, the overpass workflow, and the crossing detector. Offscreen;
// the panel's public entry points are driven directly — the mouse handlers
// call the same methods.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "document/document.hpp"
#include "document/elevation_utils.hpp"
#include "document/selection_model.hpp"
#include "panels/profile_panel.hpp"

using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::ProfilePanel;
using roadmaker::editor::SelectionModel;

namespace {

RoadId make_road(Document& document, Waypoint a, Waypoint b) {
  if (!document.push_command(
          roadmaker::edit::create_road({a, b}, roadmaker::LaneProfile::two_lane_default(), ""))) {
    throw std::runtime_error("road setup failed");
  }
  RoadId last;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { last = id; });
  return last;
}

struct Rig {
  Document document;
  SelectionModel selection{document};
  ProfilePanel panel{document, selection};
  RoadId road;

  explicit Rig(double length = 120.0) {
    road = make_road(document, Waypoint{0.0, 0.0}, Waypoint{length, 0.0});
    selection.select({.road = road, .lane = {}});
  }
};

TEST(ProfilePanel, SelectionLoadsTheFlatProfileNodes) {
  Rig rig;
  ASSERT_EQ(rig.panel.road(), rig.road);
  ASSERT_EQ(rig.panel.nodes().size(), 2U); // flat road → the two end nodes
  EXPECT_DOUBLE_EQ(rig.panel.nodes().front().z, 0.0);
}

TEST(ProfilePanel, NodeDragIsOnePreviewSessionAndOneUndoEntry) {
  Rig rig;
  const int base_entries = rig.document.undo_stack()->count();

  rig.panel.drag_node(1, 1.0);
  rig.panel.drag_node(1, 2.5); // updates against the drag base, not cumulative
  EXPECT_TRUE(rig.document.preview_active());
  rig.panel.commit_drag();
  EXPECT_FALSE(rig.document.preview_active());

  EXPECT_EQ(rig.document.undo_stack()->count(), base_entries + 1);
  const roadmaker::Road& road = *rig.document.network().road(rig.road);
  EXPECT_NEAR(roadmaker::eval_profile(road.elevation, road.length), 2.5, 1e-9);

  rig.document.undo_stack()->undo();
  EXPECT_TRUE(rig.document.network().road(rig.road)->elevation.empty());
}

TEST(ProfilePanel, CancelledDragLeavesTheDocumentByteIdentical) {
  Rig rig;
  const auto before = roadmaker::write_xodr(rig.document.network(), "p");
  rig.panel.drag_node(0, 3.0);
  rig.panel.cancel_drag();
  EXPECT_EQ(*roadmaker::write_xodr(rig.document.network(), "p"), *before);
}

TEST(ProfilePanel, GradeHandleLocksTheTangent) {
  Rig rig;
  rig.panel.drag_grade(0, 0.10);
  rig.panel.commit_drag();
  const roadmaker::Road& road = *rig.document.network().road(rig.road);
  ASSERT_FALSE(road.elevation.empty());
  EXPECT_NEAR(road.elevation.front().eval_derivative(0.0), 0.10, 1e-9);
}

TEST(ProfilePanel, InsertAndRemoveNodesAreSingleCommands) {
  Rig rig;
  // A flat (all-zero) profile is written as NO profile by kernel policy, so
  // give the road some elevation first — nodes persist once non-zero.
  rig.panel.drag_node(1, 2.0);
  rig.panel.commit_drag();
  rig.panel.insert_node(60.0);
  ASSERT_EQ(rig.panel.nodes().size(), 3U);
  rig.panel.remove_node(1);
  EXPECT_EQ(rig.panel.nodes().size(), 2U);
  // A profile keeps at least one node.
  rig.panel.remove_node(0);
  rig.panel.remove_node(0);
  EXPECT_EQ(rig.panel.nodes().size(), 1U);
}

TEST(ProfilePanel, OverpassClearsTheCrossingRoadAndKeepsGradesSane) {
  Rig rig(200.0);
  // A second road crossing the first at right angles near s=100.
  make_road(rig.document, Waypoint{100.0, -80.0}, Waypoint{100.0, 80.0});

  const auto crossings =
      roadmaker::editor::elevation::find_crossings(rig.document.network(), rig.road);
  ASSERT_EQ(crossings.size(), 1U);
  EXPECT_NEAR(crossings.front().s_self, 100.0, 1.5);

  ASSERT_TRUE(rig.panel.apply_overpass(true));
  const roadmaker::Road& road = *rig.document.network().road(rig.road);
  // Clearance respected at the crossing (crossed road is flat at z=0).
  EXPECT_NEAR(roadmaker::eval_profile(road.elevation, crossings.front().s_self),
              rig.panel.clearance(),
              1e-6);
  // Monotone s and bounded grade by construction.
  EXPECT_LE(rig.panel.max_grade(), 0.13);
  // ONE undo entry restores the flat road.
  rig.document.undo_stack()->undo();
  EXPECT_TRUE(rig.document.network().road(rig.road)->elevation.empty());
}

TEST(ProfilePanel, OverpassWithoutCrossingsRefuses) {
  Rig rig;
  EXPECT_FALSE(rig.panel.apply_overpass(true));
}

TEST(ProfilePanel, OverpassCreatesNoTopologyAtTheCrossing) {
  // Gate finding 4 diagnosis (editor path): the maintainer's "overpass produced
  // a junction-like area" is NOT a junction — apply_overpass is pure elevation.
  // Drive it through the panel and assert no junction/link is ever created at
  // the crossing (the crossing-road stays a plain, independent road).
  Rig rig(200.0);
  const RoadId crosser = make_road(rig.document, Waypoint{100.0, -80.0}, Waypoint{100.0, 80.0});
  ASSERT_EQ(rig.document.network().junction_count(), 0U);

  ASSERT_TRUE(rig.panel.apply_overpass(true));

  EXPECT_EQ(rig.document.network().junction_count(), 0U) << "overpass must not create a junction";
  // Neither road gained a junction back-reference or a link into the other.
  EXPECT_FALSE(rig.document.network().road(rig.road)->junction.is_valid());
  EXPECT_FALSE(rig.document.network().road(crosser)->junction.is_valid());
}

TEST(ProfilePanel, DeletingACrossingRoadLeavesTheOverpassIntact) {
  // Gate finding 4 integrity (editor path): with an overpass applied, deleting
  // the crossed road must leave the overpass road's geometry, elevation profile,
  // and the network validity fully intact — no cascade, no corruption.
  Rig rig(200.0);
  const RoadId crossed = rig.road;
  const RoadId overpass = make_road(rig.document, Waypoint{100.0, -80.0}, Waypoint{100.0, 80.0});
  rig.selection.select({.road = overpass, .lane = {}});
  ASSERT_TRUE(rig.panel.apply_overpass(true));

  const roadmaker::Road* before = rig.document.network().road(overpass);
  const double length_before = before->length;
  const std::size_t elevation_records = before->elevation.size();
  ASSERT_GT(elevation_records, 0U);

  ASSERT_TRUE(
      rig.document.push_command(roadmaker::edit::delete_road(rig.document.network(), crossed))
          .has_value());

  const roadmaker::Road* after = rig.document.network().road(overpass);
  ASSERT_NE(after, nullptr) << "deleting the crossed road must not remove the overpass road";
  EXPECT_EQ(rig.document.network().road(crossed), nullptr);
  EXPECT_NEAR(after->length, length_before, 1e-9);
  EXPECT_EQ(after->elevation.size(), elevation_records);
  for (const roadmaker::Diagnostic& diagnostic :
       roadmaker::validate_network(rig.document.network())) {
    EXPECT_NE(diagnostic.severity, roadmaker::Severity::Error) << diagnostic.message;
  }
}

} // namespace
