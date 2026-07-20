#include "junction_surface.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <clipper2/clipper.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "fill_backend.hpp"
#include "junction_corner_detail.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using namespace fill_backend;

using junction_corner_detail::connecting_roads;
using junction_corner_detail::corner_curve;
using junction_corner_detail::corner_faces;
using junction_corner_detail::CornerFace;
using junction_corner_detail::CornerSolution;
using junction_corner_detail::solve_corner;

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::mandatory_stations;
using mesh_detail::section_at;
using mesh_detail::StationFrame;

// The tiny-footprint, Steiner-spacing, centerline-weight, cotangent-clamp,
// union-precision, and seam-snap constants shared with surface_fill.cpp live in
// fill_backend.hpp (kFlatFloorMinArea, kSteinerStep, kCenterlineWeight,
// kMinCotWeight, kUnionPrecision, kSeamSnap). The constants below are
// junction-only (joint quads, face cuts, corner fillets, edge strips).

// Depth [m] a joint quad extends from an arm's end cross-section into the
// junction interior: bridges the union to every arm face even where turn
// footprints leave the mouth corners uncovered (03 §1 — the tee exposed
// their absence: uncovered wedges at the branch mouth, issue #103).
constexpr double kJointDepth = 2.0;

// Weld inflation [m] applied to every connecting-road footprint before the
// union: adjacent connecting roads are EXACTLY tangent (they share
// lane-boundary anchors) but sample their shared border at different
// stations, so the raw union keeps millimeter-wide zigzag channels along
// those seams — the CDT turns them into sub-degree sliver triangles.
// Inflating each ribbon makes tangent neighbors overlap, so the channels
// become interior. The floor's outer edge lands this far outside the true
// curb line — a 1 cm apron nobody stitches to (connecting-road surfaces are
// not rendered separately; the floor IS the junction surface).
constexpr double kFootprintWeld = 0.01;

// Everything the inflation pushed PAST an arm's end cross-section is cut
// back with a rectangle extending this far [m] over the road side of the
// face line, so the floor never overlaps (and z-fights) the arm's own mesh.
constexpr double kFaceCutDepth = 1.0;

// Lateral overhang [m] of the face-cut rectangle beyond the arm's outermost
// boundaries: enough to catch the inflated ribbon corners, small enough to
// never clip an unrelated ribbon passing near the arm.
constexpr double kFaceCutMargin = 0.5;

// Boundary vertices deviating less than this [m] from the segment between
// their neighbors are dropped after the union (weld-arc tessellation and
// rounding debris). Arm-face lane-boundary vertices are re-inserted
// afterwards as exact CDT vertices, so simplification never loses a stitch
// target; curved borders keep their station vertices (their sagitta at the
// meshing step is far above this).
constexpr double kBoundarySimplify = 5e-3;

// The corner-fillet geometry (radius derivation, tangent legs, curve
// sampling) lives in junction_corner_detail.{hpp,cpp} — the mesher and the
// public junction_corners() query must solve the SAME corner, so the Corner
// tool's handles sit exactly on the pavement emitted here (issue #225).

// Width [m] of the corridor strips laid along each arm's outer edge lines
// between the face corner and the fillet tangency: turn ribbons cover only
// their own lanes, so without the strips the pavement between a wide arm's
// shoulder edge and the ribbon keeps uncovered notches (the tee's 1x2 m
// mouth step, issue #103 round 2). Wide enough to overlap every ribbon,
// narrow enough to never cross an arm's opposite edge (arm half-widths are
// >= 1.75 m for any drivable profile).
constexpr double kEdgeStripWidth = 1.0;

/// Appends the corner fillet wedges and edge corridor strips for all
/// angularly adjacent arm pairs. The wedge fills the re-entrant corner up to
/// the corner curve tangent to both arms' edge lines (G1 boundary); the strips
/// pin the boundary to the exact pavement edge lines between each face corner
/// and its tangency. The corners themselves are solved by
/// junction_corner_detail — the same solve the Corner tool edits.
void append_corner_fillets(const RoadNetwork& network,
                           const Junction& junction,
                           const std::vector<CornerFace>& faces,
                           Clipper2Lib::PathsD& footprints) {
  if (faces.size() < 2) {
    return;
  }
  const auto push_ccw = [&footprints](Clipper2Lib::PathD path) {
    if (Clipper2Lib::Area(path) < 0.0) {
      std::ranges::reverse(path);
    }
    footprints.push_back(std::move(path));
  };

  for (std::size_t i = 0; i < faces.size(); ++i) {
    const CornerFace& a = faces[i];
    const CornerFace& b = faces[(i + 1) % faces.size()];
    const std::array<double, 2> pa = a.right; // A's edge facing B
    const std::array<double, 2> pb = b.left;  // B's edge facing A
    const CornerSolution solution = solve_corner(network, junction, a, b);
    if (solution.parallel_edges) {
      // Parallel edges: a straight through corridor, no corner to fillet —
      // but the corridor edge itself still needs pavement. Through ribbons
      // cover only their lanes, so a wide arm's outer band (highway
      // shoulders) would otherwise stay uncovered between the 2 m joint
      // quads: the boundary dropped to the driving edge mid-corridor with
      // two 90-degree bites. The strip pins the boundary to the edge chord;
      // the enclosed remainder of the band is paved by the hole fill below.
      if (((a.ix * (pb[0] - pa[0])) + (a.iy * (pb[1] - pa[1]))) > tol::kLength &&
          ((b.ix * (pa[0] - pb[0])) + (b.iy * (pa[1] - pb[1]))) > tol::kLength) {
        Clipper2Lib::PathD quad;
        quad.emplace_back(pa[0], pa[1]);
        quad.emplace_back(pb[0], pb[1]);
        quad.emplace_back(pb[0] + (b.iy * kEdgeStripWidth), pb[1] - (b.ix * kEdgeStripWidth));
        quad.emplace_back(pa[0] - (a.iy * kEdgeStripWidth), pa[1] + (a.ix * kEdgeStripWidth));
        push_ccw(std::move(quad));
      }
      continue;
    }
    if (!solution.valid) {
      continue;
    }
    const std::array<double, 2>& corner = solution.corner;

    // Wedge: corner -> tangency on A -> curve -> tangency on B.
    Clipper2Lib::PathD wedge;
    wedge.emplace_back(corner[0], corner[1]);
    for (const std::array<double, 2>& p : corner_curve(solution)) {
      wedge.emplace_back(p[0], p[1]);
    }
    push_ccw(std::move(wedge));

    // Corridor strips: exact pavement edge from each face corner all the way
    // to the corner point, one edge-strip width inward — the wedge covers
    // only the fillet side of the edge lines, and turn ribbons cover only
    // their own lanes, so without the strips the outermost band (shoulders)
    // keeps notches between the face and the corner.
    const auto strip = [&push_ccw](const std::array<double, 2>& from,
                                   const std::array<double, 2>& to,
                                   double nx,
                                   double ny) {
      if (std::hypot(to[0] - from[0], to[1] - from[1]) < tol::kLength) {
        return;
      }
      Clipper2Lib::PathD quad;
      quad.emplace_back(from[0], from[1]);
      quad.emplace_back(to[0], to[1]);
      quad.emplace_back(to[0] + (nx * kEdgeStripWidth), to[1] + (ny * kEdgeStripWidth));
      quad.emplace_back(from[0] + (nx * kEdgeStripWidth), from[1] + (ny * kEdgeStripWidth));
      push_ccw(std::move(quad));
    };
    strip(pa, corner, -a.iy, a.ix); // interior is left of A's right edge
    strip(pb, corner, b.iy, -b.ix); // interior is right of B's left edge
  }
}

} // namespace

SubMesh build_junction_surface(const RoadNetwork& network,
                               const Junction& junction,
                               const SamplingOptions& sampling) {
  // 1. Gather footprints, exact border rings, and centerline samples.
  Clipper2Lib::PathsD footprints;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
  double z_sum = 0.0;
  std::size_t z_count = 0;
  for (const RoadId road_id : connecting_roads(junction)) {
    const Road* road = network.road(road_id);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    RoadContribution contribution = build_contribution(network, *road, sampling);
    // The ring is built left-border-forward + right-border-reversed, which
    // winds CLOCKWISE — fine for NonZero union, but InflatePaths would
    // erode it (hole semantics). The weld inflation needs CCW.
    if (Clipper2Lib::Area(contribution.footprint) < 0.0) {
      std::ranges::reverse(contribution.footprint);
    }
    footprints.push_back(std::move(contribution.footprint));
    for (const Vec3& p : contribution.border) {
      z_sum += p.z;
      ++z_count;
    }
    border.insert(border.end(), contribution.border.begin(), contribution.border.end());
    centerline.insert(
        centerline.end(), contribution.centerline.begin(), contribution.centerline.end());
  }

  // 1b. Joint quads (03 §1): each arm's full end cross-section, extruded
  // kJointDepth into the junction, so the union reaches every arm face —
  // turn footprints alone leave the mouth corners (shoulders, obtuse-angle
  // wedges) uncovered. The cross-section vertices join the border ring: they
  // are the road mesh's exact end-station vertices (Dirichlet + snap data).
  std::vector<Vec3> face_vertices;
  Clipper2Lib::PathsD face_cuts;
  // Joint quads only for junctions whose arm list persists (≥ 2 arms —
  // the writer's rm:arms rule): a degenerate or foreign junction must mesh
  // identically before and after save/load (round-trip byte identity).
  const std::span<const RoadEnd> arms = junction.arms.size() >= 2
                                            ? std::span<const RoadEnd>(junction.arms)
                                            : std::span<const RoadEnd>();
  for (const RoadEnd& arm : arms) {
    const Road* road = network.road(arm.road);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    const double station = arm.contact == ContactPoint::Start ? 0.0 : road->plan_view.length();
    const StationFrame frame = make_frame(*road, station);
    const LaneSection& section = section_at(network, *road, station);
    const std::vector<double> offsets = boundary_offsets(network, *road, section, station);
    const double sign = arm.contact == ContactPoint::Start ? -1.0 : 1.0;
    const double ix = sign * frame.cos_h; // INTO the junction
    const double iy = sign * frame.sin_h;
    const auto left = lateral_point(frame, offsets.front());
    const auto right = lateral_point(frame, offsets.back());
    // Winding matters: InflatePaths ERODES clockwise paths (hole semantics)
    // and NonZero clipping ignores them — a Start-contact arm's quad comes
    // out CW from the same construction order, so force CCW explicitly.
    const auto push_ccw = [](Clipper2Lib::PathsD& into_paths, Clipper2Lib::PathD path) {
      if (Clipper2Lib::Area(path) < 0.0) {
        std::ranges::reverse(path);
      }
      into_paths.push_back(std::move(path));
    };
    // Joint quad: face left → face right → extruded right → extruded left.
    Clipper2Lib::PathD quad;
    quad.emplace_back(left[0], left[1]);
    quad.emplace_back(right[0], right[1]);
    quad.emplace_back(right[0] + (kJointDepth * ix), right[1] + (kJointDepth * iy));
    quad.emplace_back(left[0] + (kJointDepth * ix), left[1] + (kJointDepth * iy));
    push_ccw(footprints, std::move(quad));
    // Face-cut rectangle over the ROAD side of the face line — removes the
    // weld inflation's overhang so the floor never overlaps the arm's mesh.
    const double lx = left[0] - (kFaceCutMargin * frame.sin_h);
    const double ly = left[1] + (kFaceCutMargin * frame.cos_h);
    const double rx = right[0] + (kFaceCutMargin * frame.sin_h);
    const double ry = right[1] - (kFaceCutMargin * frame.cos_h);
    Clipper2Lib::PathD cut;
    cut.emplace_back(lx, ly);
    cut.emplace_back(rx, ry);
    cut.emplace_back(rx - (kFaceCutDepth * ix), ry - (kFaceCutDepth * iy));
    cut.emplace_back(lx - (kFaceCutDepth * ix), ly - (kFaceCutDepth * iy));
    push_ccw(face_cuts, std::move(cut));
    for (const double offset : offsets) {
      const auto p = lateral_point(frame, offset);
      border.push_back({p[0], p[1], p[2]});
      face_vertices.push_back({p[0], p[1], p[2]});
      z_sum += p[2];
      ++z_count;
    }
  }
  append_corner_fillets(network, junction, corner_faces(network, junction), footprints);
  if (footprints.empty()) {
    return {};
  }

  // 2. Weld-inflate ALL footprints (ribbons and joint quads together, so
  //    coincident internal borders inflate onto the same line instead of
  //    1 cm-parallel channels) → union → face cut-back to the exact arm
  //    cross-section lines → simplify. See the constants above.
  const Clipper2Lib::PathsD inflated = Clipper2Lib::InflatePaths(footprints,
                                                                 kFootprintWeld,
                                                                 Clipper2Lib::JoinType::Round,
                                                                 Clipper2Lib::EndType::Polygon,
                                                                 2.0,
                                                                 kUnionPrecision);
  Clipper2Lib::PathsD merged =
      Clipper2Lib::Union(inflated, Clipper2Lib::FillRule::NonZero, kUnionPrecision);
  // Deflate back: with the union this completes a morphological CLOSING —
  // tangent-seam channels stay welded shut, but the outer boundary returns
  // to the exact pavement edge instead of a 1 cm apron. The apron's
  // alternating exact/proud vertices were a visible sawtooth on the junction
  // silhouette (tee visual finding, follow-up to issue #103).
  merged = Clipper2Lib::InflatePaths(merged,
                                     -kFootprintWeld,
                                     Clipper2Lib::JoinType::Round,
                                     Clipper2Lib::EndType::Polygon,
                                     2.0,
                                     kUnionPrecision);
  merged = Clipper2Lib::Union(merged, Clipper2Lib::FillRule::NonZero, kUnionPrecision);
  if (!face_cuts.empty()) {
    merged =
        Clipper2Lib::Difference(merged, face_cuts, Clipper2Lib::FillRule::NonZero, kUnionPrecision);
  }
  merged = Clipper2Lib::SimplifyPaths(merged, kBoundarySimplify);
  // A junction's pavement is simply-connected: any hole in the union is an
  // artifact of footprints meeting without overlapping (e.g. the channel
  // between a wide-swinging turn ribbon's inner edge and the corner
  // corridor) and must be paved over, not triangulated around.
  std::erase_if(merged,
                [](const Clipper2Lib::PathD& path) { return Clipper2Lib::Area(path) < 0.0; });
  if (merged.empty()) {
    return {};
  }
  // Merge sub-0.2 m boundary segments (arc-crossing debris where footprint
  // curves meet at grazing angles): a chord that short can only pair with
  // the 1 m-eroded interior into a sub-5-degree sliver. Vertices near a
  // road border sample are stitch targets and always survive.
  {
    constexpr double kMinBoundarySegment = 0.2;
    const auto near_border = [&border](const Clipper2Lib::PointD& p) {
      for (const Vec3& b : border) {
        if (std::hypot(b.x - p.x, b.y - p.y) < 0.1) {
          return true;
        }
      }
      return false;
    };
    for (Clipper2Lib::PathD& path : merged) {
      Clipper2Lib::PathD kept;
      kept.reserve(path.size());
      for (const Clipper2Lib::PointD& p : path) {
        if (!kept.empty() &&
            std::hypot(p.x - kept.back().x, p.y - kept.back().y) < kMinBoundarySegment &&
            !near_border(p)) {
          continue;
        }
        kept.push_back(p);
      }
      // The ring wraps: the last kept vertex may crowd the first.
      while (kept.size() > 3 &&
             std::hypot(kept.back().x - kept.front().x, kept.back().y - kept.front().y) <
                 kMinBoundarySegment &&
             !near_border(kept.back())) {
        kept.pop_back();
      }
      if (kept.size() >= 3) {
        path = std::move(kept);
      }
    }
  }
  double area = 0.0;
  for (const Clipper2Lib::PathD& path : merged) {
    area += Clipper2Lib::Area(path);
  }
  const double mean_z = z_count > 0 ? z_sum / static_cast<double>(z_count) : 0.0;

  // 3. Constrained Delaunay of the boundary, with interior Steiner refinement
  //    (skipped for the flat-floor fallback — nothing to bend).
  const bool flat_floor = std::abs(area) < kFlatFloorMinArea;

  std::vector<CDT::V2d<double>> vertices;
  std::vector<CDT::Edge> edges;
  double min_x = std::numeric_limits<double>::max(), min_y = min_x;
  double max_x = std::numeric_limits<double>::lowest(), max_y = max_x;
  subdivide_rings_to_cdt(merged, vertices, edges, min_x, min_y, max_x, max_y);
  // Arm-face vertices (lane boundaries along each joint edge) become CDT
  // vertices so the floor's boundary carries the road mesh's exact
  // end-cross-section vertices — no T-vertices on joint seams (03 §5). This
  // extra injection is junction-only, so it stays caller-side (appended before
  // RemoveDuplicatesAndRemapEdges) rather than inside the shared backend.
  for (const Vec3& p : face_vertices) {
    vertices.push_back(CDT::V2d<double>{p.x, p.y});
  }
  CDT::RemoveDuplicatesAndRemapEdges(vertices, edges);

  if (!flat_floor) {
    steiner_grid_fill(merged, vertices, min_x, min_y, max_x, max_y);
  }

  std::optional<CompactMesh> mesh_opt = triangulate_region(vertices, edges);
  if (!mesh_opt) {
    return {};
  }
  CompactMesh mesh = std::move(*mesh_opt);

  // 4. Watertight stitch BEFORE elevation: snap each boundary vertex onto the
  //    exact road border vertex it approximates (bitwise-equal doubles, §5) —
  //    kSeamSnap welds the 1 cm apron corners back onto the exact face corners
  //    — then cluster-weld sub-feature debris and drop the collapsed triangles.
  stitch_and_weld(mesh, border);
  if (mesh.triangles.empty()) {
    return {};
  }

  // 5. Elevation: Dirichlet boundary z from the nearest road border (snapped
  //    vertices already carry the exact z); harmonic interior (or flat floor
  //    for tiny footprints).
  assign_boundary_elevation_and_solve(mesh, flat_floor, mean_z, border, centerline);

  return emit(mesh, fmt::format("junction {} surface", junction.odr_id));
}

} // namespace roadmaker
