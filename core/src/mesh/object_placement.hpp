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

// Internal (non-installed) helper: where an <object>'s instances actually land
// in the world. Extracted from mesh_builder.cpp's build_object_instances so the
// mesher and the prop-obstruction query (#464) cannot disagree about which
// instances exist or where they are — a query that flagged an instance the
// renderer never draws, or missed one it does, would be reporting about a
// picture the user is not looking at.
//
// A placed object stores NO world pose: its transform is derived from its
// road's frame every time (see mesh_builder.hpp's remesh_object_instances).
// This header is that derivation, and the only copy of it.

#include "roadmaker/road/object.hpp"
#include "roadmaker/road/repeat_expansion.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mesh_detail.hpp"

namespace roadmaker::object_placement {

/// One placement an object emits: its world pose plus the road-relative origin
/// it came from. `index` counts placements in emission order, so a caller can
/// name "the third tree of this line" without re-deriving the series.
struct PlacedInstance {
  std::size_t index = 0;
  std::array<double, 3> position{}; ///< world xyz [m], the instance's base
  double heading = 0.0;             ///< world heading [rad] about +Z
  double s = 0.0;                   ///< road-relative station of THIS instance
  double t = 0.0;                   ///< road-relative lateral offset [m]
};

/// Every instance `object` places on `road`, in emission order.
///
/// §13.4: a <repeat> with @distance > 0 places a SERIES and "the <repeat>
/// element takes precedence" over the object's own s/t/hdg, so the base single
/// instance is SUPPRESSED once any such repeat is present. A continuous repeat
/// (@distance == 0) expands to no discrete instance, and an object with only
/// continuous — or no — repeats falls through to the single-instance path.
///
/// Instance origins past the road end are skipped: a repeat section may legally
/// overshoot, but no instance origin should fall off the reference line.
[[nodiscard]] inline std::vector<PlacedInstance> placed_instances(const Road& road,
                                                                  const Object& object) {
  std::vector<PlacedInstance> out;

  const bool has_series_repeat =
      std::any_of(object.repeats.begin(), object.repeats.end(), [](const ObjectRepeat& repeat) {
        return repeat.distance > 0.0;
      });

  if (has_series_repeat) {
    const double road_length = road.plan_view.length();
    for (const ObjectRepeat& repeat : object.repeats) {
      // §13.4: detachFromReferenceLine draws the section "in a straight line
      // from its start to its end position" — the chord between the section's
      // start/end anchors — instead of following the reference-line curvature.
      std::array<double, 3> chord_start{};
      std::array<double, 3> chord_end{};
      double chord_heading = 0.0;
      if (repeat.detach_from_reference_line) {
        const mesh_detail::StationFrame start_frame = mesh_detail::make_frame(road, repeat.s);
        const mesh_detail::StationFrame end_frame =
            mesh_detail::make_frame(road, repeat.s + repeat.length);
        chord_start = mesh_detail::lateral_point(start_frame, repeat.t_start);
        chord_start[2] += repeat.z_offset_start;
        chord_end = mesh_detail::lateral_point(end_frame, repeat.t_end);
        chord_end[2] += repeat.z_offset_end;
        chord_heading =
            std::atan2(chord_end[1] - chord_start[1], chord_end[0] - chord_start[0]) + object.hdg;
      }

      for (const RepeatInstance& inst : expand_repeat(repeat)) {
        if (inst.s > road_length + tol::kLength) {
          continue;
        }
        std::array<double, 3> position{};
        double heading = 0.0;
        if (repeat.detach_from_reference_line) {
          const double ratio = repeat.length > 0.0 ? (inst.s - repeat.s) / repeat.length : 0.0;
          for (std::size_t axis = 0; axis < 3; ++axis) {
            position[axis] = chord_start[axis] + (chord_end[axis] - chord_start[axis]) * ratio;
          }
          heading = chord_heading;
        } else {
          const mesh_detail::StationFrame frame = mesh_detail::make_frame(road, inst.s);
          position = mesh_detail::lateral_point(frame, inst.t);
          position[2] += inst.z_offset;
          heading = std::atan2(frame.sin_h, frame.cos_h) + object.hdg;
        }
        out.push_back(PlacedInstance{.index = out.size(),
                                     .position = position,
                                     .heading = heading,
                                     .s = inst.s,
                                     .t = inst.t});
      }
    }
    return out;
  }

  const mesh_detail::StationFrame frame = mesh_detail::make_frame(road, object.s);
  std::array<double, 3> position = mesh_detail::lateral_point(frame, object.t);
  position[2] += object.z_offset;
  out.push_back(PlacedInstance{.index = 0,
                               .position = position,
                               .heading = std::atan2(frame.sin_h, frame.cos_h) + object.hdg,
                               .s = object.s,
                               .t = object.t});
  return out;
}

} // namespace roadmaker::object_placement
