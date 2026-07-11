#include "roadmaker/mesh/mesh_builder.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "junction_surface.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::mandatory_stations;
using mesh_detail::StationFrame;
using mesh_detail::surface_normal;

// M1 marking pattern for RoadMarkType::Broken: 3 m dash / 6 m gap.
constexpr double kDashLength = 3.0;
constexpr double kDashCycle = 9.0;

// Lift markings slightly above the surface to avoid z-fighting.
constexpr double kMarkingLift = 0.002;

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

/// One road's tessellation; empty (no lanes, no markings) for degenerate
/// roads — callers drop empty results.
RoadMesh build_one_road(const RoadNetwork& network,
                        RoadId road_id,
                        const Road& road,
                        const MeshOptions& options) {
  RoadMesh mesh;
  mesh.road = road_id;
  if (road.plan_view.empty() || road.sections.empty()) {
    return mesh; // parser already diagnosed these
  }

  SamplingOptions sampling = options.sampling;
  const std::vector<double> extra = mandatory_stations(network, road);
  sampling.extra_stations.insert(sampling.extra_stations.end(), extra.begin(), extra.end());
  const std::vector<double> stations = sample_stations(road.plan_view, sampling);

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
  return mesh;
}

/// One junction's blended 3D surface, keyed by id; empty mesh (no indices)
/// when the junction has no usable connecting-road footprints — callers drop
/// empty results. The pipeline itself lives in junction_surface.cpp.
JunctionFloor build_one_junction_floor(const RoadNetwork& network,
                                       JunctionId junction_id,
                                       const Junction& junction,
                                       const SamplingOptions& sampling) {
  return JunctionFloor{.junction = junction_id,
                       .mesh = build_junction_surface(network, junction, sampling)};
}

bool road_mesh_is_empty(const RoadMesh& mesh) {
  return mesh.lanes.empty() && mesh.markings.empty();
}

} // namespace

NetworkMesh build_network_mesh(const RoadNetwork& network, const MeshOptions& options) {
  NetworkMesh result;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    RoadMesh mesh = build_one_road(network, road_id, road, options);
    if (!road_mesh_is_empty(mesh)) {
      result.roads.push_back(std::move(mesh));
    }
  });
  if (options.junction_floors) {
    network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
      JunctionFloor floor =
          build_one_junction_floor(network, junction_id, junction, options.sampling);
      if (!floor.mesh.indices.empty()) {
        result.junction_floors.push_back(std::move(floor));
      }
    });
  }
  return result;
}

void remesh_roads(const RoadNetwork& network,
                  NetworkMesh& mesh,
                  std::span<const RoadId> roads,
                  const MeshOptions& options) {
  for (const RoadId road_id : roads) {
    const auto existing = std::ranges::find(mesh.roads, road_id, &RoadMesh::road);
    const Road* road = network.road(road_id);
    RoadMesh rebuilt =
        road != nullptr ? build_one_road(network, road_id, *road, options) : RoadMesh{};
    if (road == nullptr || road_mesh_is_empty(rebuilt)) {
      if (existing != mesh.roads.end()) {
        // vector::erase move-assigns the tail entries; their heap buffers
        // are stolen, not copied, so untouched roads keep their data().
        mesh.roads.erase(existing);
      }
      continue;
    }
    if (existing != mesh.roads.end()) {
      *existing = std::move(rebuilt);
    } else {
      mesh.roads.push_back(std::move(rebuilt));
    }
  }
}

void remesh_junctions(const RoadNetwork& network,
                      NetworkMesh& mesh,
                      std::span<const JunctionId> junctions,
                      const MeshOptions& options) {
  for (const JunctionId junction_id : junctions) {
    const auto existing =
        std::ranges::find(mesh.junction_floors, junction_id, &JunctionFloor::junction);
    const Junction* junction = options.junction_floors ? network.junction(junction_id) : nullptr;
    JunctionFloor rebuilt =
        junction != nullptr
            ? build_one_junction_floor(network, junction_id, *junction, options.sampling)
            : JunctionFloor{.junction = junction_id, .mesh = {}};
    if (rebuilt.mesh.indices.empty()) {
      if (existing != mesh.junction_floors.end()) {
        mesh.junction_floors.erase(existing);
      }
      continue;
    }
    if (existing != mesh.junction_floors.end()) {
      *existing = std::move(rebuilt);
    } else {
      mesh.junction_floors.push_back(std::move(rebuilt));
    }
  }
}

} // namespace roadmaker
