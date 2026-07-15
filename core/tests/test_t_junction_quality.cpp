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
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
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

/// A tee fixture before the attach runs: the authored pre-network plus the
/// attach parameters — so tests can snapshot the pre-attach state (undo
/// byte-identity) before materializing.
struct TeeSetup {
  RoadNetwork network;
  RoadEnd branch_end;
  RoadId target;
  double s = 0.0;
};

/// One authored tee: the junction plus its three arms as recorded by
/// attach_t_junction (head:End, tail:Start, branch).
struct Tee {
  RoadNetwork network;
  JunctionId junction;
};

Tee attach(RoadNetwork&& network, RoadEnd branch_end, RoadId target, double s) {
  apply_or_throw(network, roadmaker::edit::attach_t_junction(network, branch_end, target, s));
  Tee tee{.network = std::move(network)};
  tee.network.for_each_junction(
      [&](JunctionId id, const roadmaker::Junction&) { tee.junction = id; });
  return tee;
}

Tee attach(TeeSetup&& setup) {
  return attach(std::move(setup.network), setup.branch_end, setup.target, setup.s);
}

// --- Fixture matrix (§3.1 of the task prompt) --------------------------------

/// Straight E-W target, branch arriving from the south, perpendicular.
TeeSetup setup_perp() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(setup.network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
}

/// Branch heading 45° into the target (shallow same-direction merge angle).
TeeSetup setup_deg45() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(setup.network, {{-40.0, -40.0}, {-6.0, -6.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
}

/// Branch heading 135° into the target (against the +s direction).
TeeSetup setup_deg135() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(setup.network, {{40.0, -40.0}, {6.0, -6.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
}

/// Curved target of radius `r`, branch attached at the apex from outside
/// (`inside=false`, convex side) or inside (toward the arc center).
TeeSetup setup_curved(double r, bool inside) {
  TeeSetup setup;
  // Waypoints on a circle centered at (0, r); apex at the origin.
  const double half_angle = r > 50.0 ? 35.0 * kPi / 180.0 : 60.0 * kPi / 180.0;
  std::vector<Waypoint> arc;
  for (int i = -2; i <= 2; ++i) {
    const double theta = half_angle * static_cast<double>(i) / 2.0;
    arc.push_back({r * std::sin(theta), r - (r * std::cos(theta))});
  }
  setup.target = author(setup.network, arc, "1");
  setup.s = setup.network.road(setup.target)->length / 2.0;
  const RoadId branch =
      inside ? author(setup.network, {{0.0, std::min(r - 6.0, 24.0)}, {0.0, 6.0}}, "2")
             : author(setup.network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  return setup;
}

/// Asymmetric widths: the branch is narrower than the target.
TeeSetup setup_asymmetric() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  LaneProfile narrow;
  narrow.left = {LaneSpec{.type = LaneType::Driving, .width = 3.0}};
  narrow.right = {LaneSpec{.type = LaneType::Driving, .width = 3.0}};
  const RoadId branch = author(setup.network, {{0.0, -40.0}, {0.0, -6.0}}, "2", narrow);
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
}

/// Branch arriving with its Start contact (authored away from the target).
TeeSetup setup_start_contact() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  const RoadId branch = author(setup.network, {{0.0, -6.0}, {0.0, -40.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::Start};
  setup.s = 100.0;
  return setup;
}

/// Graded target (3 % climb) — the tee cut faces must inherit the profile.
TeeSetup setup_graded() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1");
  apply_or_throw(setup.network,
                 roadmaker::edit::set_elevation_profile(
                     setup.network, setup.target, {{.s = 0.0, .z = 0.0}, {.s = 200.0, .z = 6.0}}));
  const RoadId branch = author(setup.network, {{0.0, -40.0}, {0.0, -6.0}}, "2");
  apply_or_throw(setup.network,
                 roadmaker::edit::set_elevation_profile(
                     setup.network, branch, {{.s = 0.0, .z = 3.0}, {.s = 34.0, .z = 3.0}}));
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
}

/// Multi-lane target (highway template, 2 driving lanes per side): every
/// incoming lane must land on its own smoothly-fitting connecting ribbon.
TeeSetup setup_multilane() {
  TeeSetup setup;
  setup.target = author(setup.network, {{-100.0, 0.0}, {100.0, 0.0}}, "1", LaneProfile::highway());
  const RoadId branch = author(setup.network, {{0.0, -50.0}, {0.0, -10.0}}, "2");
  setup.branch_end = RoadEnd{branch, ContactPoint::End};
  setup.s = 100.0;
  return setup;
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
  return ((o1 > 0.0) != (o2 > 0.0)) && ((o3 > 0.0) != (o4 > 0.0)) && std::abs(o1) > 1e-9 &&
         std::abs(o2) > 1e-9 && std::abs(o3) > 1e-9 && std::abs(o4) > 1e-9;
}

struct QualityMetrics {
  double min_angle_deg = 180.0;
  int slivers = 0;            ///< triangles with an angle < 5°
  int degenerate = 0;         ///< zero plan-area triangles
  int flipped = 0;            ///< recomputed face normals with z <= 0
  int boundary_crossings = 0; ///< floor-boundary self-intersections
  int seam_z_mismatches = 0;  ///< coincident-in-plan vertex, different z
  int seam_near_misses = 0;   ///< boundary vertex 1e-6..5 cm from a road vertex
  double max_curvature = 0.0; ///< max |κ| over connecting roads
  int ribbon_crossings = 0;   ///< connecting-road centerline crossings
  double max_seam_dz = 0.0;   ///< connecting-road endpoint z vs arm cut-face z
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
      const std::array<std::uint32_t, 3> t{
          floor.indices[i], floor.indices[i + 1], floor.indices[i + 2]};
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
      const std::array<double, 3> len{
          std::hypot(bx - ax, by - ay), std::hypot(cx - bx, cy - by), std::hypot(ax - cx, ay - cy)};
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
        boundary.push_back({p[edge.first * 3],
                            p[(edge.first * 3) + 1],
                            p[edge.second * 3],
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
        road_vertices.push_back({road.positions[i], road.positions[i + 1], road.positions[i + 2]});
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
    const auto arm_z =
        [&](const std::optional<roadmaker::RoadLink>& link) -> std::optional<double> {
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
                     m.connection_count,
                     m.min_angle_deg,
                     m.slivers,
                     m.degenerate,
                     m.flipped,
                     m.boundary_crossings,
                     m.seam_z_mismatches,
                     m.seam_near_misses,
                     m.max_curvature,
                     m.ribbon_crossings,
                     m.max_seam_dz,
                     m.deterministic);
}

/// RM_TJ_DIAG_DIR's value, empty when unset. MSVC deprecates std::getenv
/// (C4996 under /WX); _dupenv_s is its sanctioned equivalent.
std::string diag_dir() {
#ifdef _MSC_VER
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "RM_TJ_DIAG_DIR") != 0 || value == nullptr) {
    return {};
  }
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv("RM_TJ_DIAG_DIR");
  return value == nullptr ? std::string() : std::string(value);
#endif
}

/// Dumps .xodr + .glb + metrics when RM_TJ_DIAG_DIR is set (diagnosis and PR
/// screenshot workflow — see the file header).
void dump_diagnostics(const std::string& name,
                      const Tee& tee,
                      const NetworkMesh& mesh,
                      const QualityMetrics& metrics) {
  const std::string dir = diag_dir();
  if (dir.empty()) {
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
  TeeSetup (*setup)();
};

class TJunctionQuality : public ::testing::TestWithParam<TeeCase> {};

} // namespace

TEST_P(TJunctionQuality, MeshAndGeometryInvariants) {
  const TeeCase& tc = GetParam();
  Tee tee = attach(tc.setup());
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

// G1 continuity at both contact points (§10.3 via
// asam.net:xodr:1.9.0:junctions.connection.smooth_fit): every connecting
// road's reference line starts/ends exactly on its linked arm's cut face
// with the arm's tangent heading, within rm::tol.
TEST_P(TJunctionQuality, ConnectingRoadsAreG1AtContacts) {
  Tee tee = attach(GetParam().setup());
  const roadmaker::Junction& junction = *tee.network.junction(tee.junction);
  ASSERT_FALSE(junction.connections.empty());
  int checked = 0;
  tee.network.for_each_road([&](RoadId /*id*/, const Road& road) {
    if (road.junction != tee.junction) {
      return;
    }
    const auto expect_g1 = [&](const std::optional<roadmaker::RoadLink>& link, double s_conn) {
      ASSERT_TRUE(link.has_value());
      ASSERT_TRUE(std::holds_alternative<RoadId>(link->target));
      const Road& arm = *tee.network.road(std::get<RoadId>(link->target));
      const double arm_s = link->contact == ContactPoint::Start ? 0.0 : arm.length;
      const auto arm_pose = arm.plan_view.evaluate(arm_s);
      const auto conn_pose = road.plan_view.evaluate(s_conn);
      // Position: the connecting reference line is anchored on the linked
      // lane's inner boundary — a point ON the arm's cut-face line. Check
      // the along-face-normal distance (the cut face is the line through
      // arm_pose perpendicular to the arm tangent).
      const double dx = conn_pose.x - arm_pose.x;
      const double dy = conn_pose.y - arm_pose.y;
      const double along_tangent = (dx * std::cos(arm_pose.hdg)) + (dy * std::sin(arm_pose.hdg));
      EXPECT_NEAR(along_tangent, 0.0, roadmaker::tol::kLength)
          << "contact point off the arm cut face";
      // Heading: continuous with the arm tangent (mod π — the connecting
      // road runs into or out of the arm depending on the contact).
      const double dh = std::remainder(conn_pose.hdg - arm_pose.hdg, kPi);
      EXPECT_NEAR(dh, 0.0, 1e-6) << "heading kink at the contact point";
      ++checked;
    };
    expect_g1(road.predecessor, 0.0);
    expect_g1(road.successor, road.plan_view.length());
  });
  EXPECT_GT(checked, 0);
}

// The exported tee passes our checker-rule validator with zero errors for both
// supported targets, and now closes its <boundary> (auxiliary boundary roads
// fill any gap, #62), so the boundary-omitted warning
// (junctions.boundary.close_gap_with_new_roads) no longer fires — the loop
// guards that no unexpected diagnostic slipped in.
TEST_P(TJunctionQuality, ExportValidatesCleanly) {
  Tee tee = attach(GetParam().setup());
  using roadmaker::XodrVersion;
  for (const XodrVersion version : {XodrVersion::v1_8_1, XodrVersion::v1_9_0}) {
    const auto findings = roadmaker::validate_network(
        tee.network, roadmaker::WriterOptions{.target_version = version});
    EXPECT_EQ(roadmaker::count_errors(findings), 0U);
    for (const auto& finding : findings) {
      EXPECT_EQ(finding.rule_id, roadmaker::rules::kJunctionBoundaryCloseGap)
          << "unexpected diagnostic: " << finding.message;
    }
  }
}

// Round-trip: tee → save → reload → re-mesh is bitwise identical (rm:arms
// persists the arm list, and the junction pipeline is deterministic), and
// a reload → re-save is byte-identical.
TEST_P(TJunctionQuality, SaveReloadRemeshesIdentically) {
  Tee tee = attach(GetParam().setup());
  const NetworkMesh before = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(before.junction_floors.empty());

  const auto xml = roadmaker::write_xodr(tee.network, "tee");
  ASSERT_TRUE(xml.has_value());
  auto parsed = roadmaker::parse_xodr(*xml, "<tee>");
  ASSERT_TRUE(parsed.has_value());
  const auto second = roadmaker::write_xodr(parsed->network, "tee");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*xml, *second);

  const NetworkMesh after = roadmaker::build_network_mesh(parsed->network);
  ASSERT_FALSE(after.junction_floors.empty());
  EXPECT_EQ(before.junction_floors.front().mesh.positions,
            after.junction_floors.front().mesh.positions);
  EXPECT_EQ(before.junction_floors.front().mesh.indices,
            after.junction_floors.front().mesh.indices);
}

// ONE undo step returns the pre-attach network byte-identically — including
// the branch trim and both target splits (the composite reverts in reverse).
TEST_P(TJunctionQuality, UndoRestoresPreAttachBytes) {
  TeeSetup setup = GetParam().setup();
  const auto before = roadmaker::write_xodr(setup.network, "tee");
  ASSERT_TRUE(before.has_value());

  auto command =
      roadmaker::edit::attach_t_junction(setup.network, setup.branch_end, setup.target, setup.s);
  ASSERT_TRUE(command->apply(setup.network).has_value());
  ASSERT_TRUE(command->revert(setup.network).has_value());

  const auto after = roadmaker::write_xodr(setup.network, "tee");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

// The junction surface data (§12.9–12.11) exports for tees exactly as for
// endpoint junctions: one reference line and one elevation grid
// (asam.net:xodr:1.8.0:junctions.geometry.only_one_line_element,
// …elevation_grid.only_one_elev_grid).
TEST(TJunctionExport, TeeEmitsReferenceLineAndElevationGrid) {
  Tee tee = attach(setup_graded());
  const auto xml = roadmaker::write_xodr(tee.network, "tee");
  ASSERT_TRUE(xml.has_value());
  const std::size_t junction_pos = xml->find("<junction");
  ASSERT_NE(junction_pos, std::string::npos);
  EXPECT_NE(xml->find("<planView>", junction_pos), std::string::npos);
  EXPECT_NE(xml->find("<elevationGrid", junction_pos), std::string::npos);
  EXPECT_NE(xml->find("rm:arms", junction_pos), std::string::npos);
}

namespace {

/// The floor's outer boundary as an ordered vertex loop (largest loop when
/// the CDT left more than one — the outer ring).
std::vector<std::array<double, 2>> boundary_loop(const SubMesh& floor) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
  const auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  for (std::size_t i = 0; i + 2 < floor.indices.size(); i += 3) {
    edge_count[key(floor.indices[i], floor.indices[i + 1])] += 1;
    edge_count[key(floor.indices[i + 1], floor.indices[i + 2])] += 1;
    edge_count[key(floor.indices[i + 2], floor.indices[i])] += 1;
  }
  std::map<std::uint32_t, std::vector<std::uint32_t>> adjacency;
  for (const auto& [edge, count] : edge_count) {
    if (count == 1) {
      adjacency[edge.first].push_back(edge.second);
      adjacency[edge.second].push_back(edge.first);
    }
  }
  std::vector<std::array<double, 2>> best;
  std::set<std::uint32_t> visited;
  for (const auto& [start, _] : adjacency) {
    if (visited.contains(start)) {
      continue;
    }
    std::vector<std::uint32_t> loop{start};
    visited.insert(start);
    std::uint32_t prev = start;
    std::uint32_t cur = adjacency[start].front();
    while (cur != start) {
      loop.push_back(cur);
      visited.insert(cur);
      const auto& next = adjacency[cur];
      const std::uint32_t follow = next.front() == prev ? next.back() : next.front();
      prev = cur;
      cur = follow;
    }
    if (loop.size() > best.size()) {
      best.clear();
      for (const std::uint32_t v : loop) {
        best.push_back({floor.positions[v * 3], floor.positions[(v * 3) + 1]});
      }
    }
  }
  return best;
}

double signed_area(const std::vector<std::array<double, 2>>& loop) {
  double area2 = 0.0;
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const auto& a = loop[i];
    const auto& b = loop[(i + 1) % loop.size()];
    area2 += (a[0] * b[1]) - (b[0] * a[1]);
  }
  return area2 / 2.0;
}

/// Circumradius of the triangle (a, b, c); huge for near-collinear points.
double circumradius(const std::array<double, 2>& a,
                    const std::array<double, 2>& b,
                    const std::array<double, 2>& c) {
  const double la = std::hypot(b[0] - a[0], b[1] - a[1]);
  const double lb = std::hypot(c[0] - b[0], c[1] - b[1]);
  const double lc = std::hypot(a[0] - c[0], a[1] - c[1]);
  const double area2 = std::abs(((b[0] - a[0]) * (c[1] - a[1])) - ((c[0] - a[0]) * (b[1] - a[1])));
  if (area2 < 1e-12) {
    return std::numeric_limits<double>::max();
  }
  return (la * lb * lc) / (2.0 * area2);
}

} // namespace

// Fillet existence (tee visual spec §3): the floor's outer boundary must be
// G1-smooth at every re-entrant (concave) corner — no sharp bite where two
// arms meet — and the concave arcs must not dip below the fillet radius
// floor. Convex corners (arm faces, edge/face joints) are legitimate hard
// corners and exempt.
TEST_P(TJunctionQuality, FilletedBoundaryIsSmoothAndRadiused) {
  Tee tee = attach(GetParam().setup());
  const NetworkMesh mesh = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(mesh.junction_floors.empty());

  std::vector<std::array<double, 2>> loop = boundary_loop(mesh.junction_floors.front().mesh);
  ASSERT_GE(loop.size(), 8u);
  if (signed_area(loop) < 0.0) {
    std::ranges::reverse(loop); // normalize to CCW: interior on the left
  }

  const std::size_t n = loop.size();
  std::vector<double> concave_turn_deg(n, 0.0);
  double max_concave_turn = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto& a = loop[(i + n - 1) % n];
    const auto& b = loop[i];
    const auto& c = loop[(i + 1) % n];
    const double ux = b[0] - a[0], uy = b[1] - a[1];
    const double vx = c[0] - b[0], vy = c[1] - b[1];
    const double cross = (ux * vy) - (uy * vx);
    const double dot = (ux * vx) + (uy * vy);
    const double turn = std::atan2(std::abs(cross), dot) * 180.0 / kPi;
    // CCW loop: right turns (cross < 0) are re-entrant corners.
    if (cross < 0.0) {
      concave_turn_deg[i] = turn;
      max_concave_turn = std::max(max_concave_turn, turn);
    }
  }
  // A missing fillet shows up as one sharp re-entrant corner (90° for a perp
  // tee); a filleted boundary turns through the same total angle in small
  // per-vertex steps. 20° accommodates the coarsest legitimate arc step plus
  // the tangency shortcut (kFilletTangentLift).
  EXPECT_LT(max_concave_turn, 20.0) << "re-entrant corner without a fillet arc";

  // Radius floor along concave runs: stride-2 triples keep the sagitta far
  // above the union's sub-centimeter vertex noise. The kernel targets a 3 m
  // arc but clamps to the tangent legs the cut faces leave room for — skew
  // (deg45/deg135) and narrow-branch corners legitimately land near 2 m —
  // so the gate asserts arcs never collapse to cosmetic size rather than
  // the un-clamped default.
  double min_concave_radius = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < n; ++i) {
    const std::array<std::size_t, 5> run{
        (i + n - 2) % n, (i + n - 1) % n, i, (i + 1) % n, (i + 2) % n};
    const bool all_concave =
        std::ranges::all_of(run, [&](std::size_t k) { return concave_turn_deg[k] > 2.0; });
    if (all_concave) {
      min_concave_radius =
          std::min(min_concave_radius, circumradius(loop[run[0]], loop[run[2]], loop[run[4]]));
    }
  }
  if (min_concave_radius < std::numeric_limits<double>::max()) {
    EXPECT_GE(min_concave_radius, 1.8) << "fillet arc below the radius floor";
  }
}

// Material continuity (tee visual spec §3): the floor carries the driving
// material — junction interiors export as the same asphalt as the roads
// feeding them, and the legacy junction-debug material never reappears.
TEST_P(TJunctionQuality, FloorCarriesDrivingMaterial) {
  Tee tee = attach(GetParam().setup());
  const NetworkMesh mesh = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(mesh.junction_floors.empty());
  for (const roadmaker::JunctionFloor& floor : mesh.junction_floors) {
    EXPECT_EQ(floor.mesh.material, LaneType::Driving);
  }

  const std::filesystem::path glb = std::filesystem::temp_directory_path() /
                                    fmt::format("rm_tee_material_{}.glb", GetParam().name);
  ASSERT_TRUE(roadmaker::export_glb(mesh, glb).has_value());
  std::string bytes;
  {
    // Scoped: Windows cannot remove a file a stream still holds open.
    std::ifstream in(glb, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(bytes.find("junction_floor"), std::string::npos)
      << "legacy junction-debug material leaked into the export";
  std::filesystem::remove(glb);

  // Connecting roads surface as the floor; their own grid must stay empty
  // and they carry no markings — no paint through the junction interior.
  tee.network.for_each_road([&](RoadId id, const Road& road) {
    if (road.junction != tee.junction) {
      return;
    }
    for (const RoadMesh& rm : mesh.roads) {
      if (rm.road == id) {
        EXPECT_TRUE(rm.lanes.empty()) << "connecting road double-surfaces the junction";
        EXPECT_TRUE(rm.markings.empty()) << "road marks through the junction interior";
      }
    }
  });
}

// Shading continuity (tee visual spec §3): road surfaces shade smooth along
// their length — adjacent face normals stay well under the crease threshold —
// and welded vertices (bitwise-equal positions, floor/arm seams included)
// never carry divergent normals. On the graded fixture the arm normals must
// actually tilt with the slope (the grade-blind-normal regression).
TEST_P(TJunctionQuality, SurfaceNormalsAreSmoothAndWelded) {
  constexpr double kCreaseDeg = 40.0;
  Tee tee = attach(GetParam().setup());
  const NetworkMesh mesh = roadmaker::build_network_mesh(tee.network);
  ASSERT_FALSE(mesh.junction_floors.empty());

  const auto face_normal = [](const std::vector<double>& p,
                              std::uint32_t a,
                              std::uint32_t b,
                              std::uint32_t c) {
    const double ux = p[b * 3] - p[a * 3];
    const double uy = p[(b * 3) + 1] - p[(a * 3) + 1];
    const double uz = p[(b * 3) + 2] - p[(a * 3) + 2];
    const double vx = p[c * 3] - p[a * 3];
    const double vy = p[(c * 3) + 1] - p[(a * 3) + 1];
    const double vz = p[(c * 3) + 2] - p[(a * 3) + 2];
    std::array<double, 3> n{(uy * vz) - (uz * vy), (uz * vx) - (ux * vz), (ux * vy) - (uy * vx)};
    const double len = std::hypot(n[0], std::hypot(n[1], n[2]));
    if (len > 0.0) {
      n = {n[0] / len, n[1] / len, n[2] / len};
    }
    return n;
  };
  const auto angle_deg = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double dot = std::clamp((a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]), -1.0, 1.0);
    return std::acos(dot) * 180.0 / kPi;
  };

  // (a) Adjacent-triangle face normals within each surface.
  const auto check_surface = [&](const std::vector<double>& positions,
                                 const std::vector<std::uint32_t>& indices,
                                 const char* what) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::array<double, 3>> first_face;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
      const auto n = face_normal(positions, indices[i], indices[i + 1], indices[i + 2]);
      const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> edges{
          std::minmax(indices[i], indices[i + 1]),
          std::minmax(indices[i + 1], indices[i + 2]),
          std::minmax(indices[i + 2], indices[i])};
      for (const auto& edge : edges) {
        const auto found = first_face.find(edge);
        if (found == first_face.end()) {
          first_face.emplace(edge, n);
        } else {
          EXPECT_LT(angle_deg(found->second, n), kCreaseDeg)
              << what << ": shading crease across a shared edge";
        }
      }
    }
  };
  for (const RoadMesh& road : mesh.roads) {
    std::vector<std::uint32_t> all;
    for (const RoadMesh::LanePatch& patch : road.lanes) {
      all.insert(all.end(), patch.indices.begin(), patch.indices.end());
    }
    check_surface(road.positions, all, "road");
  }
  const SubMesh& floor = mesh.junction_floors.front().mesh;
  check_surface(floor.positions, floor.indices, "floor");

  // (b) Weld check: bitwise-equal positions across the whole network mesh
  // must agree on their vertex normal (seams share vertices AND shading).
  std::map<std::array<double, 3>, std::array<double, 3>> seen;
  const auto check_welds = [&](const std::vector<double>& positions,
                               const std::vector<double>& normals) {
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
      const std::array<double, 3> pos{positions[i], positions[i + 1], positions[i + 2]};
      const std::array<double, 3> nrm{normals[i], normals[i + 1], normals[i + 2]};
      const auto found = seen.find(pos);
      if (found == seen.end()) {
        seen.emplace(pos, nrm);
      } else {
        EXPECT_LT(angle_deg(found->second, nrm), kCreaseDeg)
            << "coincident vertices with divergent normals at (" << pos[0] << ", " << pos[1] << ")";
      }
    }
  };
  for (const RoadMesh& road : mesh.roads) {
    check_welds(road.positions, road.normals);
  }
  check_welds(floor.positions, floor.normals);

  // (c) Grade regression: a graded arm's surface normals tilt with dz/ds.
  if (std::string(GetParam().name) == "graded") {
    double max_tilt = 0.0;
    for (const RoadMesh& road : mesh.roads) {
      for (std::size_t i = 0; i + 2 < road.normals.size(); i += 3) {
        max_tilt = std::max(max_tilt, std::hypot(road.normals[i], road.normals[i + 1]));
      }
    }
    EXPECT_GT(max_tilt, 0.01) << "graded roads still lit as if flat";
  }
}

// Regenerates the committed tee golden (assets/samples + fuzz corpus per the
// new-xodr-feature rule). Run manually:
// roadmaker_core_tests --gtest_also_run_disabled_tests
//   --gtest_filter='*DISABLED_WriteTeeSample'.
TEST(TJunctionQualityTools, DISABLED_WriteTeeSample) {
  namespace fs = std::filesystem;
  Tee tee = attach(setup_perp());
  ASSERT_TRUE(
      roadmaker::save_xodr(tee.network, fs::path(RM_SAMPLES_DIR) / "t_attach.xodr", "t_attach")
          .has_value());
  ASSERT_TRUE(
      roadmaker::save_xodr(tee.network, fs::path(RM_FUZZ_CORPUS_DIR) / "t_attach.xodr", "t_attach")
          .has_value());
}

INSTANTIATE_TEST_SUITE_P(
    Matrix,
    TJunctionQuality,
    ::testing::Values(TeeCase{"perp", setup_perp},
                      TeeCase{"deg45", setup_deg45},
                      TeeCase{"deg135", setup_deg135},
                      TeeCase{"r100_outside", [] { return setup_curved(100.0, false); }},
                      TeeCase{"r100_inside", [] { return setup_curved(100.0, true); }},
                      TeeCase{"r30_outside", [] { return setup_curved(30.0, false); }},
                      TeeCase{"r30_inside", [] { return setup_curved(30.0, true); }},
                      TeeCase{"asymmetric", setup_asymmetric},
                      TeeCase{"start_contact", setup_start_contact},
                      TeeCase{"graded", setup_graded},
                      TeeCase{"multilane", setup_multilane}),
    // `tee_case`, not `info` — GCC -Wshadow flags the gtest macro's internal
    // parameter of the same name.
    [](const ::testing::TestParamInfo<TeeCase>& tee_case) {
      return std::string(tee_case.param.name);
    });
