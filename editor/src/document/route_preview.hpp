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

// Turning a resolved route into drawable world geometry (p8-s3, issue #247).
//
// `osc::resolve_route` answers in LANES (`RouteLeg{road, lane, s_start,
// s_end}`); the viewport needs world-space polylines. This is the one
// projection between the two, shared by the Route tool's preview and by its
// polyline hit test — the same single-projection rule `actor_world_pose`
// established for actors: two implementations would drift, and the drift would
// only ever show as "the preview lied".

#include "roadmaker/osc/route.hpp"
#include "roadmaker/osc/scenario.hpp"

#include <array>
#include <optional>
#include <vector>

#include "document/actor_placement.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// The world polyline of one resolved leg, ordered in the leg's DIRECTION OF
/// TRAVEL — which is what `s_end < s_start` encodes for a left-hand lane, so
/// the caller must never re-sort by s. Points run along the LANE's centre line
/// (the kernel's own boundary walk, the same one the mesher and the actor snap
/// use), not the road's reference line. Empty when the leg no longer resolves
/// against this network.
[[nodiscard]] std::vector<std::array<double, 3>> route_leg_polyline(const RoadNetwork& network,
                                                                    const osc::RouteLeg& leg);

/// The pose each waypoint projects to, index-aligned with `route.waypoints`.
/// nullopt for a waypoint that names no lane (a world/road position, a
/// temporary-layer lane id) or one the network no longer resolves — the tool
/// draws what it can and marks the rest, never silently skipping an index,
/// because the index is what `set_route_waypoint` addresses.
[[nodiscard]] std::vector<std::optional<ActorPose>> route_waypoint_poses(const RoadNetwork& network,
                                                                         const osc::Route& route);

} // namespace roadmaker::editor
