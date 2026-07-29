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

// The scale path at reduced size, asserting CORRECTNESS and no timings
// (p7-s4, #244).
//
// This is the half of the harness that runs in the ordinary suite, on all
// three platforms, on every PR. The timing half lives in
// core/tests/scale/scale_bench.cpp as its own executable, because a
// measurement taken while `ctest -j 4` runs 1 800 other processes is a
// measurement of the scheduler.
//
// Splitting them this way is what keeps the scale code COMPILED and EXERCISED
// everywhere while only the numbers are Linux-gated — a bench that is the sole
// consumer of its own generator can rot on macOS and Windows unnoticed.

#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/osm/graph.hpp"
#include "roadmaker/osm/import.hpp"
#include "roadmaker/osm/network_plan.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include "scale/synthetic_district.hpp"

namespace {

/// A small district: ~7 blocks a side rather than 29, so this stays a unit
/// test. Same generator, same code path.
roadmaker::scale::DistrictSpec small_spec() {
  roadmaker::scale::DistrictSpec spec;
  spec.blocks = 7;
  return spec;
}

roadmaker::RoadNetwork georeferenced() {
  roadmaker::RoadNetwork network;
  roadmaker::GeoReference geo;
  geo.projection = roadmaker::tmerc_projection(52.3702, 4.8952).value_or(std::string{});
  network.set_georeference(geo);
  return network;
}

std::size_t road_count(const roadmaker::RoadNetwork& network) {
  std::size_t count = 0;
  network.for_each_road([&count](roadmaker::RoadId, const roadmaker::Road&) { ++count; });
  return count;
}

} // namespace

TEST(OsmScale, TheGeneratorsArithmeticMatchesWhatItActuallyEmits) {
  // The spec's counts are used by the bench to say what it measured, so they
  // must be facts rather than intentions.
  const auto spec = small_spec();
  const std::string xml = roadmaker::scale::synthetic_district_osm(spec);

  const auto parsed = roadmaker::osm::parse_osm(xml, "district");
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message);
  EXPECT_EQ(parsed->graph.ways.size(), spec.way_count());
  EXPECT_EQ(parsed->graph.nodes.size(),
            static_cast<std::size_t>(spec.blocks) * static_cast<std::size_t>(spec.blocks));
}

TEST(OsmScale, TheLatticeShareNodesSoTheTopologyPassIsReallyExercised) {
  // A district of a thousand DISCONNECTED roads would measure the road
  // authoring path and nothing else. The whole point of a lattice is that
  // every crossing is a genuinely shared node.
  const auto spec = small_spec();
  const auto parsed =
      roadmaker::osm::parse_osm(roadmaker::scale::synthetic_district_osm(spec), "district");
  ASSERT_TRUE(parsed.has_value());

  const roadmaker::RoadNetwork network = georeferenced();
  const auto transform = roadmaker::gis::crs_transform(
      roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(network.georeference()));
  ASSERT_TRUE(transform.has_value());

  const auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
  ASSERT_TRUE(planned.has_value());

  EXPECT_EQ(planned->plan.roads.size(), spec.segment_count());
  EXPECT_EQ(planned->plan.dropped_ways, 0U) << "the generator emits only mapped classifications";

  std::size_t junctions = 0;
  for (const auto& joint : planned->plan.joints) {
    if (joint.kind == roadmaker::osm::JointKind::Junction) {
      ++junctions;
    }
  }
  EXPECT_EQ(junctions, spec.junction_count());
}

TEST(OsmScale, ADistrictImportsAndUndoesCleanly) {
  roadmaker::RoadNetwork network = georeferenced();
  const auto before = roadmaker::write_xodr(network, "before");
  ASSERT_TRUE(before.has_value());

  const auto spec = small_spec();
  const auto parsed =
      roadmaker::osm::parse_osm(roadmaker::scale::synthetic_district_osm(spec), "district");
  ASSERT_TRUE(parsed.has_value());
  const auto transform = roadmaker::gis::crs_transform(
      roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(network.georeference()));
  ASSERT_TRUE(transform.has_value());
  auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
  ASSERT_TRUE(planned.has_value());

  auto command = roadmaker::osm::import_plan(network, planned->plan);
  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack.push(network, std::move(command)).has_value());

  EXPECT_EQ(stack.size(), 1U) << "a whole district must be one undo unit";
  // Authored segments plus whatever the junction generator produced — which is
  // MORE than the plan's roads, and that gap is the reason "1 000 roads" and
  // "50 km²" are not the same order of magnitude in the built network.
  EXPECT_GE(road_count(network), planned->plan.roads.size());

  // The junction generator really ran: a district whose crossings produced no
  // connecting roads is a set of disconnected streets, and the ROAD COUNT
  // alone cannot tell the difference.
  std::size_t junctions = 0;
  network.for_each_junction(
      [&junctions](roadmaker::JunctionId, const roadmaker::Junction&) { ++junctions; });
  EXPECT_EQ(junctions, spec.junction_count());
  EXPECT_GT(road_count(network), planned->plan.roads.size())
      << "no connecting roads were generated";

  ASSERT_TRUE(stack.undo(network).has_value());
  const auto after = roadmaker::write_xodr(network, "before");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}
