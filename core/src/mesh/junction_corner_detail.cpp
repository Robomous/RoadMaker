// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "junction_corner_detail.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

#include "mesh_detail.hpp"

namespace roadmaker::junction_corner_detail {

namespace {

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::section_at;
using mesh_detail::StationFrame;

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
                            const CornerFace& a,
                            const CornerFace& b) {
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

/// The authored override naming exactly the ordered pair (a, b), if any.
const JunctionCorner*
override_for(const Junction& junction, const CornerFace& a, const CornerFace& b) {
  for (const JunctionCorner& entry : junction.corners) {
    if (entry.arm_a == a.arm && entry.arm_b == b.arm) {
      return &entry;
    }
  }
  return nullptr;
}

/// Perpendicular distance from `p` to the line through `on` with direction
/// (dx, dy) (unit).
double line_distance(const std::array<double, 2>& p,
                     const std::array<double, 2>& on,
                     double dx,
                     double dy) {
  return std::abs(((p[0] - on[0]) * dy) - ((p[1] - on[1]) * dx));
}

/// Sample count for a fillet spanning `abs_sweep` radians at `radius`, at the
/// mesher's sagitta. Shared by the arc and Bezier paths so sampling density is
/// identical either way.
int fillet_steps(double abs_sweep, double radius) {
  const double step_angle =
      2.0 * std::acos(std::clamp(1.0 - (kFilletArcSagitta / radius), 0.0, 1.0));
  return std::max(4, static_cast<int>(std::ceil(abs_sweep / std::max(step_angle, 1e-3))));
}

} // namespace

std::vector<RoadId> connecting_roads(const Junction& junction) {
  std::vector<RoadId> roads;
  for (const JunctionConnection& conn : junction.connections) {
    if (std::ranges::find(roads, conn.connecting_road) == roads.end()) {
      roads.push_back(conn.connecting_road);
    }
  }
  return roads;
}

std::vector<CornerFace> corner_faces(const RoadNetwork& network, const Junction& junction) {
  std::vector<CornerFace> faces;
  // Corners only for junctions whose arm list persists (>= 2 arms — the
  // writer's rm:arms rule): a degenerate or foreign junction must mesh
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
    // Face corners oriented into the junction: a Start-contact arm's road
    // frame is flipped relative to the entering direction, so its road-left
    // corner is the entering-right one.
    const bool flipped = arm.contact == ContactPoint::Start;
    faces.push_back(CornerFace{
        .left = flipped ? std::array<double, 2>{right[0], right[1]}
                        : std::array<double, 2>{left[0], left[1]},
        .right = flipped ? std::array<double, 2>{left[0], left[1]}
                         : std::array<double, 2>{right[0], right[1]},
        .ix = ix,
        .iy = iy,
        .arm = arm,
    });
  }
  if (faces.size() < 2) {
    return faces;
  }
  // Cyclic CCW order around the shared centroid; the corner between
  // consecutive arms (A, B) is A's right edge meeting B's left edge.
  double cx = 0.0, cy = 0.0;
  for (const CornerFace& f : faces) {
    cx += (f.left[0] + f.right[0]) / 2.0;
    cy += (f.left[1] + f.right[1]) / 2.0;
  }
  cx /= static_cast<double>(faces.size());
  cy /= static_cast<double>(faces.size());
  std::ranges::sort(faces, [&](const CornerFace& fa, const CornerFace& fb) {
    const double mid_ax = ((fa.left[0] + fa.right[0]) / 2.0) - cx;
    const double mid_ay = ((fa.left[1] + fa.right[1]) / 2.0) - cy;
    const double mid_bx = ((fb.left[0] + fb.right[0]) / 2.0) - cx;
    const double mid_by = ((fb.left[1] + fb.right[1]) / 2.0) - cy;
    return std::atan2(mid_ay, mid_ax) < std::atan2(mid_by, mid_bx);
  });
  return faces;
}

CornerSolution solve_corner(const RoadNetwork& network,
                            const Junction& junction,
                            const CornerFace& a,
                            const CornerFace& b) {
  CornerSolution solution;
  solution.dir_a = {a.ix, a.iy};
  solution.dir_b = {b.ix, b.iy};

  const std::array<double, 2> pa = a.right; // A's edge facing B
  const std::array<double, 2> pb = b.left;  // B's edge facing A
  const double cross = (a.ix * b.iy) - (a.iy * b.ix);
  if (std::abs(cross) < 1e-6) {
    solution.parallel_edges = true;
    return solution;
  }
  // Edge-line intersection pa + ta·A_dir = pb + tb·B_dir.
  const double dx = pb[0] - pa[0];
  const double dy = pb[1] - pa[1];
  const double ta = ((dx * b.iy) - (dy * b.ix)) / cross;
  const double tb = ((dx * a.iy) - (dy * a.ix)) / cross;
  if (ta <= tol::kLength || tb <= tol::kLength) {
    return solution; // corner behind a face — degenerate arm layout
  }
  const std::array<double, 2> corner{pa[0] + (ta * a.ix), pa[1] + (ta * a.iy)};
  const double cos_phi = std::clamp((a.ix * b.ix) + (a.iy * b.iy), -1.0, 1.0);
  const double phi = std::acos(cos_phi); // angle between the edge rays at the corner
  if (phi < 0.1 || phi > std::numbers::pi - 0.1) {
    return solution; // near-tangent arms; a fillet would degenerate
  }
  const double tan_half = std::tan(phi / 2.0);
  const double max_radius = std::min(ta, tb) * tan_half;

  const JunctionCorner* entry = override_for(junction, a, b);
  double radius = 0.0;
  if (entry != nullptr && entry->radius.has_value()) {
    // Authored: only the geometric bound applies — the [floor, cap] band is a
    // property of the DERIVATION, not of what an author may ask for.
    radius = std::clamp(*entry->radius, kMinFilletRadius, max_radius);
    solution.radius_authored = true;
  } else if (junction.default_corner_radius.has_value()) {
    // Junction-wide default (p4-s2): authored-like, so it is bounded only by
    // the geometry, exactly as a per-corner radius is. A per-corner entry
    // above still wins.
    radius = std::clamp(*junction.default_corner_radius, kMinFilletRadius, max_radius);
    solution.radius_from_junction_default = true;
  } else {
    // Desired radius, clamped to the tangent legs the faces leave room for.
    radius = std::min(corner_fillet_radius(network, junction, a, b), max_radius);
  }
  if (radius < kMinFilletRadius) {
    return solution;
  }
  const double tangent_len = radius / tan_half;
  double extent_a = tangent_len;
  double extent_b = tangent_len;
  if (entry != nullptr && (entry->extent_a.has_value() || entry->extent_b.has_value())) {
    extent_a = std::clamp(entry->extent_a.value_or(tangent_len), kMinFilletExtent, ta);
    extent_b = std::clamp(entry->extent_b.value_or(tangent_len), kMinFilletExtent, tb);
    solution.extents_authored = true;
  }
  const std::array<double, 2> tan_a{corner[0] - (extent_a * a.ix), corner[1] - (extent_a * a.iy)};
  const std::array<double, 2> tan_b{corner[0] - (extent_b * b.ix), corner[1] - (extent_b * b.iy)};
  // Inward bisector at the corner (the arc center lies along it).
  double bis_x = -(a.ix + b.ix);
  double bis_y = -(a.iy + b.iy);
  const double bis_len = std::hypot(bis_x, bis_y);
  if (bis_len < tol::kLength) {
    return solution;
  }
  bis_x /= bis_len;
  bis_y /= bis_len;

  solution.valid = true;
  solution.corner = corner;
  solution.bisector = {bis_x, bis_y};
  solution.phi = phi;
  solution.extent_a = extent_a;
  solution.extent_b = extent_b;
  solution.tangent_a = tan_a;
  solution.tangent_b = tan_b;
  solution.radius = radius;
  solution.max_radius = max_radius;
  solution.max_extent_a = ta;
  solution.max_extent_b = tb;
  return solution;
}

std::array<double, 2> corner_curve_point(const CornerSolution& solution, double t) {
  const double w = std::sin(solution.phi / 2.0);
  const double u = 1.0 - t;
  const double b0 = u * u;
  const double b1 = 2.0 * w * t * u;
  const double b2 = t * t;
  const double denom = b0 + b1 + b2;
  if (denom <= 0.0) {
    return solution.corner;
  }
  return {
      ((b0 * solution.tangent_a[0]) + (b1 * solution.corner[0]) + (b2 * solution.tangent_b[0])) /
          denom,
      ((b0 * solution.tangent_a[1]) + (b1 * solution.corner[1]) + (b2 * solution.tangent_b[1])) /
          denom};
}

std::vector<std::array<double, 2>> corner_curve(const CornerSolution& solution) {
  std::vector<std::array<double, 2>> curve;
  if (!solution.valid) {
    return curve;
  }
  const double abs_sweep = std::numbers::pi - solution.phi;
  const int steps = fillet_steps(abs_sweep, solution.radius);
  const auto keep = [&solution, &curve](const std::array<double, 2>& p, bool endpoint) {
    // Endpoints are the exact tangencies; interior samples hugging either
    // tangent line are dropped (see kFilletTangentLift).
    if (!endpoint && (line_distance(p, solution.tangent_a, solution.dir_a[0], solution.dir_a[1]) <
                          kFilletTangentLift ||
                      line_distance(p, solution.tangent_b, solution.dir_b[0], solution.dir_b[1]) <
                          kFilletTangentLift)) {
      return;
    }
    curve.push_back(p);
  };

  if (!solution.radius_authored && !solution.extents_authored &&
      !solution.radius_from_junction_default) {
    // Derived corner: the original uniform-angle circular-arc sampling, kept
    // verbatim so unauthored junction meshes stay byte-identical.
    const double center_dist = solution.radius / std::sin(solution.phi / 2.0);
    const std::array<double, 2> center{solution.corner[0] + (solution.bisector[0] * center_dist),
                                       solution.corner[1] + (solution.bisector[1] * center_dist)};
    const double ang_a =
        std::atan2(solution.tangent_a[1] - center[1], solution.tangent_a[0] - center[0]);
    double ang_b = std::atan2(solution.tangent_b[1] - center[1], solution.tangent_b[0] - center[0]);
    // Sweep the SHORT way (the fillet arc spans pi - phi < pi).
    while (ang_b - ang_a > std::numbers::pi) {
      ang_b -= 2.0 * std::numbers::pi;
    }
    while (ang_a - ang_b > std::numbers::pi) {
      ang_b += 2.0 * std::numbers::pi;
    }
    const double sweep = ang_b - ang_a;
    const int arc_steps = fillet_steps(std::abs(sweep), solution.radius);
    for (int k = 0; k <= arc_steps; ++k) {
      const double ang = ang_a + (sweep * static_cast<double>(k) / arc_steps);
      keep({center[0] + (solution.radius * std::cos(ang)),
            center[1] + (solution.radius * std::sin(ang))},
           k == 0 || k == arc_steps);
    }
    return curve;
  }

  // Authored corner: rational quadratic Bezier. Equal extents reproduce the
  // circle of the same radius; unequal extents stay tangent to both edges,
  // which is what keeps independent per-side setbacks watertight.
  for (int k = 0; k <= steps; ++k) {
    if (k == 0) {
      curve.push_back(solution.tangent_a);
      continue;
    }
    if (k == steps) {
      curve.push_back(solution.tangent_b);
      continue;
    }
    keep(corner_curve_point(solution, static_cast<double>(k) / steps), false);
  }
  return curve;
}

} // namespace roadmaker::junction_corner_detail
