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

// Terrain-field commands (p5-s2, issue #232): set/create/remove, their
// rejections, and the round-trip oracle.
//
// The M2 oracle is "apply→revert leaves write_xodr() byte-identical". For
// terrain that oracle is VACUOUS: write_xodr carries only the sidecar
// REFERENCE, so a command that mangled every height would still pass it. The
// helper here compares the SERIALIZED GRID too (write_terrain_asc), so the test
// actually watches the field it claims to. Same class of trap as the p4-s3
// "never write what you cannot read back" rule.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/road/terrain_brush.hpp"
#include "roadmaker/xodr/terrain_sidecar.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using roadmaker::BrushMode;
using roadmaker::BrushStamp;
using roadmaker::HeightField;
using roadmaker::LaneProfile;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::write_terrain_asc;
using roadmaker::write_xodr;
using roadmaker::edit::Command;
using roadmaker::edit::create_terrain_field;
using roadmaker::edit::remove_terrain_field;
using roadmaker::edit::set_terrain_field;
using roadmaker::edit::stamp_terrain;

namespace {

RoadNetwork straight_network() {
  RoadNetwork network;
  const std::vector<Waypoint> line{{0.0, 0.0}, {100.0, 0.0}};
  [[maybe_unused]] const auto road =
      roadmaker::author_clothoid_road(network, line, LaneProfile::two_lane_rural(), "", "r0");
  return network;
}

/// The .xodr (reference) AND the .asc (grid) together — the non-vacuous state.
std::string full_snapshot(const RoadNetwork& network) {
  std::string out = *write_xodr(network, "compare");
  if (!network.terrain().empty()) {
    out += "\n---ASC---\n";
    out += *write_terrain_asc(network.terrain());
  }
  return out;
}

/// apply → snapshot ≠ before → revert restores before → re-apply reproduces →
/// final revert pristine, all compared over BOTH the .xodr and the .asc.
void expect_terrain_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = full_snapshot(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = full_snapshot(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command.revert(network).has_value());
  EXPECT_EQ(full_snapshot(network), before);
  ASSERT_TRUE(command.apply(network).has_value());
  EXPECT_EQ(full_snapshot(network), after);
  ASSERT_TRUE(command.revert(network).has_value());
  EXPECT_EQ(full_snapshot(network), before);
}

} // namespace

TEST(TerrainOperations, CreateProducesAFlatFieldAndSetsTheDirtyChannel) {
  RoadNetwork network = straight_network();
  auto command = create_terrain_field(network);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_FALSE(network.terrain().empty());

  const auto dirty = command->dirty();
  EXPECT_TRUE(dirty.terrain);
  EXPECT_TRUE(dirty.roads.empty()); // terrain reads roads, never marks them
  EXPECT_FALSE(dirty.topology);
}

TEST(TerrainOperations, CreateRoundTrips) {
  RoadNetwork network = straight_network();
  auto command = create_terrain_field(network);
  expect_terrain_round_trip(network, *command);
}

TEST(TerrainOperations, SetRoundTrips) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  HeightField raised = network.terrain();
  for (double& z : raised.heights) {
    z += 5.0;
  }
  auto command = set_terrain_field(network, raised);
  expect_terrain_round_trip(network, *command);
}

TEST(TerrainOperations, RemoveRoundTrips) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  auto command = remove_terrain_field(network);
  expect_terrain_round_trip(network, *command);
}

TEST(TerrainOperations, CreateSetRemoveThenUndoThreeTimesRestoresTheOriginal) {
  RoadNetwork network = straight_network();
  const std::string pristine = full_snapshot(network);

  auto create = create_terrain_field(network);
  ASSERT_TRUE(create->apply(network).has_value());
  const std::string created = full_snapshot(network);

  HeightField raised = network.terrain();
  raised.heights.front() += 9.0;
  auto set = set_terrain_field(network, raised);
  ASSERT_TRUE(set->apply(network).has_value());
  const std::string edited = full_snapshot(network);

  auto remove = remove_terrain_field(network);
  ASSERT_TRUE(remove->apply(network).has_value());
  EXPECT_TRUE(network.terrain().empty());

  ASSERT_TRUE(remove->revert(network).has_value());
  EXPECT_EQ(full_snapshot(network), edited);
  ASSERT_TRUE(set->revert(network).has_value());
  EXPECT_EQ(full_snapshot(network), created);
  ASSERT_TRUE(create->revert(network).has_value());
  EXPECT_EQ(full_snapshot(network), pristine);
}

// --- rejections --------------------------------------------------------------

TEST(TerrainOperations, CreateRejectsAnExistingField) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  auto again = create_terrain_field(network);
  EXPECT_FALSE(again->apply(network).has_value());
}

TEST(TerrainOperations, CreateRejectsANetworkWithoutRoads) {
  RoadNetwork empty;
  EXPECT_FALSE(create_terrain_field(empty)->apply(empty).has_value());
}

TEST(TerrainOperations, CreateRejectsANonPositiveSpacing) {
  RoadNetwork network = straight_network();
  EXPECT_FALSE(create_terrain_field(network, 0.0)->apply(network).has_value());
}

TEST(TerrainOperations, RemoveRejectsWhenThereIsNoField) {
  RoadNetwork network = straight_network();
  EXPECT_FALSE(remove_terrain_field(network)->apply(network).has_value());
}

TEST(TerrainOperations, SetRejectsAMalformedField) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());

  HeightField bad_spacing = network.terrain();
  bad_spacing.spacing = 0.0;
  EXPECT_FALSE(set_terrain_field(network, bad_spacing)->apply(network).has_value());

  HeightField wrong_count = network.terrain();
  wrong_count.heights.pop_back();
  EXPECT_FALSE(set_terrain_field(network, wrong_count)->apply(network).has_value());

  HeightField non_finite = network.terrain();
  non_finite.heights.front() = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(set_terrain_field(network, non_finite)->apply(network).has_value());
}

TEST(TerrainOperations, SetRejectsANoOp) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  auto command = set_terrain_field(network, network.terrain());
  EXPECT_FALSE(command->apply(network).has_value());
}

// --- brush strokes (p5-s4, #234) --------------------------------------------

TEST(TerrainBrushOperations, StrokeRoundTripsOverTheGridNotJustTheReference) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  // Centre the stroke on the network so it lands inside the flat field.
  const auto extent = roadmaker::field_extent(network.terrain());
  const double cx = (extent[0] + extent[2]) / 2.0;
  const double cy = (extent[1] + extent[3]) / 2.0;
  std::vector<BrushStamp> stroke{
      {cx, cy, 20.0, 3.0, BrushMode::Raise},
      {cx + 8.0, cy, 20.0, 3.0, BrushMode::Raise},
  };
  auto built = stamp_terrain(network, stroke);
  ASSERT_TRUE(built.has_value());
  // The non-vacuous oracle: apply→revert must restore the SERIALIZED GRID too.
  expect_terrain_round_trip(network, **built);
}

TEST(TerrainBrushOperations, StrokeCommandCapturesOnlyTheTerrainDirtyChannel) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  const auto extent = roadmaker::field_extent(network.terrain());
  auto built = stamp_terrain(network,
                             {{(extent[0] + extent[2]) / 2.0,
                               (extent[1] + extent[3]) / 2.0,
                               25.0,
                               2.0,
                               BrushMode::Raise}});
  ASSERT_TRUE(built.has_value());
  const auto dirty = (*built)->dirty();
  EXPECT_TRUE(dirty.terrain);
  EXPECT_TRUE(dirty.roads.empty());
  EXPECT_FALSE(dirty.topology);
}

TEST(TerrainBrushOperations, StrokeLeavesPostsOutsideItsFootprintUntouched) {
  RoadNetwork network = straight_network();
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  const HeightField before = network.terrain();
  const auto extent = roadmaker::field_extent(network.terrain());
  // A small brush at one corner of the field.
  auto built =
      stamp_terrain(network, {{extent[0] + 5.0, extent[1] + 5.0, 12.0, 4.0, BrushMode::Raise}});
  ASSERT_TRUE(built.has_value());
  ASSERT_TRUE((*built)->apply(network).has_value());
  const HeightField after = network.terrain();
  ASSERT_EQ(before.heights.size(), after.heights.size());
  // A post far from the corner (the opposite corner) is unchanged.
  EXPECT_DOUBLE_EQ(after.heights.back(), before.heights.back());
  // ...but SOMETHING changed overall.
  EXPECT_NE(after.heights, before.heights);
}

// --- DEM import (p5-s4, #234) -----------------------------------------------
// The `.asc` reader already exists (p5-s2's sidecar). Import is just: parse the
// grid, then set_terrain_field installs it verbatim (as-is in the kernel frame,
// decision D1). This locks that path end to end at the command layer.
TEST(TerrainDemImport, ParsedAscInstallsAsTheSceneFieldAndRoundTrips) {
  RoadNetwork network = straight_network();
  const std::string dem =
      "ncols 3\nnrows 2\nxllcorner 10\nyllcorner 20\ncellsize 5\nNODATA_value -9999\n"
      "3 4 5\n0 1 2\n"; // rows are NORTH-first; low-y row {0,1,2} ends up first in heights
  auto parsed = roadmaker::parse_terrain_asc(dem, "dem");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->field.cols, 3u);
  EXPECT_EQ(parsed->field.rows, 2u);
  EXPECT_DOUBLE_EQ(parsed->field.origin_x, 10.0);
  EXPECT_DOUBLE_EQ(parsed->field.spacing, 5.0);

  auto command = set_terrain_field(network, parsed->field);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.terrain(), parsed->field); // installed verbatim, as-is
  // The installed grid re-serializes byte-identically to what we read.
  EXPECT_EQ(*write_terrain_asc(network.terrain()), *write_terrain_asc(parsed->field));
}

TEST(TerrainBrushOperations, RejectsNoFieldEmptyStrokeAndNoOpStroke) {
  RoadNetwork network = straight_network();
  // No field yet.
  EXPECT_FALSE(stamp_terrain(network, {{0.0, 0.0, 10.0, 1.0, BrushMode::Raise}}).has_value());

  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
  // Empty stroke.
  EXPECT_FALSE(stamp_terrain(network, {}).has_value());
  // A stroke that misses the grid entirely changes nothing → rejected.
  EXPECT_FALSE(
      stamp_terrain(network, {{100000.0, 100000.0, 10.0, 5.0, BrushMode::Raise}}).has_value());
}
