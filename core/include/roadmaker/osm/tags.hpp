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

#include "roadmaker/export.hpp"
#include "roadmaker/road/defaults.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/// OSM tag → road semantics. The governing spec document is
/// docs/domain/osm_mapping.md, and `core/tests/test_osm_mapping.cpp` fails CI
/// when the committed tables and the code table below disagree — the same
/// divergence mechanism realism_defaults.md uses.
///
/// **This table contains no metres.** It selects a `defaults::RoadClass` or
/// adds and removes whole lanes; every width comes from `roadmaker::defaults`.
/// A width literal here would fail `test_defaults_registry.cpp`'s
/// no-private-width-table gate, and correctly so.
namespace roadmaker::osm {

/// One row of the `highway=*` table.
struct HighwayMapping {
  std::string_view value;

  /// The road class this value imports as, or nullopt when it is dropped.
  std::optional<defaults::RoadClass> road_class;

  /// A ramp or slip road (`*_link`): takes its parent's class but narrows to a
  /// single driving lane per direction, because a ramp built to its parent's
  /// full cross section is wrong everywhere.
  bool link = false;

  /// One-way by OSM convention even with no `oneway` tag.
  bool implies_oneway = false;
};

/// The mapping table, in the committed doc's order. Values absent from it are
/// dropped with a diagnostic quoting the value verbatim, so an
/// unmapped-but-real classification is visible rather than merely missing.
[[nodiscard]] RM_API std::span<const HighwayMapping> highway_mappings();

/// The row for `value`, or nullptr when the value is not in the table.
[[nodiscard]] RM_API const HighwayMapping* highway_mapping(std::string_view value);

/// `highway` values this build recognises and deliberately does not import —
/// footways, tracks, construction and the like. Listing them (rather than
/// letting them fall through the unknown path) is what lets the diagnostic say
/// *"not a road"* instead of *"unrecognised"*, which are different facts.
[[nodiscard]] RM_API std::span<const std::string_view> dropped_highway_values();

/// Whether `value` is a recognised non-road `highway` classification.
[[nodiscard]] RM_API bool is_dropped_highway(std::string_view value);

/// How a tag key is treated. `Modeled` keys reach the network; `Dropped` keys
/// are read, counted and reported once per import with the number of ways
/// carrying each — a per-way diagnostic for `surface` on a 1 600-road district
/// is 1 600 rows that all say the same thing.
enum class TagUse : std::uint8_t { Modeled, Dropped };

struct TagMapping {
  std::string_view key;
  TagUse use = TagUse::Dropped;
  /// What the key does, for the committed table. Empty for dropped keys.
  std::string_view effect;
};

[[nodiscard]] RM_API std::span<const TagMapping> tag_mappings();

// --- parsing the individual tags -------------------------------------------

/// `oneway`: yes/true/1 and -1 are one-way; no/false/0 and absent are not.
/// `reversed` reports the `-1` case, which reverses the POLYLINE rather than
/// the lane direction — see osm_mapping.md §2 for why.
struct OneWay {
  bool one_way = false;
  bool reversed = false;
};

[[nodiscard]] RM_API OneWay parse_oneway(std::string_view value);

/// An OSM lane count, or nullopt when the value is not a plain positive
/// integer (OSM permits things like `lanes=1;2`). Clamped to `kMaxLanesPerSide`
/// * 2 so a typo cannot ask for a thousand-lane road.
inline constexpr int kMaxLanesPerSide = 8;
[[nodiscard]] RM_API std::optional<int> parse_lane_count(std::string_view value);

/// A parsed `maxspeed`, ready to become a `<speed max unit>` (§10.4.1).
///
/// OSM's own convention: a bare number is km/h and an imperial value says so.
/// `none` (the German autobahn) becomes the `t_maxSpeed` string literal
/// `no limit`, which is why `max` is the verbatim spelling here too and not a
/// double.
struct MaxSpeed {
  std::string max;  ///< the @max spelling: a number, or "no limit"
  std::string unit; ///< @unit: "km/h" | "mph"; empty alongside "no limit"
};

[[nodiscard]] RM_API std::optional<MaxSpeed> parse_maxspeed(std::string_view value);

/// `layer`, defaulting to 0. **Relative and unitless**: `layer=1` means "above
/// layer 0 *here*", not "five metres up", so it is used to PARTITION the
/// topology graph and never as an elevation. Two ways meeting at a node with
/// different layers are an overpass and the road beneath it.
[[nodiscard]] RM_API int parse_layer(std::string_view value);

/// Renders docs/domain/osm_mapping.md's `rm-osm: highway` table exactly as
/// committed.
[[nodiscard]] RM_API std::string highway_mapping_markdown();

/// Renders the `rm-osm: tags` table, same contract.
[[nodiscard]] RM_API std::string tag_mapping_markdown();

} // namespace roadmaker::osm
