// Acceptance tests for the M2 junction surface EXPORT (issue #19,
// docs/design/m2/03_junction_blending.md §3): the OpenDRIVE ≥1.8 <planView>
// reference line and <elevationGrid> the writer derives from a junction's
// blended 2.5D surface, plus the structured warning that <boundary> is omitted.
//
// The tests are black-box: they serialize a network with write_xodr and parse
// the emitted junction back with pugixml, so they check exactly what a consumer
// (esmini, a validator) sees. The "true" elevation field is the junction
// surface mesh from the public build_network_mesh, sampled here the same way
// the exporter samples it — the grid must reconstruct it within tolerance.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <pugixml.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Waypoint;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return *road;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

JunctionId make_junction(RoadNetwork& network, const std::vector<RoadEnd>& ends) {
  auto command = roadmaker::edit::create_junction(network, ends);
  EXPECT_NE(command, nullptr);
  EXPECT_TRUE(command->apply(network).has_value());
  JunctionId junction;
  network.for_each_junction([&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  return junction;
}

JunctionId build_four_way(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, "4");
  return make_junction(network, {end_of(west), end_of(east), end_of(south), end_of(north)});
}

/// Constant, arm-specific elevations so the harmonic field bends across the
/// junction (mirrors the surface test's GradeMismatch scenario).
void grade_arms(RoadNetwork& network) {
  int i = 0;
  network.for_each_road([&](RoadId, roadmaker::Road& road) {
    if (!road.junction.is_valid()) { // an incoming arm, not a connecting road
      const double sign = (i % 2 == 0) ? 1.0 : -1.0;
      road.elevation = {{.s = 0.0, .a = sign * 0.6}};
      ++i;
    }
  });
}

// --- field sampling (the surface mesh is the ground-truth elevation) ---------

const SubMesh& junction_mesh(const roadmaker::NetworkMesh& mesh) {
  EXPECT_FALSE(mesh.junction_floors.empty());
  return mesh.junction_floors.front().mesh;
}

/// z of the surface at (px, py): barycentric inside the containing triangle, or
/// NaN when the point is outside the footprint (so callers can skip it).
double sample_surface(const SubMesh& m, double px, double py) {
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const auto vx = [&](std::size_t k) { return m.positions[(3 * m.indices[t + k])]; };
    const auto vy = [&](std::size_t k) { return m.positions[(3 * m.indices[t + k]) + 1]; };
    const auto vz = [&](std::size_t k) { return m.positions[(3 * m.indices[t + k]) + 2]; };
    const double ax = vx(0), ay = vy(0), bx = vx(1), by = vy(1), cx = vx(2), cy = vy(2);
    const double det = ((by - cy) * (ax - cx)) + ((cx - bx) * (ay - cy));
    if (std::abs(det) < 1e-12) {
      continue;
    }
    const double l0 = (((by - cy) * (px - cx)) + ((cx - bx) * (py - cy))) / det;
    const double l1 = (((cy - ay) * (px - cx)) + ((ax - cx) * (py - cy))) / det;
    const double l2 = 1.0 - l0 - l1;
    if (l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9) {
      return (l0 * vz(0)) + (l1 * vz(1)) + (l2 * vz(2));
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// --- parsed grid -------------------------------------------------------------

struct ParsedGrid {
  double s_start = 0.0;
  double spacing = 0.0;
  double ref_x = 0.0, ref_y = 0.0, ref_hdg = 0.0, ref_len = 0.0;
  std::size_t n_left = 0, n_right = 0;
  std::vector<std::vector<double>> rows; // per column, outermost-right → … → outermost-left
};

std::vector<double> parse_list(const char* attr) {
  std::vector<double> out;
  if (attr == nullptr) {
    return out;
  }
  std::istringstream in(attr);
  double v = 0.0;
  while (in >> v) {
    out.push_back(v);
  }
  return out;
}

ParsedGrid parse_first_junction(const std::string& xml) {
  pugi::xml_document doc;
  EXPECT_TRUE(static_cast<bool>(doc.load_string(xml.c_str())));
  const pugi::xml_node junction = doc.child("OpenDRIVE").child("junction");
  EXPECT_FALSE(junction.empty());

  // Reference line: exactly one <planView>/<geometry>/<line>.
  const pugi::xml_node plan_view = junction.child("planView");
  EXPECT_FALSE(plan_view.empty());
  EXPECT_TRUE(plan_view.next_sibling("planView").empty()); // only one
  const pugi::xml_node geometry = plan_view.child("geometry");
  EXPECT_TRUE(geometry.next_sibling("geometry").empty()); // one <geometry>
  EXPECT_FALSE(geometry.child("line").empty());           // with a <line>
  EXPECT_TRUE(geometry.child("arc").empty());
  EXPECT_TRUE(geometry.child("spiral").empty());
  EXPECT_TRUE(geometry.child("paramPoly3").empty());

  ParsedGrid grid;
  grid.ref_x = geometry.attribute("x").as_double();
  grid.ref_y = geometry.attribute("y").as_double();
  grid.ref_hdg = geometry.attribute("hdg").as_double();
  grid.ref_len = geometry.attribute("length").as_double();

  const pugi::xml_node grid_node = junction.child("elevationGrid");
  EXPECT_FALSE(grid_node.empty());
  EXPECT_TRUE(grid_node.next_sibling("elevationGrid").empty()); // only_one_elev_grid
  grid.s_start = grid_node.attribute("sStart").as_double();
  grid.spacing = grid_node.attribute("gridSpacing").as_double();

  for (pugi::xml_node e = grid_node.child("elevation"); e; e = e.next_sibling("elevation")) {
    const std::vector<double> left = parse_list(e.attribute("left").value());
    const std::vector<double> right = parse_list(e.attribute("right").value());
    const double center = e.attribute("center").as_double();
    if (grid.rows.empty()) {
      grid.n_left = left.size();
      grid.n_right = right.size();
    }
    // Rectangular grid: every column has the same left/right counts.
    EXPECT_EQ(left.size(), grid.n_left);
    EXPECT_EQ(right.size(), grid.n_right);
    std::vector<double> row;
    for (auto it = right.rbegin(); it != right.rend(); ++it) { // outermost-right first
      row.push_back(*it);
    }
    row.push_back(center);
    row.insert(row.end(), left.begin(), left.end()); // …then out to the left
    grid.rows.push_back(std::move(row));
  }
  return grid;
}

/// World position of grid node (column i, row j) — j indexed from outermost
/// right (0) through center (n_right) to outermost left.
std::array<double, 2> grid_point(const ParsedGrid& g, std::size_t i, std::size_t j) {
  const double dirx = std::cos(g.ref_hdg), diry = std::sin(g.ref_hdg);
  const double perpx = -std::sin(g.ref_hdg), perpy = std::cos(g.ref_hdg);
  const double s = g.s_start + (static_cast<double>(i) * g.spacing);
  const double t = (static_cast<double>(j) - static_cast<double>(g.n_right)) * g.spacing;
  return {g.ref_x + (s * dirx) + (t * perpx), g.ref_y + (s * diry) + (t * perpy)};
}

/// Catmull-Rom cubic through p0..p3 evaluated at u∈[0,1] between p1 and p2 —
/// the bicubic reconstruction the spec builds from cubics through four support
/// points (12.11.1), with central-difference tangents.
double catmull(double p0, double p1, double p2, double p3, double u) {
  return 0.5 *
         ((2.0 * p1) + ((-p0 + p2) * u) + (((2.0 * p0) - (5.0 * p1) + (4.0 * p2) - p3) * u * u) +
          ((-p0 + (3.0 * p1) - (3.0 * p2) + p3) * u * u * u));
}

double bicubic(const ParsedGrid& g, std::size_t i, std::size_t j, double u, double v) {
  std::array<double, 4> col{};
  for (int di = -1; di <= 2; ++di) {
    const std::vector<double>& row = g.rows[i + static_cast<std::size_t>(di)];
    col[static_cast<std::size_t>(di + 1)] = catmull(row[j - 1], row[j], row[j + 1], row[j + 2], v);
  }
  return catmull(col[0], col[1], col[2], col[3], u);
}

} // namespace

TEST(JunctionExport, ReferenceLineIsASingleLineElement) {
  RoadNetwork network;
  build_four_way(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const ParsedGrid grid = parse_first_junction(*xml); // asserts single line internally
  EXPECT_GT(grid.ref_len, 0.0);
}

TEST(JunctionExport, GridIsRectangularWithSpecSpacing) {
  RoadNetwork network;
  build_four_way(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const ParsedGrid grid = parse_first_junction(*xml);

  // Spacing is max(2, extent/32); a ~18 m junction stays at the 2 m floor.
  EXPECT_GE(grid.spacing, 2.0);
  EXPECT_GE(grid.rows.size(), 3U); // enough columns for bicubic support
  EXPECT_GT(grid.n_left, 0U);
  EXPECT_GT(grid.n_right, 0U);
}

TEST(JunctionExport, ReferenceLineReachesEntireFootprint) {
  // junctions.geometry.ref_line_definition: every junction point is reachable
  // by a perpendicular from the reference line, and lies within the grid's
  // transverse span.
  RoadNetwork network;
  build_four_way(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const ParsedGrid grid = parse_first_junction(*xml);

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh& surface = junction_mesh(mesh);
  const double dirx = std::cos(grid.ref_hdg), diry = std::sin(grid.ref_hdg);
  const double perpx = -std::sin(grid.ref_hdg), perpy = std::cos(grid.ref_hdg);
  const double t_span_left = static_cast<double>(grid.n_left) * grid.spacing;
  const double t_span_right = static_cast<double>(grid.n_right) * grid.spacing;

  for (std::size_t i = 0; i * 3 < surface.positions.size(); ++i) {
    const double dx = surface.positions[(3 * i)] - grid.ref_x;
    const double dy = surface.positions[(3 * i) + 1] - grid.ref_y;
    const double s = (dx * dirx) + (dy * diry);
    const double t = (dx * perpx) + (dy * perpy);
    EXPECT_GE(s, -1e-9);
    EXPECT_LE(s, grid.ref_len + 1e-9);
    EXPECT_LE(t, t_span_left + 1e-9);
    EXPECT_GE(t, -t_span_right - 1e-9);
  }
}

TEST(JunctionExport, GridValuesMatchTheSurfaceAtGridPoints) {
  // The serialized z at every in-footprint grid point equals the field there:
  // the exporter samples the same mesh, and num() round-trips the double.
  RoadNetwork network;
  build_four_way(network);
  grade_arms(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const ParsedGrid grid = parse_first_junction(*xml);
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh& surface = junction_mesh(mesh);

  std::size_t checked = 0;
  for (std::size_t i = 0; i < grid.rows.size(); ++i) {
    for (std::size_t j = 0; j < grid.rows[i].size(); ++j) {
      const auto p = grid_point(grid, i, j);
      const double truth = sample_surface(surface, p[0], p[1]);
      if (std::isnan(truth)) {
        continue; // margin square outside the footprint
      }
      EXPECT_NEAR(grid.rows[i][j], truth, 1e-9);
      ++checked;
    }
  }
  EXPECT_GT(checked, 0U);
}

TEST(JunctionExport, BicubicReconstructionTracksSurfaceAtNyquist) {
  // Reconstruct the field at grid-cell centers (the Nyquist limit) from the
  // exported grid via the spec's bicubic scheme and compare to the surface —
  // the chosen spacing keeps the coarse grid faithful (03 §3).
  RoadNetwork network;
  build_four_way(network);
  grade_arms(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const ParsedGrid grid = parse_first_junction(*xml);
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const SubMesh& surface = junction_mesh(mesh);

  double max_err = 0.0;
  std::size_t checked = 0;
  const std::size_t n_rows = grid.n_left + grid.n_right + 1;
  for (std::size_t i = 1; i + 2 < grid.rows.size(); ++i) {
    for (std::size_t j = 1; j + 2 < n_rows; ++j) {
      const auto p0 = grid_point(grid, i, j);
      const auto p1 = grid_point(grid, i + 1, j + 1);
      const double cx = 0.5 * (p0[0] + p1[0]);
      const double cy = 0.5 * (p0[1] + p1[1]);
      const double truth = sample_surface(surface, cx, cy);
      if (std::isnan(truth)) {
        continue; // cell center outside the footprint
      }
      const double recon = bicubic(grid, i, j, 0.5, 0.5);
      max_err = std::max(max_err, std::abs(recon - truth));
      ++checked;
    }
  }
  ASSERT_GT(checked, 0U);
  // The harmonic field is smooth; bicubic at a 2 m Nyquist stays well within a
  // 1 cm modeling tolerance of the piecewise-linear surface.
  EXPECT_LT(max_err, 0.01);
}

TEST(JunctionExport, BoundaryOmissionIsWarnedWithRuleId) {
  RoadNetwork network;
  build_four_way(network);
  const auto findings = roadmaker::validate_network(network);
  const auto it = std::find_if(findings.begin(), findings.end(), [](const auto& d) {
    return d.rule_id == roadmaker::rules::kJunctionBoundaryCloseGap;
  });
  ASSERT_NE(it, findings.end());
  EXPECT_EQ(it->severity, roadmaker::Severity::Warning);
}

TEST(JunctionExport, WriteIsDeterministic) {
  RoadNetwork network;
  build_four_way(network);
  grade_arms(network);
  const auto a = roadmaker::write_xodr(network, "j");
  const auto b = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, *b);
}
