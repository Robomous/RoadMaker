#include "roadmaker/mesh/mesh_builder.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <clipper2/clipper.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace roadmaker {

namespace {

// M1 marking pattern for RoadMarkType::Broken: 3 m dash / 6 m gap.
constexpr double kDashLength = 3.0;
constexpr double kDashCycle = 9.0;

// Lift markings and drop junction floors slightly to avoid z-fighting.
constexpr double kMarkingLift = 0.002;
constexpr double kFloorDrop = 0.02;

/// Vertical profile values of one road at a station.
struct StationFrame {
  double x, y, z;
  double cos_h, sin_h;
  double cos_r, sin_r; // superelevation roll
};

StationFrame make_frame(const Road& road, double s) {
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
std::array<double, 3> lateral_point(const StationFrame& f, double t) {
  return {
      f.x - (t * f.sin_h * f.cos_r),
      f.y + (t * f.cos_h * f.cos_r),
      f.z + (t * f.sin_r),
  };
}

/// Surface normal at a station (independent of t in this approximation):
/// normal = cos(roll)·ẑ − sin(roll)·N̂ with N̂ the leftward lateral.
std::array<double, 3> surface_normal(const StationFrame& f) {
  return {f.sin_r * f.sin_h, -f.sin_r * f.cos_h, f.cos_r};
}

/// Lateral boundary offsets (leftmost first) of a section at station s.
/// Boundary count = lanes-left-of-center + lanes-right-of-center + 1; the
/// center boundary sits at laneOffset(s).
std::vector<double> boundary_offsets(const RoadNetwork& network,
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
std::vector<double> mandatory_stations(const RoadNetwork& network, const Road& road) {
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

/// The outer boundary offset of a lane (its t at the edge away from the
/// reference line) — where its marking is painted.
double outer_boundary_offset(const RoadNetwork& network,
                             const Road& road,
                             const LaneSection& section,
                             const Lane& lane,
                             double s) {
  const std::vector<double> offsets = boundary_offsets(network, road, section, s);
  const std::size_t center_index = static_cast<std::size_t>(std::ranges::count_if(
      section.lanes, [&](LaneId id) { return network.lane(id)->odr_id > 0; }));
  if (lane.odr_id == 0) {
    return offsets[center_index];
  }
  // p-th lane out from the center on its side.
  std::size_t p = 0;
  for (const LaneId lane_id : section.lanes) {
    const Lane& other = *network.lane(lane_id);
    if ((lane.odr_id > 0) == (other.odr_id > 0) && other.odr_id != 0 &&
        std::abs(other.odr_id) <= std::abs(lane.odr_id)) {
      ++p;
    }
  }
  return lane.odr_id > 0 ? offsets[center_index - p] : offsets[center_index + p];
}

void emit_marking_strip(const Road& road,
                        double s_begin,
                        double s_end,
                        double t,
                        double width,
                        std::vector<double> section_stations,
                        SubMesh& out) {
  // Clamp the strip's sampling to [s_begin, s_end].
  std::erase_if(section_stations,
                [&](double s) { return s < s_begin - tol::kLength || s > s_end + tol::kLength; });
  if (section_stations.empty() || section_stations.front() > s_begin + tol::kLength) {
    section_stations.insert(section_stations.begin(), s_begin);
  }
  if (section_stations.back() < s_end - tol::kLength) {
    section_stations.push_back(s_end);
  }
  const double half = width / 2.0;
  const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
  for (const double s : section_stations) {
    const StationFrame frame = make_frame(road, s);
    for (const double offset : {t + half, t - half}) {
      const auto p = lateral_point(frame, offset);
      out.positions.insert(out.positions.end(), {p[0], p[1], p[2] + kMarkingLift});
      const auto n = surface_normal(frame);
      out.normals.insert(out.normals.end(), n.begin(), n.end());
    }
  }
  for (std::uint32_t row = 0; row + 1 < section_stations.size(); ++row) {
    const std::uint32_t a = base + (row * 2); // left edge of strip
    const std::uint32_t b = a + 1;            // right edge
    const std::uint32_t c = a + 3;            // next right
    const std::uint32_t d = a + 2;            // next left
    out.indices.insert(out.indices.end(), {a, b, c, a, c, d});
  }
}

void build_markings(const RoadNetwork& network,
                    const Road& road,
                    const RoadId road_id,
                    const std::vector<double>& stations,
                    RoadMesh& mesh) {
  for (std::size_t si = 0; si < road.sections.size(); ++si) {
    const LaneSection& section = *network.lane_section(road.sections[si]);
    const double section_end = si + 1 < road.sections.size()
                                   ? network.lane_section(road.sections[si + 1])->s0
                                   : road.plan_view.length();

    std::vector<double> section_stations;
    for (const double s : stations) {
      if (s >= section.s0 - tol::kLength && s <= section_end + tol::kLength) {
        section_stations.push_back(s);
      }
    }

    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      for (std::size_t mi = 0; mi < lane.road_marks.size(); ++mi) {
        const RoadMark& mark = lane.road_marks[mi];
        if (mark.type == RoadMarkType::None || mark.width <= 0.0) {
          continue;
        }
        const double mark_begin = section.s0 + mark.s_offset;
        const double mark_end = mi + 1 < lane.road_marks.size()
                                    ? section.s0 + lane.road_marks[mi + 1].s_offset
                                    : section_end;
        if (mark_end <= mark_begin) {
          continue;
        }

        SubMesh strip;
        strip.material = lane.type;
        strip.name = fmt::format("road {} lane {} marking", road.odr_id, lane.odr_id);

        if (mark.type == RoadMarkType::Broken) {
          for (double dash = mark_begin; dash < mark_end; dash += kDashCycle) {
            const double dash_end = std::min(dash + kDashLength, mark_end);
            const double t = outer_boundary_offset(network, road, section, lane, dash);
            emit_marking_strip(road, dash, dash_end, t, mark.width, section_stations, strip);
          }
        } else {
          // Solid and every multi-line type render as one solid strip in
          // M1 (M2: double lines with proper spacing).
          const double t = outer_boundary_offset(network, road, section, lane, mark_begin);
          emit_marking_strip(road, mark_begin, mark_end, t, mark.width, section_stations, strip);
        }
        if (!strip.indices.empty()) {
          mesh.markings.push_back(std::move(strip));
        }
      }
    }
    (void)road_id;
  }
}

void build_road_mesh(const RoadNetwork& network,
                     RoadId road_id,
                     const Road& road,
                     const MeshOptions& options,
                     NetworkMesh& out) {
  if (road.plan_view.empty() || road.sections.empty()) {
    return; // parser already diagnosed these
  }

  SamplingOptions sampling = options.sampling;
  const std::vector<double> extra = mandatory_stations(network, road);
  sampling.extra_stations.insert(sampling.extra_stations.end(), extra.begin(), extra.end());
  const std::vector<double> stations = sample_stations(road.plan_view, sampling);

  RoadMesh mesh;
  mesh.road = road_id;
  mesh.name = road.name.empty() ? fmt::format("road {}", road.odr_id) : road.name;

  for (std::size_t si = 0; si < road.sections.size(); ++si) {
    const LaneSection& section = *network.lane_section(road.sections[si]);
    const double section_end = si + 1 < road.sections.size()
                                   ? network.lane_section(road.sections[si + 1])->s0
                                   : road.plan_view.length();

    std::vector<double> rows;
    for (const double s : stations) {
      if (s >= section.s0 - tol::kLength && s <= section_end + tol::kLength) {
        rows.push_back(std::clamp(s, section.s0, section_end));
      }
    }
    if (rows.size() < 2) {
      continue;
    }

    const std::uint32_t grid_base = static_cast<std::uint32_t>(mesh.positions.size() / 3);
    std::size_t columns = 0;

    // Vertex grid: one row per station, one column per lane boundary.
    std::vector<std::vector<double>> row_offsets;
    row_offsets.reserve(rows.size());
    for (const double s : rows) {
      const StationFrame frame = make_frame(road, s);
      std::vector<double> offsets = boundary_offsets(network, road, section, s);
      columns = offsets.size();
      for (const double t : offsets) {
        const auto p = lateral_point(frame, t);
        mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
        const auto n = surface_normal(frame);
        mesh.normals.insert(mesh.normals.end(), n.begin(), n.end());
      }
      row_offsets.push_back(std::move(offsets));
    }

    // One patch per non-center lane; triangles skip degenerate quads.
    const std::size_t center_col = static_cast<std::size_t>(std::ranges::count_if(
        section.lanes, [&](LaneId id) { return network.lane(id)->odr_id > 0; }));
    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id == 0) {
        continue;
      }
      std::size_t p = 0;
      for (const LaneId other_id : section.lanes) {
        const Lane& other = *network.lane(other_id);
        if ((lane.odr_id > 0) == (other.odr_id > 0) && other.odr_id != 0 &&
            std::abs(other.odr_id) <= std::abs(lane.odr_id)) {
          ++p;
        }
      }
      // Columns of this lane's left and right boundaries in the grid.
      const std::size_t col_left = lane.odr_id > 0 ? center_col - p : center_col + p - 1;
      const std::size_t col_right = col_left + 1;

      RoadMesh::LanePatch patch;
      patch.lane = lane_id;
      patch.odr_lane_id = lane.odr_id;
      patch.material = lane.type;

      for (std::uint32_t row = 0; row + 1 < rows.size(); ++row) {
        const double width_here =
            std::abs(row_offsets[row][col_left] - row_offsets[row][col_right]);
        const double width_next =
            std::abs(row_offsets[row + 1][col_left] - row_offsets[row + 1][col_right]);
        if (width_here < tol::kLength && width_next < tol::kLength) {
          continue; // fully pinched-off quad
        }
        const std::uint32_t a = grid_base + (row * static_cast<std::uint32_t>(columns)) +
                                static_cast<std::uint32_t>(col_left);
        const std::uint32_t b = grid_base + (row * static_cast<std::uint32_t>(columns)) +
                                static_cast<std::uint32_t>(col_right);
        const std::uint32_t c = b + static_cast<std::uint32_t>(columns);
        const std::uint32_t d = a + static_cast<std::uint32_t>(columns);
        patch.indices.insert(patch.indices.end(), {a, b, c, a, c, d});
      }
      if (!patch.indices.empty()) {
        mesh.lanes.push_back(std::move(patch));
      }
    }
  }

  if (options.markings) {
    build_markings(network, road, road_id, stations, mesh);
  }

  if (!mesh.lanes.empty() || !mesh.markings.empty()) {
    out.roads.push_back(std::move(mesh));
  }
}

/// Plan-view footprint of a road: left boundary forward, right boundary
/// reversed, as a closed Clipper2 path.
Clipper2Lib::PathD road_footprint(const RoadNetwork& network, const Road& road) {
  SamplingOptions sampling;
  sampling.extra_stations = mandatory_stations(network, road);
  const std::vector<double> stations = sample_stations(road.plan_view, sampling);

  Clipper2Lib::PathD path;
  auto section_at = [&](double s) -> const LaneSection& {
    const LaneSection* result = network.lane_section(road.sections.front());
    for (const LaneSectionId id : road.sections) {
      const LaneSection& section = *network.lane_section(id);
      if (section.s0 <= s + tol::kLength) {
        result = &section;
      }
    }
    return *result;
  };

  for (const double s : stations) {
    const StationFrame frame = make_frame(road, s);
    const auto offsets = boundary_offsets(network, road, section_at(s), s);
    const auto p = lateral_point(frame, offsets.front());
    path.emplace_back(p[0], p[1]);
  }
  for (auto it = stations.rbegin(); it != stations.rend(); ++it) {
    const StationFrame frame = make_frame(road, *it);
    const auto offsets = boundary_offsets(network, road, section_at(*it), *it);
    const auto p = lateral_point(frame, offsets.back());
    path.emplace_back(p[0], p[1]);
  }
  return path;
}

void build_junction_floor(const RoadNetwork& network, const Junction& junction, NetworkMesh& out) {
  // Union of the connecting roads' plan-view footprints.
  Clipper2Lib::PathsD footprints;
  double z_sum = 0.0;
  std::size_t z_count = 0;
  network.for_each_road([&](RoadId, const Road& road) {
    if (!road.junction.is_valid() || network.junction(road.junction) == nullptr) {
      return;
    }
    if (&*network.junction(road.junction) != &junction) {
      return;
    }
    if (road.plan_view.empty() || road.sections.empty()) {
      return;
    }
    footprints.push_back(road_footprint(network, road));
    z_sum += eval_profile(road.elevation, 0.0);
    ++z_count;
  });
  if (footprints.empty()) {
    return;
  }
  const double floor_z = (z_count > 0 ? z_sum / static_cast<double>(z_count) : 0.0) - kFloorDrop;

  const Clipper2Lib::PathsD merged = Clipper2Lib::Union(footprints, Clipper2Lib::FillRule::NonZero);
  if (merged.empty()) {
    return;
  }

  // Constrained triangulation of the merged outline(s).
  CDT::Triangulation<double> cdt;
  std::vector<CDT::V2d<double>> vertices;
  std::vector<CDT::Edge> edges;
  for (const Clipper2Lib::PathD& path : merged) {
    const std::size_t first = vertices.size();
    for (const Clipper2Lib::PointD& point : path) {
      vertices.push_back(CDT::V2d<double>{point.x, point.y});
    }
    for (std::size_t i = first; i < vertices.size(); ++i) {
      const std::size_t next = (i + 1 < vertices.size()) ? i + 1 : first;
      edges.emplace_back(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>(next));
    }
  }
  CDT::RemoveDuplicatesAndRemapEdges(vertices, edges);
  cdt.insertVertices(vertices);
  cdt.insertEdges(edges);
  cdt.eraseOuterTrianglesAndHoles();
  if (cdt.triangles.empty()) {
    return;
  }

  SubMesh floor;
  floor.material = LaneType::None;
  floor.name = fmt::format("junction {} floor", junction.odr_id);
  floor.positions.reserve(cdt.vertices.size() * 3);
  for (const auto& vertex : cdt.vertices) {
    floor.positions.insert(floor.positions.end(), {vertex.x, vertex.y, floor_z});
    floor.normals.insert(floor.normals.end(), {0.0, 0.0, 1.0});
  }
  for (const auto& triangle : cdt.triangles) {
    // CDT emits CCW triangles; keep them CCW viewed from +Z.
    floor.indices.insert(floor.indices.end(),
                         {triangle.vertices[0], triangle.vertices[1], triangle.vertices[2]});
  }
  out.junction_floors.push_back(std::move(floor));
}

} // namespace

NetworkMesh build_network_mesh(const RoadNetwork& network, const MeshOptions& options) {
  NetworkMesh result;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    build_road_mesh(network, road_id, road, options, result);
  });
  if (options.junction_floors) {
    network.for_each_junction([&](JunctionId, const Junction& junction) {
      build_junction_floor(network, junction, result);
    });
  }
  return result;
}

} // namespace roadmaker
