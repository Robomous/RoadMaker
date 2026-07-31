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

// Actor placement helpers (p8-s2, issue #246), the scenario twin of
// prop_placement.hpp: resolve a world cursor to a LANE, mint a unique entity
// name, and project a lane anchor back to a world pose for the ghost and the
// renderer. Pure geometry over the kernel — no widgets, no kernel changes — so
// it unit-tests headless.
//
// WHY THIS IS EDITOR-SIDE AND THE CATALOGUE IS NOT. The catalogue had to be
// kernel-side because a headless GW-6 replay builds actors from it. The SNAP
// does not: a replay names its road and lane directly (`python/examples/
// scenario_actors.py` does exactly that), so resolving a mouse position to a
// lane is a UI concern. That is the same line `nearest_road_station` already
// sits on — prop placement's snap lives here too, for the same reason.

#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/id.hpp"

#include <array>
#include <optional>
#include <string>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osc {
struct Scenario;
} // namespace roadmaker::osc

namespace roadmaker::editor {

/// How far off a road's reference line a drop may land and still snap to it.
///
/// Generous on purpose — wider than a carriageway, so a click near the kerb
/// still finds the road — because the LANE test below is what actually decides
/// whether the drop is legal. A tight radius here would reject drops that are
/// over a real lane, and report it as "no road in reach" rather than as what it
/// is.
inline constexpr double kActorSnapMaxT = 20.0;

/// A lane anchor: everything a `<LanePosition>` needs, in OpenDRIVE `@id`
/// STRINGS rather than arena handles (ADR-0014 §5).
struct LaneAnchor {
  /// The arena id, for the CALLER's own use (framing, highlighting). It never
  /// reaches the `.xosc` — `road_odr_id` does.
  RoadId road;

  std::string road_odr_id;
  std::string lane_odr_id;
  double s = 0.0;

  /// Lateral offset from the LANE's centre line. Always 0 from
  /// `nearest_lane_anchor` — an actor belongs in the middle of its lane — but
  /// modeled so a later drag can offset within the lane without changing shape.
  double offset = 0.0;
};

/// The driving lane nearest (x, y), or nullopt when none is in reach.
///
/// Two distinct refusals, deliberately NOT collapsed into one:
///   * no road within `max_t` — the cursor is off the network entirely;
///   * a road, but no DRIVING lane at that station — the cursor is over a
///     sidewalk, a median or the centre lane.
/// Both return nullopt here; `actor_drop_hint` turns them into the message the
/// tool shows, because "no road in reach" would be a lie for the second.
///
/// Lane 0 is never returned: it is the centre lane, it carries no traffic, and
/// it has no width. Neither is a non-driving lane — placing a car on a sidewalk
/// is a placement a simulator would have to reject later, and refusing at the
/// click is the point (GW-6 step 2).
[[nodiscard]] std::optional<LaneAnchor>
nearest_lane_anchor(const RoadNetwork& network, double x, double y, double max_t = kActorSnapMaxT);

/// Why a drop at (x, y) was refused, as a sentence the tool can show. Empty
/// when a drop there WOULD succeed.
///
/// Separate from the resolver so the tool never has to invent wording, and so
/// the distinction above survives: a drop over a sidewalk says so rather than
/// claiming there is no road.
[[nodiscard]] std::string
actor_drop_hint(const RoadNetwork& network, double x, double y, double max_t = kActorSnapMaxT);

/// The lowest unused `<ScenarioObject @name>` with the archetype's stem:
/// `Car1`, `Car2`, … `@name` is the key every `entityRef` resolves through and
/// the writer refuses a duplicate, so uniqueness here is correctness and not
/// cosmetics.
[[nodiscard]] std::string next_actor_name(const osc::Scenario& scenario, std::string_view key);

/// The world pose a lane anchor projects to: {x, y, z} plus the heading of the
/// lane's travel direction.
///
/// ★ THE GHOST AND THE COMMITTED ACTOR BOTH GO THROUGH HERE, so what the user
/// sees under the cursor is where the actor lands. Two implementations would
/// drift, and the drift would only ever show as "the preview lied".
///
/// Heading follows the lane's DIRECTION OF TRAVEL, not the reference line: a
/// right-hand lane (negative id) travels with +s, a left-hand lane against it.
/// An actor facing backwards down its lane is the kind of error that renders
/// convincingly and simulates absurdly.
struct ActorPose {
  std::array<double, 3> position{};
  double heading = 0.0;
};

[[nodiscard]] std::optional<ActorPose> actor_world_pose(const RoadNetwork& network,
                                                        const osc::LanePosition& position);

/// `actor_world_pose` for an anchor, without going through a `<LanePosition>`.
[[nodiscard]] std::optional<ActorPose> actor_world_pose(const RoadNetwork& network,
                                                        const LaneAnchor& anchor);

/// The `<LanePosition>` an anchor authors.
[[nodiscard]] osc::LanePosition to_lane_position(const LaneAnchor& anchor);

} // namespace roadmaker::editor
