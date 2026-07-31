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
/// WHAT IS AND IS NOT HERE. p8-s1 declares only rules this writer actually
/// enforces. `general.file_ending`, `reference_control.road_network_reference`
/// and the other `should` advisories are not blocking findings, and
/// `xml.valid_schema` is not writer-checkable at all; declaring them now would
/// leave dead constants, and a rule id nothing cites is decoration. The reader
/// and the full validator (p8-s1 PR-C) extend this list as they gain checks.
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

} // namespace roadmaker::osc::rules
