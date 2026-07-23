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

#include "roadmaker/road/id.hpp"

#include <string>
#include <vector>

namespace roadmaker {

/// Where a Surface's boundary comes from. P2 ships only Derived (auto-formed
/// from the roads that enclose an area); P5 adds Authored (node-graph).
enum class BoundarySource { Derived };

/// A ground surface filling an area enclosed by roads (#215, GW-2 step 5).
/// In P2 the boundary is DERIVED: `bounding_roads` is the ordered ring of
/// roads whose inner edges enclose the region, recomputed by derive_surfaces.
struct Surface {
  BoundarySource source = BoundarySource::Derived;
  std::vector<RoadId> bounding_roads; ///< ordered ring; deterministic

  /// Ground material name ("" = default grass; e.g. "asphalt", "concrete").
  /// A derived surface can never carry a lane-level OpenDRIVE <material>, so
  /// this is its permanent home (p6-s2). Round-trips as a `material` attribute
  /// on the surface's `<userData code="rm:surface">`, written only when set.
  std::string material;

  friend bool operator==(const Surface&, const Surface&) = default;
};

} // namespace roadmaker
