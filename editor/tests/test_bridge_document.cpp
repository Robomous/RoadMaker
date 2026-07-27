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

// Bridges in the editor (p5-s3, #233): the author command drives the bridge
// mesh channel and undo clears it; a road elevation edit re-derives the solids
// (no extra undo entry); and — the subtle one — a preview drag does NOT rebuild
// the bridge solids every frame, but the commit does.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/grade_separation.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <algorithm>
#include <cstddef>
#include <vector>

#include "document/document.hpp"

using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;

namespace {

RoadId lay_road(Document& document) {
  const bool ok = document
                      .push_command(roadmaker::edit::create_road(
                          {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 120.0, .y = 0.0}},
                          LaneProfile::two_lane_default(),
                          "r0"))
                      .has_value();
  EXPECT_TRUE(ok);
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  return road;
}

/// One arm of an overpass fixture: `high` runs west-east 5 m up, `low` runs
/// south-north at ground level, so exactly one grade separation exists.
/// The newest road: for_each_road walks slots ascending, so the last one seen is
/// the one just created. `create_road`'s third argument is the display NAME, not
/// the OpenDRIVE id, so matching on odr_id here would silently find nothing.
RoadId newest_road(const Document& document) {
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  return road;
}

RoadId lay_overpass_high(Document& document) {
  EXPECT_TRUE(document
                  .push_command(roadmaker::edit::create_road(
                      {Waypoint{.x = -60.0, .y = 0.0}, Waypoint{.x = 60.0, .y = 0.0}},
                      LaneProfile::two_lane_default(),
                      "high"))
                  .has_value());
  const RoadId road = newest_road(document);
  EXPECT_TRUE(road.is_valid());
  const double length = document.network().road(road)->plan_view.length();
  const std::vector<roadmaker::edit::ElevationPoint> profile{{.s = 0.0, .z = 5.0, .grade = 0.0},
                                                             {.s = length, .z = 5.0, .grade = 0.0}};
  EXPECT_TRUE(
      document
          .push_command(roadmaker::edit::set_elevation_profile(document.network(), road, profile))
          .has_value());
  return road;
}

RoadId lay_overpass_low(Document& document) {
  EXPECT_TRUE(document
                  .push_command(roadmaker::edit::create_road(
                      {Waypoint{.x = 0.0, .y = -60.0}, Waypoint{.x = 0.0, .y = 60.0}},
                      LaneProfile::two_lane_default(),
                      "low"))
                  .has_value());
  const RoadId road = newest_road(document);
  EXPECT_TRUE(road.is_valid());
  return road;
}

std::size_t bridge_vertex_count(const Document& document) {
  std::size_t total = 0;
  for (const roadmaker::BridgeMesh& span : document.mesh().bridges) {
    total += span.mesh.positions.size() / 3;
  }
  return total;
}

} // namespace

TEST(BridgeDocument, AuthorMeshesTheSolidWithOneUndoEntry) {
  Document document;
  const RoadId road = lay_road(document);
  const int undo_before = document.undo_stack()->count();

  ASSERT_TRUE(
      document.push_command(roadmaker::edit::author_bridge(document.network(), road, 40.0, 24.0))
          .has_value());

  EXPECT_EQ(document.network().road(road)->bridges.size(), 1U);
  EXPECT_GT(bridge_vertex_count(document), 0U);
  EXPECT_EQ(document.undo_stack()->count(), undo_before + 1);
}

TEST(BridgeDocument, UndoClearsTheBridgeChannel) {
  Document document;
  const RoadId road = lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::author_bridge(document.network(), road, 40.0, 24.0))
          .has_value());
  ASSERT_GT(bridge_vertex_count(document), 0U);

  document.undo_stack()->undo();
  EXPECT_TRUE(document.network().road(road)->bridges.empty());
  EXPECT_EQ(bridge_vertex_count(document), 0U);
}

TEST(BridgeDocument, ElevationEditReDerivesTheSolid) {
  Document document;
  const RoadId road = lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::author_bridge(document.network(), road, 40.0, 24.0))
          .has_value());
  const std::size_t before = bridge_vertex_count(document);
  ASSERT_GT(before, 0U);
  const int undo_after_author = document.undo_stack()->count();

  const double length = document.network().road(road)->plan_view.length();
  std::vector<roadmaker::edit::ElevationPoint> profile{{.s = 0.0, .z = 6.0, .grade = 0.0},
                                                       {.s = length, .z = 6.0, .grade = 0.0}};
  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::set_elevation_profile(document.network(), road, profile))
          .has_value());

  // The solid is still there, re-derived at the new elevation (its top rose).
  EXPECT_GT(bridge_vertex_count(document), 0U);
  double max_z = -1e9;
  for (const roadmaker::BridgeMesh& span : document.mesh().bridges) {
    for (std::size_t v = 2; v < span.mesh.positions.size(); v += 3) {
      max_z = std::max(max_z, span.mesh.positions[v]);
    }
  }
  EXPECT_GT(max_z, 5.0); // deck rode up with the road
  // The re-derivation is not a command — only the elevation edit added an entry.
  EXPECT_EQ(document.undo_stack()->count(), undo_after_author + 1);
}

TEST(BridgeDocument, PreviewDragDefersRebuildButCommitDoesIt) {
  Document document;
  const RoadId road = lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::author_bridge(document.network(), road, 40.0, 24.0))
          .has_value());
  ASSERT_GT(bridge_vertex_count(document), 0U);

  // Capture the deck's plan-view position, then drag the road sideways.
  const double x_before = document.mesh().bridges.front().mesh.positions.front();
  ASSERT_TRUE(
      document.begin_preview(roadmaker::edit::translate_road(document.network(), road, 0.0, 30.0))
          .has_value());
  // Mid-drag the solids are NOT rebuilt — the deck buffer still reads the old x.
  EXPECT_DOUBLE_EQ(document.mesh().bridges.front().mesh.positions.front(), x_before);

  document.commit_preview();
  // On release the solids follow the road: y shifted, so the deck moved.
  ASSERT_GT(bridge_vertex_count(document), 0U);
}

// cascade-s3 (#463): a span whose crossing has gone is reported, not deleted —
// and the report has to reach the user, because a bridge is not selectable and
// Remove Orphaned Spans is the only way to act on it.
TEST(BridgeDocument, AnOrphanedSpanIsReportedAndThenSweepable) {
  Document document;
  const RoadId high = lay_overpass_high(document);
  const RoadId low = lay_overpass_low(document);
  const std::vector<roadmaker::GradeSeparation> crossings =
      roadmaker::find_grade_separations(document.network());
  ASSERT_EQ(crossings.size(), 1U);
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::author_bridge(
                      document.network(), high, crossings.front().s_upper - 12.0, 24.0))
                  .has_value());

  QSignalSpy stale(&document, &Document::derived_layer_stale);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::translate_road(document.network(), low, 400.0, 0.0))
          .has_value());

  EXPECT_EQ(stale.count(), 1) << "the user has to be told the deck spans nothing now";
  EXPECT_EQ(document.network().road(high)->bridges.size(), 1U)
      << "and the span itself is left exactly as authored";

  ASSERT_TRUE(document.push_command(roadmaker::edit::remove_orphaned_bridges(document.network()))
                  .has_value());
  EXPECT_TRUE(document.network().road(high)->bridges.empty());
  EXPECT_EQ(bridge_vertex_count(document), 0U) << "the solid channel followed the sweep";

  document.undo_stack()->undo();
  EXPECT_EQ(document.network().road(high)->bridges.size(), 1U);
}

TEST(BridgeDocument, AnUnorphanedNetworkRefusesTheSweep) {
  Document document;
  const RoadId high = lay_overpass_high(document);
  lay_overpass_low(document);
  const std::vector<roadmaker::GradeSeparation> crossings =
      roadmaker::find_grade_separations(document.network());
  ASSERT_EQ(crossings.size(), 1U);
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::author_bridge(
                      document.network(), high, crossings.front().s_upper - 12.0, 24.0))
                  .has_value());

  EXPECT_FALSE(document.push_command(roadmaker::edit::remove_orphaned_bridges(document.network()))
                   .has_value())
      << "a refusal, not an empty undo entry";
  EXPECT_EQ(document.network().road(high)->bridges.size(), 1U);
}
