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

/// Resolving a scenario `<Route>` against a live road network (p8-s3, issue
/// #247) — the one place the `.xosc` and the `.xodr` are read together.
///
/// WHY THIS IS A SEPARATE HEADER FROM `osc/writer.hpp`. `validate_scenario`
/// takes a `Scenario` and nothing else, and that is structural rather than an
/// oversight: a `Scenario` holds OpenDRIVE `@id` STRINGS, never arena handles
/// (ADR-0014 §5), so it cannot tell whether the road a waypoint names exists,
/// let alone whether a car can drive from one waypoint to the next. Everything
/// here needs the network, so it lives apart rather than growing an overload
/// that silently does more.
///
/// WHY IT IS KERNEL-SIDE. GW-6 step 8 — *edit the network so a route is
/// invalidated, and confirm the route is diagnosed rather than silently deleted
/// or silently re-routed* — has to be replayable headlessly, and
/// `python/CMakeLists.txt` links `roadmaker::core` alone. A resolver living in
/// the editor could not produce that evidence at all.
///
/// ★ THIS MODULE NEVER MUTATES AND NEVER RE-ROUTES. It reports. A waypoint that
/// no longer resolves, or a gap with no drivable path, produces a `Diagnostic`
/// naming the route and the waypoint index, and leaves `complete` false. Both
/// of the tempting "helpful" behaviours — dropping the dead waypoint, or
/// solving around it — destroy the thing the user authored and give them no way
/// to know it happened.
///
/// Reference: ASAM OpenSCENARIO XML 1.4.0 §7.7 (Route, Waypoint,
/// AssignRouteAction) and Annex C.8; ASAM OpenDRIVE 1.9.0 §9 (lane linkage) and
/// §12 (junctions). The OpenSCENARIO text is NOT tracked in this repository;
/// fetch it with `scripts/fetch_asam_specs.py --std openscenario`.

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osc {

/// One contiguous run of a resolved route along ONE lane.
///
/// The unit is a LANE, not a road, and that is what makes the record usable:
/// a lane belongs to exactly one lane section, so `lane` pins the cross-section
/// too and a caller sampling the leg cannot drift onto a differently-numbered
/// lane where the road re-sections.
struct RouteLeg {
  RoadId road;

  /// The exact lane travelled. Unique within its section, so it identifies the
  /// cross-section as well.
  LaneId lane;

  /// The lane's OpenDRIVE `@id`, for diagnostics and for the `.xosc`'s own
  /// spelling. Negative = right of the reference line.
  int lane_odr_id = 0;

  /// Road `s` where this leg begins and ends [m].
  ///
  /// ★ `s_end < s_start` WHEN THE LEG TRAVELS AGAINST +s, which a left-hand
  /// lane always does. Callers must not assume an ascending interval; that
  /// assumption is how a route drawn through a left-hand lane comes out
  /// backwards or empty.
  double s_start = 0.0;
  double s_end = 0.0;
};

/// What `resolve_route` found.
struct ResolvedRoute {
  /// The lanes the route traverses, in travel order. Partial when `complete` is
  /// false: the legs that WERE solved are still here, because a caller drawing
  /// the route should show what it could resolve and mark the gap rather than
  /// show nothing.
  std::vector<RouteLeg> legs;

  /// Everything the resolution had to cope with. Never empty when `complete` is
  /// false — an incomplete resolution that said nothing would be the silent
  /// failure this module exists to prevent.
  std::vector<Diagnostic> findings;

  /// True only when every waypoint resolved AND every consecutive pair is
  /// joined by a drivable path.
  bool complete = false;
};

/// Upper bound on lanes visited while joining one pair of waypoints.
///
/// A bound rather than an unbounded search: a lane graph is small, but a
/// malformed junction can make it cyclic, and a resolver called on every
/// topology change must not become the thing that hangs the editor. Exceeding
/// it is reported like any other failure to join.
inline constexpr std::size_t kMaxRouteSearchLanes = 4096;

/// Resolves `route` against `network`.
///
/// Waypoints are joined pairwise by a breadth-first search over lane linkage:
/// `Lane::predecessor`/`successor` inside a road, `Road::predecessor`/
/// `successor` between roads, and `JunctionConnection::lane_links` through a
/// junction — which is what makes a solved path go through the junction's
/// connecting roads (its authored maneuvers) rather than across it in a
/// straight line.
///
/// The search is DIRECTED by the lane's own direction of travel: a right-hand
/// lane (negative `@id`) is left at the road's END, a left-hand lane at its
/// START. A route that can only be driven backwards is therefore reported as
/// unreachable rather than solved, which is the honest answer.
///
/// Only `<LanePosition>` waypoints resolve to a lane. A `<RoadPosition>` names
/// a road but not a lane and a `<WorldPosition>` names neither; both are
/// reported (citing `routing.ambiguous_route_waypoints` where it applies) and
/// leave the route incomplete rather than being snapped to a guess.
[[nodiscard]] RM_API ResolvedRoute resolve_route(const RoadNetwork& network, const Route& route);

/// Every route in `scenario`, checked against `network`.
///
/// The cross-document half of `validate_scenario`, which cannot see a network
/// at all. Reports an unresolvable waypoint, an unreachable gap, and a waypoint
/// that sits inside a junction (`routing.route_waypoints_locations`: "route
/// waypoints should not be located in junctions to avoid ambiguity") — the last
/// of which is not checkable from the `.xosc` alone, since junction membership
/// is a fact about the `.xodr`.
///
/// Empty when every route resolves. Intended to be re-run whenever the network
/// changes, which is how a route survives an edit to the road beneath it and
/// still gets diagnosed when the edit genuinely broke it (GW-6 steps 7 and 8).
[[nodiscard]] RM_API std::vector<Diagnostic> validate_routes(const RoadNetwork& network,
                                                             const Scenario& scenario);

/// Every `<Route>` a scenario assigns, in document order, paired with the
/// entity it belongs to.
///
/// A plain accessor rather than a loop every caller re-writes: the editor draws
/// them, `validate_routes` checks them, and Python replays them, and three
/// hand-rolled walks of `<Init>`/`<Private>`/`<PrivateAction>` would drift.
struct AssignedRoute {
  /// `<Private @entityRef>` — the entity this route is assigned to.
  std::string entity_ref;
  const Route* route = nullptr;
};

[[nodiscard]] RM_API std::vector<AssignedRoute> assigned_routes(const Scenario& scenario);

} // namespace roadmaker::osc
