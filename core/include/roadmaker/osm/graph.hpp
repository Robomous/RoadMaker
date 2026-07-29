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
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/// OpenStreetMap ingest — ASAM-side conversion lives in `osm/network_plan.hpp`.
///
/// Format scope is ADR-0012: the `.osm` XML interchange format is read here
/// with the pugixml this project already links, and `.osm.pbf` is refused by
/// name because every real one stores its blobs zlib-deflated and zlib is not
/// a dependency (it arrives with #484; the reader itself is #494). The
/// Protocol Buffers half was never the obstacle.
namespace roadmaker::osm {

/// Guardrails, in the style of `lidar::kMaxCloudPoints` and
/// `gis::kMaxVectorVertices`. A file over budget is REFUSED with the numbers
/// named, never silently truncated — a district that quietly stopped halfway
/// looks exactly like a small district.
inline constexpr std::size_t kMaxNodes = 4ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxWays = 512ULL * 1024ULL;

using OsmId = std::int64_t;

struct OsmNode {
  double lon_deg = 0.0;
  double lat_deg = 0.0;
};

struct OsmWay {
  OsmId id = 0;
  std::vector<OsmId> refs;
  /// (key, value) in document order.
  std::vector<std::pair<std::string, std::string>> tags;

  /// The value of `key`, or an empty view when the way does not carry it.
  [[nodiscard]] RM_API std::string_view tag(std::string_view key) const;
  [[nodiscard]] RM_API bool has_tag(std::string_view key) const;
  /// A closed ring (an area outline, or a roundabout drawn as one way).
  [[nodiscard]] RM_API bool closed() const;
};

struct OsmGraph {
  std::unordered_map<OsmId, OsmNode> nodes;
  std::vector<OsmWay> ways;

  /// {min_lon, min_lat, max_lon, max_lat} in degrees — from the file's own
  /// `<bounds>` when it has one, otherwise from the nodes it actually carries.
  std::array<double, 4> bounds{};

  /// Always "EPSG:4326". Carried as data rather than assumed so this importer
  /// makes the SAME `gis::crs_transform` call the GIS and lidar importers do —
  /// a reader that hardcoded the transform would be the one importer that
  /// could disagree with the other two about where a place is.
  std::string crs = "EPSG:4326";

  /// What the FILE contained, before any filtering. `ways` holds only what
  /// survived `OsmReadOptions`; these two say how much did not.
  std::size_t source_node_count = 0;
  std::size_t source_way_count = 0;

  /// `<relation>` elements, counted while parsing and never used. Reported, so
  /// a user who mapped turn restrictions is told directly that they did not
  /// survive rather than being left to notice.
  std::size_t relation_count = 0;
  std::size_t turn_restriction_count = 0;

  [[nodiscard]] bool empty() const { return ways.empty(); }
};

struct OsmParseResult {
  OsmGraph graph;
  std::vector<Diagnostic> diagnostics;
};

struct OsmReadOptions {
  std::size_t max_nodes = kMaxNodes;
  std::size_t max_ways = kMaxWays;

  /// Keep ways carrying no `highway` tag. Off by default: a district's
  /// buildings, waterways and landuse outnumber its roads several times over,
  /// and this sprint imports centrelines only.
  bool keep_non_highway_ways = false;
};

/// Reads a `.osm` XML file. Refuses `.osm.pbf` by name, citing #494.
[[nodiscard]] RM_API Expected<OsmParseResult> load_osm(const std::filesystem::path& path,
                                                       const OsmReadOptions& options = {});

/// The same parse over an in-memory document.
///
/// Exists for two callers that matter: the scale harness, which generates a
/// 50 km² district rather than committing forty megabytes of it, and a
/// fuzz target, which wants an entry point that touches no filesystem.
[[nodiscard]] RM_API Expected<OsmParseResult>
parse_osm(std::string_view xml, std::string_view source_name, const OsmReadOptions& options = {});

/// Whether the extension is one `load_osm` reads. Separate from the reader so
/// a file-dialog filter and the reader cannot disagree about what is openable.
[[nodiscard]] RM_API bool is_osm_extension(const std::filesystem::path& path);

} // namespace roadmaker::osm
