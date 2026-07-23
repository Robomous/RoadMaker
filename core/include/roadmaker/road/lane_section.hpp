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
