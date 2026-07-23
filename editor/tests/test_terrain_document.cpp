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

// Terrain in the editor (p5-s2, #232): the create/remove commands drive the
// terrain mesh channel and undo restores it; a road edit rebuilds terrain; and
// — the subtle one — a preview drag does NOT re-triangulate the whole terrain
// every frame, but the commit does.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "document/document.hpp"

using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;

namespace {

/// A single straight road; enough to bound a terrain field.
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

std::size_t terrain_vertex_count(const Document& document) {
  return document.mesh().terrain.positions.size() / 3;
}

} // namespace

TEST(TerrainDocument, CreateProducesOneUndoEntryAndMeshesTheChannel) {
  Document document;
  lay_road(document);
  const int base = document.undo_stack()->index();

  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_terrain_field(document.network())).has_value());
  EXPECT_GT(terrain_vertex_count(document), 0U);
  EXPECT_EQ(document.undo_stack()->index(), base + 1); // exactly one entry
}

TEST(TerrainDocument, RemoveClearsTheChannelAndUndoRestoresIt) {
  Document document;
  lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_terrain_field(document.network())).has_value());
  const std::size_t with_terrain = terrain_vertex_count(document);
  ASSERT_GT(with_terrain, 0U);

  ASSERT_TRUE(
      document.push_command(roadmaker::edit::remove_terrain_field(document.network())).has_value());
  EXPECT_EQ(terrain_vertex_count(document), 0U);

  document.undo_stack()->undo(); // undo the remove
  EXPECT_EQ(terrain_vertex_count(document), with_terrain);
  document.undo_stack()->undo(); // undo the create
  EXPECT_EQ(terrain_vertex_count(document), 0U);
}

TEST(TerrainDocument, ARoadEditRebuildsTheTerrain) {
  Document document;
  const RoadId road = lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_terrain_field(document.network())).has_value());

  // Raise the road; the ground beside it should follow, so the terrain buffer
  // changes (a flat field had every z at 0).
  const double length = document.network().road(road)->plan_view.length();
  std::vector<roadmaker::edit::ElevationPoint> profile{
      {.s = 0.0, .z = 0.0, .grade = 0.0},
      {.s = length / 2.0, .z = 7.0, .grade = 0.0},
      {.s = length, .z = 0.0, .grade = 0.0},
  };
  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::set_elevation_profile(document.network(), road, profile))
          .has_value());

  double max_z = 0.0;
  for (std::size_t i = 2; i < document.mesh().terrain.positions.size(); i += 3) {
    max_z = std::max(max_z, document.mesh().terrain.positions[i]);
  }
  EXPECT_GT(max_z, 1.0) << "the terrain channel did not follow the raised road";
}

TEST(TerrainDocument, APreviewDragDefersTerrainUntilCommit) {
  Document document;
  const RoadId road = lay_road(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_terrain_field(document.network())).has_value());

  // A flat field: every terrain z is 0. Capture the buffer, then start a preview
  // that translates the road far to the side.
  const std::vector<double> before = document.mesh().terrain.positions;
  ASSERT_FALSE(before.empty());

  ASSERT_TRUE(
      document.begin_preview(roadmaker::edit::translate_road(document.network(), road, 0.0, 40.0))
          .has_value());
  // Mid-drag: terrain must be untouched (rebuilding it every frame would stall a
  // large scene). The road moved, but the ground channel is still the pre-drag
  // buffer.
  EXPECT_EQ(document.mesh().terrain.positions, before) << "terrain was re-triangulated mid-drag";

  document.commit_preview();
  // On release the terrain is rebuilt around the road's new position — the
  // footprint it cuts moved, so the buffer must differ.
  EXPECT_NE(document.mesh().terrain.positions, before) << "terrain was not rebuilt on drag commit";
  EXPECT_GT(terrain_vertex_count(document), 0U);
}

TEST(TerrainDocument, NoFieldMeansNoTerrainChannel) {
  Document document;
  lay_road(document);
  EXPECT_EQ(terrain_vertex_count(document), 0U);
}
