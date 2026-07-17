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
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using namespace fill_backend;

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

// Corner fillets (tee visual finding, follow-up to issue #103): at every
// re-entrant corner between two adjacent arms the pavement edge follows an
// arc tangent to both arms' outer edge lines — the strongest visual
// signature of a real intersection. Radius floor below; the desired radius
// derives from the corner turn's connecting road (its centerline min
// radius approximates the inner-edge geometry) and is clamped to what the
// arm faces leave room for.
constexpr double kFilletRadiusFloor = 3.0;

// Cap on the derived fillet radius: past this the corner reads as a slip
// lane, and long tangent legs start colliding with neighboring corners.
constexpr double kFilletRadiusCap = 15.0;

// Fillet arcs are sampled to this sagitta [m]. Must stay above
// kBoundarySimplify or SimplifyPaths would flatten the arc back into a
// chord after the union.
constexpr double kFilletArcSagitta = 8e-3;

// Arc samples closer than this [m] to either tangent edge line are dropped:
// a sample millimeters off the line it is tangent to pairs with the edge
// into a sub-degree CDT sliver. The boundary leaves the exact tangency
// point with a <= 8 cm chord shortcut instead — invisible, and the first
// departure angle stays above the mesh's 5-degree sliver gate.
constexpr double kFilletTangentLift = 0.08;

// Width [m] of the corridor strips laid along each arm's outer edge lines
// between the face corner and the fillet tangency: turn ribbons cover only
// their own lanes, so without the strips the pavement between a wide arm's
// shoulder edge and the ribbon keeps uncovered notches (the tee's 1x2 m
// mouth step, issue #103 round 2). Wide enough to overlap every ribbon,
// narrow enough to never cross an arm's opposite edge (arm half-widths are
// >= 1.75 m for any drivable profile).
constexpr double kEdgeStripWidth = 1.0;

/// Connecting roads of this junction, in connection order, de-duplicated.
std::vector<RoadId> connecting_roads(const Junction& junction) {
  std::vector<RoadId> roads;
  for (const JunctionConnection& conn : junction.connections) {
    if (std::ranges::find(roads, conn.connecting_road) == roads.end()) {
      roads.push_back(conn.connecting_road);
    }
  }
  return roads;
}

/// One arm's face data for fillet construction, oriented INTO the junction:
/// `left`/`right` are the outermost pavement corners as seen entering, and
/// (ix, iy) the unit into-junction direction.
struct ArmFace {
  std::array<double, 2> left;
  std::array<double, 2> right;
  double ix = 0.0;
  double iy = 0.0;
};

/// Minimum plan-view curvature radius of a connecting road, sampled — the
/// fillet-radius derivation treats it as the corner turn's inner-edge scale.
double min_plan_radius(const Road& road) {
  constexpr int kSamples = 16;
  const double length = road.plan_view.length();
  if (length <= tol::kLength) {
    return std::numeric_limits<double>::max();
  }
  double max_kappa = 0.0;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = length * static_cast<double>(i) / kSamples;
    max_kappa = std::max(max_kappa, std::abs(road.plan_view.evaluate(s).curvature));
  }
  return max_kappa > tol::kLength ? 1.0 / max_kappa : std::numeric_limits<double>::max();
}

double point_segment_distance(const std::array<double, 2>& p,
                              const std::array<double, 2>& a,
                              const std::array<double, 2>& b) {
  const double abx = b[0] - a[0];
  const double aby = b[1] - a[1];
  const double len2 = (abx * abx) + (aby * aby);
  double t = 0.0;
  if (len2 > 0.0) {
    t = std::clamp((((p[0] - a[0]) * abx) + ((p[1] - a[1]) * aby)) / len2, 0.0, 1.0);
  }
  const double dx = p[0] - (a[0] + (t * abx));
  const double dy = p[1] - (a[1] + (t * aby));
  return std::hypot(dx, dy);
}

/// Fillet radius for the corner between faces `a` and `b`: the smallest
/// plan-view radius among connecting roads that run face-to-face across this
/// corner (the turn whose inner edge shapes it), floored and capped.
double corner_fillet_radius(const RoadNetwork& network,
                            const Junction& junction,
                            const ArmFace& a,
                            const ArmFace& b) {
  constexpr double kFaceMatch = 1.5; // endpoint-to-face tolerance [m]
  double derived = std::numeric_limits<double>::max();
  for (const RoadId road_id : connecting_roads(junction)) {
    const Road* road = network.road(road_id);
    if (road == nullptr || road->plan_view.empty()) {
      continue;
    }
    const PathPoint start = road->plan_view.evaluate(0.0);
    const PathPoint end = road->plan_view.evaluate(road->plan_view.length());
    const std::array<double, 2> p0{start.x, start.y};
    const std::array<double, 2> p1{end.x, end.y};
    const bool a_to_b = point_segment_distance(p0, a.left, a.right) < kFaceMatch &&
                        point_segment_distance(p1, b.left, b.right) < kFaceMatch;
    const bool b_to_a = point_segment_distance(p0, b.left, b.right) < kFaceMatch &&
                        point_segment_distance(p1, a.left, a.right) < kFaceMatch;
    if (a_to_b || b_to_a) {
      derived = std::min(derived, min_plan_radius(*road));
    }
  }
  if (derived == std::numeric_limits<double>::max()) {
    return kFilletRadiusFloor;
  }
  return std::clamp(derived, kFilletRadiusFloor, kFilletRadiusCap);
}

/// Appends the corner fillet wedges and edge corridor strips for all
/// angularly adjacent arm pairs. The wedge fills the re-entrant corner up to
/// an arc tangent to both arms' edge lines (G1 boundary); the strips pin the
/// boundary to the exact pavement edge lines between each face corner and
/// its tangency.
void append_corner_fillets(const RoadNetwork& network,
                           const Junction& junction,
                           std::vector<ArmFace> faces,
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

  // Cyclic CCW order around the shared centroid; the corner between
  // consecutive arms (A, B) is A's right edge meeting B's left edge.
  double cx = 0.0, cy = 0.0;
  for (const ArmFace& f : faces) {
    cx += (f.left[0] + f.right[0]) / 2.0;
    cy += (f.left[1] + f.right[1]) / 2.0;
  }
  cx /= static_cast<double>(faces.size());
  cy /= static_cast<double>(faces.size());
  std::ranges::sort(faces, [&](const ArmFace& fa, const ArmFace& fb) {
    const double mid_ax = ((fa.left[0] + fa.right[0]) / 2.0) - cx;
    const double mid_ay = ((fa.left[1] + fa.right[1]) / 2.0) - cy;
    const double mid_bx = ((fb.left[0] + fb.right[0]) / 2.0) - cx;
    const double mid_by = ((fb.left[1] + fb.right[1]) / 2.0) - cy;
    return std::atan2(mid_ay, mid_ax) < std::atan2(mid_by, mid_bx);
  });

  for (std::size_t i = 0; i < faces.size(); ++i) {
    const ArmFace& a = faces[i];
    const ArmFace& b = faces[(i + 1) % faces.size()];
    const std::array<double, 2> pa = a.right; // A's edge facing B
    const std::array<double, 2> pb = b.left;  // B's edge facing A
    const double cross = (a.ix * b.iy) - (a.iy * b.ix);
    if (std::abs(cross) < 1e-6) {
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
    // Edge-line intersection pa + ta·A_dir = pb + tb·B_dir.
    const double dx = pb[0] - pa[0];
    const double dy = pb[1] - pa[1];
    const double ta = ((dx * b.iy) - (dy * b.ix)) / cross;
    const double tb = ((dx * a.iy) - (dy * a.ix)) / cross;
    if (ta <= tol::kLength || tb <= tol::kLength) {
      continue; // corner behind a face — degenerate arm layout
    }
    const std::array<double, 2> corner{pa[0] + (ta * a.ix), pa[1] + (ta * a.iy)};
    const double cos_phi = std::clamp((a.ix * b.ix) + (a.iy * b.iy), -1.0, 1.0);
    const double phi = std::acos(cos_phi); // angle between the edge rays at the corner
    if (phi < 0.1 || phi > std::numbers::pi - 0.1) {
      continue; // near-tangent arms; a fillet would degenerate
    }
    const double tan_half = std::tan(phi / 2.0);
    // Desired radius, clamped to the tangent legs the faces leave room for.
    const double radius =
        std::min(corner_fillet_radius(network, junction, a, b), std::min(ta, tb) * tan_half);
    if (radius < 0.05) {
      continue;
    }
    const double tangent_len = radius / tan_half;
    const std::array<double, 2> tan_a{corner[0] - (tangent_len * a.ix),
                                      corner[1] - (tangent_len * a.iy)};
    const std::array<double, 2> tan_b{corner[0] - (tangent_len * b.ix),
                                      corner[1] - (tangent_len * b.iy)};
    // Arc center: along the inward bisector at R/sin(phi/2) from the corner.
    double bis_x = -(a.ix + b.ix);
    double bis_y = -(a.iy + b.iy);
    const double bis_len = std::hypot(bis_x, bis_y);
    if (bis_len < tol::kLength) {
      continue;
    }
    bis_x /= bis_len;
    bis_y /= bis_len;
    const double center_dist = radius / std::sin(phi / 2.0);
    const std::array<double, 2> center{corner[0] + (bis_x * center_dist),
                                       corner[1] + (bis_y * center_dist)};

    // Wedge: corner -> tangency on A -> arc -> tangency on B.
    Clipper2Lib::PathD wedge;
    wedge.emplace_back(corner[0], corner[1]);
    const double ang_a = std::atan2(tan_a[1] - center[1], tan_a[0] - center[0]);
    double ang_b = std::atan2(tan_b[1] - center[1], tan_b[0] - center[0]);
    // Sweep the SHORT way (the fillet arc spans pi - phi < pi).
    while (ang_b - ang_a > std::numbers::pi) {
      ang_b -= 2.0 * std::numbers::pi;
    }
    while (ang_a - ang_b > std::numbers::pi) {
      ang_b += 2.0 * std::numbers::pi;
    }
    const double sweep = ang_b - ang_a;
    const double step_angle =
        2.0 * std::acos(std::clamp(1.0 - (kFilletArcSagitta / radius), 0.0, 1.0));
    const int steps =
        std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / std::max(step_angle, 1e-3))));
    const auto line_distance = [](const std::array<double, 2>& p,
                                  const std::array<double, 2>& on,
                                  double dx2,
                                  double dy2) {
      return std::abs(((p[0] - on[0]) * dy2) - ((p[1] - on[1]) * dx2));
    };
    for (int k = 0; k <= steps; ++k) {
      const double ang = ang_a + (sweep * static_cast<double>(k) / steps);
      const std::array<double, 2> p{center[0] + (radius * std::cos(ang)),
                                    center[1] + (radius * std::sin(ang))};
      // Endpoints are the exact tangencies; interior samples hugging either
      // tangent line are dropped (see kFilletTangentLift).
      if (k != 0 && k != steps &&
          (line_distance(p, tan_a, a.ix, a.iy) < kFilletTangentLift ||
           line_distance(p, tan_b, b.ix, b.iy) < kFilletTangentLift)) {
        continue;
      }
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
  std::vector<ArmFace> faces;
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
    // Face corners oriented into the junction: a Start-contact arm's road
    // frame is flipped relative to the entering direction, so its road-left
    // corner is the entering-right one.
    const bool flipped = arm.contact == ContactPoint::Start;
    faces.push_back(ArmFace{
        .left = flipped ? std::array<double, 2>{right[0], right[1]}
                        : std::array<double, 2>{left[0], left[1]},
        .right = flipped ? std::array<double, 2>{left[0], left[1]}
                         : std::array<double, 2>{right[0], right[1]},
        .ix = ix,
        .iy = iy,
    });
  }
  append_corner_fillets(network, junction, faces, footprints);
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
