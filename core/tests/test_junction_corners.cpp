// Junction corner solve (p4-s1, issue #225): the public junction_corners()
// query is the SAME solve the junction-surface mesher runs, so the Corner
// tool's handles sit exactly on the emitted pavement. These tests pin the
// derived corner geometry, the authored-override semantics (radius clamped to
// what the arm faces leave room for, independent per-side extents), the
// rational-quadratic corner curve (a true circular arc for equal extents, G1
// for unequal ones), and the mesh staying watertight with overrides applied.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
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
  network.for_each_junction([&](JunctionId id, const Junction&) { junction = id; });
  return junction;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

/// The four-way fixture the junction-surface suite meshes: perpendicular arms
/// stopping 6 m short of the origin.
JunctionId build_four_way(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, "4");
  return make_junction(network, {end_of(west), end_of(east), end_of(south), end_of(north)});
}

/// The same four-way with a 20 m arm gap: enough edge run that the derived
/// radius (not the geometric bound) is what limits the fillet, and enough room
/// for asymmetric authored extents.
JunctionId build_roomy_four_way(RoadNetwork& network) {
  const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
  return make_junction(network, {end_of(west), end_of(east), end_of(south), end_of(north)});
}

double cross2(const std::array<double, 2>& a, const std::array<double, 2>& b) {
  return (a[0] * b[1]) - (a[1] * b[0]);
}

std::array<double, 2> sub(const std::array<double, 2>& a, const std::array<double, 2>& b) {
  return {a[0] - b[0], a[1] - b[1]};
}

/// The corner curve as documented on JunctionCornerInfo: the rational
/// quadratic Bezier P0=tangent_a, P1=corner, P2=tangent_b with w=sin(phi/2).
/// Re-derived here so the tests pin the contract, not the implementation.
std::array<double, 2> bezier(const JunctionCornerInfo& c, double t) {
  const double w = std::sin(c.phi / 2.0);
  const double u = 1.0 - t;
  const double b0 = u * u;
  const double b1 = 2.0 * w * t * u;
  const double b2 = t * t;
  const double denom = b0 + b1 + b2;
  return {((b0 * c.tangent_a[0]) + (b1 * c.corner[0]) + (b2 * c.tangent_b[0])) / denom,
          ((b0 * c.tangent_a[1]) + (b1 * c.corner[1]) + (b2 * c.tangent_b[1])) / denom};
}

std::array<double, 2> normalized(const std::array<double, 2>& v) {
  const double len = std::hypot(v[0], v[1]);
  return len > 0.0 ? std::array<double, 2>{v[0] / len, v[1] / len}
                   : std::array<double, 2>{0.0, 0.0};
}

/// Sets (or replaces) the authored override for the corner between `info`'s
/// arms. Corner entries are plain data on the junction — the editor reaches
/// them through the command layer, tests through the arena.
void set_override(RoadNetwork& network, JunctionId id, const JunctionCorner& entry) {
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

// --- Watertightness (the invariants test_t_junction_quality.cpp asserts) ------

struct Segment {
  double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
};

bool proper_intersect(const Segment& s, const Segment& t) {
  const auto orient = [](double ax, double ay, double bx, double by, double px, double py) {
    return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
  };
  const double o1 = orient(s.ax, s.ay, s.bx, s.by, t.ax, t.ay);
  const double o2 = orient(s.ax, s.ay, s.bx, s.by, t.bx, t.by);
  const double o3 = orient(t.ax, t.ay, t.bx, t.by, s.ax, s.ay);
  const double o4 = orient(t.ax, t.ay, t.bx, t.by, s.bx, s.by);
  return ((o1 > 0.0) != (o2 > 0.0)) && ((o3 > 0.0) != (o4 > 0.0)) && std::abs(o1) > 1e-9 &&
         std::abs(o2) > 1e-9 && std::abs(o3) > 1e-9 && std::abs(o4) > 1e-9;
}

struct FloorQuality {
  int degenerate = 0;
  int flipped = 0;
  int boundary_crossings = 0;
  int seam_near_misses = 0;
  int seam_z_mismatches = 0;
};

/// Same checks as the tee quality matrix: no degenerate/flipped triangles, no
/// floor-boundary self-intersection, and every boundary vertex either
/// bitwise-stitched onto a road-mesh vertex or clear of one.
FloorQuality floor_quality(const NetworkMesh& mesh) {
  FloorQuality q;
  if (mesh.junction_floors.empty()) {
    return q;
  }
  const SubMesh& floor = mesh.junction_floors.front().mesh;
  const auto& p = floor.positions;
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
  const auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  for (std::size_t i = 0; i + 2 < floor.indices.size(); i += 3) {
    const std::array<std::uint32_t, 3> t{
        floor.indices[i], floor.indices[i + 1], floor.indices[i + 2]};
    const double ax = p[t[0] * 3], ay = p[(t[0] * 3) + 1];
    const double bx = p[t[1] * 3], by = p[(t[1] * 3) + 1];
    const double cx = p[t[2] * 3], cy = p[(t[2] * 3) + 1];
    const double area2 = ((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay));
    if (std::abs(area2) < 1e-9) {
      ++q.degenerate;
      continue;
    }
    if (area2 < 0.0) {
      ++q.flipped;
    }
    ++edge_count[key(t[0], t[1])];
    ++edge_count[key(t[1], t[2])];
    ++edge_count[key(t[2], t[0])];
  }

  std::vector<Segment> boundary;
  std::vector<bool> on_boundary(p.size() / 3, false);
  for (const auto& [edge, count] : edge_count) {
    if (count != 1) {
      continue;
    }
    on_boundary[edge.first] = true;
    on_boundary[edge.second] = true;
    boundary.push_back(
        {p[edge.first * 3], p[(edge.first * 3) + 1], p[edge.second * 3], p[(edge.second * 3) + 1]});
  }
  for (std::size_t i = 0; i < boundary.size(); ++i) {
    for (std::size_t j = i + 1; j < boundary.size(); ++j) {
      if (proper_intersect(boundary[i], boundary[j])) {
        ++q.boundary_crossings;
      }
    }
  }

  std::vector<std::array<double, 3>> road_vertices;
  for (const RoadMesh& road : mesh.roads) {
    for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
      road_vertices.push_back({road.positions[i], road.positions[i + 1], road.positions[i + 2]});
    }
  }
  for (std::size_t i = 0; i < on_boundary.size(); ++i) {
    if (!on_boundary[i]) {
      continue;
    }
    const double x = p[i * 3], y = p[(i * 3) + 1], z = p[(i * 3) + 2];
    double best = std::numeric_limits<double>::max();
    double best_z = 0.0;
    for (const auto& rv : road_vertices) {
      const double d2 = ((rv[0] - x) * (rv[0] - x)) + ((rv[1] - y) * (rv[1] - y));
      if (d2 < best) {
        best = d2;
        best_z = rv[2];
      }
    }
    if (best < 1e-12) {
      if (std::abs(best_z - z) > 1e-9) {
        ++q.seam_z_mismatches;
      }
    } else if (best < 0.005 * 0.005) {
      ++q.seam_near_misses;
    }
  }
  return q;
}

void expect_watertight(const NetworkMesh& mesh) {
  const FloorQuality q = floor_quality(mesh);
  EXPECT_EQ(q.degenerate, 0);
  EXPECT_EQ(q.flipped, 0);
  EXPECT_EQ(q.boundary_crossings, 0);
  EXPECT_EQ(q.seam_near_misses, 0);
  EXPECT_EQ(q.seam_z_mismatches, 0);
}

// --- Tests -------------------------------------------------------------------

TEST(JunctionCorners, FourWayYieldsFourCcwCorners) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> corners = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(corners.size(), 4U);

  double total_turn = 0.0;
  for (const JunctionCornerInfo& c : corners) {
    // Consecutive pairs: each corner's arm_b opens the next corner.
    EXPECT_NEAR(std::hypot(c.dir_a[0], c.dir_a[1]), 1.0, 1e-12);
    EXPECT_NEAR(std::hypot(c.dir_b[0], c.dir_b[1]), 1.0, 1e-12);
    // Perpendicular arms: the edge rays meet at a right angle.
    EXPECT_NEAR(c.phi, std::numbers::pi / 2.0, 1e-9);

    // Tangencies lie on their edge lines (through the face corner, along dir).
    EXPECT_NEAR(cross2(sub(c.tangent_a, c.face_a), c.dir_a), 0.0, 1e-9);
    EXPECT_NEAR(cross2(sub(c.tangent_b, c.face_b), c.dir_b), 0.0, 1e-9);
    EXPECT_NEAR(
        std::hypot(c.corner[0] - c.tangent_a[0], c.corner[1] - c.tangent_a[1]), c.extent_a, 1e-9);
    EXPECT_NEAR(
        std::hypot(c.corner[0] - c.tangent_b[0], c.corner[1] - c.tangent_b[1]), c.extent_b, 1e-9);

    // The apex sits on the inward bisector.
    EXPECT_NEAR(cross2(sub(c.apex(), c.corner), c.bisector), 0.0, 1e-9);
    EXPECT_GT(((c.apex()[0] - c.corner[0]) * c.bisector[0]) +
                  ((c.apex()[1] - c.corner[1]) * c.bisector[1]),
              0.0);

    ASSERT_GE(c.curve.size(), 2U);
    EXPECT_LT(std::hypot(c.curve.front()[0] - c.tangent_a[0], c.curve.front()[1] - c.tangent_a[1]),
              1e-9);
    EXPECT_LT(std::hypot(c.curve.back()[0] - c.tangent_b[0], c.curve.back()[1] - c.tangent_b[1]),
              1e-9);
    total_turn += std::numbers::pi - c.phi;
  }
  // Four right-angle corners walked CCW turn through a full circle.
  EXPECT_NEAR(total_turn, 2.0 * std::numbers::pi, 1e-9);

  // The four corner points are the four quadrant diagonals of the crossing.
  std::vector<double> angles;
  angles.reserve(corners.size());
  for (const JunctionCornerInfo& c : corners) {
    angles.push_back(std::atan2(c.corner[1], c.corner[0]));
  }
  std::ranges::sort(angles);
  for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
    EXPECT_NEAR(angles[i + 1] - angles[i], std::numbers::pi / 2.0, 1e-9);
  }
}

TEST(JunctionCorners, NoOverrideUsesDerivedRadius) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  for (const JunctionCornerInfo& c : roadmaker::junction_corners(network, junction)) {
    EXPECT_FALSE(c.radius_authored);
    EXPECT_FALSE(c.extents_authored);
    // Derived: min(connecting-road radius clamped to [3, 15], geometric max).
    // The 6 m arm gap leaves under 3 m of edge run, so the geometric bound is
    // what binds here (the roomy fixture below exercises the other branch).
    EXPECT_LT(c.max_radius, 3.0);
    EXPECT_NEAR(c.radius, c.max_radius, 1e-9);
    // Symmetric legs: radius / tan(phi/2) on both sides.
    const double leg = c.radius / std::tan(c.phi / 2.0);
    EXPECT_NEAR(c.extent_a, leg, 1e-12);
    EXPECT_NEAR(c.extent_b, leg, 1e-12);
    EXPECT_NEAR(
        c.max_radius, std::min(c.max_extent_a, c.max_extent_b) * std::tan(c.phi / 2.0), 1e-12);
  }
}

TEST(JunctionCorners, AuthoredRadiusIsRespected) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  // Deliberately below the DERIVED floor (3 m): the floor is a property of
  // the derivation, not a bound on what an author may ask for.
  const double authored = std::min(2.0, before.front().max_radius * 0.5);
  set_override(network,
               junction,
               JunctionCorner{.arm_a = before.front().arm_a,
                              .arm_b = before.front().arm_b,
                              .radius = authored,
                              .extent_a = std::nullopt,
                              .extent_b = std::nullopt});

  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(after.size(), before.size());
  EXPECT_TRUE(after.front().radius_authored);
  EXPECT_FALSE(after.front().extents_authored);
  EXPECT_NEAR(after.front().radius, authored, 1e-12);
  EXPECT_NEAR(after.front().extent_a, authored / std::tan(after.front().phi / 2.0), 1e-12);
  // The other corners keep the derivation — the override is per pair.
  for (std::size_t i = 1; i < after.size(); ++i) {
    EXPECT_FALSE(after[i].radius_authored);
    EXPECT_NEAR(after[i].radius, before[i].radius, 1e-12);
  }
}

TEST(JunctionCorners, AbsurdAuthoredRadiusClampsToMax) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  set_override(network,
               junction,
               JunctionCorner{.arm_a = before.front().arm_a,
                              .arm_b = before.front().arm_b,
                              .radius = 1e6,
                              .extent_a = std::nullopt,
                              .extent_b = std::nullopt});

  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(after.size(), before.size());
  EXPECT_TRUE(after.front().radius_authored);
  EXPECT_NEAR(after.front().radius, after.front().max_radius, 1e-12);
  // Clamped, not dropped: the corner still solves and still fillets.
  EXPECT_GE(after.front().curve.size(), 2U);
}

TEST(JunctionCorners, DormantOverrideIsIgnored) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_GE(before.size(), 2U);
  // A pair that is not CCW-adjacent (the two opposite arms of corner 0/1).
  set_override(network,
               junction,
               JunctionCorner{.arm_a = before.front().arm_a,
                              .arm_b = before[1].arm_b,
                              .radius = 1.0,
                              .extent_a = std::nullopt,
                              .extent_b = std::nullopt});

  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    EXPECT_FALSE(after[i].radius_authored);
    EXPECT_NEAR(after[i].radius, before[i].radius, 1e-12);
  }
  // The entry survives untouched — it wakes up if the pair becomes adjacent.
  EXPECT_EQ(network.junction(junction)->corners.size(), 1U);
}

TEST(JunctionCorners, RoomyFourWayUsesTheDerivedRadiusBand) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const std::vector<JunctionCornerInfo> corners = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(corners.size(), 4U);
  for (const JunctionCornerInfo& c : corners) {
    EXPECT_FALSE(c.radius_authored);
    // Roomy arms: the geometric bound no longer binds, so the derivation's
    // [3, 15] band is what the radius lands in.
    EXPECT_GT(c.max_radius, 15.0);
    EXPECT_GE(c.radius, 3.0 - 1e-12);
    EXPECT_LE(c.radius, 15.0 + 1e-12);
  }
}

TEST(JunctionCorners, AuthoredExtentsMoveTangenciesAndStayTangent) {
  RoadNetwork network;
  const JunctionId junction = build_roomy_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  const double ea = 0.2 * before.front().max_extent_a;
  const double eb = 0.8 * before.front().max_extent_b;
  ASSERT_GT(std::abs(ea - eb), 0.1);
  set_override(network,
               junction,
               JunctionCorner{.arm_a = before.front().arm_a,
                              .arm_b = before.front().arm_b,
                              .radius = std::nullopt,
                              .extent_a = ea,
                              .extent_b = eb});

  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(after.empty());
  const JunctionCornerInfo& c = after.front();
  EXPECT_TRUE(c.extents_authored);
  EXPECT_NEAR(c.extent_a, ea, 1e-12);
  EXPECT_NEAR(c.extent_b, eb, 1e-12);
  EXPECT_NEAR(std::hypot(c.corner[0] - c.tangent_a[0], c.corner[1] - c.tangent_a[1]), ea, 1e-12);
  EXPECT_NEAR(std::hypot(c.corner[0] - c.tangent_b[0], c.corner[1] - c.tangent_b[1]), eb, 1e-12);

  // G1 on the documented curve: the rational quadratic Bezier leaves
  // tangent_a along +dir_a and reaches tangent_b along -dir_b.
  // Its end tangents are the control legs, which lie exactly on the edges…
  EXPECT_NEAR(cross2(sub(c.corner, c.tangent_a), c.dir_a), 0.0, 1e-12);
  EXPECT_NEAR(cross2(sub(c.corner, c.tangent_b), c.dir_b), 0.0, 1e-12);
  // …so the chord to a near-endpoint sample converges onto them linearly.
  EXPECT_NEAR(cross2(normalized(sub(bezier(c, 1e-5), c.tangent_a)), c.dir_a), 0.0, 1e-4);
  EXPECT_NEAR(cross2(normalized(sub(c.tangent_b, bezier(c, 1.0 - 1e-5))), c.dir_b), 0.0, 1e-4);
  // Every sample the mesher emits lies on that curve.
  for (const std::array<double, 2>& p : c.curve) {
    double best = std::numeric_limits<double>::max();
    for (int k = 0; k <= 2000; ++k) {
      const std::array<double, 2> q = bezier(c, static_cast<double>(k) / 2000.0);
      best = std::min(best, std::hypot(q[0] - p[0], q[1] - p[1]));
    }
    EXPECT_LT(best, 1e-6);
  }
  // The polyline still departs along the edges: the tangent-lift rule trades
  // the first few samples for a short chord, which must stay a shallow cut.
  ASSERT_GE(c.curve.size(), 4U);
  const std::array<double, 2> first = normalized(sub(c.curve[1], c.curve[0]));
  const std::array<double, 2> last =
      normalized(sub(c.curve[c.curve.size() - 1], c.curve[c.curve.size() - 2]));
  EXPECT_LT(std::abs(cross2(sub(c.curve[1], c.tangent_a), c.dir_a)), 0.25);
  EXPECT_LT(std::abs(cross2(sub(c.curve[c.curve.size() - 2], c.tangent_b), c.dir_b)), 0.25);
  EXPECT_GT((first[0] * c.dir_a[0]) + (first[1] * c.dir_a[1]), 0.0);
  EXPECT_LT((last[0] * c.dir_b[0]) + (last[1] * c.dir_b[1]), 0.0);
}

TEST(JunctionCorners, EqualExtentsGiveACircularArc) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  // An authored radius runs the rational-Bezier path (the derived path keeps
  // the legacy arc sampler); equal extents must still land on the circle.
  const double authored = 0.6 * before.front().max_radius;
  set_override(network,
               junction,
               JunctionCorner{.arm_a = before.front().arm_a,
                              .arm_b = before.front().arm_b,
                              .radius = authored,
                              .extent_a = std::nullopt,
                              .extent_b = std::nullopt});

  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(after.empty());
  const JunctionCornerInfo& c = after.front();
  ASSERT_TRUE(c.radius_authored);
  EXPECT_NEAR(c.extent_a, c.extent_b, 1e-12);
  const double center_dist = c.radius / std::sin(c.phi / 2.0);
  const std::array<double, 2> center{c.corner[0] + (c.bisector[0] * center_dist),
                                     c.corner[1] + (c.bisector[1] * center_dist)};
  ASSERT_GE(c.curve.size(), 4U);
  for (const std::array<double, 2>& p : c.curve) {
    EXPECT_NEAR(std::hypot(p[0] - center[0], p[1] - center[1]), c.radius, 1e-9);
  }
  EXPECT_NEAR(std::hypot(c.apex()[0] - center[0], c.apex()[1] - center[1]), c.radius, 1e-9);
}

TEST(JunctionCorners, StaleIdAndUnfilletableJunctionsAreEmpty) {
  RoadNetwork network;
  build_four_way(network);
  EXPECT_TRUE(roadmaker::junction_corners(network, JunctionId{}).empty());

  // Two collinear arms: both "corners" are parallel-edge corridors, so there
  // is nothing to fillet.
  RoadNetwork straight;
  const RoadId west = author(straight, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author(straight, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const JunctionId corridor = make_junction(straight, {end_of(west), end_of(east)});
  EXPECT_TRUE(roadmaker::junction_corners(straight, corridor).empty());
}

TEST(JunctionCorners, IsDeterministic) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  set_override(network,
               junction,
               JunctionCorner{.arm_a = roadmaker::junction_corners(network, junction).front().arm_a,
                              .arm_b = roadmaker::junction_corners(network, junction).front().arm_b,
                              .radius = 5.0,
                              .extent_a = 2.0,
                              .extent_b = 7.0});
  const std::vector<JunctionCornerInfo> first = roadmaker::junction_corners(network, junction);
  const std::vector<JunctionCornerInfo> second = roadmaker::junction_corners(network, junction);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].corner, second[i].corner);
    EXPECT_EQ(first[i].radius, second[i].radius);
    EXPECT_EQ(first[i].extent_a, second[i].extent_a);
    EXPECT_EQ(first[i].extent_b, second[i].extent_b);
    EXPECT_EQ(first[i].curve, second[i].curve);
  }
}

TEST(JunctionCorners, MeshStaysWatertightWithAuthoredRadius) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  for (const JunctionCornerInfo& c : before) {
    set_override(network,
                 junction,
                 JunctionCorner{.arm_a = c.arm_a,
                                .arm_b = c.arm_b,
                                .radius = 0.8 * c.max_radius,
                                .extent_a = std::nullopt,
                                .extent_b = std::nullopt});
  }
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.junction_floors.size(), 1U);
  expect_watertight(mesh);
}

TEST(JunctionCorners, MeshStaysWatertightWithAsymmetricExtents) {
  RoadNetwork network;
  const JunctionId junction = build_four_way(network);
  const std::vector<JunctionCornerInfo> before = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(before.empty());
  for (const JunctionCornerInfo& c : before) {
    set_override(network,
                 junction,
                 JunctionCorner{.arm_a = c.arm_a,
                                .arm_b = c.arm_b,
                                .radius = std::nullopt,
                                .extent_a = 0.25 * c.max_extent_a,
                                .extent_b = 0.85 * c.max_extent_b});
  }
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.junction_floors.size(), 1U);
  expect_watertight(mesh);
  // The mesher consumed the same solve: the floor boundary reaches the
  // authored tangency points.
  const std::vector<JunctionCornerInfo> after = roadmaker::junction_corners(network, junction);
  ASSERT_FALSE(after.empty());
  const SubMesh& floor = mesh.junction_floors.front().mesh;
  for (const JunctionCornerInfo& c : after) {
    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 2 < floor.positions.size(); i += 3) {
      best = std::min(
          best,
          std::hypot(floor.positions[i] - c.tangent_a[0], floor.positions[i + 1] - c.tangent_a[1]));
    }
    EXPECT_LT(best, 0.05);
  }
}

} // namespace
