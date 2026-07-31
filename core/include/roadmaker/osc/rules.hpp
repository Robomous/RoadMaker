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

/// ASAM OpenSCENARIO checker-rule UIDs cited by the scenario writer and
/// validator (p8-s1, issue #245).
///
/// The twin of `roadmaker/xodr/rules.hpp`, and deliberately a SEPARATE
/// namespace from `roadmaker::rules` (ADR-0014 §7): mixing `asam.net:xodr:`
/// and `asam.net:xosc:` UIDs in one namespace leaves no call-site spelling
/// that says which standard a rule belongs to.
///
/// UID grammar, identical to OpenDRIVE's:
///   <emanating-entity>:<standard>:<definition-setting>:<rule_set.rule_name>
/// The version component is THE REVISION THE RULE FIRST APPEARED IN, not the
/// revision of the document being checked — the convention `xodr/rules.hpp`
/// already states, and which the 1.4.0 text corroborates by citing 1.0.0,
/// 1.1.0 and 1.2.0 ids side by side.
///
/// ★ EVERY id in the 1.4.0 catalogue is stamped 1.0.0, 1.1.0 or 1.2.0 — NONE
/// at 1.3.0 or 1.4.0 (71 distinct ids in Annex C). That is why defaulting the
/// writer to revision 1.2 (ADR-0014 §3) forfeits no rule this file could
/// otherwise cite: the conservative default costs nothing.
///
/// WHAT IS AND IS NOT HERE. This file declares only rules something actually
/// cites — a rule id nothing cites is decoration. PR-B declared the five the
/// writer enforces; PR-C adds the five the READER makes checkable, three of
/// which fire from `load_xosc` rather than `validate_scenario` because they
/// need a path and a `Scenario` has none.
///
/// TWO RULES ARE DELIBERATELY ABSENT, and each would be wrong to cite:
///   - `asam.net:xosc:1.1.0:general.references_to_scenario_object` constrains
///     an `entityRef` target to be a `Vehicle` or a `Pedestrian`. A
///     `ScenarioObject` whose entity object is `std::monostate` may be a
///     `CatalogReference` that resolves to a vehicle, so citing this against
///     one would report legal input as a violation. The dangling-`entityRef`
///     finding therefore stays rule-less.
///   - `asam.net:xosc:1.0.0:data_type.time_format` says date-times use ISO 8601
///     "Basic Notation", while the schema type it constrains is
///     `xsd:dateTime`, which admits only the EXTENDED form. The schema wins
///     (`osc/scenario.hpp:81-85`); citing the rule would reject the very date
///     this writer emits.
///
/// Descriptions are quoted VERBATIM from ASAM OpenSCENARIO XML 1.4.0 Annex C.
/// The specification is not tracked in this repository (no redistribution
/// grant, unlike OpenDRIVE); fetch it with
/// `scripts/fetch_asam_specs.py --std openscenario`.

#pragma once

#include <string_view>

namespace roadmaker::osc::rules {

/// "Element names at each level shall be unique at that level. There shall be
/// no more than one element with the same name at the same level (within the
/// same directly enclosing element)."
///
/// Cited for a duplicate `ScenarioObject/@name` (the key every `entityRef`
/// resolves through) and a duplicate `TrafficSignalController/@name`.
/// Duplicate `Phase/@name`s are NOT cited here: the writer de-duplicates them
/// rather than refusing, because `roadmaker::SignalPhase::name` is legally
/// empty and a whole cycle must not become unwritable over a label.
inline constexpr std::string_view kUniqueElementNames =
    "asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level";

/// "The `::` shall not be used in names itself."
///
/// `::` is the parameter/catalog reference separator, so a name containing it
/// is ambiguous with a qualified reference.
inline constexpr std::string_view kNoDoubleColonPrefix =
    "asam.net:xosc:1.0.0:naming.no_double_colon_prefix_in_names";

/// "The attribute `duration` in the complex type `Phase` shall contain
/// non-negative values."
///
/// ★ NON-NEGATIVE, despite the rule's name: a zero-length phase is legal and
/// only a negative duration is refused. Reading the name instead of the text
/// yields a writer that rejects valid input.
inline constexpr std::string_view kPhaseDurationNonNegative =
    "asam.net:xosc:1.0.0:data_type.phase_duration_positive";

/// "The attribute `trafficSignalId` in `TrafficSignalState` shall reference a
/// valid element (element exists and of correct type) within the referenced
/// road network file."
///
/// The writer can only check that the id is present — whether it names a live
/// `<signal>` depends on the `.xodr` this scenario points at, which the writer
/// does not read. An EMPTY id is refused outright: it is the failure ADR-0014
/// §5 exists to prevent, a file that looks entirely right and references
/// nothing.
inline constexpr std::string_view kTrafficSignalStateReferences =
    "asam.net:xosc:1.0.0:reference_control.traffic_signal_state_references";

/// "The attribute `reference` in `TrafficSignalController` shall reference an
/// existing `TrafficSignalController` with the given `name` within the
/// scenario. The attribute `name` in `TrafficSignalController` can reference a
/// valid element (element exists and of correct type) within the referenced
/// road network file."
///
/// The second sentence is why `TrafficSignalController::name` carries the
/// OpenDRIVE controller `@id` and not a human-readable label. The first is
/// checkable in full here, since both ends are in the same document.
inline constexpr std::string_view kTrafficSignalControllerReferences =
    "asam.net:xosc:1.0.0:reference_control.traffic_signal_controller_references";

/// "The condition delay shall be non negative."
///
/// Cited for a negative `Condition/@delay`. `osc/scenario.hpp` named this rule
/// from the first commit, but nothing checked it until the reader made a
/// foreign delay reachable — a rule quoted in a comment and enforced nowhere is
/// the same decoration as an uncited constant, just harder to notice.
inline constexpr std::string_view kConditionDelayNonNegative =
    "asam.net:xosc:1.0.0:data_type.condition_delay_not_negative";

/// "ASAM OpenSCENARIO allows a road network to be linked. This is optional,
/// because the scenario is also valid without a defined road network. In most
/// cases, however, an unspecified road network will be a mistake that the user
/// would like to see highlighted. Therefore, in the `RoadNetwork` element a
/// `LogicFile` reference should be present."
///
/// ★ "SHOULD", and the rule's own text says a scenario without a road network
/// is VALID — so this is a `Severity::Warning` and never blocks a write. It is
/// also why an absent `<RoadNetwork>` is not reported against
/// `kValidSchema`: the standard explicitly permits the state.
inline constexpr std::string_view kRoadNetworkReference =
    "asam.net:xosc:1.0.0:reference_control.road_network_reference";

/// "If a `LogicFile` or `SceneGraphFile` element is defined, the `filepath`
/// attribute should point to a resolvable file."
///
/// LOAD-TIME ONLY. Resolution is relative to the scenario document's own
/// directory, which is how a simulator resolves it — so `load_xosc` can check
/// it and `parse_xosc` (a buffer, with no directory) and `validate_scenario`
/// (a `Scenario`, which stores no path) cannot.
inline constexpr std::string_view kRoadNetworkAvailability =
    "asam.net:xosc:1.0.0:reference_control.road_network_availability";

/// "Scenario descriptions should have the file extension `.xosc`."
///
/// Load-time only, for the same reason: it is a fact about the path.
inline constexpr std::string_view kFileEnding = "asam.net:xosc:1.0.0:general.file_ending";

/// "If no road network (`logicFile`) is defined in the scenario, the scenario
/// shall not use one of the following elements: `RoadPosition`, `LanePosition`,
/// `GeoPosition`, `LaneChangeAction`, `InfrastructureAction`,
/// `EndOfRoadCondition`, `OffroadCondition`, `TrafficSignalAction`,
/// `TrafficSignalCondition`, `TrafficSignalControllerCondition`,
/// `TrafficSignalController`."
///
/// ★ CHECKABLE IN FULL, unlike most reference rules here: both ends are inside
/// one `Scenario`. A `<LanePosition>` or a `<TrafficSignalController>` with no
/// `<LogicFile>` names a road network that the document itself never links, so
/// nothing can resolve the id — an actor that is nowhere, in a file that
/// otherwise looks complete.
///
/// This is a `Severity::Error` and it BLOCKS the write, which is the difference
/// between it and `kRoadNetworkReference` beside it: that one is a "should" the
/// standard explicitly permits violating, this one is a "shall not".
///
/// ★ CITED ONLY FOR POSITIONS SO FAR, not for the `<TrafficSignalController>`
/// arm of the same list. The controller arm is equally real, but p8-s1 shipped
/// controllers without it and enforcing it retroactively would make ~20 signal
/// tests — and any scenario authored against that behaviour — unwritable. It is
/// its own change with its own issue, not a side effect of p8-s2.
inline constexpr std::string_view kInvalidElementsIfNoRoadNetwork =
    "asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network";

/// "If a position is used, which refers to a road and/or lane of the road
/// network, then the given road id or lane id shall exist in the referenced
/// road network."
///
/// The writer can only check that the ids are PRESENT — whether they name a
/// live `<road>`/`<lane>` depends on the `.xodr` this scenario points at, which
/// the writer does not read. That is the same half-check
/// `kTrafficSignalStateReferences` makes, and for the same reason: an EMPTY id
/// is refused outright, because it is the ADR-0014 §5 failure — a file that
/// looks entirely right and references nothing.
inline constexpr std::string_view kRoadLaneExists =
    "asam.net:xosc:1.0.0:reference_control.road_lane_exists";

/// "If a `Position` is used, which refers to a road and/or lane of the road
/// network, then the given s-coordinate shall be within the boundaries of the
/// referenced road or lane, and the t-coordinate or offset should be within the
/// boundaries of the referenced road or lane."
///
/// Cited for a NEGATIVE `@s` only. The upper bound is the road's length, which
/// lives in the `.xodr` and not here; the lower bound is 0 in every road that
/// can exist ("Range: [0..inf[", §7.6), so a negative station is outside the
/// boundaries of any road the reference could resolve to.
inline constexpr std::string_view kRoadLaneOffsetInBounds =
    "asam.net:xosc:1.0.0:positioning.road_lane_offset_in_bounds";

/// "Based on the determined version of the checked file (Element `FileHeader`,
/// attributes `revMajor` and `revMinor`), it shall comply with the schema of
/// the detected version."
///
/// Cited by the READER, and only for the fragment of the schema a reader can
/// honestly check without one: a required element that is absent
/// (`<FileHeader>`, `<CatalogLocations>`, `<Entities>`, `<Storyboard>`,
/// `<Storyboard><Init>`). There is no XSD in this tree and CI cannot carry one
/// (the maintainer ruling on #257), so full schema validation is esmini's
/// answer, not this constant's — the two are additive, and neither replaces
/// the other.
inline constexpr std::string_view kValidSchema = "asam.net:xosc:1.0.0:xml.valid_schema";

} // namespace roadmaker::osc::rules
