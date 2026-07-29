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

// The scale harness (p7-s4, #244) — the measurement half of the sprint, and
// the discharge of #54's "1 000-road network and 50 km² import" gate.
//
// WHY THIS IS ITS OWN EXECUTABLE, not registered with gtest_discover_tests.
// `ctest --preset ci-linux -j 4` runs some 1 800 processes on a four-core
// runner. A timing test inside that set is measuring the scheduler, not the
// code. This binary is built and run by its own CI job, serially, alone.
// `core/tests/test_osm_scale.cpp` — which IS in the normal suite — builds the
// same district at reduced size and asserts CORRECTNESS only, so the scale
// path is compiled and exercised on all three platforms every PR while only
// the NUMBERS are Linux-gated.
//
// RUNNER-VARIANCE POLICY, extending core/tests/test_remesh_budget.cpp's:
//
//   * Median of N, never mean, never max.
//   * The real number is PRINTED on every run; the ceiling is a 10x
//     regression alarm, not a specification. The dev-machine targets below
//     are what a change should actually be read against.
//   * ubuntu-latest only (runner cost x1), serial, in its own job.
//   * RAISING A CEILING IS A PRODUCT DECISION. A PR that raises one must move
//     the dev target in the same diff and name it in CHANGELOG.md. A silently
//     loosened ceiling is a failure mode this repository has already been
//     bitten by (see ADR-0011's second passed sabotage).
//
// THE THRESHOLDS BELOW ARE PROPOSALS awaiting maintainer sign-off. #54 and the
// archived M3b seed both say the metric NAMES and the SIZES are fixed and the
// values are "proposed for maintainer sign-off"; these are the first ones.
//
// AND ONE OF THEM IS EMBARRASSING ON PURPOSE. The build step's ceiling is far
// above where it should sit, because the measurement this harness was written
// to take found the step is SUPER-LINEAR in road count: about 18x the roads
// costs about two orders of magnitude more time (#502). The ceiling gates
// against further regression; it is not a number anyone should be pleased
// with, and #502 owns bringing it down. Writing a flattering ceiling by
// shrinking the district would have been the other option, and it would have
// dodged the only question #54 actually asked.

#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/osm/graph.hpp"
#include "roadmaker/osm/import.hpp"
#include "roadmaker/osm/network_plan.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "peak_rss.hpp"
#include "synthetic_district.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// Every measurement the run produced, written out as JSON at the end. The CI
/// job asserts the file exists and carries every key BEFORE uploading it: a
/// bench that quietly did not run is otherwise indistinguishable from a green
/// one.
std::map<std::string, double>& metrics() {
  static std::map<std::string, double> values;
  return values;
}

void record(const std::string& key, double value) {
  metrics()[key] = value;
}

/// Median, never mean: one scheduling hiccup on a shared runner moves a mean
/// and does not move a median.
double median_ms(std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  const std::size_t middle = samples.size() / 2;
  std::ranges::nth_element(samples, samples.begin() + static_cast<std::ptrdiff_t>(middle));
  return samples[middle];
}

template <typename Fn>
double time_median_ms(int runs, Fn&& body) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(runs));
  for (int i = 0; i < runs; ++i) {
    const auto start = Clock::now();
    body();
    const auto stop = Clock::now();
    samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
  }
  return median_ms(samples);
}

roadmaker::RoadNetwork georeferenced() {
  roadmaker::RoadNetwork network;
  roadmaker::GeoReference geo;
  geo.projection = roadmaker::tmerc_projection(52.3702, 4.8952).value_or(std::string{});
  network.set_georeference(geo);
  return network;
}

/// The district, generated once and shared. Building it is not on any
/// measured path.
/// Set once by main() from `--blocks=N`, then read everywhere.
roadmaker::scale::DistrictSpec& measured_spec() {
  static roadmaker::scale::DistrictSpec value;
  return value;
}

const std::string& district_xml() {
  static const std::string xml = roadmaker::scale::synthetic_district_osm(measured_spec());
  return xml;
}

/// The imported network, built once and reused by the load and drag metrics.
roadmaker::RoadNetwork& imported_network() {
  static roadmaker::RoadNetwork network = [] {
    roadmaker::RoadNetwork built = georeferenced();
    const auto parsed = roadmaker::osm::parse_osm(district_xml(), "district");
    EXPECT_TRUE(parsed.has_value());
    const auto transform = roadmaker::gis::crs_transform(
        roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(built.georeference()));
    EXPECT_TRUE(transform.has_value());
    auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
    EXPECT_TRUE(planned.has_value());
    auto command = roadmaker::osm::import_plan(built, planned->plan);
    roadmaker::edit::EditStack stack;
    EXPECT_TRUE(stack.push(built, std::move(command)).has_value());
    return built;
  }();
  return network;
}

std::size_t road_count(const roadmaker::RoadNetwork& network) {
  std::size_t count = 0;
  network.for_each_road([&count](roadmaker::RoadId, const roadmaker::Road&) { ++count; });
  return count;
}

} // namespace

TEST(Scale, TheDefaultDistrictIsTheSizeBothInheritedTargetsName) {
  // Asserted on the DEFAULT spec, not on whatever RM_SCALE_BLOCKS asked for:
  // the inherited target is a property of this harness, and a job that shrank
  // the district must not also shrink the claim.
  const roadmaker::scale::DistrictSpec spec; // NOLINT: the default, deliberately
  std::printf("[ Scale ] district: %.1f km², %zu ways, %zu segments, %zu junctions\n",
              spec.area_km2(),
              spec.way_count(),
              spec.segment_count(),
              spec.junction_count());
  record("district_area_km2", spec.area_km2());
  record("district_segments", static_cast<double>(spec.segment_count()));
  // ...and separately, what this RUN measured, so a metrics file can never
  // imply a size it did not exercise.
  std::printf("[ Scale ] THIS RUN: %.1f km², %zu segments (--blocks=%d)\n",
              measured_spec().area_km2(),
              measured_spec().segment_count(),
              measured_spec().blocks);
  record("measured_area_km2", measured_spec().area_km2());
  record("measured_blocks", static_cast<double>(measured_spec().blocks));

  // #54 asks for a 1 000-road network AND a ~50 km² import. One district
  // satisfies both, which is why the spec is shaped the way it is.
  EXPECT_GE(spec.area_km2(), 45.0);
  EXPECT_LE(spec.area_km2(), 55.0);
  EXPECT_GE(spec.segment_count(), 1000U);
}

TEST(Scale, ParseAndPlan) {
  const roadmaker::RoadNetwork network = georeferenced();
  const auto transform = roadmaker::gis::crs_transform(
      roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(network.georeference()));
  ASSERT_TRUE(transform.has_value());

  std::size_t roads = 0;
  const double ms = time_median_ms(5, [&] {
    const auto parsed = roadmaker::osm::parse_osm(district_xml(), "district");
    ASSERT_TRUE(parsed.has_value());
    const auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
    ASSERT_TRUE(planned.has_value());
    roads = planned->plan.roads.size();
  });

  std::printf(
      "[ Scale ] parse + plan: median %.1f ms for %zu roads (dev target < 2000 ms)\n", ms, roads);
  record("parse_and_plan_ms", ms);
  // Against the size THIS RUN used, not a fixed number: a run with
  // RM_SCALE_BLOCKS set must still prove the planner produced everything the
  // generator emitted, and a hardcoded 1 000 would just fail on a small run
  // while proving nothing on a large one.
  EXPECT_EQ(roads, measured_spec().segment_count());
  EXPECT_LT(ms, 20'000.0);
}

TEST(Scale, NetworkBuild) {
  // THE DOMINANT TERM, and the number most likely to move the design.
  // create_junction generates a connecting road per permitted (incoming lane,
  // outgoing lane) pair, so a district's ~841 four-way nodes produce several
  // thousand connecting roads ON TOP of the authored ones. "1 000 roads" and
  // "50 km²" are not the same order of magnitude in the network the kernel
  // ends up holding, which is worth knowing before reading the number.
  const auto parsed = roadmaker::osm::parse_osm(district_xml(), "district");
  ASSERT_TRUE(parsed.has_value());

  roadmaker::RoadNetwork probe = georeferenced();
  const auto transform = roadmaker::gis::crs_transform(
      roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(probe.georeference()));
  ASSERT_TRUE(transform.has_value());
  const auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
  ASSERT_TRUE(planned.has_value());

  std::size_t total = 0;
  // ONE run, not a median of three. This step is MINUTES at district scale
  // (see the note above and #502), and a twenty-minute gate that
  // nobody waits for is worse than a single honest measurement.
  const double ms = time_median_ms(1, [&] {
    roadmaker::RoadNetwork network = georeferenced();
    auto command = roadmaker::osm::import_plan(network, planned->plan);
    roadmaker::edit::EditStack stack;
    ASSERT_TRUE(stack.push(network, std::move(command)).has_value());
    total = road_count(network);
  });

  std::printf("[ Scale ] network build: %.1f ms -> %zu roads incl. connecting "
              "(target once #502 lands: < 5000 ms at 50 km²)\n",
              ms,
              total);
  record("network_build_ms", ms);
  record("network_roads", static_cast<double>(total));

  // The junction generator really ran: connecting roads outnumber the
  // authored ones several times over, and THAT is why "1 000 roads" and
  // "50 km²" are not the same order of magnitude in the built network.
  EXPECT_GT(total, planned->plan.roads.size());

  // 300 s, which is deliberately far above where this should sit. The step is
  // super-linear (#502) — 84 segments cost 92 ms and 420 cost 19 200, five
  // times the roads for two hundred times the time — so this ceiling gates
  // against FURTHER regression and nothing more. It comes down with #502, in
  // the same diff as the dev target, per the policy at the top of this file.
  EXPECT_LT(ms, 300'000.0);
}

TEST(Scale, XodrLoad) {
  const auto written = roadmaker::write_xodr(imported_network(), "district");
  ASSERT_TRUE(written.has_value());
  record("xodr_bytes", static_cast<double>(written->size()));

  const double ms = time_median_ms(3, [&] {
    const auto reloaded = roadmaker::parse_xodr(*written);
    ASSERT_TRUE(reloaded.has_value());
  });
  std::printf("[ Scale ] .xodr load: median %.1f ms for %zu bytes (dev target < 3000 ms)\n",
              ms,
              written->size());
  record("xodr_load_ms", ms);
  EXPECT_LT(ms, 30'000.0);
}

TEST(Scale, NodeDragLatencyDoesNotGrowWithTheNetwork) {
  // The 10x extension of test_remesh_budget.cpp, with the SAME target and the
  // SAME multiplier: a frame budget does not get bigger because the network
  // did. Re-meshing one road should be independent of network size, and a
  // number that scales with N is precisely the bug this metric exists to find
  // — so the 100-road figure is printed beside it for comparison.
  roadmaker::RoadNetwork& big = imported_network();
  roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(big);

  roadmaker::RoadId subject;
  big.for_each_road([&subject](roadmaker::RoadId id, const roadmaker::Road&) {
    if (!subject.is_valid()) {
      subject = id;
    }
  });
  ASSERT_TRUE(subject.is_valid());

  const std::array<roadmaker::RoadId, 1> dirty{subject};
  const double ms = time_median_ms(100, [&] { roadmaker::remesh_roads(big, mesh, dirty); });

  std::printf("[ Scale ] one-road remesh on a %zu-road network: median %.3f ms "
              "(dev target < 16 ms, the M2 frame budget)\n",
              road_count(big),
              ms);
  record("node_drag_ms", ms);
  EXPECT_LT(ms, 160.0);
}

TEST(Scale, PeakMemory) {
  // Touch the network and its mesh so the peak reflects both.
  roadmaker::RoadNetwork& network = imported_network();
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);

  // The allocator-INDEPENDENT number, printed on every platform. This is what
  // a regression should be read from; peak RSS is what a leak shows up in.
  std::size_t mesh_bytes = 0;
  for (const auto& road : mesh.roads) {
    mesh_bytes += road.positions.capacity() * sizeof(double);
    mesh_bytes += road.normals.capacity() * sizeof(double);
    mesh_bytes += road.uvs.capacity() * sizeof(double);
  }
  std::printf("[ Scale ] mesh byte accounting: %.1f MB across %zu road meshes\n",
              static_cast<double>(mesh_bytes) / (1024.0 * 1024.0),
              mesh.roads.size());
  record("mesh_mb", static_cast<double>(mesh_bytes) / (1024.0 * 1024.0));

  const auto peak = roadmaker::scale::peak_rss_bytes();
  if (!peak) {
    std::printf("[ Scale ] peak RSS: unavailable on this platform\n");
    record("peak_rss_mb", -1.0);
    GTEST_SKIP() << "no peak-RSS probe on this platform";
  }
  const double megabytes = static_cast<double>(*peak) / (1024.0 * 1024.0);
  std::printf("[ Scale ] peak RSS: %.1f MB (dev target < 1500 MB)\n", megabytes);
  record("peak_rss_mb", megabytes);

  if constexpr (!roadmaker::scale::peak_rss_is_gateable()) {
    // Printed, not gated. A ceiling calibrated against glibc's allocator is
    // not a ceiling against another one, and a gate that means different
    // things on different platforms is worse than no gate.
    GTEST_SKIP() << "peak RSS is reported but not gated off Linux";
  }
  EXPECT_LT(megabytes, 4096.0);
}

TEST(Scale, TheCostDoesNotGrowQuadraticallyWithTheDistrict) {
  // A RATIO, which survives runner noise in a way no absolute does — and the
  // only assertion in this bench that stays meaningful on a bad runner.
  // Doubling the blocks quadruples the roads; quadratic cost would be ~16x.
  const auto measure = [](int blocks) {
    roadmaker::scale::DistrictSpec spec;
    spec.blocks = blocks;
    const std::string xml = roadmaker::scale::synthetic_district_osm(spec);
    const roadmaker::RoadNetwork network = georeferenced();
    const auto transform = roadmaker::gis::crs_transform(
        roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(network.georeference()));
    return time_median_ms(3, [&] {
      const auto parsed = roadmaker::osm::parse_osm(xml, "district");
      const auto planned = roadmaker::osm::plan_network(parsed->graph, *transform);
      (void)planned;
    });
  };

  const double at_15 = measure(15);
  const double at_30 = measure(30);
  const double ratio = at_15 > 0.0 ? at_30 / at_15 : 0.0;
  std::printf("[ Scale ] parse+plan 15 blocks %.1f ms, 30 blocks %.1f ms, ratio %.2f "
              "(linear-ish ~4, quadratic ~16)\n",
              at_15,
              at_30,
              ratio);
  record("growth_ratio", ratio);
  EXPECT_LT(ratio, 8.0) << "cost is growing faster than the road count";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  measured_spec() = roadmaker::scale::spec_from_args(argc, argv);
  const int status = RUN_ALL_TESTS();

  // Always written, pass or fail: a red run's numbers are the interesting
  // ones. The CI job checks the keys before uploading.
  std::ofstream out("scale-metrics.json");
  out << "{\n";
  bool first = true;
  for (const auto& [key, value] : metrics()) {
    out << (first ? "" : ",\n") << "  \"" << key << "\": " << value;
    first = false;
  }
  out << "\n}\n";
  return status;
}
