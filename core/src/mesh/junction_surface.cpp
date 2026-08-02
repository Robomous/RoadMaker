/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "junction_surface.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
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
#include <string>
#include <utility>
#include <vector>

#include "fill_backend.hpp"
#include "junction_corner_detail.hpp"
#include "junction_fill_spans.hpp"
#include "junction_sidewalk_bands.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using namespace fill_backend;

using junction_corner_detail::arm_face;
using junction_corner_detail::ArmFace;
using junction_corner_detail::connecting_roads;
using junction_corner_detail::corner_curve;
using junction_corner_detail::corner_faces;
using junction_corner_detail::corner_override;
using junction_corner_detail::CornerFace;
using junction_corner_detail::CornerSolution;
using junction_corner_detail::solve_corner;

using junction_fill_spans::collect_fill_spans;
using junction_fill_spans::JunctionFillSpan;

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
    if (solution.through_edge) {
      // One pavement edge curving through the junction: pave the corridor the
      // edge itself describes, sampled at the fillet sagitta, rather than the
      // straight-ray corner it has no business having (#356).
      const std::array<double, 2>& c = solution.arc_center;
      const double r = std::abs(solution.arc_radius);
      const double ang_a = std::atan2(pa[1] - c[1], pa[0] - c[0]);
      double ang_b = std::atan2(pb[1] - c[1], pb[0] - c[0]);
      while (ang_b - ang_a > std::numbers::pi) {
        ang_b -= 2.0 * std::numbers::pi;
      }
      while (ang_a - ang_b > std::numbers::pi) {
        ang_b += 2.0 * std::numbers::pi;
      }
      const double sweep = ang_b - ang_a;
      const double step =
          2.0 *
          std::acos(std::clamp(1.0 - (junction_corner_detail::kFilletArcSagitta / r), 0.0, 1.0));
      const int steps =
          std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / std::max(step, 1e-3))));
      // One strip width INWARD, exactly as the straight strips run. Inward is
      // toward the arc centre only for a convex through-arm; for a concave one
      // (the arms meeting on the inside of the curve) the pavement is on the
      // far side, so the strip has to widen the radius instead. The sign of
      // `arc_radius` carries that.
      const double r_in = std::max(r - std::copysign(kEdgeStripWidth, solution.arc_radius), 0.0);
      Clipper2Lib::PathD corridor;
      for (int k = 0; k <= steps; ++k) {
        const double ang = ang_a + (sweep * static_cast<double>(k) / steps);
        corridor.emplace_back(c[0] + (r * std::cos(ang)), c[1] + (r * std::sin(ang)));
      }
      for (int k = steps; k >= 0; --k) {
        const double ang = ang_a + (sweep * static_cast<double>(k) / steps);
        corridor.emplace_back(c[0] + (r_in * std::cos(ang)), c[1] + (r_in * std::sin(ang)));
      }
      push_ccw(std::move(corridor));
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

// --- Authored corner overlays (p4-s2, issue #226) ----------------------------

// Height [m] every authored overlay floats above the floor it covers. Large
// enough that no depth buffer confuses the two, small enough to read as flush
// pavement detail rather than a curb.
constexpr double kJunctionDetailLift = 0.01;

// Depth [m] a median nose reaches into the junction from its arm's face — the
// painted/raised nose that keeps opposing traffic apart at the mouth.
constexpr double kMedianNoseDepth = 2.0;

/// Appends one flat-shaded triangle (its own three vertices, one face normal
/// forced into the +Z hemisphere so the overlay is never backfaced).
void push_triangle(SubMesh& sub,
                   const std::array<double, 3>& a,
                   const std::array<double, 3>& b,
                   const std::array<double, 3>& c) {
  const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
  const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
  double nx = (uy * vz) - (uz * vy);
  double ny = (uz * vx) - (ux * vz);
  double nz = (ux * vy) - (uy * vx);
  const double len = std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
  if (len < tol::kLength) {
    return; // degenerate sliver
  }
  nx /= len;
  ny /= len;
  nz /= len;
  if (nz < 0.0) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  }
  const auto base = static_cast<std::uint32_t>(sub.positions.size() / 3);
  for (const std::array<double, 3>& p : {a, b, c}) {
    sub.positions.insert(sub.positions.end(), {p[0], p[1], p[2]});
    sub.normals.insert(sub.normals.end(), {nx, ny, nz});
  }
  sub.indices.insert(sub.indices.end(), {base, base + 1, base + 2});
}

/// Signed plan area of a closed ring (positive = CCW).
double ring_area(const std::vector<std::array<double, 3>>& ring) {
  double area = 0.0;
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const std::array<double, 3>& p = ring[i];
    const std::array<double, 3>& q = ring[(i + 1) % ring.size()];
    area += (p[0] * q[1]) - (q[0] * p[1]);
  }
  return 0.5 * area;
}

/// Fan-triangulates `ring` from vertex 0. Only valid for rings star-shaped
/// about that apex — the corner wedge (apex = the edge-line intersection) and
/// the median quads both are.
void fan_triangulate(SubMesh& sub, std::vector<std::array<double, 3>> ring) {
  if (ring.size() < 3) {
    return;
  }
  if (ring_area(ring) < 0.0) {
    // Reversing the tail is the same cyclic ring wound the other way, with the
    // apex still first — which the fan below requires.
    std::reverse(ring.begin() + 1, ring.end());
  }
  for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
    push_triangle(sub, ring[0], ring[i], ring[i + 1]);
  }
}

/// The median material governing `arm`'s nose. An arm belongs to two corners:
/// the one it enters as `arm_a` wins, the one it enters as `arm_b` is the
/// fallback, and an arm neither corner paints gets no nose at all.
const std::string* median_material_for_arm(const Junction& junction, const RoadEnd& arm) {
  for (const JunctionCorner& entry : junction.corners) {
    if (entry.arm_a == arm && entry.median_material.has_value()) {
      return &*entry.median_material;
    }
  }
  for (const JunctionCorner& entry : junction.corners) {
    if (entry.arm_b == arm && entry.median_material.has_value()) {
      return &*entry.median_material;
    }
  }
  return nullptr;
}

// --- authored span controls (p4-s5, issue #320) ------------------------------
//
// Everything below is JUNCTION-LOCAL on purpose: fill_backend.hpp is shared
// bit-for-bit with the P2 ground-surface fill and must not learn about
// priorities. It runs only when a span carries an authored control; at all
// defaults build_junction_surface takes the verbatim legacy path, so bit
// identity for every pre-existing network is structural rather than numeric.

/// One INCLUDED span's data for the overlap arbitration: the RAW (pre-inflate)
/// footprint whose overlaps the sort index arbitrates, and the border samples
/// that supply the elevation where it wins.
struct SpanPriority {
  const Clipper2Lib::PathD* footprint;
  const std::vector<Vec3>* border;
  int sort_index;
};

/// The highest sort index among included spans whose raw footprint contains
/// (px, py) — the rank a span must match to have a say there. Nullopt where no
/// ribbon covers the point at all (the joint quads, corner fillets and edge
/// strips pave ground no connecting road claims), which lets every included
/// span speak.
///
/// `fill_backend::inside_path`, never `Clipper2Lib::PointInPolygon` — the
/// latter truncates PathD edge deltas to integers and is wrong below 1 m
/// (#442). A ribbon footprint is meter-scale so this misfired less often than
/// the Steiner gate did, but by the identical mechanism.
std::optional<int> max_sort_at(const std::vector<SpanPriority>& priorities, double px, double py) {
  std::optional<int> best;
  const Clipper2Lib::PointD probe{px, py};
  for (const SpanPriority& entry : priorities) {
    if (fill_backend::inside_path(*entry.footprint, probe)) {
      best = best.has_value() ? std::max(*best, entry.sort_index) : entry.sort_index;
    }
  }
  return best;
}

/// Dirichlet source for one floor boundary vertex under authored controls.
///
/// "Higher wins": only spans whose sort index reaches the highest one covering
/// the point may supply its elevation. Two things outrank that rule, both for
/// watertightness — a vertex EXACTLY coincident with any sample takes that
/// sample's z even if the span was excluded or outranked (test_junction_surface
/// check 4 compares exactly those xy-coincident floor/road pairs), and arm-face
/// samples are always candidates, since a seam vertex must keep the arm road's
/// own z whatever the interior spans say.
double nearest_border_z_prioritized(const std::vector<SpanPriority>& priorities,
                                    const std::vector<Vec3>& faces,
                                    const std::vector<Vec3>& every_sample,
                                    double px,
                                    double py) {
  for (const Vec3& sample : every_sample) {
    const double dx = sample.x - px;
    const double dy = sample.y - py;
    if ((dx * dx) + (dy * dy) < 1e-12) {
      return sample.z;
    }
  }
  const std::optional<int> rank = max_sort_at(priorities, px, py);
  double best = std::numeric_limits<double>::max();
  double z = 0.0;
  const auto scan = [&best, &z, px, py](const Vec3& sample) {
    const double d = ((sample.x - px) * (sample.x - px)) + ((sample.y - py) * (sample.y - py));
    if (d < best) {
      best = d;
      z = sample.z;
    }
  };
  for (const SpanPriority& entry : priorities) {
    if (rank.has_value() && entry.sort_index < *rank) {
      continue;
    }
    for (const Vec3& sample : *entry.border) {
      scan(sample);
    }
  }
  for (const Vec3& sample : faces) {
    scan(sample);
  }
  return z;
}

/// fill_backend::assign_boundary_elevation_and_solve with the prioritized
/// Dirichlet source substituted. The harmonic solve itself is the shared one.
void assign_prioritized_elevation(CompactMesh& mesh,
                                  bool flat_floor,
                                  double mean_z,
                                  const std::vector<SpanPriority>& priorities,
                                  const std::vector<Vec3>& faces,
                                  const std::vector<Vec3>& every_sample,
                                  const std::vector<Vec3>& centerline) {
  const std::vector<bool> on_boundary = boundary_flags(mesh);
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    if (flat_floor) {
      mesh.vertices[i].z = mean_z;
    } else if (on_boundary[i]) {
      mesh.vertices[i].z = nearest_border_z_prioritized(
          priorities, faces, every_sample, mesh.vertices[i].x, mesh.vertices[i].y);
    }
  }
  if (!flat_floor) {
    solve_elevation(mesh, on_boundary, centerline);
  }
}

// --- sidewalk band split (p4-s3 follow-up, issue #357) -----------------------
//
// A junction of sidewalked roads must carry the sidewalk band continuously
// around each corner and stamp it with LaneType::Sidewalk, instead of paving
// the whole floor as one Driving slab. The floor union already reaches the
// sidewalk OUTER edge at every arm mouth (the joint quads span the full cross
// section) and rounds the corners (append_corner_fillets), so the sidewalk
// pavement is already IN the floor — it just needs to be segmented out by
// material. We do that by classifying the floor's finished triangles against a
// per-corner band polygon, so the split shares the floor's exact vertices
// (watertight) and never re-triangulates (deterministic, byte-stable).

// ---------------------------------------------------------------------------
// Sidewalk bands (issues #357, #402)
//
// A junction of sidewalked roads must carry the sidewalk band continuously
// around each corner and stamp it with LaneType::Sidewalk, instead of paving
// the whole floor as one Driving slab. The floor union already reaches the
// sidewalk OUTER edge at every arm mouth and rounds the corners, so the
// sidewalk pavement is already IN the floor — it only needs segmenting.
//
// The band geometry itself lives in junction_sidewalk_bands.cpp and is shared
// with the public query, so the seam a test measures is the seam the mesher
// builds. Here we do the two things that need the floor: clip each band to the
// footprint union (a tight junction gets the band it has room for rather than
// none), and CONSTRAIN the triangulation to the seam. That constraint is what
// makes the split exact: classifying finished triangles against an
// unconstrained triangulation is wrong by one triangle, and at a kSteinerStep
// of 2 m against a 1.8 m sidewalk one triangle is the whole band (#402).
// ---------------------------------------------------------------------------

/// One corner's band as the floor pipeline needs it: the region to classify
/// against, and the seam polylines to constrain the triangulation to. Both are
/// clipped to the floor, so both are empty where the floor has no room.
struct BandDef {
  Clipper2Lib::PathsD region;
  std::vector<Clipper2Lib::PathD> seams;
  std::string surface;
};

/// Distance from `pt` to the outline of `paths` — used to tell an edge that
/// merely COINCIDES with the floor boundary from one that runs through the
/// floor's interior.
double distance_to_outline(const Clipper2Lib::PathsD& paths, const Clipper2Lib::PointD& pt) {
  double best = std::numeric_limits<double>::max();
  for (const Clipper2Lib::PathD& path : paths) {
    for (std::size_t i = 0, j = path.size() - 1; i < path.size(); j = i++) {
      const Clipper2Lib::PointD& a = path[j];
      const Clipper2Lib::PointD& b = path[i];
      const double ex = b.x - a.x;
      const double ey = b.y - a.y;
      const double len2 = (ex * ex) + (ey * ey);
      double t = 0.0;
      if (len2 > 1e-18) {
        t = std::clamp((((pt.x - a.x) * ex) + ((pt.y - a.y) * ey)) / len2, 0.0, 1.0);
      }
      best = std::min(best, std::hypot(pt.x - (a.x + (t * ex)), pt.y - (a.y + (t * ey))));
    }
  }
  return best;
}

/// Clips one ideal band to the floor union and works out which parts of the
/// clipped outline the triangulation has to be constrained to.
///
/// Only the INTERIOR parts: an outline edge that coincides with the floor's own
/// boundary (the arm faces, the curb line where the sidewalk is the outermost
/// lane) is already a constraint of the triangulation, while an edge running
/// through the floor's interior — the seam against the carriageway, the far cap
/// of a one-sided stub, the curb side of a band that has a shoulder outboard of
/// it — is what triangles would otherwise straddle.
///
/// Each interior run is an OPEN chord with both ends on the floor boundary,
/// never a closed ring. That distinction is load-bearing: CDT's
/// `eraseOuterTrianglesAndHoles` classifies by crossing depth, so a closed
/// interior loop would be read as a HOLE and erased, punching the band out of
/// the floor. A band whose outline is interior all the way round would be
/// exactly that, so it is left unconstrained rather than risked.
BandDef clip_band(const JunctionSidewalkBand& band, const Clipper2Lib::PathsD& floor) {
  // A band edge within this distance of the floor boundary IS that boundary:
  // the union rounds to kUnionPrecision and the weld apron adds a centimeter.
  constexpr double kOnBoundary = 0.02;

  BandDef out;
  out.surface = band.surface;
  const Clipper2Lib::PathsD ring{band_ring(band)};
  out.region = Clipper2Lib::Intersect(ring, floor, Clipper2Lib::FillRule::NonZero, kUnionPrecision);

  for (const Clipper2Lib::PathD& path : out.region) {
    const std::size_t n = path.size();
    if (n < 3) {
      continue;
    }
    // interior[i] describes the edge from path[i] to path[i + 1].
    std::vector<bool> interior(n, false);
    std::size_t interior_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const Clipper2Lib::PointD& a = path[i];
      const Clipper2Lib::PointD& b = path[(i + 1) % n];
      const Clipper2Lib::PointD mid{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
      interior[i] = distance_to_outline(floor, mid) > kOnBoundary;
      interior_count += interior[i] ? 1 : 0;
    }
    if (interior_count == 0 || interior_count == n) {
      continue; // nothing to constrain, or a ring CDT would erase as a hole
    }
    // Walk from an edge the floor boundary already owns, so every run this
    // collects starts and ends on that boundary.
    std::size_t start = 0;
    while (interior[start]) {
      ++start;
    }
    Clipper2Lib::PathD run;
    for (std::size_t step = 0; step < n; ++step) {
      const std::size_t i = (start + step) % n;
      if (interior[i]) {
        if (run.empty()) {
          run.push_back(path[i]);
        }
        run.push_back(path[(i + 1) % n]);
      } else if (!run.empty()) {
        out.seams.push_back(std::move(run));
        run.clear();
      }
    }
    if (!run.empty()) {
      out.seams.push_back(std::move(run));
    }
  }
  return out;
}

/// Adds one interior run to the CDT input as a chain of constrained edges,
/// subdivided to
/// the Steiner step so the seam carries the same vertex density as the
/// boundary rings it meets (an unsubdivided long constraint would fan
/// sub-degree triangles across it, exactly as fill_backend's boundary comment
/// explains).
///
/// Each endpoint lands ON a boundary edge rather than on one of its vertices,
/// so the boundary edge is SPLIT there: the chord then terminates on a shared
/// vertex instead of crossing a constraint, which is what keeps the seam
/// exactly where the band says it is. Snapping to the nearest ring vertex
/// instead would drag the seam end by up to half a Steiner step.
void inject_seam_constraints(const Clipper2Lib::PathD& seam,
                             std::vector<CDT::V2d<double>>& vertices,
                             std::vector<CDT::Edge>& edges) {
  constexpr double kOnEdge = 1e-4; // 0.1 mm: Clipper2 rounds to kUnionPrecision

  const auto split_boundary_edge_at = [&vertices, &edges](const Clipper2Lib::PointD& p) {
    for (std::size_t e = 0; e < edges.size(); ++e) {
      const CDT::V2d<double>& va = vertices[edges[e].v1()];
      const CDT::V2d<double>& vb = vertices[edges[e].v2()];
      const double ex = vb.x - va.x;
      const double ey = vb.y - va.y;
      const double len2 = (ex * ex) + (ey * ey);
      if (len2 < kOnEdge * kOnEdge) {
        continue;
      }
      const double t = (((p.x - va.x) * ex) + ((p.y - va.y) * ey)) / len2;
      if (t <= 0.0 || t >= 1.0) {
        continue; // beyond the segment, or exactly on a vertex (nothing to split)
      }
      if (std::hypot(p.x - (va.x + (t * ex)), p.y - (va.y + (t * ey))) > kOnEdge) {
        continue;
      }
      const auto split = static_cast<CDT::VertInd>(vertices.size());
      vertices.push_back(CDT::V2d<double>{p.x, p.y});
      const CDT::VertInd far_end = edges[e].v2();
      edges[e] = CDT::Edge(edges[e].v1(), split);
      edges.emplace_back(split, far_end);
      return;
    }
  };

  split_boundary_edge_at(seam.front());
  split_boundary_edge_at(seam.back());

  std::vector<CDT::VertInd> chain;
  const auto push_point = [&vertices, &chain](double x, double y) {
    chain.push_back(static_cast<CDT::VertInd>(vertices.size()));
    vertices.push_back(CDT::V2d<double>{x, y});
  };
  for (std::size_t i = 0; i + 1 < seam.size(); ++i) {
    const Clipper2Lib::PointD& a = seam[i];
    const Clipper2Lib::PointD& b = seam[i + 1];
    push_point(a.x, a.y);
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    const int pieces = static_cast<int>(len / kSteinerStep);
    for (int k = 1; k <= pieces - 1; ++k) {
      const double f = static_cast<double>(k) / static_cast<double>(pieces);
      push_point(a.x + (f * (b.x - a.x)), a.y + (f * (b.y - a.y)));
    }
  }
  push_point(seam.back().x, seam.back().y);
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    edges.emplace_back(chain[i], chain[i + 1]);
  }
}

/// Emits the triangles of `floor` whose per-triangle `assignment` equals
/// `want` as a fresh SubMesh, copying the floor's exact positions AND normals
/// (so a seam vertex is bit-identical in every region it appears in) and
/// remapping to a dense index range in triangle order (deterministic).
SubMesh emit_floor_region(const SubMesh& floor,
                          const std::vector<int>& assignment,
                          int want,
                          LaneType material,
                          std::string surface,
                          std::string name) {
  SubMesh out;
  out.material = material;
  out.surface = std::move(surface);
  out.name = std::move(name);
  const std::size_t vertex_count = floor.positions.size() / 3;
  std::vector<std::uint32_t> remap(vertex_count, std::numeric_limits<std::uint32_t>::max());
  const std::size_t tri_count = floor.indices.size() / 3;
  for (std::size_t t = 0; t < tri_count; ++t) {
    if (assignment[t] != want) {
      continue;
    }
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t v = floor.indices[(t * 3) + k];
      if (remap[v] == std::numeric_limits<std::uint32_t>::max()) {
        remap[v] = static_cast<std::uint32_t>(out.positions.size() / 3);
        out.positions.insert(
            out.positions.end(),
            {floor.positions[v * 3], floor.positions[(v * 3) + 1], floor.positions[(v * 3) + 2]});
        out.normals.insert(
            out.normals.end(),
            {floor.normals[v * 3], floor.normals[(v * 3) + 1], floor.normals[(v * 3) + 2]});
      }
      out.indices.push_back(remap[v]);
    }
  }
  return out;
}

/// The floor plus the bands it was triangulated against — the two halves of
/// one pass, kept together so the classifier and the constraints can never
/// disagree about where a band is.
struct FloorBuild {
  SubMesh floor;
  std::vector<BandDef> bands;
};

FloorBuild
build_floor(const RoadNetwork& network, const Junction& junction, const SamplingOptions& sampling) {
  // 1. Gather footprints, exact border rings, and centerline samples.
  Clipper2Lib::PathsD footprints;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
  double z_sum = 0.0;
  std::size_t z_count = 0;
  // The per-road grouping (p4-s5, issue #320) lives in junction_fill_spans so
  // the public junction_surface_spans() query and the mesher share one
  // definition of a span; flattening here keeps the legacy input order exactly.
  const std::vector<JunctionFillSpan> spans = collect_fill_spans(network, junction, sampling);
  // Nothing authored on any span (every pre-p4-s5 network) ⇒ the verbatim
  // legacy path below, so bit identity at defaults is structural.
  const bool plain = std::ranges::none_of(spans, [](const JunctionFillSpan& entry) {
    return !entry.included || entry.sort_index != 0;
  });
  // `border` collects EVERY span's samples: they are the watertight stitch's
  // snap targets, and excluding a span must never move the pavement's seams.
  // `included_border` is the subset that still gets a say in the elevation and
  // still protects nearby boundary debris from the short-segment merge — the
  // two are the same vector whenever nothing is authored.
  std::vector<Vec3> included_border;
  for (const JunctionFillSpan& span : spans) {
    footprints.push_back(span.contribution.footprint);
    border.insert(border.end(), span.contribution.border.begin(), span.contribution.border.end());
    if (!span.included) {
      continue;
    }
    for (const Vec3& p : span.contribution.border) {
      z_sum += p.z;
      ++z_count;
    }
    included_border.insert(
        included_border.end(), span.contribution.border.begin(), span.contribution.border.end());
    centerline.insert(
        centerline.end(), span.contribution.centerline.begin(), span.contribution.centerline.end());
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
      // Arm faces are seams, never spans: they are always included and can
      // never be outranked, whatever the interior ribbons author.
      included_border.push_back({p[0], p[1], p[2]});
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
    // An EXCLUDED span stops protecting debris here: the arc crossings its
    // footprint causes may now merge away, which is exactly the triangulation
    // escape valve Include Samples is for.
    const auto near_border = [&included_border](const Clipper2Lib::PointD& p) {
      for (const Vec3& b : included_border) {
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

  // Sidewalk bands, clipped to this floor. Their seams become constrained
  // edges BEFORE the triangulation, which is what makes the material split
  // exact (#402). Nothing is injected when no arm carries a sidewalk, so a
  // rural junction triangulates exactly as it always did and its floor stays
  // byte-identical.
  std::vector<BandDef> bands;
  for (const JunctionSidewalkBand& ideal : junction_sidewalk_bands_of(network, junction)) {
    BandDef band = clip_band(ideal, merged);
    if (band.region.empty()) {
      continue;
    }
    for (const Clipper2Lib::PathD& seam : band.seams) {
      inject_seam_constraints(seam, vertices, edges);
    }
    bands.push_back(std::move(band));
  }

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
  if (plain) {
    assign_boundary_elevation_and_solve(mesh, flat_floor, mean_z, border, centerline);
  } else {
    // Overlap arbitration (p4-s5): included spans only, in connection order.
    std::vector<SpanPriority> priorities;
    priorities.reserve(spans.size());
    for (const JunctionFillSpan& span : spans) {
      if (span.included) {
        priorities.push_back(SpanPriority{.footprint = &span.contribution.footprint,
                                          .border = &span.contribution.border,
                                          .sort_index = span.sort_index});
      }
    }
    // A span's centerline pull applies only where it is not outranked —
    // otherwise a buried ribbon would still drag the interior toward its own
    // grade after the boundary handed the region to the winner.
    std::vector<Vec3> constrained;
    constrained.reserve(centerline.size());
    for (const JunctionFillSpan& span : spans) {
      if (!span.included) {
        continue;
      }
      for (const Vec3& sample : span.contribution.centerline) {
        const std::optional<int> rank = max_sort_at(priorities, sample.x, sample.y);
        if (!rank.has_value() || span.sort_index >= *rank) {
          constrained.push_back(sample);
        }
      }
    }
    assign_prioritized_elevation(
        mesh, flat_floor, mean_z, priorities, face_vertices, border, constrained);
  }

  SubMesh out = emit(mesh, fmt::format("junction {} surface", junction.odr_id));
  // Junction-wide carriageway material (p4-s2): empty means the derived
  // asphalt look, mirroring Surface::material.
  out.surface = junction.material;
  return FloorBuild{.floor = std::move(out), .bands = std::move(bands)};
}

} // namespace

SubMesh build_junction_surface(const RoadNetwork& network,
                               const Junction& junction,
                               const SamplingOptions& sampling) {
  return build_floor(network, junction, sampling).floor;
}

JunctionFloorSplit build_junction_floor_split(const RoadNetwork& network,
                                              const Junction& junction,
                                              const SamplingOptions& sampling) {
  JunctionFloorSplit result;
  FloorBuild built = build_floor(network, junction, sampling);
  const SubMesh& floor = built.floor;

  // No sidewalks anywhere (or nothing the floor had room for) => the floor is
  // returned VERBATIM, byte-identical to the pre-#357 single-material floor.
  // No constraint was injected in that case either, so the triangulation
  // itself is untouched: the feature never reaches a rural junction.
  if (built.bands.empty() || floor.indices.empty()) {
    result.carriageway = floor;
    return result;
  }

  // Classify every floor triangle by the band (if any) that contains its
  // centroid. -1 is the carriageway. The seams are constrained edges of this
  // triangulation, so no triangle straddles one and the centroid decides
  // exactly — this is a lookup now, not an approximation. Bands never overlap
  // (each corner owns one arm edge per side), so first match wins.
  const std::size_t tri_count = floor.indices.size() / 3;
  std::vector<int> assignment(tri_count, -1);
  bool any_sidewalk = false;
  for (std::size_t t = 0; t < tri_count; ++t) {
    const std::uint32_t i0 = floor.indices[t * 3];
    const std::uint32_t i1 = floor.indices[(t * 3) + 1];
    const std::uint32_t i2 = floor.indices[(t * 3) + 2];
    const Clipper2Lib::PointD centroid{
        (floor.positions[i0 * 3] + floor.positions[i1 * 3] + floor.positions[i2 * 3]) / 3.0,
        (floor.positions[(i0 * 3) + 1] + floor.positions[(i1 * 3) + 1] +
         floor.positions[(i2 * 3) + 1]) /
            3.0};
    for (std::size_t k = 0; k < built.bands.size(); ++k) {
      if (fill_backend::inside_region(built.bands[k].region, centroid)) {
        assignment[t] = static_cast<int>(k);
        any_sidewalk = true;
        break;
      }
    }
  }

  // Degenerate guard: if the band(s) somehow swallow the whole floor, keep the
  // floor whole rather than emit an empty carriageway (which the caller drops).
  std::size_t carriageway_tris = 0;
  for (const int a : assignment) {
    if (a == -1) {
      ++carriageway_tris;
    }
  }
  if (!any_sidewalk || carriageway_tris == 0) {
    result.carriageway = floor;
    return result;
  }

  result.carriageway =
      emit_floor_region(floor, assignment, -1, LaneType::Driving, floor.surface, floor.name);
  for (std::size_t k = 0; k < built.bands.size(); ++k) {
    SubMesh band = emit_floor_region(floor,
                                     assignment,
                                     static_cast<int>(k),
                                     LaneType::Sidewalk,
                                     built.bands[k].surface,
                                     fmt::format("junction {} corner sidewalk", junction.odr_id));
    if (!band.indices.empty()) {
      result.sidewalk_bands.push_back(std::move(band));
    }
  }
  return result;
}

std::vector<SubMesh> build_junction_corner_details(const RoadNetwork& network,
                                                   const Junction& junction,
                                                   const SamplingOptions& sampling) {
  (void)sampling; // overlays are face/corner geometry only — no station sampling
  std::vector<SubMesh> details;
  if (junction.corners.empty()) {
    return details; // nothing authored — the common case, and free
  }

  // Median noses: each arm's median lanes, extruded kMedianNoseDepth into the
  // junction from the arm face. (The continuous sidewalk band is emitted by
  // split_junction_floor_sidewalks — issue #357 — as part of the floor, not as
  // a floating overlay.)
  const std::span<const RoadEnd> arms = junction.arms.size() >= 2
                                            ? std::span<const RoadEnd>(junction.arms)
                                            : std::span<const RoadEnd>();
  for (const RoadEnd& arm : arms) {
    const std::string* material = median_material_for_arm(junction, arm);
    if (material == nullptr) {
      continue;
    }
    const std::optional<ArmFace> face = arm_face(network, arm);
    if (!face) {
      continue;
    }
    SubMesh nose;
    nose.material = LaneType::Median;
    nose.surface = *material;
    nose.name = fmt::format("junction {} median nose", junction.odr_id);
    for (std::size_t k = 0; k < face->types.size();) {
      if (face->types[k] != LaneType::Median) {
        ++k;
        continue;
      }
      std::size_t end = k;
      while (end + 1 < face->types.size() && face->types[end + 1] == LaneType::Median) {
        ++end;
      }
      const std::array<double, 3> p0 = lateral_point(face->frame, face->offsets[k]);
      const std::array<double, 3> p1 = lateral_point(face->frame, face->offsets[end + 1]);
      const double z = ((p0[2] + p1[2]) / 2.0) + kJunctionDetailLift;
      const double dx = kMedianNoseDepth * face->ix;
      const double dy = kMedianNoseDepth * face->iy;
      fan_triangulate(nose,
                      {std::array<double, 3>{p0[0], p0[1], z},
                       std::array<double, 3>{p1[0], p1[1], z},
                       std::array<double, 3>{p1[0] + dx, p1[1] + dy, z},
                       std::array<double, 3>{p0[0] + dx, p0[1] + dy, z}});
      k = end + 1;
    }
    if (!nose.indices.empty()) {
      details.push_back(std::move(nose));
    }
  }
  return details;
}

} // namespace roadmaker
