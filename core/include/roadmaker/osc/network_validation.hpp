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

/// The scenario checks that need the ROAD NETWORK (issue #533).
///
/// ★ WHY THIS EXISTS: THE SIGNAL HALF OF A SCENARIO WAS CHECKED BY NOBODY.
///
/// Three checkers see a scenario and each sees a different slice:
///
///   * `osc::validate_scenario` (`osc/writer.hpp`) takes a `Scenario` ALONE.
///     Every reference whose two ends are inside the document it resolves in
///     full; every reference whose other end is in the `.xodr` it can only
///     check for emptiness. `osc/rules.hpp` says so twice, on
///     `kTrafficSignalStateReferences` and on `kRoadLaneExists`.
///   * esmini, the external gate CI runs, resolves lane anchors against the
///     `.xodr` — and was MEASURED (2026-07-30 and 2026-08-01, on the pinned
///     v3.5.0, recorded on #257 and in `.github/workflows/ci.yml`) to accept a
///     dangling `trafficSignalId`, a garbage `@state`, a dangling
///     `trafficSignalControllerRef` and a nonexistent `@phase` **in complete
///     silence**: exit 0, no error line, a byte-identical log.
///   * This function, which closes exactly that gap.
///
/// So a scenario whose traffic-light half references nothing at all used to
/// export clean, load clean, and be wrong — GW-6's pass criterion *"a
/// traffic-light condition references a controller that exists in the exported
/// `.xodr`, by the same id"* had no automated check anywhere.
///
/// NOTHING HERE BLOCKS A WRITE, and that is structural rather than a policy
/// choice: `write_xosc` takes no `RoadNetwork`, so it cannot run these checks
/// even if it wanted to. A `Severity::Error` here means "this reference
/// resolves to nothing in the network you gave me", and a caller decides what
/// to do about it — the editor surfaces it in the Diagnostics dock, a Python
/// replay reads the list.
///
/// ONE CALL IS THE WHOLE CROSS-DOCUMENT CHECK. `validate_routes`
/// (`osc/route.hpp`) is the route-shaped half and predates this; it stays
/// public because the route overlay needs only that half, but
/// `validate_scenario_against_network` runs it too, so no caller has to
/// remember both.

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osc {

/// Every reference in `scenario` whose other end lives in `network`, checked.
///
/// Empty when the scenario resolves completely. Findings cite the ASAM
/// checker-rule UID (`osc/rules.hpp`) whose *other half* `validate_scenario`
/// already reports:
///
///   * `TrafficSignalState/@trafficSignalId`, `TrafficSignalCondition/@name`
///     and `TrafficSignalStateAction/@name` -> a live `<signal @id>`
///     (`reference_control.traffic_signal_state_references`);
///   * `TrafficSignalController/@name`, and the
///     `@trafficSignalControllerRef` of a controller condition or action ->
///     a live `<controller @id>`
///     (`reference_control.traffic_signal_controller_references`);
///   * every modeled `<RoadPosition>`/`<LanePosition>` -> a live road, a live
///     lane in the section governing its `s`, and an `s` within the road
///     (`reference_control.road_lane_exists`,
///     `positioning.road_lane_offset_in_bounds`);
///   * every assigned `<Route>`, via `validate_routes`.
///
/// ★ THE UPPER s-BOUND IS THE POINT OF THE POSITION CHECK.
/// `validate_scenario` already refuses a NEGATIVE `s` — the lower bound is 0 on
/// every road that can exist, so it needs no network. The upper bound is the
/// road's own length, which lives in the `.xodr`; this is the only place it can
/// be checked, and esmini was measured to truncate rather than refuse it.
///
/// WHAT IS DELIBERATELY NOT CHECKED. `AbsoluteTargetLane/@value` names a lane
/// on whatever road the entity happens to be on AT RUNTIME, which no static
/// check can know — reporting it against the entity's initial road would fire
/// on correct scenarios. `TrafficSignalState/@state`'s spelling is
/// engine-directed by §10.10 and has no closed range to check against.
[[nodiscard]] RM_API std::vector<Diagnostic>
validate_scenario_against_network(const Scenario& scenario, const RoadNetwork& network);

} // namespace roadmaker::osc
