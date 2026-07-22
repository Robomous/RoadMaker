// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <optional>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// Plan-view crossing detection between reference lines. The single authority
/// for "do these two roads cross, and where" — used by the Create Road tool to
/// decide whether a drawn road forms a 4-way crossing, and by the cross_roads
/// assembly to place the junction. All geometry (the ReferenceLine → Clothoids
/// list conversion and the clothoid-list intersection) lives here in the
/// kernel; the editor only consumes the result.
namespace roadmaker {

/// A plan-view crossing of two reference lines: the station on each line and
/// the inertial point where they meet.
struct RoadCrossing {
  double s_a = 0.0; ///< station on road A [m]
  double s_b = 0.0; ///< station on road B [m]
  Waypoint point;   ///< inertial crossing point [m]
};

/// Every plan-view crossing of roads `a` and `b`, ascending by `s_a`. Empty
/// when the reference lines are disjoint. Errors (InvalidArgument): a stale id,
/// `a == b`, or an empty plan view on either road.
///
/// paramPoly3 records (rare outside imported files) are approximated by a
/// single G1 clothoid through their endpoints for the crossing test — exact
/// for the line/arc/spiral geometry the authoring tools produce.
[[nodiscard]] RM_API Expected<std::vector<RoadCrossing>>
road_intersections(const RoadNetwork& network, RoadId a, RoadId b);

/// A crossing of a fitted reference line against one road's body.
struct BodyCrossing {
  RoadId road;         ///< the crossed road
  double s_line = 0.0; ///< station on the fitted line [m]
  double s_road = 0.0; ///< station on the crossed road [m]
  Waypoint point;      ///< inertial crossing point [m]
};

/// The first crossing (smallest station on `fitted`) of `fitted` against the
/// body of any road other than `exclude`, strictly interior to BOTH curves —
/// so an endpoint touch (a tee or a link) is not reported as a crossing. Junction
/// connecting roads are skipped (their geometry is generated, not authored).
/// nullopt when `fitted` crosses no road's interior.
[[nodiscard]] RM_API std::optional<BodyCrossing>
first_body_crossing(const RoadNetwork& network, const ReferenceLine& fitted, RoadId exclude);

/// EVERY road whose body `fitted` crosses (one crossing per road — its earliest
/// interior hit), ascending by station on `fitted`. Same interior/exclude/
/// connecting-road rules as first_body_crossing. Empty when `fitted` crosses no
/// road's interior. Drives the Create Road tool's multi-crossing commit, where a
/// single stroke forms one junction per crossed road (#354).
[[nodiscard]] RM_API std::vector<BodyCrossing>
body_crossings(const RoadNetwork& network, const ReferenceLine& fitted, RoadId exclude);

} // namespace roadmaker
