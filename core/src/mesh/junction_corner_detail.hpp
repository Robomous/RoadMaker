// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Internal (non-installed) header: the junction corner solve shared by the
// junction surface mesher (junction_surface.cpp) and the public corner query
// (junction_corners.cpp). Keeping ONE solver is the whole point — the Corner
// tool's handles must sit exactly on the pavement the mesher emits
// (p4-s1, issue #225).

#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <vector>

namespace roadmaker {

struct Junction;

namespace junction_corner_detail {

/// Corner fillets (tee visual finding, follow-up to issue #103): at every
/// re-entrant corner between two adjacent arms the pavement edge follows a
/// curve tangent to both arms' outer edge lines — the strongest visual
/// signature of a real intersection. Radius floor below; the desired radius
/// derives from the corner turn's connecting road (its centerline min radius
/// approximates the inner-edge geometry) and is clamped to what the arm faces
/// leave room for.
inline constexpr double kFilletRadiusFloor = 3.0;

/// Cap on the DERIVED fillet radius: past this the corner reads as a slip
/// lane, and long tangent legs start colliding with neighboring corners. An
/// authored radius is not capped by this — only by the geometric `max_radius`.
inline constexpr double kFilletRadiusCap = 15.0;

/// Fillet curves are sampled to this sagitta [m]. Must stay above
/// junction_surface.cpp's kBoundarySimplify or SimplifyPaths would flatten the
/// curve back into a chord after the union.
inline constexpr double kFilletArcSagitta = 8e-3;

/// Curve samples closer than this [m] to either tangent edge line are dropped:
/// a sample millimeters off the line it is tangent to pairs with the edge into
/// a sub-degree CDT sliver. The boundary leaves the exact tangency point with
/// a <= 8 cm chord shortcut instead — invisible, and the first departure angle
/// stays above the mesh's 5-degree sliver gate.
inline constexpr double kFilletTangentLift = 0.08;

/// Smallest effective fillet radius [m]; below it the corner is left sharp.
inline constexpr double kMinFilletRadius = 0.05;

/// Smallest authored tangent-leg setback [m].
inline constexpr double kMinFilletExtent = 0.05;

/// Connecting roads of this junction, in connection order, de-duplicated.
[[nodiscard]] std::vector<RoadId> connecting_roads(const Junction& junction);

/// One arm's face for corner construction, oriented INTO the junction:
/// `left`/`right` are the outermost pavement corners as seen entering, and
/// (ix, iy) the unit into-junction direction. `arm` is the corner's identity
/// half — JunctionCorner entries name the arm pair, not face coordinates.
struct CornerFace {
  std::array<double, 2> left{};
  std::array<double, 2> right{};
  double ix = 0.0;
  double iy = 0.0;
  RoadEnd arm;
};

/// The arm faces of `junction`, INTO the junction, sorted CCW around their
/// centroid — the cyclic order in which consecutive pairs form corners
/// (A's right edge meeting B's left edge). Empty unless the junction's arm
/// list persists (>= 2 arms, the writer's rm:arms rule), so a degenerate or
/// foreign junction meshes identically before and after save/load.
[[nodiscard]] std::vector<CornerFace> corner_faces(const RoadNetwork& network,
                                                   const Junction& junction);

/// The solved fillet between two CCW-adjacent faces. `valid` is false for the
/// pairs the mesher does not fillet; `parallel_edges` singles out the
/// straight-through corridor case (no corner at all), which the mesher paves
/// with an edge strip instead.
struct CornerSolution {
  bool valid = false;
  bool parallel_edges = false;

  /// Edge-line intersection: A's right edge meeting B's left edge.
  std::array<double, 2> corner{};

  /// Unit edge-ray directions, pointing INTO the junction.
  std::array<double, 2> dir_a{};
  std::array<double, 2> dir_b{};

  /// Unit inward bisector at `corner` (points into the pavement).
  std::array<double, 2> bisector{};

  /// Angle [rad] between the two edge rays at `corner`.
  double phi = 0.0;

  /// Effective tangent-leg setbacks [m] from `corner` back along each edge.
  double extent_a = 0.0;
  double extent_b = 0.0;

  /// Tangency points, `corner - extent_x * dir_x`.
  std::array<double, 2> tangent_a{};
  std::array<double, 2> tangent_b{};

  /// Effective fillet radius [m] (the symmetric-leg radius).
  double radius = 0.0;

  /// Largest radius the arm faces leave room for: min(ta, tb) * tan(phi/2).
  double max_radius = 0.0;

  /// Free run [m] of each edge line from its face corner to `corner`.
  double max_extent_a = 0.0;
  double max_extent_b = 0.0;

  /// True when a JunctionCorner override supplied the value.
  bool radius_authored = false;
  bool extents_authored = false;

  /// True when the radius came from `Junction::default_corner_radius` rather
  /// than from a per-corner entry or the derivation (p4-s2, issue #226).
  bool radius_from_junction_default = false;
};

/// Solves the corner between CCW-adjacent faces `a` and `b`, honouring any
/// JunctionCorner override in `junction.corners` that names exactly this
/// ordered arm pair. Pure and deterministic.
[[nodiscard]] CornerSolution solve_corner(const RoadNetwork& network,
                                          const Junction& junction,
                                          const CornerFace& a,
                                          const CornerFace& b);

/// The fillet boundary, tangent_a → tangent_b inclusive, sampled at
/// kFilletArcSagitta. Purely derived corners keep the original
/// uniform-angle circular-arc sampling verbatim so their meshes stay
/// byte-identical; authored corners use the rational quadratic Bezier
/// (P0=tangent_a, P1=corner, P2=tangent_b, w=sin(phi/2)) which is the same
/// circle for equal extents and stays G1-tangent for unequal ones.
/// Interior samples hugging either tangent line are dropped
/// (see kFilletTangentLift). Empty when `solution` is not valid.
[[nodiscard]] std::vector<std::array<double, 2>> corner_curve(const CornerSolution& solution);

/// The corner curve at parameter `t` in [0, 1] — the rational quadratic
/// Bezier, used for the apex handle (t = 0.5).
[[nodiscard]] std::array<double, 2> corner_curve_point(const CornerSolution& solution, double t);

} // namespace junction_corner_detail

} // namespace roadmaker
