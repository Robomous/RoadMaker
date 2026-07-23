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

// Height-field persistence (p5-s2, issue #232, decision D2): the .asc sidecar
// round-trip, the rm:terrain reference in the .xodr, and the save→load→save
// byte-identical guarantee. The key invariant a scene without terrain writes no
// rm:terrain and stays byte-identical to a pre-terrain file.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/terrain_sidecar.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using roadmaker::author_clothoid_road;
using roadmaker::HeightField;
using roadmaker::is_safe_sidecar_reference;
using roadmaker::LaneProfile;
using roadmaker::load_terrain_asc;
using roadmaker::load_xodr;
using roadmaker::parse_terrain_asc;
using roadmaker::parse_xodr;
using roadmaker::RoadNetwork;
using roadmaker::sample_height;
using roadmaker::save_xodr;
using roadmaker::Waypoint;
using roadmaker::write_terrain_asc;
using roadmaker::write_xodr;

namespace {

HeightField ramp_field() {
  HeightField field;
  field.origin_x = -20.0;
  field.origin_y = -20.0;
  field.spacing = 10.0;
  field.cols = 4;
  field.rows = 3;
  // A deliberately ASYMMETRIC field so a flipped row order reads back wrong.
  field.heights = {0.0, 1.0, 2.0, 3.0, 10.0, 11.0, 12.0, 13.0, 20.0, 21.0, 22.0, 23.0};
  return field;
}

RoadNetwork network_with_terrain() {
  RoadNetwork network;
  const std::vector<Waypoint> line{{0.0, 0.0}, {100.0, 0.0}};
  [[maybe_unused]] const auto road =
      author_clothoid_road(network, line, LaneProfile::two_lane_rural(), "", "r0");
  HeightField field = ramp_field();
  field.sidecar = "scene.terrain.asc";
  network.set_terrain(std::move(field));
  return network;
}

} // namespace

TEST(TerrainPersistence, AscTextRoundTripsAFieldExactly) {
  const HeightField field = ramp_field();
  const auto text = write_terrain_asc(field);
  ASSERT_TRUE(text.has_value()) << text.error().message;

  const auto parsed = parse_terrain_asc(*text, "test");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  // sidecar is not part of the .asc content, so compare the geometry only.
  HeightField expected = field;
  expected.sidecar.clear();
  EXPECT_EQ(parsed->field, expected);
  EXPECT_TRUE(parsed->diagnostics.empty());
}

TEST(TerrainPersistence, RowOrderSurvivesTheNorthSouthFlip) {
  // The store is south-first, the format is north-first; a field whose north
  // edge is high must read back with its north edge high.
  const HeightField field = ramp_field();
  const auto text = write_terrain_asc(field);
  ASSERT_TRUE(text.has_value());
  const auto parsed = parse_terrain_asc(*text, "test");
  ASSERT_TRUE(parsed.has_value());
  // Sample the south edge (low y) and the north edge (high y): south is the 0..3
  // row, north is the 20..23 row.
  EXPECT_DOUBLE_EQ(sample_height(parsed->field, -20.0, -20.0), 0.0);
  EXPECT_DOUBLE_EQ(sample_height(parsed->field, -20.0, 0.0), 20.0);
}

TEST(TerrainPersistence, NoDataCellsBecomeZeroWithAWarning) {
  std::string asc = "ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 10\nNODATA_value -9999\n"
                    "5 -9999\n1 2\n";
  const auto parsed = parse_terrain_asc(asc, "test");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_DOUBLE_EQ(sample_height(parsed->field, 10.0, 10.0), 0.0); // the NODATA post
  ASSERT_EQ(parsed->diagnostics.size(), 1U);
  EXPECT_EQ(parsed->diagnostics.front().severity, roadmaker::Severity::Warning);
}

TEST(TerrainPersistence, AMalformedHeaderFailsTheParse) {
  EXPECT_FALSE(parse_terrain_asc("ncols 2\nnrows 2\n", "test").has_value()); // no cellsize
  EXPECT_FALSE(parse_terrain_asc("ncols x\nnrows 2\ncellsize 10\n0 0", "t").has_value());
  EXPECT_FALSE(parse_terrain_asc("ncols 2\nnrows 2\ncellsize -1\n0 0 0 0", "t").has_value());
}

TEST(TerrainPersistence, TooFewValuesFailsTheParse) {
  EXPECT_FALSE(
      parse_terrain_asc("ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 10\n1 2 3", "t")
          .has_value());
}

TEST(TerrainPersistence, EmptyFieldIsNotSerializable) {
  EXPECT_FALSE(write_terrain_asc(HeightField{}).has_value());
}

TEST(TerrainPersistence, ASceneWithoutTerrainWritesNoReference) {
  RoadNetwork network;
  const std::vector<Waypoint> line{{0.0, 0.0}, {50.0, 0.0}};
  [[maybe_unused]] const auto road =
      author_clothoid_road(network, line, LaneProfile::two_lane_rural(), "", "r0");
  const auto text = write_xodr(network);
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text->find("rm:terrain"), std::string::npos);
}

TEST(TerrainPersistence, InMemoryRoundTripKeepsTheReferenceWithoutTheGrid) {
  const RoadNetwork network = network_with_terrain();
  const auto text = write_xodr(network);
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("rm:terrain"), std::string::npos);
  EXPECT_NE(text->find("scene.terrain.asc"), std::string::npos);

  // parse_xodr has no directory, so it keeps the reference and leaves the grid
  // empty — and re-writing must reproduce the SAME reference byte for byte.
  const auto reparsed = parse_xodr(*text, "mem");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.terrain().empty());
  EXPECT_EQ(reparsed->network.terrain().sidecar, "scene.terrain.asc");
  const auto rewritten = write_xodr(reparsed->network);
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*text, *rewritten);
}

TEST(TerrainPersistence, SaveLoadSaveIsByteIdenticalWithAFieldOnDisk) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "rm_terrain_persist_test";
  std::filesystem::create_directories(dir);
  const std::filesystem::path xodr = dir / "scene.xodr";

  const RoadNetwork network = network_with_terrain();
  ASSERT_TRUE(save_xodr(network, xodr, "scene").has_value());
  // The sidecar landed next to the .xodr.
  EXPECT_TRUE(std::filesystem::exists(dir / "scene.terrain.asc"));

  const auto loaded = load_xodr(xodr);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_FALSE(loaded->network.terrain().empty());
  // The grid came back through the sidecar, not just the reference.
  EXPECT_DOUBLE_EQ(sample_height(loaded->network.terrain(), -20.0, 0.0), 20.0);

  const std::filesystem::path xodr2 = dir / "scene2.xodr";
  ASSERT_TRUE(save_xodr(loaded->network, xodr2, "scene").has_value());

  // Sidecar name is derived from the document stem, so re-saving under a new
  // stem produces its own sidecar; compare the .asc CONTENT for equality.
  const auto asc1 = load_terrain_asc(dir / "scene.terrain.asc");
  ASSERT_TRUE(asc1.has_value());
  ASSERT_TRUE(std::filesystem::exists(dir / "scene.terrain.asc"));
  const auto reloaded = load_xodr(xodr2);
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(reloaded->network.terrain(), loaded->network.terrain());

  std::filesystem::remove_all(dir);
}

TEST(TerrainPersistence, AMissingSidecarWarnsAndStillLoadsTheNetwork) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "rm_terrain_missing_test";
  std::filesystem::create_directories(dir);
  const std::filesystem::path xodr = dir / "scene.xodr";

  const RoadNetwork network = network_with_terrain();
  ASSERT_TRUE(save_xodr(network, xodr, "scene").has_value());
  std::filesystem::remove(dir / "scene.terrain.asc"); // delete the sidecar

  const auto loaded = load_xodr(xodr);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->network.road_count(), 1U);                       // network intact
  EXPECT_TRUE(loaded->network.terrain().empty());                    // no grid
  EXPECT_EQ(loaded->network.terrain().sidecar, "scene.terrain.asc"); // reference kept
  bool warned = false;
  for (const auto& d : loaded->diagnostics) {
    warned = warned || d.message.find("rm:terrain") != std::string::npos;
  }
  EXPECT_TRUE(warned);

  std::filesystem::remove_all(dir);
}

TEST(TerrainPersistence, UnsafeReferencesAreRejected) {
  EXPECT_TRUE(is_safe_sidecar_reference("scene.terrain.asc"));
  EXPECT_FALSE(is_safe_sidecar_reference(""));
  EXPECT_FALSE(is_safe_sidecar_reference("/etc/passwd"));
  EXPECT_FALSE(is_safe_sidecar_reference("../outside.asc"));
  EXPECT_FALSE(is_safe_sidecar_reference("sub/dir.asc"));
  EXPECT_FALSE(is_safe_sidecar_reference("C:\\win.asc"));
}
