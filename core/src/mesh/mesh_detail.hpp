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

// Internal (non-installed) meshing helpers shared by mesh_builder.cpp and
// junction_surface.cpp: station frames, lateral offsetting, lane-boundary
// offsets, and the mandatory-station set. Kept header-inline so both
// translation units agree bit-for-bit on where a road's surface vertices
// land — the junction surface stitches to those exact positions (§5 of
// docs/design/m2/03_junction_blending.md).

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace roadmaker::mesh_detail {

/// Vertical profile values of one road at a station.
struct StationFrame {
  double x, y, z;
  double cos_h, sin_h;
  double cos_r, sin_r; // superelevation roll
  double dz_ds;        // longitudinal grade (elevation profile slope)
};

inline StationFrame make_frame(const Road& road, double s) {
  const PathPoint pose = road.plan_view.evaluate(s);
  const double roll = eval_profile(road.superelevation, s);
  return StationFrame{
      .x = pose.x,
      .y = pose.y,
      .z = eval_profile(road.elevation, s),
      .cos_h = std::cos(pose.hdg),
      .sin_h = std::sin(pose.hdg),
      .cos_r = std::cos(roll),
      .sin_r = std::sin(roll),
      .dz_ds = eval_profile_derivative(road.elevation, s),
  };
}

/// Point at lateral offset t (positive = left) from the reference line,
/// with superelevation applied as a roll about the (planar) tangent.
/// M1 approximation: the tangent's dz/ds tilt is ignored in the lateral
/// direction — documented seam for M2.
inline std::array<double, 3> lateral_point(const StationFrame& f, double t) {
  return {
      f.x - (t * f.sin_h * f.cos_r),
      f.y + (t * f.cos_h * f.cos_r),
      f.z + (t * f.sin_r),
  };
}

/// Surface normal at a station (independent of t in this approximation):
/// tangent x lateral with the tangent carrying the longitudinal grade, so a
/// climbing road's shading tilts with it — grade-blind normals lit every
/// graded surface as if flat and creased visibly against the junction
/// floors' geometric normals (tee visual finding, follow-up to issue #103).
inline std::array<double, 3> surface_normal(const StationFrame& f) {
  // T = (cos_h, sin_h, dz/ds), L = leftward lateral with superelevation.
  const std::array<double, 3> t{f.cos_h, f.sin_h, f.dz_ds};
  const std::array<double, 3> l{-f.sin_h * f.cos_r, f.cos_h * f.cos_r, f.sin_r};
  std::array<double, 3> n{
      (t[1] * l[2]) - (t[2] * l[1]), (t[2] * l[0]) - (t[0] * l[2]), (t[0] * l[1]) - (t[1] * l[0])};
  const double len = std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2]));
  if (len > 0.0) {
    n = {n[0] / len, n[1] / len, n[2] / len};
  }
  return n;
}

/// Lateral boundary offsets (leftmost first) of a section at station s.
/// Thin alias for the public roadmaker::lane_boundary_offsets so the mesher and
/// the editor's boundary pick share one implementation (see network.hpp).
inline std::vector<double> boundary_offsets(const RoadNetwork& network,
                                            const Road& road,
                                            const LaneSection& section,
                                            double s) {
  return lane_boundary_offsets(network, road, section, s);
}

/// Stations every profile of this road must sample (record joints are added
/// by sample_stations itself).
inline std::vector<double> mandatory_stations(const RoadNetwork& network, const Road& road) {
  std::vector<double> stations;
  auto add_knots = [&stations](const std::vector<Poly3>& profile, double base) {
    for (const Poly3& poly : profile) {
      stations.push_back(base + poly.s);
    }
  };
  add_knots(road.lane_offset, 0.0);
  add_knots(road.elevation, 0.0);
  add_knots(road.superelevation, 0.0);
  for (const LaneSectionId section_id : road.sections) {
    const LaneSection& section = *network.lane_section(section_id);
    stations.push_back(section.s0);
    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      add_knots(lane.widths, section.s0);
      for (const RoadMark& mark : lane.road_marks) {
        stations.push_back(section.s0 + mark.s_offset);
      }
      // Material boundaries (§11.8.2) land on station rows so each lane patch
      // carries a single surface code with an exact edge at the record start.
      for (const LaneMaterial& material : lane.materials) {
        stations.push_back(section.s0 + material.s_offset);
      }
    }
  }
  return stations;
}

/// The lane section governing global station s (last section with s0 <= s).
inline const LaneSection& section_at(const RoadNetwork& network, const Road& road, double s) {
  const LaneSection* result = network.lane_section(road.sections.front());
  for (const LaneSectionId id : road.sections) {
    const LaneSection& section = *network.lane_section(id);
    if (section.s0 <= s + tol::kLength) {
      result = &section;
    }
  }
  return *result;
}

} // namespace roadmaker::mesh_detail
