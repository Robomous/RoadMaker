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

/// OpenSCENARIO XML deserialization into the scenario model (p8-s1, issue
/// #245) — the twin of `roadmaker/xodr/reader.hpp`, holding the same contract:
/// a parse fails outright only on a structural problem, and every element the
/// parser did not model is a `Diagnostic` plus a preserved fragment, NEVER a
/// silent drop.
///
/// WHAT THE ROUND TRIP ACTUALLY GUARANTEES, stated here because the honest
/// claim is narrower than "byte-stable" and the wide one keeps being reached
/// for. `write_xosc` emits a canonical attribute order and an always-present
/// skeleton, so a foreign file is RE-CANONICALIZED the first time it is
/// written — quoting, whitespace and attribute order all normalize (fmt-s2's
/// caveat, #326). What holds is idempotence from the RoadMaker-authored form:
///
///     write_xosc(S) == write_xosc(parse_xosc(write_xosc(S)).scenario)
///
/// byte for byte, at every revision. That is the property the golden-workflow
/// replays fingerprint state with, and the same one `write_xodr` has had since
/// M2. `core/tests/test_xosc_reader.cpp` proves it both ways round.

#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace roadmaker::osc {

/// A successful parse: the document plus everything the parser had to warn
/// about.
struct XoscParseResult {
  Scenario scenario;
  std::vector<Diagnostic> diagnostics;

  /// `FileHeader/@revMajor` / `@revMinor`, 0 when absent.
  ///
  /// RECORDED HERE AND DELIBERATELY NOT IN THE MODEL. The writer re-derives
  /// both from `WriteOptions::target_version` so that the revision a file
  /// declares can never disagree with the content it carries
  /// (`osc/scenario.hpp:90-93`); routing them through `preserved.attributes`
  /// instead would make the writer emit each attribute TWICE. This mirrors
  /// `header_attribute_is_owned` on the OpenDRIVE side
  /// (`core/src/xodr/reader.cpp:316-319`).
  int rev_major = 0;
  int rev_minor = 0;
};

/// Parses an OpenSCENARIO XML document from an in-memory buffer.
///
/// Fails outright (`Expected` error) on exactly three structural problems:
/// malformed XML, a missing `<OpenSCENARIO>` root, and a root that declares a
/// catalog rather than a scenario (`<Catalog>` — a legal OpenSCENARIO document
/// this version has no model for, refused with its own error rather than
/// silently yielding an empty scenario).
///
/// Everything else is a `Diagnostic`: an unmodeled element is preserved
/// verbatim and reported, an unparseable number keeps its original spelling in
/// `preserved.attributes` rather than becoming a silent zero, and a missing
/// required element is reported and defaulted.
///
/// `source_name` appears in error contexts only.
[[nodiscard]] RM_API Expected<XoscParseResult>
parse_xosc(std::string_view xml_text, std::string_view source_name = "<memory>");

/// Reads and parses a `.xosc` file (binary mode; CRLF-safe).
///
/// Adds the two findings only a PATH can support, which is why they are not
/// reachable from `validate_scenario` (a `Scenario` knows neither its own file
/// name nor its directory):
///   - `general.file_ending` — "Scenario descriptions should have the file
///     extension `.xosc`."
///   - `reference_control.road_network_availability` — "If a `LogicFile` or
///     `SceneGraphFile` element is defined, the `filepath` attribute should
///     point to a resolvable file." Resolved relative to the document's own
///     directory, which is how a simulator resolves it and why an in-memory
///     parse cannot check it.
///
/// Both are advisories (`Severity::Warning`): the specification words them as
/// "should", and a scenario whose network is not yet on disk still loads.
[[nodiscard]] RM_API Expected<XoscParseResult> load_xosc(const std::filesystem::path& path);

} // namespace roadmaker::osc
