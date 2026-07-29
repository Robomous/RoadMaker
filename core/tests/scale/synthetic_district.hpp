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

#include <cstddef>
#include <string>

namespace roadmaker::scale {

/// A synthetic OSM district, generated as `.osm` XML IN MEMORY.
///
/// Not a committed fixture, deliberately. The repository's largest committed
/// fixture is thirteen kilobytes and a 50 km² extract is tens of megabytes, so
/// this is generated at run time and never written to disk.
///
/// **That split is the honest part, and it is written down rather than
/// implied:** CORRECTNESS is tested against the small committed fixtures in
/// `core/tests/data/osm/`, which are real files with real bytes, because
/// (as `gen_osm_fixtures.py` says of its own output) a reader checked only
/// against bytes the same test just built agrees with itself about everything,
/// including its own mistakes. This generator's output asserts **timings
/// only**.
///
/// Placed at Amsterdam and deliberately NOT at (0, 0), for the same reason the
/// committed fixtures are: an origin-centred district hides every
/// false-easting and central-meridian mistake there is.
struct DistrictSpec {
  /// Blocks per side. 29 blocks at 250 m is 7.1 km × 7.1 km = **50.4 km²**,
  /// which is the size #54 named — and it yields **1 624 road segments** and
  /// **841 four-way junctions**, so ONE input satisfies both inherited targets
  /// rather than needing two.
  int blocks = 29;
  double block_spacing_m = 250.0;

  /// Arterials every 4th street, collectors every 2nd, locals elsewhere — so
  /// the mapping table is on the measured path rather than bypassed by a
  /// district made entirely of one class.
  int arterial_every = 4;
  int collector_every = 2;

  [[nodiscard]] double area_km2() const {
    const double side = (blocks - 1) * block_spacing_m;
    return (side * side) / 1'000'000.0;
  }

  /// Ways before splitting: one per row plus one per column.
  [[nodiscard]] std::size_t way_count() const { return static_cast<std::size_t>(2 * blocks); }

  /// Road segments after splitting at shared nodes.
  [[nodiscard]] std::size_t segment_count() const {
    return static_cast<std::size_t>(2 * blocks * (blocks - 1));
  }

  /// Nodes that become junctions: the interior crossings (degree 4) PLUS the
  /// non-corner edge nodes (degree 3, a T where a row meets a column's end).
  /// The four corners are degree 2 and become plain links, not junctions.
  [[nodiscard]] std::size_t junction_count() const {
    return static_cast<std::size_t>(((blocks - 2) * (blocks - 2)) + (4 * (blocks - 2)));
  }
};

/// The spec the bench runs, from a `--blocks=N` argument.
///
/// **Why this is configurable at all, stated rather than buried.** The full
/// 50 km² district is the size #54 named and the size `DistrictSpec`'s default
/// carries — but the BUILD step at that size is super-linear (#502) and takes
/// minutes, and a CI gate that risks its own timeout is not a gate. So the job
/// runs a smaller district and the full-size numbers are measured by hand
/// until #502 brings the cost down.
///
/// The alternative was to shrink the district permanently and report a
/// flattering number, which would have dodged the only question #54 asked.
///
/// A command-line flag rather than an environment variable: MSVC treats
/// `getenv`'s deprecation as an error under this project's warnings-as-errors,
/// and the knob is more visible in the CI step's own command line anyway.
[[nodiscard]] DistrictSpec spec_from_args(int argc, char** argv);

/// Renders the district as `.osm` XML.
///
/// The parse is deliberately ON the measured path: the bench feeds this string
/// through `osm::parse_osm` rather than constructing an `OsmGraph` directly, so
/// the number it reports is the one a user would feel.
[[nodiscard]] std::string synthetic_district_osm(const DistrictSpec& spec);

} // namespace roadmaker::scale
