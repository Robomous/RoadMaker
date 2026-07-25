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

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <string>
#include <vector>

namespace roadmaker {

/// One corner's sidewalk band in plan view (issue #402): the strip of junction
/// floor that continues the arms' sidewalk pavement around the corner.
///
/// `outer` is the CURB line — the arms' sidewalk curbs plus, where the corner
/// is filleted, the corner curve. `inner` is that same line pushed one sidewalk
/// width toward the carriageway: it is the SEAM between sidewalk and
/// carriageway, and the mesher constrains the floor triangulation to it so no
/// triangle straddles it.
///
/// Where an arm carries something OUTBOARD of its sidewalk (a shoulder, a
/// border), the curb line is not the arm's outer pavement edge and the corner
/// curve is not the curb curve: the fillet is solved on the outermost edge, so
/// both rings are offset inward by that outboard width. The band then has a
/// material boundary on BOTH sides — sidewalk-to-shoulder as well as
/// sidewalk-to-carriageway — and both are constrained.
///
/// The two rings are PARALLEL: `inner[k]` is the point across the band from
/// `outer[k]`, so `outer.size() == inner.size()` always, `outer[k]`/`inner[k]`
/// bound the band's width there, and their midpoint is the band's mid-line.
/// The closed polygon is `outer` followed by `inner` reversed.
struct JunctionSidewalkBand {
  /// The corner's ordered arm pair, CCW: A's right edge meets B's left edge.
  RoadEnd arm_a;
  RoadEnd arm_b;

  std::vector<std::array<double, 2>> outer; ///< curb line
  std::vector<std::array<double, 2>> inner; ///< seam with the carriageway

  /// The authored `JunctionCorner::sidewalk_material` override, empty when the
  /// corner authors none (the band then keeps the generated sidewalk look).
  std::string surface;

  /// True when the band follows the corner curve, i.e. the corner solved to a
  /// fillet. False for the two shapes that have no curve to follow, both of
  /// which used to produce no band at all: a sharp corner the fillet solver
  /// rejects (the band runs to the apex instead) and the straight-through
  /// corridor of two parallel arms (it spans from one curb to the other).
  ///
  /// A corner where only ONE adjacent arm is sidewalked still wraps: the
  /// sidewalked side's width is carried across and the band runs to the CROWN
  /// of the fillet, then stops. It does not continue onto the arm that has no
  /// sidewalk — a fillet can be tens of meters long, and following it to the
  /// far tangency would stamp footway down that arm's mouth.
  bool wraps_corner = false;
};

/// Every sidewalk band of `junction_id`, one per corner that has at least one
/// sidewalked adjacent arm, in the mesher's CCW corner order.
///
/// This is the IDEAL geometry: pure, deterministic, and independent of the
/// floor's triangulation or even of the floor's extent — the mesher clips each
/// band to the junction footprint before using it, so a band here may reach
/// past a tight junction's floor. It is the same query the mesher builds its
/// triangulation constraints from, which is what makes it the oracle a test
/// can measure the realized seam against.
///
/// A sidewalk is recognised as the OUTERMOST LANE OF SIDEWALK TYPE, not merely
/// the outermost lane, so a cross section that parks a shoulder or a border
/// outboard of the sidewalk still bands correctly.
///
/// Returns empty when the id is stale, the junction has fewer than two usable
/// arms, or no arm carries a sidewalk.
[[nodiscard]] RM_API std::vector<JunctionSidewalkBand>
junction_sidewalk_bands(const RoadNetwork& network, JunctionId junction_id);

} // namespace roadmaker
