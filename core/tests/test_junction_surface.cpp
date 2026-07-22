// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Acceptance tests for the M2 junction surface (issue #18,
// docs/design/m2/03_junction_blending.md): watertightness, unflipped
// height-field normals, seam-elevation continuity, deterministic regeneration,
// and the flat-floor fallback for tiny footprints.
//
// Scenarios are built through the real editor path — author_clothoid_road +
// edit::create_junction (issue #17) — because the surface is a pure function
// of network topology, not of any new xodr element. The env-gated
// WriteFixtures test serializes each scenario to core/tests/fixtures/junctions
// and the fuzz corpus so the committed fixtures can be regenerated on demand.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionCorner;
using roadmaker::JunctionCornerInfo;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::LaneSpec;
using roadmaker::LaneType;
using roadmaker::NetworkMesh;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMesh;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Waypoint;

namespace {

RoadId author(RoadNetwork& network,
              std::vector<Waypoint> waypoints,
              const char* odr_id,
              const LaneProfile& profile = LaneProfile::two_lane_default()) {
  auto road = roadmaker::author_clothoid_road(network, waypoints, profile, "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

/// Applies create_junction for `ends` and returns the generated junction.
JunctionId make_junction(RoadNetwork& network, const std::vector<RoadEnd>& ends) {
  auto command = roadmaker::edit::create_junction(network, ends);
  if (command == nullptr) {
    throw std::runtime_error("create_junction: null command");
  }
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("create_junction: " + applied.error().message);
  }
  JunctionId junction;
  network.for_each_junction([&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  return junction;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

// --- Scenario builders (also serialized by WriteFixtures) ---------------------

JunctionId build_t_junction(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  return make_junction(network, {end_of(west), end_of(east), end_of(south)});
}

JunctionId build_four_way(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, "4");
  return make_junction(network, {end_of(west), end_of(east), end_of(south), end_of(north)});
}

JunctionId build_five_way(RoadNetwork& network) {
  std::vector<RoadEnd> ends;
  for (int i = 0; i < 5; ++i) {
    const double angle = (2.0 * std::numbers::pi * static_cast<double>(i)) / 5.0;
    const double fx = 40.0 * std::cos(angle);
    const double fy = 40.0 * std::sin(angle);
    const double nx = 7.0 * std::cos(angle);
    const double ny = 7.0 * std::sin(angle);
    const RoadId arm =
        author(network, {Waypoint{fx, fy}, Waypoint{nx, ny}}, std::to_string(i + 1).c_str());
    ends.push_back(end_of(arm));
  }
  return make_junction(network, ends);
}

JunctionId build_merge_15deg(RoadNetwork& network) {
  // Two arms converging on the origin ~15° apart — near-parallel slivers.
  const double half = (15.0 / 2.0) * (std::numbers::pi / 180.0);
  const RoadId upper = author(
      network, {Waypoint{-40.0, 40.0 * std::tan(half)}, Waypoint{-6.0, 6.0 * std::tan(half)}}, "1");
  const RoadId lower =
      author(network,
             {Waypoint{-40.0, -40.0 * std::tan(half)}, Waypoint{-6.0, -6.0 * std::tan(half)}},
             "2");
  return make_junction(network, {end_of(upper), end_of(lower)});
}

JunctionId build_tiny_footprint(RoadNetwork& network) {
  // Arm ends ~1.2 m apart → a sub-4 m² connecting-road footprint → flat floor.
  const RoadId left = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-0.6, 0.0}}, "1");
  const RoadId right = author(network, {Waypoint{40.0, 0.0}, Waypoint{0.6, 0.0}}, "2");
  return make_junction(network, {end_of(left), end_of(right)});
}

// --- expect_watertight -------------------------------------------------------

double
triangle_area_xy(const std::vector<double>& p, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  const double ax = p[a * 3], ay = p[(a * 3) + 1];
  const double bx = p[b * 3], by = p[(b * 3) + 1];
  const double cx = p[c * 3], cy = p[(c * 3) + 1];
  return 0.5 * (((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay)));
}

/// The junction submesh is a valid 2.5D height-field surface stitched to the
/// road meshes (03 §5): manifold-with-boundary, no flipped normals, no slivers,
/// and every boundary vertex coincident with a road vertex shares its z exactly.
void expect_watertight(const NetworkMesh& mesh) {
  ASSERT_FALSE(mesh.junction_floors.empty());

  // All road (lane-grid) vertices, for the seam-coincidence check.
  std::vector<std::array<double, 3>> road_vertices;
  for (const RoadMesh& road : mesh.roads) {
    for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
      road_vertices.push_back({road.positions[i], road.positions[i + 1], road.positions[i + 2]});
    }
  }

  for (const roadmaker::JunctionFloor& jf : mesh.junction_floors) {
    const SubMesh& s = jf.mesh;
    ASSERT_FALSE(s.indices.empty());
    ASSERT_EQ(s.positions.size(), s.normals.size());
    const std::size_t vertex_count = s.positions.size() / 3;

    // (1) Manifold: no edge shared by more than two triangles; no degenerate
    //     triangles (a height field has strictly positive plan area).
    auto key = [](std::uint32_t a, std::uint32_t b) {
      return a < b ? std::pair{a, b} : std::pair{b, a};
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
    for (std::size_t i = 0; i + 2 < s.indices.size(); i += 3) {
      const std::uint32_t a = s.indices[i], b = s.indices[i + 1], c = s.indices[i + 2];
      EXPECT_GT(std::abs(triangle_area_xy(s.positions, a, b, c)), 1e-9) << "degenerate triangle";
      ++edge_count[key(a, b)];
      ++edge_count[key(b, c)];
      ++edge_count[key(c, a)];
    }
    // (2) Boundary edges (used once) form closed loops: every vertex has an
    //     even number of incident boundary edges.
    std::vector<int> boundary_degree(vertex_count, 0);
    for (const auto& [edge, count] : edge_count) {
      EXPECT_LE(count, 2) << "non-manifold edge";
      if (count == 1) {
        ++boundary_degree[edge.first];
        ++boundary_degree[edge.second];
      }
    }
    for (const int degree : boundary_degree) {
      EXPECT_EQ(degree % 2, 0) << "dangling boundary edge";
    }

    // (3) No flipped normals: every normal points into the +Z hemisphere and
    //     is unit length.
    for (std::size_t i = 0; i < vertex_count; ++i) {
      const double nx = s.normals[(i * 3)];
      const double ny = s.normals[(i * 3) + 1];
      const double nz = s.normals[(i * 3) + 2];
      EXPECT_GT(nz, 0.0) << "flipped normal at vertex " << i;
      EXPECT_NEAR(std::sqrt((nx * nx) + (ny * ny) + (nz * nz)), 1.0, 1e-9);
    }

    // (4) Seam continuity: a boundary vertex coincident (in xy) with a road
    //     vertex shares its elevation exactly (watertight stitch).
    for (std::size_t i = 0; i < vertex_count; ++i) {
      if (boundary_degree[i] == 0) {
        continue;
      }
      const double x = s.positions[(i * 3)];
      const double y = s.positions[(i * 3) + 1];
      const double z = s.positions[(i * 3) + 2];
      for (const auto& rv : road_vertices) {
        const double d2 = ((rv[0] - x) * (rv[0] - x)) + ((rv[1] - y) * (rv[1] - y));
        if (d2 < 1e-12) { // coincident in plan → must share elevation
          EXPECT_NEAR(rv[2], z, 1e-9) << "seam z mismatch at boundary vertex " << i;
        }
      }
    }
  }
}

// --- Authored corner overlays (p4-s2, issue #226) ----------------------------

/// A four-way with a 20 m arm gap — roomy enough that the derived fillet is
/// bounded by the derivation and not clamped to a couple of meters by the arm
/// faces, so the corner wedges enclose real area.
JunctionId build_roomy_four_way(RoadNetwork& network,
                                const LaneProfile& profile = LaneProfile::two_lane_default()) {
  const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1", profile);
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2", profile);
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3", profile);
  const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4", profile);
  return make_junction(network, {end_of(west), end_of(east), end_of(south), end_of(north)});
}

/// Urban cross-section with a median lane innermost on both sides — the
/// profile the median-nose overlay needs.
LaneProfile median_profile() {
  return LaneProfile{
      .left = {LaneSpec{.type = LaneType::Median, .width = 2.0},
               LaneSpec{.type = LaneType::Driving, .width = 3.5, .outer_marking = true}},
      .right = {LaneSpec{.type = LaneType::Median, .width = 2.0},
                LaneSpec{.type = LaneType::Driving, .width = 3.5, .outer_marking = true}},
      .center_marking = false,
  };
}

/// Sets (or replaces) the authored override for an arm pair. Corner entries are
/// plain data on the junction; the editor reaches them through the command
/// layer, tests through the arena.
void set_corner(RoadNetwork& network, JunctionId id, const JunctionCorner& entry) {
  Junction* junction = network.junction(id);
  const auto match = [&entry](const JunctionCorner& c) {
    return c.arm_a == entry.arm_a && c.arm_b == entry.arm_b;
  };
  const auto it = std::ranges::find_if(junction->corners, match);
  if (it != junction->corners.end()) {
    *it = entry;
  } else {
    junction->corners.push_back(entry);
  }
}

/// The solved corner of `id` in which `arm` plays the requested role.
JunctionCornerInfo
corner_with(const RoadNetwork& network, JunctionId id, const RoadEnd& arm, bool as_arm_a) {
  for (const JunctionCornerInfo& info : roadmaker::junction_corners(network, id)) {
    if ((as_arm_a ? info.arm_a : info.arm_b) == arm) {
      return info;
    }
  }
  throw std::runtime_error("corner_with: no such corner");
}

/// Every overlay of the (single) junction floor carrying `material`.
std::vector<const SubMesh*> details_of(const NetworkMesh& mesh, LaneType material) {
  std::vector<const SubMesh*> found;
  for (const roadmaker::JunctionFloor& floor : mesh.junction_floors) {
    for (const SubMesh& detail : floor.details) {
      if (detail.material == material) {
        found.push_back(&detail);
      }
    }
  }
  return found;
}

std::array<double, 2> centroid_xy(const SubMesh& s) {
  double x = 0.0, y = 0.0;
  const std::size_t count = s.positions.size() / 3;
  for (std::size_t i = 0; i < count; ++i) {
    x += s.positions[i * 3];
    y += s.positions[(i * 3) + 1];
  }
  const auto n = static_cast<double>(std::max<std::size_t>(count, 1));
  return {x / n, y / n};
}

/// The median nose sitting at an arm's mouth — identified by geometry, since a
/// junction paints one nose per arm and they all share a name.
const SubMesh* nose_near(const NetworkMesh& mesh, const std::array<double, 2>& at) {
  const SubMesh* best = nullptr;
  double best_d2 = 9.0; // within 3 m of the expected mouth
  for (const SubMesh* nose : details_of(mesh, LaneType::Median)) {
    const std::array<double, 2> c = centroid_xy(*nose);
    const double d2 = ((c[0] - at[0]) * (c[0] - at[0])) + ((c[1] - at[1]) * (c[1] - at[1]));
    if (d2 < best_d2) {
      best_d2 = d2;
      best = nose;
    }
  }
  return best;
}

/// The floor is never cut for an overlay: its buffers must stay bit-identical.
void expect_floor_identical(const NetworkMesh& before, const NetworkMesh& after) {
  ASSERT_EQ(before.junction_floors.size(), after.junction_floors.size());
  ASSERT_FALSE(before.junction_floors.empty());
  for (std::size_t i = 0; i < before.junction_floors.size(); ++i) {
    EXPECT_EQ(before.junction_floors[i].mesh.positions, after.junction_floors[i].mesh.positions);
    EXPECT_EQ(before.junction_floors[i].mesh.normals, after.junction_floors[i].mesh.normals);
    EXPECT_EQ(before.junction_floors[i].mesh.indices, after.junction_floors[i].mesh.indices);
  }
}

} // namespace

TEST(JunctionSurface, TJunctionIsWatertightHeightField) {
  RoadNetwork network;
  build_t_junction(network);
  expect_watertight(roadmaker::build_network_mesh(network));
}

TEST(JunctionSurface, FourWayGoldenIsWatertight) {
  RoadNetwork network;
  build_four_way(network);
  expect_watertight(roadmaker::build_network_mesh(network));
}

TEST(JunctionSurface, FiveWayIsWatertight) {
  RoadNetwork network;
  build_five_way(network);
  expect_watertight(roadmaker::build_network_mesh(network));
}

TEST(JunctionSurface, Merge15DegIsWatertight) {
  RoadNetwork network;
  build_merge_15deg(network);
  expect_watertight(roadmaker::build_network_mesh(network));
}

TEST(JunctionSurface, RegenerationIsByteIdentical) {
  RoadNetwork network;
  build_four_way(network);
  const NetworkMesh a = roadmaker::build_network_mesh(network);
  const NetworkMesh b = roadmaker::build_network_mesh(network);
  ASSERT_EQ(a.junction_floors.size(), b.junction_floors.size());
  ASSERT_FALSE(a.junction_floors.empty());
  for (std::size_t i = 0; i < a.junction_floors.size(); ++i) {
    EXPECT_EQ(a.junction_floors[i].mesh.positions, b.junction_floors[i].mesh.positions);
    EXPECT_EQ(a.junction_floors[i].mesh.normals, b.junction_floors[i].mesh.normals);
    EXPECT_EQ(a.junction_floors[i].mesh.indices, b.junction_floors[i].mesh.indices);
  }
}

TEST(JunctionSurface, GradeMismatchStaysWithinIncomingBounds) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);

  // Give the connecting roads a spread of constant elevations (as elevation
  // editing / node moves would, marking the junction dirty). The harmonic
  // membrane must not overshoot the boundary elevations (maximum principle —
  // no bumps, 03 §6 "steep grade mismatch").
  double sign = 1.0;
  network.for_each_road([&](RoadId, roadmaker::Road& road) {
    if (road.junction == junction) {
      road.elevation = {{.s = 0.0, .a = sign * 0.75}};
      sign = -sign;
    }
  });
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(mesh.junction_floors.empty());
  const SubMesh& s = mesh.junction_floors[0].mesh;

  double min_boundary_z = std::numeric_limits<double>::max();
  double max_boundary_z = std::numeric_limits<double>::lowest();
  // Boundary vertices carry the Dirichlet values; recover them via edge count.
  auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
  for (std::size_t i = 0; i + 2 < s.indices.size(); i += 3) {
    ++edge_count[key(s.indices[i], s.indices[i + 1])];
    ++edge_count[key(s.indices[i + 1], s.indices[i + 2])];
    ++edge_count[key(s.indices[i + 2], s.indices[i])];
  }
  std::vector<bool> on_boundary(s.positions.size() / 3, false);
  for (const auto& [edge, count] : edge_count) {
    if (count == 1) {
      on_boundary[edge.first] = true;
      on_boundary[edge.second] = true;
    }
  }
  for (std::size_t i = 0; i < on_boundary.size(); ++i) {
    if (on_boundary[i]) {
      min_boundary_z = std::min(min_boundary_z, s.positions[(i * 3) + 2]);
      max_boundary_z = std::max(max_boundary_z, s.positions[(i * 3) + 2]);
    }
  }
  ASSERT_LT(min_boundary_z, max_boundary_z);
  for (std::size_t i = 2; i < s.positions.size(); i += 3) {
    EXPECT_GE(s.positions[i], min_boundary_z - 1e-6);
    EXPECT_LE(s.positions[i], max_boundary_z + 1e-6);
  }
}

TEST(JunctionSurface, TinyFootprintFallsBackToFlatFloor) {
  RoadNetwork network;
  build_tiny_footprint(network);
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(mesh.junction_floors.empty());
  const SubMesh& s = mesh.junction_floors[0].mesh;

  // Flat floor: a single elevation everywhere, all normals exactly +Z.
  const double z0 = s.positions[2];
  for (std::size_t i = 2; i < s.positions.size(); i += 3) {
    EXPECT_NEAR(s.positions[i], z0, 1e-9);
  }
  for (std::size_t i = 0; i + 2 < s.normals.size(); i += 3) {
    EXPECT_NEAR(s.normals[i], 0.0, 1e-12);
    EXPECT_NEAR(s.normals[i + 1], 0.0, 1e-12);
    EXPECT_NEAR(s.normals[i + 2], 1.0, 1e-12);
  }
}

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed .xodr fixtures + fuzz-corpus entries by running the
// test binary with --gtest_also_run_disabled_tests and
// --gtest_filter='*DISABLED_WriteFixtures'.
TEST(JunctionSurface, DISABLED_WriteFixtures) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(RM_FIXTURES_DIR);
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);
  fs::create_directories(dir);

  // The T-junction golden is already a committed sample (assets/samples); these
  // are the new degenerate goldens for #18.
  const std::vector<std::pair<std::string, JunctionId (*)(RoadNetwork&)>> scenarios{
      {"four_way", build_four_way},
      {"five_way", build_five_way},
      {"merge_15deg", build_merge_15deg},
      {"tiny_footprint", build_tiny_footprint},
  };
  for (const auto& [name, builder] : scenarios) {
    RoadNetwork network;
    builder(network);
    const std::string file = name + ".xodr";
    ASSERT_TRUE(roadmaker::save_xodr(network, dir / file, name).has_value()) << name;
    ASSERT_TRUE(roadmaker::save_xodr(network, corpus / file, name).has_value()) << name;
  }
}

// Loads every committed fixture and asserts the surface is watertight — proves
// the .xodr fixtures parse and blend, complementing the in-memory scenarios.
TEST(JunctionSurface, CommittedFixturesAreWatertight) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(RM_FIXTURES_DIR);
  if (!fs::exists(dir)) {
    GTEST_SKIP() << "no committed fixtures yet";
  }
  int loaded = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() != ".xodr") {
      continue;
    }
    auto result = roadmaker::load_xodr(entry.path());
    ASSERT_TRUE(result.has_value()) << entry.path().filename();
    const NetworkMesh mesh = roadmaker::build_network_mesh(result->network);
    if (mesh.junction_floors.empty()) {
      continue; // tiny_footprint below the flat-floor threshold may still floor
    }
    expect_watertight(mesh);
    ++loaded;
  }
  EXPECT_GT(loaded, 0);
}

// --- Authored corner overlays (p4-s2, issue #226) ----------------------------

TEST(JunctionSurface, NoAuthoredMaterialsEmitsNoDetailsAndIdenticalFloor) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(baseline.junction_floors.empty());
  EXPECT_TRUE(baseline.junction_floors[0].details.empty());

  // A corner entry that authors no material at all must change nothing: no
  // overlay, and a floor that is still bit-identical (overlays never cut it).
  const JunctionCornerInfo info = roadmaker::junction_corners(network, junction).at(0);
  set_corner(network, junction, JunctionCorner{.arm_a = info.arm_a, .arm_b = info.arm_b});
  const NetworkMesh after = roadmaker::build_network_mesh(network);
  EXPECT_TRUE(after.junction_floors[0].details.empty());
  expect_floor_identical(baseline, after);
}

TEST(JunctionSurface, CornerSidewalkMaterialEmitsSidewalkWedge) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);

  const JunctionCornerInfo info = roadmaker::junction_corners(network, junction).at(0);
  set_corner(network,
             junction,
             JunctionCorner{.arm_a = info.arm_a,
                            .arm_b = info.arm_b,
                            .sidewalk_material = std::string("concrete")});

  const NetworkMesh after = roadmaker::build_network_mesh(network);
  const std::vector<const SubMesh*> wedges = details_of(after, LaneType::Sidewalk);
  ASSERT_EQ(wedges.size(), 1U);
  EXPECT_EQ(wedges[0]->surface, "concrete");
  EXPECT_FALSE(wedges[0]->indices.empty());
  EXPECT_EQ(wedges[0]->indices.size() % 3, 0U);
  EXPECT_EQ(wedges[0]->positions.size(), wedges[0]->normals.size());
  // The wedge covers the rounded-away corner: every vertex sits within the
  // fillet's reach of the edge-line intersection, above the floor.
  for (std::size_t i = 0; i + 2 < wedges[0]->positions.size(); i += 3) {
    EXPECT_LT(std::hypot(wedges[0]->positions[i] - info.corner[0],
                         wedges[0]->positions[i + 1] - info.corner[1]),
              info.max_extent_a + info.max_extent_b);
  }
  // Overlay only — the floor itself is untouched.
  expect_floor_identical(baseline, after);
}

TEST(JunctionSurface, MedianNoseEmittedForMedianArm) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network, median_profile());
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);
  EXPECT_TRUE(details_of(baseline, LaneType::Median).empty());

  const JunctionCornerInfo info = roadmaker::junction_corners(network, junction).at(0);
  set_corner(network,
             junction,
             JunctionCorner{.arm_a = info.arm_a,
                            .arm_b = info.arm_b,
                            .median_material = std::string("grass")});

  const NetworkMesh after = roadmaker::build_network_mesh(network);
  const std::vector<const SubMesh*> noses = details_of(after, LaneType::Median);
  ASSERT_FALSE(noses.empty());
  for (const SubMesh* nose : noses) {
    EXPECT_EQ(nose->surface, "grass");
    EXPECT_FALSE(nose->indices.empty());
    EXPECT_EQ(nose->indices.size() % 3, 0U);
    // Flat, and lifted clear of the floor it covers.
    const double z0 = nose->positions[2];
    for (std::size_t i = 2; i < nose->positions.size(); i += 3) {
      EXPECT_NEAR(nose->positions[i], z0, 1e-12);
    }
  }
  expect_floor_identical(baseline, after);
}

TEST(JunctionSurface, MedianNoseSharedArmPrefersArmACorner) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network, median_profile());
  // The west arm ("1"): its face is at x = -20 and the nose reaches 2 m in.
  RoadEnd arm{};
  network.for_each_road([&](RoadId road_id, const roadmaker::Road& road) {
    if (road.odr_id == "1") {
      arm = end_of(road_id);
    }
  });

  const JunctionCornerInfo as_a = corner_with(network, junction, arm, /*as_arm_a=*/true);
  const JunctionCornerInfo as_b = corner_with(network, junction, arm, /*as_arm_a=*/false);
  set_corner(network,
             junction,
             JunctionCorner{.arm_a = as_a.arm_a,
                            .arm_b = as_a.arm_b,
                            .median_material = std::string("primary")});
  set_corner(network,
             junction,
             JunctionCorner{.arm_a = as_b.arm_a,
                            .arm_b = as_b.arm_b,
                            .median_material = std::string("fallback")});

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh* nose = nose_near(mesh, {-19.0, 0.0});
  ASSERT_NE(nose, nullptr);
  EXPECT_EQ(nose->surface, "primary");
}

TEST(JunctionSurface, MedianNoseFallsBackToArmBCorner) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network, median_profile());
  RoadEnd arm{};
  network.for_each_road([&](RoadId road_id, const roadmaker::Road& road) {
    if (road.odr_id == "1") {
      arm = end_of(road_id);
    }
  });

  // Only the corner where the west arm is arm_b authors a median material.
  const JunctionCornerInfo as_b = corner_with(network, junction, arm, /*as_arm_a=*/false);
  set_corner(network,
             junction,
             JunctionCorner{.arm_a = as_b.arm_a,
                            .arm_b = as_b.arm_b,
                            .median_material = std::string("fallback")});

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh* nose = nose_near(mesh, {-19.0, 0.0});
  ASSERT_NE(nose, nullptr);
  EXPECT_EQ(nose->surface, "fallback");
}

TEST(JunctionSurface, JunctionFloorCarriesMaterialCode) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  EXPECT_TRUE(roadmaker::build_network_mesh(network).junction_floors[0].mesh.surface.empty());

  network.junction(junction)->material = "concrete";
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_FALSE(mesh.junction_floors.empty());
  EXPECT_EQ(mesh.junction_floors[0].mesh.surface, "concrete");
}

// --- Authored surface spans (p4-s5, issue #320) ------------------------------

namespace {

/// Plan area of every junction floor in the mesh — the coverage an excluded
/// span must not change.
double floor_area(const NetworkMesh& mesh) {
  double total = 0.0;
  for (const roadmaker::JunctionFloor& floor : mesh.junction_floors) {
    for (std::size_t i = 0; i + 2 < floor.mesh.indices.size(); i += 3) {
      total += std::abs(triangle_area_xy(floor.mesh.positions,
                                         floor.mesh.indices[i],
                                         floor.mesh.indices[i + 1],
                                         floor.mesh.indices[i + 2]));
    }
  }
  return total;
}

/// Gives the connecting roads a spread of constant elevations, the way a node
/// move or elevation edit would. On a FLAT junction every span's border agrees
/// on z = 0, so which span supplies the Dirichlet value is unobservable —
/// grade is what makes the span controls have anything to say.
///
/// The arms are deliberately left flat, so the floor and the arm road meshes
/// disagree at the mouth BY CONSTRUCTION. Tests built on this fixture therefore
/// never assert expect_watertight — for the same reason
/// GradeMismatchStaysWithinIncomingBounds does not. Watertightness under the
/// span controls is asserted on the flat fixture instead.
void grade_turns(RoadNetwork& network, JunctionId junction) {
  double sign = 1.0;
  network.for_each_road([&](RoadId, roadmaker::Road& road) {
    if (road.junction == junction) {
      road.elevation = {{.s = 0.0, .a = sign * 0.75}};
      sign = -sign;
    }
  });
}

} // namespace

TEST(JunctionSurface, SpanQueryMatchesTheMesherInputs) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const std::vector<roadmaker::JunctionSurfaceSpanInfo> spans =
      roadmaker::junction_surface_spans(network, junction);
  ASSERT_FALSE(spans.empty());

  // One span per connecting road, in connection order, de-duplicated — the
  // same set and order the floor union is built from.
  std::vector<RoadId> turns;
  for (const roadmaker::JunctionConnection& connection : network.junction(junction)->connections) {
    if (std::ranges::find(turns, connection.connecting_road) == turns.end()) {
      turns.push_back(connection.connecting_road);
    }
  }
  ASSERT_EQ(spans.size(), turns.size());
  for (std::size_t i = 0; i < spans.size(); ++i) {
    EXPECT_EQ(spans[i].road, turns[i]);
    EXPECT_EQ(spans[i].road_odr_id, network.road(turns[i])->odr_id);
    EXPECT_TRUE(spans[i].included);
    EXPECT_EQ(spans[i].sort_index, 0);
    EXPECT_FALSE(spans[i].authored);
    EXPECT_GE(spans[i].footprint.size(), 3U);
    EXPECT_EQ(spans[i].border.size(), spans[i].footprint.size());
    EXPECT_EQ(spans[i].centerline.size(), spans[i].footprint.size() / 2);
  }

  // A stale id, and a junction with no floor to control, both yield nothing.
  EXPECT_TRUE(roadmaker::junction_surface_spans(network, JunctionId{}).empty());
  RoadNetwork bare;
  EXPECT_TRUE(roadmaker::junction_surface_spans(bare, bare.create_junction("9", "empty")).empty());
}

TEST(JunctionSurface, SpanQueryReportsAuthoredValues) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const RoadId first = roadmaker::junction_surface_spans(network, junction).front().road;
  network.junction(junction)->surface_spans.push_back(
      roadmaker::SurfaceSpan{.road = first, .included = false, .sort_index = 3});

  const roadmaker::JunctionSurfaceSpanInfo info =
      roadmaker::junction_surface_spans(network, junction).front();
  EXPECT_EQ(info.road, first);
  EXPECT_FALSE(info.included);
  EXPECT_EQ(info.sort_index, 3);
  EXPECT_TRUE(info.authored);
}

TEST(JunctionSurface, DefaultedSpanRecordsAreByteIdenticalToNoRecords) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);

  // The fast path is keyed on the EFFECTIVE values, not on the absence of
  // records: a junction carrying nothing but defaulted records must still take
  // the verbatim legacy path.
  for (const roadmaker::JunctionSurfaceSpanInfo& info :
       roadmaker::junction_surface_spans(network, junction)) {
    network.junction(junction)->surface_spans.push_back(roadmaker::SurfaceSpan{.road = info.road});
  }
  expect_floor_identical(baseline, roadmaker::build_network_mesh(network));
}

TEST(JunctionSurface, ExcludedSpanKeepsCoverageAndStaysWatertight) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);
  const RoadId first = roadmaker::junction_surface_spans(network, junction).front().road;

  network.junction(junction)->surface_spans.push_back(
      roadmaker::SurfaceSpan{.road = first, .included = false});
  const NetworkMesh excluded = roadmaker::build_network_mesh(network);

  // Exclusion is SAMPLES-ONLY: the footprint stays in the union, so the floor
  // still paves exactly the same ground and <boundary> never moves...
  EXPECT_NEAR(floor_area(excluded), floor_area(baseline), 1e-6);
  // ...and it is still a valid, seam-exact height field. The seam-exactness
  // exception is what carries check 4 here: an excluded span's border vertices
  // are still snap targets and still hand their exact z to any floor vertex
  // that lands on them.
  expect_watertight(excluded);
}

TEST(JunctionSurface, ExcludedSpanDropsItsElevationInfluence) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  grade_turns(network, junction);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);
  const RoadId first = roadmaker::junction_surface_spans(network, junction).front().road;

  network.junction(junction)->surface_spans.push_back(
      roadmaker::SurfaceSpan{.road = first, .included = false});
  const NetworkMesh excluded = roadmaker::build_network_mesh(network);

  // The escape valve did something: with the grades spread, the excluded
  // span's border no longer supplies any elevation it used to win.
  EXPECT_NE(baseline.junction_floors[0].mesh.positions, excluded.junction_floors[0].mesh.positions);
  EXPECT_NEAR(floor_area(excluded), floor_area(baseline), 1e-6);
}

TEST(JunctionSurface, SortIndexFlipsTheElevationWinnerDeterministically) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  grade_turns(network, junction);
  const NetworkMesh baseline = roadmaker::build_network_mesh(network);
  const std::vector<roadmaker::JunctionSurfaceSpanInfo> spans =
      roadmaker::junction_surface_spans(network, junction);
  ASSERT_GE(spans.size(), 2U);

  // Raising one span above every other changes which border supplies the
  // elevation wherever the ribbons overlap.
  network.junction(junction)->surface_spans.push_back(
      roadmaker::SurfaceSpan{.road = spans.front().road, .sort_index = 1});
  const NetworkMesh raised = roadmaker::build_network_mesh(network);
  EXPECT_NE(baseline.junction_floors[0].mesh.positions, raised.junction_floors[0].mesh.positions);

  // Deterministic: the same network builds byte-identically twice, and the
  // plan-view triangulation is untouched (priority only moves elevations).
  expect_floor_identical(raised, roadmaker::build_network_mesh(network));
  EXPECT_EQ(raised.junction_floors[0].mesh.indices, baseline.junction_floors[0].mesh.indices);
}

TEST(JunctionSurface, ExcludedAndRaisedSpansSurviveSaveReload) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  grade_turns(network, junction);
  const std::vector<roadmaker::JunctionSurfaceSpanInfo> spans =
      roadmaker::junction_surface_spans(network, junction);
  ASSERT_GE(spans.size(), 2U);
  network.junction(junction)->surface_spans = {
      roadmaker::SurfaceSpan{.road = spans[0].road, .included = false},
      roadmaker::SurfaceSpan{.road = spans[1].road, .sort_index = 2}};
  const NetworkMesh before = roadmaker::build_network_mesh(network);

  const auto xml = roadmaker::write_xodr(network, "surface_spans");
  ASSERT_TRUE(xml.has_value());
  auto reparsed = roadmaker::parse_xodr(*xml, "surface_spans");
  ASSERT_TRUE(reparsed.has_value());
  expect_floor_identical(before, roadmaker::build_network_mesh(reparsed->network));
}
