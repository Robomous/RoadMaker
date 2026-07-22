// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <string>
#include <vector>

namespace roadmaker {

/// The solved geometry of one junction corner — the single source shared by
/// the mesher (junction_surface.cpp), the editor's Corner tool, and the
/// properties pane (p4-s1, issue #225). Everything is plan-view (x, y) in the
/// kernel frame; z is the junction's derived floor and is not part of the
/// corner solve.
struct JunctionCornerInfo {
  /// Identity — the CCW-adjacent arm pair, matching JunctionCorner::arm_a/b.
  RoadEnd arm_a;
  RoadEnd arm_b;

  /// The edge-line intersection: where arm_a's right edge meets arm_b's left
  /// edge if neither were filleted. The apex handle's reference point.
  std::array<double, 2> corner{};

  /// Unit directions of the two edge rays, pointing INTO the junction (i.e.
  /// from each arm's face corner toward `corner`).
  std::array<double, 2> dir_a{};
  std::array<double, 2> dir_b{};

  /// Unit inward bisector at `corner` (points into the pavement).
  std::array<double, 2> bisector{};

  /// The arm faces' outer corners the edge rays start from.
  std::array<double, 2> face_a{};
  std::array<double, 2> face_b{};

  /// Angle [rad] between the two edge rays at `corner`.
  double phi = 0.0;

  /// Effective tangent-leg setbacks [m] from `corner` back along each edge —
  /// the tangency points are `corner - extent_a * dir_a` and
  /// `corner - extent_b * dir_b`.
  double extent_a = 0.0;
  double extent_b = 0.0;

  /// Tangency (control) points on each edge line.
  std::array<double, 2> tangent_a{};
  std::array<double, 2> tangent_b{};

  /// Effective fillet radius [m] — for equal extents this is the true circular
  /// radius; with unequal extents it is the radius the symmetric leg would
  /// give, i.e. what the Corner Radius attribute shows and edits.
  double radius = 0.0;

  /// Largest radius the arm faces leave room for [m] — the spin box's upper
  /// bound. `min(leg_a, leg_b) * tan(phi/2)` where leg_x is the free run of
  /// each edge from its face corner to `corner`.
  double max_radius = 0.0;

  /// Free run [m] of each edge line from its face corner to `corner` — the
  /// upper bound on the matching extent.
  double max_extent_a = 0.0;
  double max_extent_b = 0.0;

  /// True when the junction carries a JunctionCorner override supplying this
  /// value (as opposed to the derivation).
  bool radius_authored = false;
  bool extents_authored = false;

  /// True when `radius` came from `Junction::default_corner_radius` — i.e. no
  /// per-corner entry supplied one, but the junction-wide default did
  /// (p4-s2, issue #226). Never true together with `radius_authored`.
  bool radius_from_junction_default = false;

  /// The corner's authored overlay materials (bare catalog names), empty when
  /// unset. Plain data members — no accessors: an out-of-line member function
  /// in this installed header would need its own RM_API (see `apex()`).
  std::string sidewalk_material;
  std::string median_material;

  /// The corner curve, tangent_a → tangent_b, sampled at the mesher's fillet
  /// sagitta. A rational quadratic Bezier (P0=tangent_a, P1=corner,
  /// P2=tangent_b, w=sin(phi/2)) — exactly the circular arc when the extents
  /// are equal, and G1-tangent to both edges when they are not, which is what
  /// keeps independent per-side extents watertight.
  std::vector<std::array<double, 2>> curve;

  /// Midpoint of `curve` (the Bezier at t=0.5) — the apex control vertex.
  ///
  /// RM_API sits on the member rather than the struct: the definition lives in
  /// junction_corners.cpp (it evaluates the curve through the internal
  /// junction_corner_detail solver, so it cannot be inlined here), and under
  /// RM_BUILD_SHARED=ON the kernel hides symbols by default — an unexported
  /// out-of-line member fails to link from the editor, the bindings and the
  /// tests. Exporting the whole struct instead would drag its std::vector
  /// member into MSVC's dll-interface warning (C4251), and no struct in this
  /// kernel is exported wholesale.
  [[nodiscard]] RM_API std::array<double, 2> apex() const;
};

/// Solves every corner of `junction_id` in CCW order, applying any authored
/// JunctionCorner overrides. Returns empty when the junction has fewer than
/// two usable arms (nothing to fillet) or the id is stale. A pair whose edge
/// rays are parallel, meet behind a face, or subtend a near-degenerate angle is
/// skipped — exactly the pairs the mesher does not fillet.
[[nodiscard]] RM_API std::vector<JunctionCornerInfo> junction_corners(const RoadNetwork& network,
                                                                      JunctionId junction_id);

} // namespace roadmaker
