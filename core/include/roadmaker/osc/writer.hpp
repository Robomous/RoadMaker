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

/// OpenSCENARIO XML serialization for the scenario model (p8-s1, issue #245).
///
/// The twin of `roadmaker/xodr/writer.hpp`, deliberately down to the shape of
/// its API: a revision-targetable options struct, a non-blocking validator
/// beside the writer, a buffer form and a path form.

#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace roadmaker::osc {

/// OpenSCENARIO revisions the writer can target.
///
/// THE DEFAULT IS THE OLDER ONE, exactly as `XodrVersion` defaults to v1_8_1
/// (`xodr/writer.hpp:39-48`) — and here the reason is concrete rather than
/// conservative habit. Validation for this project means "esmini accepts the
/// file" (ADR-0014 §3, the maintainer ruling on #257), and CI pins esmini
/// v3.5.0; emitting a newer revision by default would make p8-s1's own
/// acceptance gate unreachable without first bumping the pinned validator.
///
/// The choice costs nothing in citable rules: every checker-rule id in the
/// 1.4.0 catalogue is stamped 1.0.0, 1.1.0 or 1.2.0 and none 1.3.0 or 1.4.0
/// (`roadmaker/osc/rules.hpp`). What 1.2 forfeits is exactly one attribute,
/// `Phase/@semantics`, created in 1.4.0 — and since `Phase/@name` is required
/// and synthesized anyway, naming a phase for its semantic is both revisions'
/// answer.
enum class OscVersion {
  v1_2,
  v1_4,
};

/// Serialization options.
struct WriteOptions {
  OscVersion target_version = OscVersion::v1_2;
};

/// Checker-rule validation against the OpenSCENARIO catalogue.
///
/// Every finding cites the normative rule UID (`roadmaker/osc/rules.hpp`) when
/// one exists, and carries an empty `rule_id` when the finding is a
/// RoadMaker-side structural limitation with no rule behind it — the same
/// split `validate_network` uses (`xodr/writer.hpp:66-75`).
///
/// Findings never block writing on their own; `write_xosc` refuses only what
/// it cannot serialize, and that refusal is defined in terms of the
/// `Severity::Error` findings this function returns.
[[nodiscard]] RM_API std::vector<Diagnostic> validate_scenario(const Scenario& scenario,
                                                               const WriteOptions& options = {});

/// Serializes the scenario as OpenSCENARIO XML targeting
/// `options.target_version` (1.2 by default).
///
/// Refuses, with `Error{code = InvalidArgument}`, a scenario that would
/// produce structurally invalid OpenSCENARIO — every `Severity::Error` finding
/// of `validate_scenario`, of which the load-bearing ones are:
///   - an entity with an empty or duplicate `@name` (every `entityRef`
///     resolves through it)
///   - an `entityRef` naming no entity in this scenario
///   - a `TrafficSignalState` with an empty `@trafficSignalId`
///   - a phase with a negative `@duration` (zero is legal)
///   - a `@reference` naming no controller, or a `@delay` without one
///   - a preserved fragment that is not well-formed XML
///
/// That last one is a deliberate strengthening of the OpenDRIVE writer, which
/// ignores the parse status when re-emitting a preserved fragment and can
/// therefore drop one silently — the opposite of the never-drop contract both
/// formats are held to (ADR-0014 §6).
///
/// Output is deterministic: no clock is read, no locale-dependent formatting
/// is used, and every collection is emitted in vector order. Two calls on one
/// `Scenario` return byte-identical strings, which is what lets a scenario
/// take part in the same undo/redo fingerprinting `write_xodr` already
/// supports.
[[nodiscard]] RM_API Expected<std::string> write_xosc(const Scenario& scenario,
                                                      const WriteOptions& options = {});

/// write_xosc + save to disk (binary mode, '\n' line endings).
///
/// Unlike `save_xodr` there is no sidecar and no document-name argument:
/// `<FileHeader>` already models `@description` and `@author`, so a name
/// parameter would be a second source of truth for a field the model owns.
[[nodiscard]] RM_API Expected<void> save_xosc(const Scenario& scenario,
                                              const std::filesystem::path& path,
                                              const WriteOptions& options = {});

} // namespace roadmaker::osc
