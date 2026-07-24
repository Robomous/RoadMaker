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
#include "roadmaker/road/id.hpp"

#include <vector>

namespace roadmaker {

class RoadNetwork;

/// A detected overpass (p5-s3, #233): two roads whose reference lines cross in
/// plan view, whose elevations at the crossing differ by at least the clearance
/// threshold, and which NO junction connects. That last clause is what makes it
/// an overpass rather than an intersection, and is why this is a query on the
/// network (which knows the junctions) rather than on geometry alone
/// (docs/design/materials-structures/02_bridge_generator.md §4).
struct GradeSeparation {
  RoadId upper;           ///< the road carried over (the higher elevation)
  RoadId lower;           ///< the road passed under
  double s_upper = 0.0;   ///< crossing station on `upper`
  double s_lower = 0.0;   ///< crossing station on `lower`
  double clearance = 0.0; ///< z(upper, s_upper) - z(lower, s_lower), >= 0

  friend bool operator==(const GradeSeparation&, const GradeSeparation&) = default;
};

/// Minimum vertical gap for a crossing to count as a grade separation (design §4).
inline constexpr double kDefaultClearance = 3.0;

/// Every grade separation in the network, one per crossing road-pair (the
/// crossing with the greatest clearance when a pair crosses more than once).
/// `clearance` default 3.0 m. A pair connected by a junction is never reported,
/// nor is a pair whose vertical gap is under the threshold — those are ordinary
/// intersections. The result carries everything the generator and the auto-offer
/// need: which road is on top, and where each is crossed.
[[nodiscard]] RM_API std::vector<GradeSeparation>
find_grade_separations(const RoadNetwork& network, double clearance = kDefaultClearance);

} // namespace roadmaker
