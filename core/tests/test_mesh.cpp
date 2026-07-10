#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinAbs;
using roadmaker::NetworkMesh;
using roadmaker::RoadMesh;

namespace {

roadmaker::RoadNetwork load_sample(const char* name) {
  auto result = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / name);
  REQUIRE(result.has_value());
  return std::move(result->network);
}

/// Signed area of a triangle in the XY plane (positive = CCW from +Z).
double
triangle_area_xy(const std::vector<double>& p, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  const double ax = p[a * 3], ay = p[(a * 3) + 1];
  const double bx = p[b * 3], by = p[(b * 3) + 1];
  const double cx = p[c * 3], cy = p[(c * 3) + 1];
  return 0.5 * (((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay)));
}

double patch_area_xy(const std::vector<double>& positions,
                     const std::vector<std::uint32_t>& indices) {
  double area = 0.0;
  for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
    area += triangle_area_xy(positions, indices[i], indices[i + 1], indices[i + 2]);
  }
  return area;
}

} // namespace

TEST_CASE("straight road meshes with correct extents and areas", "[mesh]") {
  const auto network = load_sample("straight_road.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);

  REQUIRE(mesh.roads.size() == 1);
  const RoadMesh& road = mesh.roads[0];

  // 4 lanes with area (sidewalk, driving x2, shoulder); center lane has none.
  REQUIRE(road.lanes.size() == 4);

  // Lateral extent: left sidewalk outer edge at +5.5, right shoulder at -5.
  double min_y = 1e9;
  double max_y = -1e9;
  for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
    min_y = std::min(min_y, road.positions[i + 1]);
    max_y = std::max(max_y, road.positions[i + 1]);
  }
  REQUIRE_THAT(max_y, WithinAbs(5.5, 1e-9));
  REQUIRE_THAT(min_y, WithinAbs(-5.0, 1e-9));

  // Flat road: all z = 0, all normals point up, all triangles CCW.
  for (std::size_t i = 2; i < road.positions.size(); i += 3) {
    REQUIRE_THAT(road.positions[i], WithinAbs(0.0, 1e-12));
  }
  for (std::size_t i = 2; i < road.normals.size(); i += 3) {
    REQUIRE_THAT(road.normals[i], WithinAbs(1.0, 1e-12));
  }

  // Per-lane plan-view areas match width x length.
  for (const RoadMesh::LanePatch& patch : road.lanes) {
    const double area = patch_area_xy(road.positions, patch.indices);
    REQUIRE(area > 0.0); // CCW everywhere
    double expected = 0.0;
    switch (patch.odr_lane_id) {
    case 2:
      expected = 2.0 * 100.0;
      break;
    case 1:
    case -1:
      expected = 3.5 * 100.0;
      break;
    case -2:
      expected = 1.5 * 100.0;
      break;
    default:
      FAIL("unexpected lane patch");
    }
    REQUIRE_THAT(area, WithinAbs(expected, 1e-6));
  }

  // Markings: center broken line + two solid edge lines.
  REQUIRE(road.markings.size() == 3);
  const auto broken = std::ranges::find_if(road.markings, [](const roadmaker::SubMesh& m) {
    return m.name.find("lane 0") != std::string::npos;
  });
  REQUIRE(broken != road.markings.end());
  // Dashes: 100 m at 3 m dash / 9 m cycle => 12 dashes, 2 quads... each dash
  // is one quad (2 triangles = 6 indices) at minimum.
  REQUIRE(broken->indices.size() >= 11 * 6);
  const auto solid = std::ranges::find_if(road.markings, [](const roadmaker::SubMesh& m) {
    return m.name.find("lane 1") != std::string::npos;
  });
  REQUIRE(solid != road.markings.end());
  // Solid strip area ~ width x length.
  REQUIRE_THAT(patch_area_xy(solid->positions, solid->indices), WithinAbs(0.12 * 100.0, 1e-3));
}

TEST_CASE("curved road applies elevation and superelevation", "[mesh]") {
  const auto network = load_sample("curved_road.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  REQUIRE(mesh.roads.size() == 1);
  const RoadMesh& road = mesh.roads[0];

  // Elevation: z rises along the road (b = 0.02 at s=0).
  double max_z = -1e9;
  for (std::size_t i = 2; i < road.positions.size(); i += 3) {
    max_z = std::max(max_z, road.positions[i]);
  }
  REQUIRE(max_z > 0.5);

  // Superelevation beyond s=30 tilts normals off vertical somewhere.
  bool tilted = false;
  for (std::size_t i = 2; i < road.normals.size(); i += 3) {
    if (road.normals[i] < 1.0 - 1e-6) {
      tilted = true;
      break;
    }
  }
  REQUIRE(tilted);
}

TEST_CASE("t_junction builds one junction floor", "[mesh]") {
  const auto network = load_sample("t_junction.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);

  REQUIRE(mesh.roads.size() == 5);
  REQUIRE(mesh.junction_floors.size() == 1);

  const roadmaker::SubMesh& floor = mesh.junction_floors[0];
  REQUIRE_FALSE(floor.indices.empty());

  // The floor must cover at least the through-connection footprint
  // (20 m x 3.5 m) and sit slightly below the surface.
  REQUIRE(patch_area_xy(floor.positions, floor.indices) > 20.0 * 3.5 * 0.9);
  REQUIRE(floor.positions[2] < 0.0);
}

TEST_CASE("mesh options can disable markings and floors", "[mesh]") {
  const auto network = load_sample("t_junction.xodr");
  roadmaker::MeshOptions options;
  options.markings = false;
  options.junction_floors = false;
  const NetworkMesh mesh = roadmaker::build_network_mesh(network, options);
  REQUIRE(mesh.junction_floors.empty());
  for (const RoadMesh& road : mesh.roads) {
    REQUIRE(road.markings.empty());
  }
}

TEST_CASE("degenerate networks produce empty meshes", "[mesh]") {
  const roadmaker::RoadNetwork network;
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  REQUIRE(mesh.roads.empty());
  REQUIRE(mesh.junction_floors.empty());
}
