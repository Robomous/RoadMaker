// T-junction geometry & mesh quality matrix (hardening sprint, GW-1 gate
// finding): attach_t_junction must produce standards-correct, well-conditioned
// geometry for every tee configuration the editor can author — not only the
// endpoint-junction shapes the M2 generator was validated on.
//
// Each fixture runs the real kernel path (author_clothoid_road +
// edit::attach_t_junction), meshes, and asserts the junction pipeline's
// quality invariants (docs/domain/geometry.md):
//   - no sliver triangles (< 5°) above a small budget, no degenerate/flipped
//     triangles, no floor-boundary self-intersection
//   - watertight seams: any floor boundary vertex near a road-mesh vertex is
//     bitwise-stitched to it (03_junction_blending.md §5)
//   - connecting-road curvature stays drivable (the gap auto-size contract,
//     docs/design/hardening/t_junction.md)
//   - connecting roads of the same movement side do not cross
//     (asam.net:xodr:1.9.0:junctions.connection.smooth_fit — lanes must fit
//     smoothly, which crossed ribbons cannot)
//   - elevation continuity at every arm seam (the tee cut faces inherit the
//     target profile at s±gap)
//   - deterministic re-mesh (03 §7.4)
//
// Set RM_TJ_DIAG_DIR=<dir> to dump per-fixture .xodr + .glb + metrics.txt for
// visual inspection (esmini bisection, PR screenshots).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::LaneSpec;
using roadmaker::LaneType;
using roadmaker::NetworkMesh;
using roadmaker::Road;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMesh;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Waypoint;

namespace {

constexpr double kPi = std::numbers::pi;

/// Slivers below this angle destroy shading and z-buffer precision.
constexpr double kMinTriangleAngleDeg = 5.0;

/// Small budget for angle-limited triangles: the CDT may keep a couple of
/// acute corners where three arm boundaries meet.
constexpr int kSliverBudget = 2;

/// Drivable curvature bound for generated turns: r_min = 6 m is the tightest
/// urban curb-return radius the templates should ever need (the gap
/// auto-sizer's contract keeps fitted turns at or below this).
constexpr double kMaxDrivableCurvature = 1.0 / 6.0;

RoadId author(RoadNetwork& network,
              std::vector<Waypoint> waypoints,
              const char* odr_id,
              const LaneProfile& profile = LaneProfile::two_lane_rural()) {
  auto road = roadmaker::author_clothoid_road(network, waypoints, profile, "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

void apply_or_throw(RoadNetwork& network, std::unique_ptr<roadmaker::edit::Command> command) {
  if (command == nullptr) {
    throw std::runtime_error("null command");
  }
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error(applied.error().message);
  }
}

/// One authored tee: the junction plus its three arms as recorded by
/// attach_t_junction (head:End, tail:Start, branch).
struct Tee {
  RoadNetwork network;
  JunctionId junction;
};

Tee attach(RoadNetwork&& network, RoadEnd branch_end, RoadId target, double s,
           const roadmaker::edit::TAttachOptions& options = {}) {
  apply_or_throw(network, roadmaker::edit::attach_t_junction(network, branch_end, target, s, options));
  Tee tee{.network = std::move(network)};
  tee.network.for_each_junction(
      [&](JunctionId id, const roadmaker::Junction&) { tee.junction = id; });
  return tee;
}

// --- Fixture matrix (§3.1 of the task prompt) --------------------------------

/// Straight E-W target, branch arriving from the south, perpendicular.
Tee build_perp() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

/// Branch heading 45° into the target (shallow same-direction merge angle).
Tee build_deg45() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(network, {{-40.0, -40.0}, {-6.0, -6.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

/// Branch heading 135° into the target (against the +s direction).
Tee build_deg135() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(network, {{40.0, -40.0}, {6.0, -6.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

/// Curved target of radius `r`, branch attached at the apex from outside
/// (`inside=false`, convex side) or inside (toward the arc center).
Tee build_curved(double r, bool inside) {
  RoadNetwork network;
  // Waypoints on a circle centered at (0, r); apex at the origin.
  const double half_angle = r > 50.0 ? 35.0 * kPi / 180.0 : 60.0 * kPi / 180.0;
  std::vector<Waypoint> arc;
  for (int i = -2; i <= 2; ++i) {
    const double theta = half_angle * static_cast<double>(i) / 2.0;
    arc.push_back({r * std::sin(theta), r - (r * std::cos(theta))});
  }
  const RoadId target = author(network, arc, "1");
  const double s_apex = network.road(target)->length / 2.0;
  const RoadId branch = inside
      ? author(network, {{0.0, std::min(r - 6.0, 24.0)}, {0.0, 6.0}}, "2")
      : author(network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, s_apex);
}

/// Asymmetric widths: the branch is narrower than the target.
Tee build_asymmetric() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  LaneProfile narrow;
  narrow.left = {LaneSpec{.type = LaneType::Driving, .width = 3.0}};
  narrow.right = {LaneSpec{.type = LaneType::Driving, .width = 3.0}};
  const RoadId branch = author(network, {{0.0, -40.0}, {0.0, -6.0}}, "2", narrow);
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

/// Branch arriving with its Start contact (authored away from the target).
Tee build_start_contact() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(network, {{0.0, -6.0}, {0.0, -40.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::Start}, target, 100.0);
}

/// Graded target (3 % climb) — the tee cut faces must inherit the profile.
Tee build_graded() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  apply_or_throw(network,
                 roadmaker::edit::set_elevation_profile(
                     network, target, {{.s = 0.0, .z = 0.0}, {.s = 200.0, .z = 6.0}}));
  const RoadId branch = author(network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  apply_or_throw(network,
                 roadmaker::edit::set_elevation_profile(
                     network, branch, {{.s = 0.0, .z = 3.0}, {.s = 34.0, .z = 3.0}}));
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

/// Multi-lane target (highway template, 2 driving lanes per side): every
/// incoming lane must land on its own smoothly-fitting connecting ribbon.
Tee build_multilane() {
  RoadNetwork network;
  const RoadId target = author(network, {{-100.0, 0.0}, {100.0, 0.0}}, "1", LaneProfile::highway());
  const RoadId branch = author(network, {{0.0, -50.0}, {0.0, -10.0}}, "2");
  return attach(std::move(network), RoadEnd{branch, ContactPoint::End}, target, 100.0);
}

// --- Metrics ------------------------------------------------------------------

struct Segment {
  double ax, ay, bx, by;
};

bool proper_intersect(const Segment& s, const Segment& t) {
  const auto orient = [](double ax, double ay, double bx, double by, double cx, double cy) {
    return ((bx - ax) * (cy - ay)) - ((by - ay) * (cx - ax));
  };
  const double o1 = orient(s.ax, s.ay, s.bx, s.by, t.ax, t.ay);
  const double o2 = orient(s.ax, s.ay, s.bx, s.by, t.bx, t.by);
  const double o3 = orient(t.ax, t.ay, t.bx, t.by, s.ax, s.ay);
  const double o4 = orient(t.ax, t.ay, t.bx, t.by, s.bx, s.by);
  return ((o1 > 0.0) != (o2 > 0.0)) && ((o3 > 0.0) != (o4 > 0.0)) &&
         std::abs(o1) > 1e-9 && std::abs(o2) > 1e-9 && std::abs(o3) > 1e-9 && std::abs(o4) > 1e-9;
}

struct QualityMetrics {
  double min_angle_deg = 180.0;
  int slivers = 0;             ///< triangles with an angle < 5°
  int degenerate = 0;          ///< zero plan-area triangles
  int flipped = 0;             ///< recomputed face normals with z <= 0
  int boundary_crossings = 0;  ///< floor-boundary self-intersections
  int seam_z_mismatches = 0;   ///< coincident-in-plan vertex, different z
  int seam_near_misses = 0;    ///< boundary vertex 1e-6..5 cm from a road vertex
  double max_curvature = 0.0;  ///< max |κ| over connecting roads
  int ribbon_crossings = 0;    ///< connecting-road centerline crossings
  double max_seam_dz = 0.0;    ///< connecting-road endpoint z vs arm cut-face z
  bool deterministic = true;
  int connection_count = 0;
};

double road_z(const Road& road, double s) {
  return roadmaker::eval_profile(road.elevation, s);
}

QualityMetrics compute_metrics(const Tee& tee, const NetworkMesh& mesh) {
  QualityMetrics m;
  const roadmaker::Junction& junction = *tee.network.junction(tee.junction);
  m.connection_count = static_cast<int>(junction.connections.size());

  // -- Floor triangle quality + boundary extraction.
  if (!mesh.junction_floors.empty()) {
    const SubMesh& floor = mesh.junction_floors.front().mesh;
    const auto& p = floor.positions;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
    const auto key = [](std::uint32_t a, std::uint32_t b) {
      return a < b ? std::pair{a, b} : std::pair{b, a};
    };
    for (std::size_t i = 0; i + 2 < floor.indices.size(); i += 3) {
      const std::array<std::uint32_t, 3> t{floor.indices[i], floor.indices[i + 1],
                                           floor.indices[i + 2]};
      const double ax = p[t[0] * 3], ay = p[(t[0] * 3) + 1];
      const double bx = p[t[1] * 3], by = p[(t[1] * 3) + 1];
      const double cx = p[t[2] * 3], cy = p[(t[2] * 3) + 1];
      const double area2 = ((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay));
      if (std::abs(area2) < 1e-9) {
        ++m.degenerate;
        continue;
      }
      if (area2 < 0.0) {
        ++m.flipped; // plan-view CW = flipped height-field triangle
      }
      const std::array<double, 3> len{std::hypot(bx - ax, by - ay), std::hypot(cx - bx, cy - by),
                                      std::hypot(ax - cx, ay - cy)};
      double tri_min_angle = 180.0;
      for (int k = 0; k < 3; ++k) {
        const double a = len[static_cast<std::size_t>(k)];
        const double b = len[static_cast<std::size_t>((k + 1) % 3)];
        const double c = len[static_cast<std::size_t>((k + 2) % 3)];
        const double cosv = std::clamp(((b * b) + (c * c) - (a * a)) / (2.0 * b * c), -1.0, 1.0);
        tri_min_angle = std::min(tri_min_angle, std::acos(cosv) * 180.0 / kPi);
      }
      m.min_angle_deg = std::min(m.min_angle_deg, tri_min_angle);
      if (tri_min_angle < kMinTriangleAngleDeg) {
        ++m.slivers;
      }
      ++edge_count[key(t[0], t[1])];
      ++edge_count[key(t[1], t[2])];
      ++edge_count[key(t[2], t[0])];
    }

    // -- Boundary self-intersection.
    std::vector<Segment> boundary;
    for (const auto& [edge, count] : edge_count) {
      if (count == 1) {
        boundary.push_back({p[edge.first * 3], p[(edge.first * 3) + 1], p[edge.second * 3],
                            p[(edge.second * 3) + 1]});
      }
    }
    for (std::size_t i = 0; i < boundary.size(); ++i) {
      for (std::size_t j = i + 1; j < boundary.size(); ++j) {
        if (proper_intersect(boundary[i], boundary[j])) {
          ++m.boundary_crossings;
        }
      }
    }

    // -- Seam stitching against road-mesh vertices.
    std::vector<std::array<double, 3>> road_vertices;
    for (const RoadMesh& road : mesh.roads) {
      for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
        road_vertices.push_back(
            {road.positions[i], road.positions[i + 1], road.positions[i + 2]});
      }
    }
    std::vector<bool> on_boundary(p.size() / 3, false);
    for (const auto& [edge, count] : edge_count) {
      if (count == 1) {
        on_boundary[edge.first] = true;
        on_boundary[edge.second] = true;
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
          ++m.seam_z_mismatches;
        }
      } else if (best < 0.005 * 0.005) {
        // A boundary vertex just off a road vertex is a failed stitch. The
        // window stops below the floor's deliberate 1 cm weld apron along
        // unrendered ribbon borders (junction_surface.cpp kFootprintWeld).
        ++m.seam_near_misses;
      }
    }
  }

  // -- Connecting-road curvature, elevation continuity, ribbon crossings.
  struct Ribbon {
    std::vector<std::array<double, 2>> line;
    std::optional<roadmaker::RoadLink> from;
    std::optional<roadmaker::RoadLink> to;
    int from_lane = 0;
    int to_lane = 0;
  };
  std::vector<Ribbon> centerlines;
  tee.network.for_each_road([&](RoadId /*road_id*/, const Road& road) {
    if (road.junction != tee.junction) {
      return;
    }
    std::vector<std::array<double, 2>> line;
    const double length = road.plan_view.length();
    const int steps = std::max(8, static_cast<int>(length));
    for (int i = 0; i <= steps; ++i) {
      const double s = length * static_cast<double>(i) / static_cast<double>(steps);
      const auto pt = road.plan_view.evaluate(s);
      m.max_curvature = std::max(m.max_curvature, std::abs(pt.curvature));
      line.push_back({pt.x, pt.y});
    }
    // The single driving lane (-1) carries the linked incoming/outgoing
    // lane ids of the movement.
    int from_lane = 0;
    int to_lane = 0;
    for (const auto section_id : road.sections) {
      const auto& section = *tee.network.lane_section(section_id);
      for (const auto lane_id : section.lanes) {
        const auto& lane = *tee.network.lane(lane_id);
        if (lane.odr_id == -1) {
          from_lane = lane.predecessor.value_or(0);
          to_lane = lane.successor.value_or(0);
        }
      }
    }
    centerlines.push_back(
        Ribbon{std::move(line), road.predecessor, road.successor, from_lane, to_lane});

    // Endpoint elevation vs the linked arm's cut-face elevation.
    const auto arm_z = [&](const std::optional<roadmaker::RoadLink>& link) -> std::optional<double> {
      if (!link.has_value() || !std::holds_alternative<RoadId>(link->target)) {
        return std::nullopt;
      }
      const Road* arm = tee.network.road(std::get<RoadId>(link->target));
      if (arm == nullptr) {
        return std::nullopt;
      }
      return road_z(*arm, link->contact == ContactPoint::Start ? 0.0 : arm->length);
    };
    if (const auto z = arm_z(road.predecessor); z.has_value()) {
      m.max_seam_dz = std::max(m.max_seam_dz, std::abs(*z - road_z(road, 0.0)));
    }
    if (const auto z = arm_z(road.successor); z.has_value()) {
      m.max_seam_dz = std::max(m.max_seam_dz, std::abs(*z - road_z(road, road.length)));
    }
  });
  // Connections that FAN OUT of the same incoming arm from DIFFERENT lanes
  // must never cross — a crossing means the generator swapped the lane order,
  // and swapped ribbons cannot fit their linked lanes smoothly
  // (asam.net:xodr:1.9.0:junctions.connection.smooth_fit). Everything else
  // crosses legitimately in a common junction: opposing left turns, a left
  // turn over the opposing through path, and even a turn entering an OUTER
  // lane across the through path it merges beside.
  const auto same_link = [](const std::optional<roadmaker::RoadLink>& p,
                            const std::optional<roadmaker::RoadLink>& q) {
    return p.has_value() && q.has_value() && p->target == q->target && p->contact == q->contact;
  };
  for (std::size_t a = 0; a < centerlines.size(); ++a) {
    for (std::size_t b = a + 1; b < centerlines.size(); ++b) {
      if (!same_link(centerlines[a].from, centerlines[b].from) ||
          centerlines[a].from_lane == centerlines[b].from_lane) {
        continue;
      }
      int crossings = 0;
      const auto& la = centerlines[a].line;
      const auto& lb = centerlines[b].line;
      for (std::size_t i = 0; i + 1 < la.size(); ++i) {
        // Skip the first/last 2 m: movements sharing an arm meet legitimately.
        const bool near_end_a = i < 2 || i + 3 > la.size();
        for (std::size_t j = 0; j + 1 < lb.size(); ++j) {
          const bool near_end_b = j < 2 || j + 3 > lb.size();
          if (near_end_a || near_end_b) {
            continue;
          }
          const Segment sa{la[i][0], la[i][1], la[i + 1][0], la[i + 1][1]};
          const Segment sb{lb[j][0], lb[j][1], lb[j + 1][0], lb[j + 1][1]};
          if (proper_intersect(sa, sb)) {
            ++crossings;
          }
        }
      }
      m.ribbon_crossings += crossings;
    }
  }
  return m;
}

std::string format_metrics(const QualityMetrics& m) {
  return fmt::format("connections={} min_angle={:.2f}deg slivers={} degenerate={} flipped={} "
                     "boundary_crossings={} seam_z_mismatches={} seam_near_misses={} "
                     "max_curvature={:.4f} ribbon_crossings={} max_seam_dz={:.6f} "
                     "deterministic={}",
                     m.connection_count, m.min_angle_deg, m.slivers, m.degenerate, m.flipped,
                     m.boundary_crossings, m.seam_z_mismatches, m.seam_near_misses,
                     m.max_curvature, m.ribbon_crossings, m.max_seam_dz, m.deterministic);
}

/// Dumps .xodr + .glb + metrics when RM_TJ_DIAG_DIR is set (diagnosis and PR
/// screenshot workflow — see the file header).
void dump_diagnostics(const std::string& name,
                      const Tee& tee,
                      const NetworkMesh& mesh,
                      const QualityMetrics& metrics) {
  const char* dir = std::getenv("RM_TJ_DIAG_DIR");
  if (dir == nullptr) {
    return;
  }
  namespace fs = std::filesystem;
  const fs::path base = fs::path(dir);
  fs::create_directories(base);
  ASSERT_TRUE(roadmaker::save_xodr(tee.network, base / (name + ".xodr"), name).has_value());
  ASSERT_TRUE(roadmaker::export_glb(mesh, base / (name + ".glb")).has_value());
  std::ofstream out(base / (name + ".metrics.txt"));
  out << format_metrics(metrics) << "\n";
}

struct TeeCase {
  const char* name;
  Tee (*build)();
};

class TJunctionQuality : public ::testing::TestWithParam<TeeCase> {};

} // namespace

TEST_P(TJunctionQuality, MeshAndGeometryInvariants) {
  const TeeCase& tc = GetParam();
  Tee tee = tc.build();
  ASSERT_TRUE(tee.junction.is_valid());

  NetworkMesh mesh = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(mesh.junction_floors.empty()) << "tee produced no junction floor";

  QualityMetrics m = compute_metrics(tee, mesh);

  // Determinism (03 §7.4): a second build is bitwise identical.
  const NetworkMesh again = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(again.junction_floors.empty());
  m.deterministic =
      again.junction_floors.front().mesh.positions == mesh.junction_floors.front().mesh.positions &&
      again.junction_floors.front().mesh.indices == mesh.junction_floors.front().mesh.indices;

  RecordProperty("metrics", format_metrics(m));
  fmt::print("[{}] {}\n", tc.name, format_metrics(m));
  dump_diagnostics(tc.name, tee, mesh, m);

  EXPECT_GT(m.connection_count, 0);
  EXPECT_EQ(m.degenerate, 0);
  EXPECT_EQ(m.flipped, 0);
  EXPECT_LE(m.slivers, kSliverBudget);
  EXPECT_EQ(m.boundary_crossings, 0);
  EXPECT_EQ(m.seam_z_mismatches, 0);
  EXPECT_EQ(m.seam_near_misses, 0);
  EXPECT_LE(m.max_curvature, kMaxDrivableCurvature);
  EXPECT_EQ(m.ribbon_crossings, 0);
  EXPECT_LE(m.max_seam_dz, roadmaker::tol::kLength);
  EXPECT_TRUE(m.deterministic);
}

INSTANTIATE_TEST_SUITE_P(Matrix,
                         TJunctionQuality,
                         ::testing::Values(TeeCase{"perp", build_perp},
                                           TeeCase{"deg45", build_deg45},
                                           TeeCase{"deg135", build_deg135},
                                           TeeCase{"r100_outside", [] { return build_curved(100.0, false); }},
                                           TeeCase{"r100_inside", [] { return build_curved(100.0, true); }},
                                           TeeCase{"r30_outside", [] { return build_curved(30.0, false); }},
                                           TeeCase{"r30_inside", [] { return build_curved(30.0, true); }},
                                           TeeCase{"asymmetric", build_asymmetric},
                                           TeeCase{"start_contact", build_start_contact},
                                           TeeCase{"graded", build_graded},
                                           TeeCase{"multilane", build_multilane}),
                         [](const ::testing::TestParamInfo<TeeCase>& info) {
                           return std::string(info.param.name);
                         });
