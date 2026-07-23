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

// The height-field sidecar (p5-s2, issue #232 — maintainer decision D2).
//
// OpenDRIVE has no terrain carrier, and a grid blob does not belong in
// <userData>: ADR-0008's Layer 1 is for SPARSE enrichment. The field is
// Layer-2 scene data, so until fmt-s1 (#325) delivers a native container it
// lives in a sidecar file beside the .xodr, referenced from a Layer-1
// <userData code="rm:terrain"> element. When the container lands, the sidecar
// moves inside it unchanged.
//
// The format is the ESRI ASCII grid (`.asc`) that decision D1 already commits
// p5-s4's DEM ingest to, so ONE reader and ONE writer serve both the sidecar
// and imported DEMs, and p5-s4 inherits this code rather than growing a second
// grid format.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {

/// Successful sidecar parse: the field plus everything the reader had to warn
/// about (NODATA cells coerced to 0, a short final row, unknown header keys).
/// Mirrors XodrParseResult — nothing is ever dropped silently.
struct TerrainParseResult {
  HeightField field;
  std::vector<Diagnostic> diagnostics;
};

/// The value written for a cell with no data. Only ever produced on WRITE for
/// the header line — a RoadMaker field has a height at every post — but read
/// back and coerced to 0.0 (with a diagnostic) so foreign DEMs load.
inline constexpr double kAscNoData = -9999.0;

/// Serializes the field as an ESRI ASCII grid.
///
/// Row order is the format's, NORTH first (high y), which is the reverse of
/// HeightField::heights (low y first) — the flip happens here and in the
/// parser, and nowhere else. `xllcorner`/`yllcorner` are the grid's lower-left
/// post, i.e. exactly origin_x/origin_y.
///
/// Values use the same shortest-round-trip spelling as the .xodr writer, so
/// the text is stable and the save → load → save round-trip is byte-identical.
/// Refuses an empty or malformed field (that is the caller's bug: a scene with
/// no terrain writes no sidecar at all).
[[nodiscard]] RM_API Expected<std::string> write_terrain_asc(const HeightField& field);

/// Parses an ESRI ASCII grid from memory. Fails outright (Expected error) only
/// on a structurally unusable file — a missing or non-numeric header key, a
/// non-positive cellsize, or fewer value rows than `nrows` promises.
[[nodiscard]] RM_API Expected<TerrainParseResult>
parse_terrain_asc(std::string_view text, std::string_view source_name = "<memory>");

/// write_terrain_asc + save to disk (binary mode, '\n' line endings, matching
/// save_xodr so the two files agree on line endings across platforms).
[[nodiscard]] RM_API Expected<void> save_terrain_asc(const HeightField& field,
                                                     const std::filesystem::path& path);

/// Reads and parses a sidecar file (binary mode; CRLF-safe).
[[nodiscard]] RM_API Expected<TerrainParseResult>
load_terrain_asc(const std::filesystem::path& path);

/// True when `reference` is safe to use as a sidecar name relative to the
/// document's directory: non-empty, not absolute, no directory separators, and
/// no ".." component. A reference that fails this is data from a foreign file
/// and must never be resolved against the filesystem — the reader keeps it (so
/// the round-trip stays honest) but refuses to open it.
[[nodiscard]] RM_API bool is_safe_sidecar_reference(std::string_view reference);

} // namespace roadmaker
