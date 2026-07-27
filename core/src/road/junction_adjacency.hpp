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

// Internal (non-installed) helper: "are these two roads part of the same
// intersection?". Extracted from grade_separation.cpp so the grade-separation
// query and the prop-obstruction query (#464) share ONE answer. Both need it
// for the same reason — a pair of roads a junction connects is meeting by
// design, so neither an overpass nor an obstruction can be inferred from their
// geometry crossing.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <optional>

namespace roadmaker::road_detail {

/// A road's junction memberships. THREE slots, and all three matter: the
/// junction at the start end, the junction at the end end, and — for a
/// connecting road — the junction that owns it. Checking only `road.junction`
/// misses every approach arm, which is exactly the case that makes a corner
/// streetlight look like an obstruction.
using TouchedJunctions = std::array<std::optional<JunctionId>, 3>;

/// Junctions a road touches: the junction at either end plus, for a connecting
/// road, its owning junction. Two roads sharing any of these are "connected"
/// and can never form an overpass (design §4) nor obstruct one another (#464).
[[nodiscard]] inline TouchedJunctions
touched_junctions(const RoadNetwork& network, RoadId id, const Road& road) {
  TouchedJunctions out{};
  out[0] = edit::junction_at_end(network, RoadEnd{.road = id, .contact = ContactPoint::Start});
  out[1] = edit::junction_at_end(network, RoadEnd{.road = id, .contact = ContactPoint::End});
  if (road.junction.is_valid()) {
    out[2] = road.junction;
  }
  return out;
}

/// True when the two roads share any touched junction.
[[nodiscard]] inline bool junction_connected(const TouchedJunctions& a, const TouchedJunctions& b) {
  for (const std::optional<JunctionId>& ja : a) {
    if (!ja.has_value()) {
      continue;
    }
    for (const std::optional<JunctionId>& jb : b) {
      if (jb.has_value() && *jb == *ja) {
        return true;
      }
    }
  }
  return false;
}

/// True when `junction` is one the road touches — the single-junction form,
/// for asking whether a prop's anchor road is an arm of the floor it overlaps.
[[nodiscard]] inline bool touches_junction(const TouchedJunctions& a, JunctionId junction) {
  for (const std::optional<JunctionId>& ja : a) {
    if (ja.has_value() && *ja == junction) {
      return true;
    }
  }
  return false;
}

} // namespace roadmaker::road_detail
