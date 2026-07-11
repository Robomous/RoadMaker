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
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
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
