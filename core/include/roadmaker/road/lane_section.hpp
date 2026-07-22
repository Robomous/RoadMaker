// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/road/id.hpp"

#include <vector>

namespace roadmaker {

/// Cross-section layout of a road, valid from s0 until the next section.
struct LaneSection {
  /// Owning road (back-reference).
  RoadId road;

  /// Section start along the road reference line [m].
  double s0 = 0.0;

  /// All lanes including the center lane 0, sorted by OpenDRIVE lane id
  /// DESCENDING: leftmost lane first, then down to the rightmost. Maintained
  /// by RoadNetwork::add_lane — do not reorder by hand.
  std::vector<LaneId> lanes;
};

} // namespace roadmaker
