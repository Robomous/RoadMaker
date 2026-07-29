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

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace roadmaker {

/// A plan-view point [m] in the kernel frame, as the simplifier sees it.
/// Deliberately not `Waypoint`: this header is pure geometry and knows nothing
/// about roads.
using Point2 = std::array<double, 2>;

/// Ramer–Douglas–Peucker simplification of an OPEN polyline.
///
/// Returns **the indices of the retained points**, ascending, always including
/// `0` and `points.size() - 1`. Fewer than three points are returned unchanged.
///
/// **Indices, not points, and that is load-bearing.** A caller frequently needs
/// to know *which* source vertices survived, not merely what the simplified
/// line looks like — in an OSM import a vertex may also be a topology node, and
/// a simplifier that returned points makes "was this junction node dropped?"
/// unanswerable. Callers wanting the points can map the indices; callers
/// wanting the answer cannot recover it from points.
///
/// The implementation is **iterative (an explicit stack), never recursive**:
/// the textbook formulation recurses once per retained vertex, and this
/// function is reachable from a file parser, where a 200 000-vertex input is a
/// stack overflow rather than a slow day.
///
/// `tolerance` is the greatest perpendicular distance [m] a dropped vertex may
/// sit from the retained line. RDP guarantees the result satisfies it — see
/// `polyline_deviation`, which measures what actually happened rather than
/// restating what was asked for. A non-positive tolerance retains everything.
///
/// **Cost, and the input that makes it bad.** Typically O(n log n), because
/// each split lands near the middle of its span. The worst case is O(n²), and
/// it is reachable rather than theoretical: a polyline whose farthest vertex is
/// always the FIRST interior one splits maximally unbalanced every time. A
/// 200 000-vertex sawtooth built exactly that way measured **165 seconds**
/// here. Real survey and traced geometry does not look like that, but a file
/// is an untrusted input and its vertex count is not our choice — so a caller
/// reading from disk must bound the polyline length rather than assume the
/// typical case. The OSM importer does: ways are split at their shared nodes
/// before this is called, and the result is capped at
/// `osm::kMaxWaypointsPerRoad`.
[[nodiscard]] RM_API std::vector<std::size_t> simplify_polyline(std::span<const Point2> points,
                                                                double tolerance);

/// The greatest perpendicular distance [m] from any source vertex to the
/// polyline through `kept`.
///
/// This is the number a "we simplified your road" diagnostic must quote.
/// Quoting the tolerance instead reports the request, not the result: two ways
/// simplified at the same tolerance can deviate by 0.02 m and 0.49 m, and only
/// one of those is worth a user's attention.
///
/// `kept` must be ascending and within range; an empty or single-element `kept`
/// yields 0.0.
[[nodiscard]] RM_API double polyline_deviation(std::span<const Point2> points,
                                               std::span<const std::size_t> kept);

/// Drops interior points closer than `epsilon` [m] to the previously retained
/// point, returning the retained indices. **The first and last are always
/// retained**, however short the polyline — in an import they carry the
/// topology, and a collapsed endpoint silently unlinks a road.
///
/// Needed because clothoid fitting refuses coincident consecutive waypoints
/// outright, and is numerically degenerate well before it refuses: a fit
/// through two points three millimetres apart is not a curve anyone authored.
/// So the epsilon that matters is road-scale, not `tol::kLength`.
[[nodiscard]] RM_API std::vector<std::size_t> drop_near_duplicates(std::span<const Point2> points,
                                                                   double epsilon);

} // namespace roadmaker
