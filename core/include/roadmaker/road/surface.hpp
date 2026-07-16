#pragma once

#include "roadmaker/road/id.hpp"

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
  friend bool operator==(const Surface&, const Surface&) = default;
};

} // namespace roadmaker
