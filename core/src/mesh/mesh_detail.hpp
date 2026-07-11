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
/// normal = cos(roll)·ẑ − sin(roll)·N̂ with N̂ the leftward lateral.
inline std::array<double, 3> surface_normal(const StationFrame& f) {
  return {f.sin_r * f.sin_h, -f.sin_r * f.cos_h, f.cos_r};
}

/// Lateral boundary offsets (leftmost first) of a section at station s.
/// Boundary count = lanes-left-of-center + lanes-right-of-center + 1; the
/// center boundary sits at laneOffset(s).
inline std::vector<double> boundary_offsets(const RoadNetwork& network,
                                            const Road& road,
                                            const LaneSection& section,
                                            double s) {
  const double ds = s - section.s0;
  const double center = eval_profile(road.lane_offset, s);

  std::vector<double> left; // innermost -> outermost
  std::vector<double> right;
  for (const LaneId lane_id : section.lanes) {
    const Lane& lane = *network.lane(lane_id);
    if (lane.odr_id == 0) {
      continue;
    }
    const double width = std::max(0.0, eval_profile(lane.widths, ds));
    if (lane.odr_id > 0) {
      left.push_back(width);
    } else {
      right.push_back(width);
    }
  }
  // section.lanes is sorted leftmost-first, so `left` was collected
  // outermost-first; accumulate from the center outwards.
  std::vector<double> offsets;
  offsets.reserve(left.size() + right.size() + 1);

  double t = center;
  std::vector<double> left_out(left.size());
  for (std::size_t i = left.size(); i-- > 0;) { // innermost (+1) first
    t += left[i];
    left_out[i] = t;
  }
  offsets.insert(offsets.end(), left_out.begin(), left_out.end());
  offsets.push_back(center);
  t = center;
  for (const double width : right) {
    t -= width;
    offsets.push_back(t);
  }
  return offsets;
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
