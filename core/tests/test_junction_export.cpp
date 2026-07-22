// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <pugixml.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
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

/// Links an arm road's junction-facing end to the junction (mirrors
/// materialize_junction: End contact → successor slot, Start → predecessor).
void link_arm_to_junction(RoadNetwork& network, const RoadEnd& arm, JunctionId junction) {
  const roadmaker::RoadLink link{.target = junction, .contact = ContactPoint::Start};
  roadmaker::Road& road = *network.road(arm.road);
  if (arm.contact == ContactPoint::Start) {
    road.predecessor = link;
  } else {
    road.successor = link;
  }
}

/// Adds a hand-built connecting road bridging two arms (a line arm-end→arm-end),
/// tagged @junction with predecessor/successor links, plus its connection-table
/// entry — mirroring materialize_junction so build_junction_boundary treats it
/// as a real bridge.
void bridge_arms(RoadNetwork& network,
                 JunctionId junction,
                 const RoadEnd& from,
                 const RoadEnd& to,
                 const char* odr_id) {
  const auto end_pos = [&](const RoadEnd& arm) {
    const roadmaker::Road& road = *network.road(arm.road);
    const double s = arm.contact == ContactPoint::Start ? 0.0 : road.plan_view.length();
    const roadmaker::PathPoint p = road.plan_view.evaluate(s);
    return Waypoint{p.x, p.y};
  };
  const RoadId conn = author(network, {end_pos(from), end_pos(to)}, odr_id);
  roadmaker::Road& road = *network.road(conn);
  road.junction = junction;
  road.predecessor = roadmaker::RoadLink{.target = from.road, .contact = from.contact};
  road.successor = roadmaker::RoadLink{.target = to.road, .contact = to.contact};
  network.junction(junction)->connections.push_back(roadmaker::JunctionConnection{
      .incoming_road = from.road, .connecting_road = conn, .contact_point = ContactPoint::Start});
}

/// A three-arm junction with a deliberate boundary gap: arms at 0°/120°/240°
/// meeting near the origin, but only two of the three CCW-adjacent pairs are
/// bridged by a connecting road. The unbridged pair (arm "3" ↔ arm "1") is a
/// gap the boundary can only close with an auxiliary boundary road (#62). The
/// generator never produces this — every ordered arm pair it can reach is
/// bridged — so it is built by hand.
JunctionId build_gapped_three_arm(RoadNetwork& network) {
  const RoadId a = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "1");         // 0°
  const RoadId b = author(network, {Waypoint{-20.0, 34.64}, Waypoint{-3.0, 5.196}}, "2");   // 120°
  const RoadId c = author(network, {Waypoint{-20.0, -34.64}, Waypoint{-3.0, -5.196}}, "3"); // 240°
  const JunctionId junction = network.create_junction("10", "");
  network.junction(junction)->arms = {end_of(a), end_of(b), end_of(c)};
  link_arm_to_junction(network, end_of(a), junction);
  link_arm_to_junction(network, end_of(b), junction);
  link_arm_to_junction(network, end_of(c), junction);
  // Bridge (a,b) and (b,c); leave (c,a) a gap.
  bridge_arms(network, junction, end_of(a), end_of(b), "5");
  bridge_arms(network, junction, end_of(b), end_of(c), "6");
  return junction;
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
      // Only cells whose full 4×4 bicubic stencil lies on the surface: the
      // margin samples extend the field C0 (nearest vertex) per the
      // exporter's spec-conformant extrapolation — they are not field data
      // and reconstruction through them measures the margin, not the grid.
      bool stencil_on_surface = true;
      for (std::size_t si = i - 1; si <= i + 2 && stencil_on_surface; ++si) {
        for (std::size_t sj = j - 1; sj <= j + 2 && stencil_on_surface; ++sj) {
          const auto sp = grid_point(grid, si, sj);
          stencil_on_surface = !std::isnan(sample_surface(surface, sp[0], sp[1]));
        }
      }
      if (!stencil_on_surface) {
        continue;
      }
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
  // Interior bicubic at the 2 m Nyquist vs the piecewise-linear (~2 m
  // triangles) surface of a genuinely graded field: bounded by the two
  // schemes' linearization difference. (Before issue #103 the connecting
  // roads carried no elevation, so this field was accidentally flat and the
  // old 1 cm bound was vacuous.)
  EXPECT_LT(max_err, 0.05);
}

// --- <boundary> (§12.10, M3a phase 2b #62) -----------------------------------

struct ParsedSegment {
  std::string type;    // "lane" | "joint"
  std::string road_id; // @roadId
  std::string s_start; // lane: @sStart
  std::string s_end;   // lane: @sEnd
  int boundary_lane = 0;
};

std::vector<ParsedSegment> parse_first_boundary(const std::string& xml) {
  pugi::xml_document doc;
  EXPECT_TRUE(doc.load_string(xml.c_str()));
  const pugi::xml_node boundary = doc.child("OpenDRIVE").child("junction").child("boundary");
  std::vector<ParsedSegment> segments;
  for (const pugi::xml_node seg : boundary.children("segment")) {
    segments.push_back(ParsedSegment{.type = seg.attribute("type").value(),
                                     .road_id = seg.attribute("roadId").value(),
                                     .s_start = seg.attribute("sStart").value(),
                                     .s_end = seg.attribute("sEnd").value(),
                                     .boundary_lane = seg.attribute("boundaryLane").as_int()});
  }
  return segments;
}

TEST(JunctionBoundary, EmittedAsClosedAlternatingLoopForGeneratedJunction) {
  RoadNetwork network;
  build_four_way(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());

  const std::vector<ParsedSegment> segments = parse_first_boundary(*xml);
  // A 4-arm junction: one lane segment per arm pair + one joint cap per arm.
  ASSERT_EQ(segments.size(), 8U);
  std::size_t lanes = 0;
  std::size_t joints = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    // Segments alternate lane, joint, lane, joint … (closed CCW loop).
    const bool expect_lane = (i % 2 == 0);
    EXPECT_EQ(segments[i].type, expect_lane ? "lane" : "joint");
    (segments[i].type == "lane" ? lanes : joints)++;
  }
  EXPECT_EQ(lanes, 4U);
  EXPECT_EQ(joints, 4U);
}

TEST(JunctionBoundary, SegmentsReferenceRealRoadsWithSaneAttributes) {
  RoadNetwork network;
  build_four_way(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const std::vector<ParsedSegment> segments = parse_first_boundary(*xml);

  for (const ParsedSegment& seg : segments) {
    const RoadId road = network.find_road(seg.road_id);
    ASSERT_TRUE(road.is_valid()) << "segment references unknown road " << seg.road_id;
    if (seg.type == "lane") {
      // Lane segments follow a connecting road (junction set); boundaryLane is
      // the connecting road's single driving lane; sStart/sEnd are contact
      // keywords.
      EXPECT_TRUE(network.road(road)->junction.is_valid());
      EXPECT_EQ(seg.boundary_lane, -1);
      EXPECT_TRUE(seg.s_start == "begin" || seg.s_start == "end");
      EXPECT_TRUE(seg.s_end == "begin" || seg.s_end == "end");
      EXPECT_NE(seg.s_start, seg.s_end);
    } else {
      // Joint caps are on the incoming arm roads (not junction-internal).
      EXPECT_FALSE(network.road(road)->junction.is_valid());
    }
  }
}

TEST(JunctionBoundary, EmittingBoundaryClearsTheOmittedWarning) {
  RoadNetwork network;
  build_four_way(network);
  const auto findings = roadmaker::validate_network(network);
  const bool warned = std::any_of(findings.begin(), findings.end(), [](const auto& d) {
    return d.rule_id == roadmaker::rules::kJunctionBoundaryCloseGap;
  });
  EXPECT_FALSE(warned);
}

TEST(JunctionBoundary, ThreeWayJunctionAlsoCloses) {
  RoadNetwork network;
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  make_junction(network, {end_of(west), end_of(east), end_of(south)});

  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const std::vector<ParsedSegment> segments = parse_first_boundary(*xml);
  ASSERT_EQ(segments.size(), 6U); // 3 lane + 3 joint

  const auto findings = roadmaker::validate_network(network);
  EXPECT_FALSE(std::any_of(findings.begin(), findings.end(), [](const auto& d) {
    return d.rule_id == roadmaker::rules::kJunctionBoundaryCloseGap;
  }));
}

TEST(JunctionBoundary, RoundTripsAsAFixedPoint) {
  RoadNetwork network;
  build_four_way(network);
  const auto written = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(written.has_value());
  // The boundary is derived, ignored on read, and regenerated identically on
  // the next write — a byte-stable fixed point.
  const auto reparsed = roadmaker::parse_xodr(*written, "j");
  ASSERT_TRUE(reparsed.has_value());
  const auto rewritten = roadmaker::write_xodr(reparsed->network, "j");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

// --- auxiliary boundary roads (§12.10 close_gap_with_new_roads, #62) ----------

bool boundary_warned(const RoadNetwork& network) {
  const auto findings = roadmaker::validate_network(network);
  return std::any_of(findings.begin(), findings.end(), [](const auto& d) {
    return d.rule_id == roadmaker::rules::kJunctionBoundaryCloseGap;
  });
}

/// The <road> elements in an xodr, as (id, junction, is_aux) triples.
struct ParsedRoad {
  std::string id;
  std::string junction;
  bool is_aux = false;
};

std::vector<ParsedRoad> parse_roads(const std::string& xml) {
  pugi::xml_document doc;
  EXPECT_TRUE(doc.load_string(xml.c_str()));
  std::vector<ParsedRoad> roads;
  for (const pugi::xml_node road : doc.child("OpenDRIVE").children("road")) {
    ParsedRoad out{.id = road.attribute("id").value(),
                   .junction = road.attribute("junction").value()};
    for (const pugi::xml_node ud : road.children("userData")) {
      if (std::string_view(ud.attribute("code").value()) == "rm:aux_boundary") {
        out.is_aux = true;
      }
    }
    roads.push_back(std::move(out));
  }
  return roads;
}

TEST(AuxBoundaryRoad, ClosesTheGapWithABoundaryLaneZeroSegment) {
  RoadNetwork network;
  build_gapped_three_arm(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());

  // The boundary now closes: 3 lane + 3 joint segments (like any 3-arm), one of
  // the lane segments is the aux road's boundaryLane 0.
  const std::vector<ParsedSegment> segments = parse_first_boundary(*xml);
  ASSERT_EQ(segments.size(), 6U);
  const auto aux_lane_segments = static_cast<std::size_t>(
      std::count_if(segments.begin(), segments.end(), [](const ParsedSegment& s) {
        return s.type == "lane" && s.boundary_lane == 0;
      }));
  EXPECT_EQ(aux_lane_segments, 1U);

  // Exactly one auxiliary <road> was emitted, tagged @junction + rm:aux_boundary,
  // and the boundaryLane-0 segment references it.
  const std::vector<ParsedRoad> roads = parse_roads(*xml);
  const auto aux_roads = static_cast<std::size_t>(
      std::count_if(roads.begin(), roads.end(), [](const ParsedRoad& r) { return r.is_aux; }));
  ASSERT_EQ(aux_roads, 1U);
  const auto aux =
      std::find_if(roads.begin(), roads.end(), [](const ParsedRoad& r) { return r.is_aux; });
  EXPECT_EQ(aux->junction, "10");
  const auto seg = std::find_if(segments.begin(), segments.end(), [](const ParsedSegment& s) {
    return s.type == "lane" && s.boundary_lane == 0;
  });
  EXPECT_EQ(seg->road_id, aux->id);

  EXPECT_FALSE(boundary_warned(network)) << "closing the gap must clear the warning";
}

TEST(AuxBoundaryRoad, SegmentsAlternateLaneJointAndClose) {
  RoadNetwork network;
  build_gapped_three_arm(network);
  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  const std::vector<ParsedSegment> segments = parse_first_boundary(*xml);
  ASSERT_EQ(segments.size(), 6U);
  std::size_t lanes = 0;
  std::size_t joints = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    EXPECT_EQ(segments[i].type, (i % 2 == 0) ? "lane" : "joint");
    (segments[i].type == "lane" ? lanes : joints)++;
  }
  EXPECT_EQ(lanes, 3U);
  EXPECT_EQ(joints, 3U);
}

TEST(AuxBoundaryRoad, ReaderDropsItSoTheModelIsUnchanged) {
  RoadNetwork network;
  build_gapped_three_arm(network);
  std::size_t original_roads = 0;
  network.for_each_road([&](RoadId, const roadmaker::Road&) { ++original_roads; });
  ASSERT_EQ(original_roads, 5U); // 3 arms + 2 connecting

  const auto xml = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xml.has_value());
  // The written file carries the aux road among its <road>s...
  const std::vector<ParsedRoad> written_roads = parse_roads(*xml);
  EXPECT_EQ(written_roads.size(), 6U);

  // ...but parsing it back drops the aux road, so the model is unchanged.
  const auto reparsed = roadmaker::parse_xodr(*xml, "j");
  ASSERT_TRUE(reparsed.has_value());
  std::size_t reparsed_roads = 0;
  reparsed->network.for_each_road([&](RoadId, const roadmaker::Road&) { ++reparsed_roads; });
  EXPECT_EQ(reparsed_roads, 5U);
}

TEST(AuxBoundaryRoad, RoundTripsAsAFixedPoint) {
  RoadNetwork network;
  build_gapped_three_arm(network);
  const auto written = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = roadmaker::parse_xodr(*written, "j");
  ASSERT_TRUE(reparsed.has_value());
  const auto rewritten = roadmaker::write_xodr(reparsed->network, "j");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

TEST(AuxBoundaryRoad, WriteIsDeterministic) {
  RoadNetwork network;
  build_gapped_three_arm(network);
  const auto a = roadmaker::write_xodr(network, "j");
  const auto b = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, *b);
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

// --- dangling arm roads (#311) ----------------------------------------------
//
// erase_road drops the junction CONNECTIONS that reference a road but
// deliberately leaves Junction::arms alone (network.hpp: arms are the
// generator's input, reconciled by regenerate_junction). A junction can
// therefore hold a RoadEnd whose RoadId no longer resolves — also reachable
// from a hand-edited or foreign .xodr. Every export path must treat such an
// arm as absent, never dereference it.

TEST(DanglingArm, WriteXodrSurvivesAnErasedArmRoad) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const RoadEnd doomed = network.junction(junction)->arms.front();
  ASSERT_TRUE(network.erase_road(doomed.road));
  ASSERT_NE(network.junction(junction), nullptr);
  ASSERT_EQ(network.road(doomed.road), nullptr) << "the arm must be dangling for this test";

  const auto xodr = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xodr.has_value()) << xodr.error().message;
}

TEST(DanglingArm, StaleRoadLinksAreOmittedNotWritten) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const RoadEnd doomed = network.junction(junction)->arms.front();
  const std::string erased_odr_id = network.road(doomed.road)->odr_id;
  ASSERT_TRUE(network.erase_road(doomed.road));

  const auto xodr = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xodr.has_value()) << xodr.error().message;

  pugi::xml_document doc;
  ASSERT_TRUE(doc.load_string(xodr->c_str()));
  // The surviving connecting roads that bridged the erased arm keep their
  // OTHER end; only the reference to the gone road disappears.
  for (const pugi::xml_node road : doc.child("OpenDRIVE").children("road")) {
    for (const pugi::xml_node end : road.child("link").children()) {
      if (std::string_view(end.attribute("elementType").value()) == "road") {
        EXPECT_NE(std::string_view(end.attribute("elementId").value()), erased_odr_id)
            << "road " << road.attribute("id").value() << " still names the erased road";
      }
    }
  }
  // A <link> is absent, never emitted empty.
  for (const pugi::xml_node road : doc.child("OpenDRIVE").children("road")) {
    const pugi::xml_node link = road.child("link");
    EXPECT_TRUE(link.empty() || link.first_child())
        << "empty <link> on road " << road.attribute("id").value();
  }
}

TEST(DanglingArm, ValidateNetworkWarnsAboutTheOmittedLink) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const RoadEnd doomed = network.junction(junction)->arms.front();
  ASSERT_TRUE(network.erase_road(doomed.road));

  const std::vector<roadmaker::Diagnostic> findings = roadmaker::validate_network(network);
  const auto omitted = std::ranges::find_if(findings, [](const roadmaker::Diagnostic& d) {
    return d.message.find("no longer exists") != std::string::npos;
  });
  ASSERT_NE(omitted, findings.end()) << "the dropped link must be reported, never silent";
  EXPECT_EQ(omitted->severity, roadmaker::Severity::Warning);
  EXPECT_EQ(omitted->rule_id, "asam.net:xodr:1.4.0:ids.only_ref_defined_ids")
      << "cites the wrong rule";
}

TEST(DanglingArm, WriteWithADanglingArmIsDeterministic) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  ASSERT_TRUE(network.erase_road(network.junction(junction)->arms.front().road));
  const auto a = roadmaker::write_xodr(network, "j");
  const auto b = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, *b);
}

TEST(DanglingArm, StaleJunctionLinksAreOmittedToo) {
  // erase_junction detaches the roads that pointed at it (network.hpp), so a
  // surviving approach road's predecessor/successor names a JunctionId that no
  // longer resolves — the same hole on the junction side of the variant.
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::string erased_odr_id = network.junction(junction)->odr_id;
  ASSERT_TRUE(network.erase_junction(junction));
  ASSERT_EQ(network.junction(junction), nullptr);

  const auto xodr = roadmaker::write_xodr(network, "j");
  ASSERT_TRUE(xodr.has_value()) << xodr.error().message;

  pugi::xml_document doc;
  ASSERT_TRUE(doc.load_string(xodr->c_str()));
  for (const pugi::xml_node road : doc.child("OpenDRIVE").children("road")) {
    for (const pugi::xml_node end : road.child("link").children()) {
      if (std::string_view(end.attribute("elementType").value()) == "junction") {
        EXPECT_NE(std::string_view(end.attribute("elementId").value()), erased_odr_id);
      }
    }
  }
}
