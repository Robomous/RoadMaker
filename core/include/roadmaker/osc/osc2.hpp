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

/// ASAM OpenSCENARIO **DSL v2.2.0** export — a documented concrete-scenario
/// subset, emitted from the same internal model that writes 1.x (p8-s6, #327).
///
/// ★ EXPORT ONLY, AND THAT IS THE WHOLE DESIGN. There is no `.osc` reader, no
/// grammar, no parser dependency and no plan for one; the round trip is via the
/// internal model and the project container (ADR-0008), never via re-importing
/// OSC 2.x. A future parser dependency goes through the dependency policy's
/// stop-and-ask before it is even prototyped.
///
/// ★ AND IT IS A DIFFERENT LANGUAGE, not a different serialization of the same
/// one. OpenSCENARIO XML 1.x is XML; OpenSCENARIO DSL 2.x is a Python-style
/// textual DSL with significant indentation, `#` comments and the `.osc`
/// extension. Nothing in `osc/writer.hpp` is reusable here beyond the model it
/// reads.
///
/// WHAT "CONCRETE" MEANS HERE IS THE STANDARD'S OWN DEFINITION (§6.3.1.2.1):
/// *"a scenario for which the exact evaluation of any of its parameters are
/// completely determined to a fixed value for any point in time"*. RoadMaker's
/// model holds exactly that — a named map, named actors, fixed speeds — and
/// holds nothing that could express a logical or abstract one, which is why the
/// subset is concrete rather than a choice between levels.
///
/// ★ THE SUBSET IS NARROW ON PURPOSE, AND ITS EDGE IS REPORTED RATHER THAN
/// SILENT. Every construct emitted here appears in the specification's own
/// normative text; anything the model can hold and this subset cannot express
/// produces a `Diagnostic` naming it (`validate_osc2_subset`). A wider emitter
/// guessed at modifier signatures would be worse than a small correct one — the
/// output has no schema, no parser and no simulator in CI to contradict it, so
/// nothing but the specification and this file's own honesty stands behind it.
///
/// Reference: ASAM OpenSCENARIO DSL v2.2.0 — §6.1 (writing scenarios), §6.3.1
/// (abstraction levels, and the concrete-scenario example this emitter is
/// shaped after), §7.7.5.2 (importing the standard library), §8.7.7 (the
/// `vehicle` actor), §8.x physical types (unit literals). The specification is
/// NOT tracked in this repository and must never be: like OpenSCENARIO XML it
/// carries no redistribution grant (third_party/asam/README.md). It is
/// published openly and can be read online.

#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker::osc {

/// The DSL edition this emitter targets, as the standard spells it.
///
/// NAMED RATHER THAN "2.x": the edition matters more for the DSL than for the
/// XML standard, because the language itself changed between 2.0, 2.1 and 2.2 —
/// so "OpenSCENARIO 2.x export" without a version is not a claim anyone can
/// check.
inline constexpr std::string_view kOsc2Version = "2.2.0";

/// The file extension DSL files take (§7.7.5.1: *"this differentiates such
/// files from the XML-based … files using the .xosc file extension"*).
inline constexpr std::string_view kOsc2Extension = ".osc";

/// One row of the documented subset.
///
/// ★ THIS ARRAY IS THE SOURCE OF TRUTH AND THE DOC MIRRORS IT.
/// `docs/domain/openscenario.md`'s OpenSCENARIO 2.x tables are checked against
/// these two spans by `Osc2Subset.TheCommittedDocMatchesTheRegistry`, so a
/// construct added to the emitter without a doc row — or a doc row for
/// something the emitter does not emit — fails CI rather than review. The
/// `test_defaults_registry.cpp` mechanism (#413), applied to a second
/// document.
struct Osc2SubsetRow {
  /// The DSL construct, exactly as the doc's first column names it.
  std::string_view construct;
  /// What in the `.xosc` model produces it.
  std::string_view source;
};

/// What the emitter writes, in emission order.
[[nodiscard]] RM_API std::span<const Osc2SubsetRow> osc2_supported();

/// What it deliberately does not, each with the reason the doc gives.
[[nodiscard]] RM_API std::span<const Osc2SubsetRow> osc2_unsupported();

/// Options for `write_osc2`.
struct Osc2WriteOptions {
  /// The scenario's declared name. `top` is the conventional entry point.
  std::string scenario_name = "top";
};

/// Everything in `scenario` that the documented subset cannot express.
///
/// Never empty-by-luck: this walks the model looking for content, so a scenario
/// carrying a storyboard, a route or a signal controller says so. Findings are
/// `Severity::Warning` — the 2.x file is a lossy, export-only view by
/// definition, and refusing to write one because it is lossy would make the
/// feature unusable. `write_osc2` returns them too.
[[nodiscard]] RM_API std::vector<Diagnostic> validate_osc2_subset(const Scenario& scenario);

/// Emits the concrete-scenario subset as OpenSCENARIO DSL text.
///
/// Deterministic, like `write_xosc`: no clock, no locale-dependent formatting,
/// every collection in vector order, so two calls on one `Scenario` return
/// byte-identical strings.
///
/// Refuses, with `Error{code = InvalidArgument}`, only what it cannot name: a
/// scenario with no entities at all (a concrete scenario with no actor
/// describes nothing), and an entity whose name cannot be rendered as a DSL
/// identifier without colliding with another one.
[[nodiscard]] RM_API Expected<std::string> write_osc2(const Scenario& scenario,
                                                      const Osc2WriteOptions& options = {});

/// write_osc2 + save to disk (binary mode, '\n' line endings).
///
/// ★ INDENTATION IS SYNTAX in this language, so the newline policy is not
/// cosmetic the way it is for XML: a file rewritten with CRLF by a careless
/// tool is a file whose block structure a strict reader may reject. Written in
/// binary mode for exactly that reason, the same way `save_xosc` is.
[[nodiscard]] RM_API Expected<void> save_osc2(const Scenario& scenario,
                                              const std::filesystem::path& path,
                                              const Osc2WriteOptions& options = {});

} // namespace roadmaker::osc
