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

/// Building the OpenSCENARIO traffic-signal model from a live road network
/// (p8-s1 PR-D, issue #245) — ADR-0014 §8, "the only genuinely new modelling"
/// in this sprint.
///
/// THE ONLY `osc/` HEADER THAT INCLUDES `road/`, and deliberately so.
/// `osc/scenario.hpp` models the FILE and stays free of every `road/` include,
/// so that a `Scenario` can be read, written and edited with no network in
/// hand. The decomposition is the one place that needs both, and confining the
/// coupling to this header is what keeps that true.
///
/// WHY A DECOMPOSITION AND NOT A COPY. RoadMaker stores a signal cycle
/// PER JUNCTION, across N controllers: one timeline whose every phase carries a
/// row of states, one per member controller. OpenSCENARIO has no such object.
/// Its `TrafficSignalController` has ONE `@name` and its OWN `Phase` list, and
/// §10.10 builds its signal groups from the ASAM OpenDRIVE `<controller>`
/// element in the specification's own words. So one junction timeline becomes
/// ONE `TrafficSignalController` PER MEMBER `<controller>`, each carrying the
/// same phase names and durations, differing only in the row of states it
/// holds.
///
/// Reference: ASAM OpenSCENARIO XML 1.4.0 §6.11, §10.10; ASAM OpenDRIVE
/// 1.8.1/1.9.0 §14.6 (which excludes the cycle from its own scope and points
/// at OpenSCENARIO for it). The OpenSCENARIO text is NOT tracked here — no
/// redistribution grant, unlike OpenDRIVE; fetch it with
/// `scripts/fetch_asam_specs.py --std openscenario`.

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <string_view>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osc {

/// The `SignalState` -> `TrafficSignalState/@state` spelling.
///
/// ★ ENGINE-DIRECTED AND VALIDATED BY NOTHING, which is measured rather than
/// assumed. The specification leaves the token open (§10.10: a signal modelled
/// as a whole box carries a COMPOSITE value such as "on;off;off", one modelled
/// per light bulb carries "on"/"off", and the OpenDRIVE signal's `@type`
/// decides which — the worked example uses no colour words at all). And the
/// engine CI pins has no opinion either: running the tracked fixture through
/// esmini v3.5.0 with `state="wibble"` produced a BYTE-IDENTICAL log and exit
/// 0, including with a `TrafficSignalControllerAction` in a story genuinely
/// executing against the controller (p8-s1 PR-C, #525).
///
/// So the choice rests on RoadMaker's own semantics: `SignalState` is a colour
/// enum and these are its colours, which is also what esmini's own example
/// scenarios use. It is ONE FUNCTION WIDE on purpose — reversing it is one
/// edit — and it is recorded as UNVALIDATED, not as proven.
///
/// Total over the enum by a `switch` with no `default`, not a chain of
/// conditionals: a fifth `SignalState` must break the build here rather than
/// silently export as one of the four (the `gl_renderer` two-way-ternary
/// lesson, p7-s3).
[[nodiscard]] RM_API std::string_view state_token(SignalState state);

/// What `decompose_junction_signals` produced, and everything it had to say
/// about the network while producing it.
///
/// Findings and controllers are returned TOGETHER rather than the function
/// failing: a junction with one stale signal head still has a cycle worth
/// exporting, and dropping the whole decomposition over it would be exactly
/// the silent-loss failure the never-drop contract forbids. Nothing here is
/// `Severity::Error` — every finding names something the export coped with.
struct JunctionSignalDecomposition {
  /// One controller per member `<controller>`, SORTED BY `@name` (the
  /// OpenDRIVE `@id`). Empty when the junction has no cycle to export, which
  /// is always accompanied by a finding saying so.
  std::vector<TrafficSignalController> controllers;

  std::vector<Diagnostic> findings;
};

/// One junction's signal timeline as OpenSCENARIO controllers (ADR-0014 §8).
///
/// Reads the `JunctionPhasePlan` from `junction_phases()`, NEVER
/// `Junction::phases` directly. The stored form is sparse and RED BY OMISSION
/// (`road/junction.hpp:324-326`) — a phase lists only its non-Red controllers,
/// so an all-red clearance phase stores an empty list. Exported from the raw
/// storage, a signal RoadMaker shows as red would appear in the file as a
/// signal that is NEVER red, in a document whose every count still looks
/// right. The plan is the Red-filled form, and it is the only correct input.
///
/// Each `TrafficSignalState/@trafficSignalId` is resolved through
/// `network.signal(id)->odr_id`. A `SignalId` is a generational arena handle —
/// runtime-only, never valid across a load (ADR-0014 §5) — so writing one
/// produces a file that looks entirely right and references nothing. A handle
/// that no longer resolves, or a signal with an empty `@id`, is a finding and
/// is omitted; it never becomes an empty attribute.
///
/// Returns an empty controller list plus a finding for a junction with no
/// cycle: a stale `JunctionId`, a span (virtual) junction, an unsignalized
/// junction, and one signalized only by a STATIC template.
///
/// ONE KNOWN LIMIT, stated rather than left to be discovered. A head that TWO
/// member controllers both `<control>` is resolved by the plan exactly once,
/// first-wins (`mesh/junction_phases.cpp:182-187`), because a physical head
/// shows one colour. It is therefore exported under BOTH groups carrying that
/// single resolved state. Omitting it from the second would leave that
/// controller a phase naming fewer heads than it controls — breaking the dense
/// contract §8 exists for — and inventing a second colour would be a claim
/// about the hardware that RoadMaker cannot make.
[[nodiscard]] RM_API JunctionSignalDecomposition
decompose_junction_signals(const RoadNetwork& network, JunctionId junction);

} // namespace roadmaker::osc
