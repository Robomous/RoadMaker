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

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace roadmaker {

/// OpenDRIVE versions the writer can target. The header carries only
/// revMajor/revMinor (1.8.1 §6.4.1, 1.9.0 §6.4.1), so both spec patch
/// levels serialize as revMajor="1" with revMinor 8 or 9.
enum class XodrVersion {
  v1_8_1,
  v1_9_0,
};

/// Serialization options. The 1.8.1 default is the slim-fidelity M2
/// scope (docs/design/m2/00_overview.md §5); 1.9.0 is selectable for
/// consumers that validate against the newer checker-rule catalog.
struct WriterOptions {
  XodrVersion target_version = XodrVersion::v1_8_1;

  /// validate_network warns when a road's elevation grade |dz/ds| exceeds
  /// this anywhere (hardening sprint workstream C — advisory, no ASAM rule
  /// id; 0.12 = 12 %). Non-positive disables the check.
  double max_grade_warning = 0.12;
};

/// Checker-rule validation against the target version's catalog.
///
/// Every finding is a Diagnostic citing the normative rule UID
/// (roadmaker/xodr/rules.hpp) when one exists. Rules present in only one
/// version's catalog are cited only when targeting that version (e.g.
/// junctions.common.not_only_two appears in the 1.9.0 Annex F catalog but
/// not in 1.8.1 Annex E). Findings never block writing — callers decide;
/// the writer refuses only networks it cannot serialize (see write_xodr).
[[nodiscard]] RM_API std::vector<Diagnostic> validate_network(const RoadNetwork& network,
                                                              const WriterOptions& options = {});

/// Serializes the network as OpenDRIVE XML targeting
/// options.target_version (1.8.1 default).
///
/// Validates before writing — a network that would produce structurally
/// invalid OpenDRIVE is refused (Error, code InvalidArgument) rather than
/// written:
///   - every road has plan-view geometry and at least one lane section
///   - geometry records are contiguous (G1 position/heading within rm::tol)
///   - lane links reference lanes that exist in the neighboring section
///
/// Output is deterministic (no timestamps) so round-trip tests and version
/// control stay stable.
[[nodiscard]] RM_API Expected<std::string> write_xodr(const RoadNetwork& network,
                                                      std::string_view document_name = "roadmaker",
                                                      const WriterOptions& options = {});

/// write_xodr + save to disk (binary mode, '\n' line endings).
[[nodiscard]] RM_API Expected<void> save_xodr(const RoadNetwork& network,
                                              const std::filesystem::path& path,
                                              std::string_view document_name = "roadmaker",
                                              const WriterOptions& options = {});

} // namespace roadmaker
