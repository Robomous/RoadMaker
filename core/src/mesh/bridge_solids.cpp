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

#include "bridge_solids.hpp"

#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>
#include <manifold/manifold.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "manifold_bridge.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::StationFrame;
using mesh_detail::surface_normal;

/// The lane section covering station `s` (the one with the greatest s0 <= s).
const LaneSection* covering_section(const RoadNetwork& network, const Road& road, double s) {
  const LaneSection* best = nullptr;
  for (const LaneSectionId id : road.sections) {
    const LaneSection* section = network.lane_section(id);
    if (section == nullptr) {
      continue;
    }
    if (section->s0 <= s + tol::kLength && (best == nullptr || section->s0 > best->s0)) {
      best = section;
    }
  }
  return best;
}

/// The deck's lateral bounds: the widest road cross-section over [s0, s1] grown
/// by the overhang on each side. {t_right (min), t_left (max)}.
struct DeckExtent {
  double t_right = 0.0;
  double t_left = 0.0;
  bool valid = false;
};

DeckExtent
deck_extent(const RoadNetwork& network, const Road& road, double s0, double s1, double overhang) {
  DeckExtent extent;
  double lo = 0.0;
  double hi = 0.0;
  constexpr int kSamples = 12;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = s0 + ((s1 - s0) * static_cast<double>(i) / kSamples);
    const LaneSection* section = covering_section(network, road, s);
    if (section == nullptr) {
      continue;
    }
    const std::vector<double> offsets = mesh_detail::boundary_offsets(network, road, *section, s);
    if (offsets.empty()) {
      continue;
    }
    const auto [mn, mx] = std::minmax_element(offsets.begin(), offsets.end());
    if (!extent.valid) {
      lo = *mn;
      hi = *mx;
      extent.valid = true;
    } else {
      lo = std::min(lo, *mn);
      hi = std::max(hi, *mx);
    }
  }
  if (!extent.valid || hi - lo < tol::kLength) {
    extent.valid = false;
    return extent;
  }
  extent.t_right = lo - overhang;
  extent.t_left = hi + overhang;
  return extent;
}

/// A sweep frame that rides the road: (t, w) at fraction u maps to the road
/// surface point at lateral offset t, pushed by w along the local up normal.
bridge::SweepFrameFn ride_road(const Road& road, double s_start, double s_len) {
  return [&road, s_start, s_len](double t, double w, double u) {
    const StationFrame f = make_frame(road, s_start + (u * s_len));
    const std::array<double, 3> p = lateral_point(f, t);
    const std::array<double, 3> n = surface_normal(f);
    return std::array<double, 3>{p[0] + (w * n[0]), p[1] + (w * n[1]), p[2] + (w * n[2])};
  };
}

/// Even pier stations across the span: none up to `pier_free_span`, otherwise
/// enough that every gap is <= `pier_spacing` (design §4 — a rule, not a count).
std::vector<double> pier_stations(double s0, double span, const BridgeParams& params) {
  const std::size_t piers = bridge_pier_count(span, params);
  std::vector<double> out;
  const std::size_t gaps = piers + 1;
  for (std::size_t i = 1; i <= piers; ++i) {
    out.push_back(s0 + (span * static_cast<double>(i) / static_cast<double>(gaps)));
  }
  return out;
}

/// A pier (or nullopt if the deck sits at or below the ground here): a square
/// column from the ground up to the deck underside at the road centre.
std::optional<manifold::Manifold>
pier_at(const RoadNetwork& network, const Road& road, double s, const BridgeParams& params) {
  const StationFrame f = make_frame(road, s);
  const std::array<double, 3> centre = lateral_point(f, 0.0);
  const double ground = sample_height(network.terrain(), centre[0], centre[1]);
  const double deck_bottom = f.z - params.deck_depth;
  const double height = deck_bottom - ground;
  if (height < 0.1) {
    return std::nullopt; // road is at/under the ground — no pier to build
  }
  return bridge::box({centre[0], centre[1], ground + (height / 2.0)},
                     {params.pier_size, params.pier_size, height});
}

/// An abutment: a short full-width wall under the deck at one end, from the deck
/// underside down toward the ground. nullopt when the deck is at/under grade.
std::optional<manifold::Manifold> abutment_at(const RoadNetwork& network,
                                              const Road& road,
                                              double s_start,
                                              double s_len,
                                              const DeckExtent& extent,
                                              const BridgeParams& params) {
  const StationFrame f = make_frame(road, s_start + (s_len / 2.0));
  const std::array<double, 3> centre = lateral_point(f, 0.0);
  const double ground = sample_height(network.terrain(), centre[0], centre[1]);
  const double deck_bottom = f.z - params.deck_depth;
  const double wall = deck_bottom - ground;
  if (wall < 0.1) {
    return std::nullopt;
  }
  // Cross-section is the deck's full width; w spans from the deck underside down
  // to the ground. Swept along the short abutment length so it follows heading.
  const std::vector<bridge::SectionPoint> section{{extent.t_right, -params.deck_depth - wall},
                                                  {extent.t_left, -params.deck_depth - wall},
                                                  {extent.t_left, -params.deck_depth},
                                                  {extent.t_right, -params.deck_depth}};
  manifold::Manifold solid = bridge::sweep_section(section, 2, ride_road(road, s_start, s_len));
  if (solid.Status() != manifold::Manifold::Error::NoError) {
    return std::nullopt;
  }
  return solid;
}

} // namespace

std::size_t bridge_pier_count(double span, const BridgeParams& params) {
  if (span <= params.pier_free_span || params.pier_spacing <= 0.0) {
    return 0;
  }
  const auto gaps = static_cast<std::size_t>(std::ceil(span / params.pier_spacing));
  return gaps > 0 ? gaps - 1 : 0;
}

std::optional<SubMesh> build_bridge_solid(const RoadNetwork& network,
                                          const Road& road,
                                          const Bridge& bridge,
                                          const BridgeParams& params) {
  const double road_length = road.plan_view.length();
  const double s0 = std::clamp(bridge.s, 0.0, road_length);
  const double s1 = std::clamp(bridge.s + bridge.length, 0.0, road_length);
  const double span = s1 - s0;
  // Refuse a span too short to build a sensible solid — the structure would be
  // thicker than it is long (design §5). Not a crash, not a sliver.
  if (span < 2.0 * params.deck_depth) {
    return std::nullopt;
  }

  const DeckExtent extent = deck_extent(network, road, s0, s1, params.deck_overhang);
  if (!extent.valid) {
    return std::nullopt;
  }

  // ~2 m stations, at least four, so a curve or a width taper is captured.
  const int n_div = std::max(4, static_cast<int>(std::ceil(span / 2.0)));
  const bridge::SweepFrameFn frame = ride_road(road, s0, span);

  std::vector<manifold::Manifold> parts;

  // Deck: the full cross-section, deck_depth thick, swept along the span.
  const std::vector<bridge::SectionPoint> deck_section{{extent.t_right, -params.deck_depth},
                                                       {extent.t_left, -params.deck_depth},
                                                       {extent.t_left, 0.0},
                                                       {extent.t_right, 0.0}};
  manifold::Manifold deck = bridge::sweep_section(deck_section, n_div, frame);
  if (deck.Status() != manifold::Manifold::Error::NoError || deck.NumTri() == 0) {
    return std::nullopt;
  }
  parts.push_back(std::move(deck));

  // Guardrails: a rail on each deck edge, sitting on top (w in [0, height]).
  const std::vector<bridge::SectionPoint> rail_left{
      {extent.t_left - params.guardrail_width, 0.0},
      {extent.t_left, 0.0},
      {extent.t_left, params.guardrail_height},
      {extent.t_left - params.guardrail_width, params.guardrail_height}};
  const std::vector<bridge::SectionPoint> rail_right{
      {extent.t_right, 0.0},
      {extent.t_right + params.guardrail_width, 0.0},
      {extent.t_right + params.guardrail_width, params.guardrail_height},
      {extent.t_right, params.guardrail_height}};
  for (const std::vector<bridge::SectionPoint>& rail : {rail_left, rail_right}) {
    manifold::Manifold bar = bridge::sweep_section(rail, n_div, frame);
    if (bar.Status() == manifold::Manifold::Error::NoError && bar.NumTri() > 0) {
      parts.push_back(std::move(bar));
    }
  }

  // Piers under the deck per the span-length rule.
  for (const double s : pier_stations(s0, span, params)) {
    if (std::optional<manifold::Manifold> pier = pier_at(network, road, s, params)) {
      parts.push_back(std::move(*pier));
    }
  }

  // Abutments at each end, a short wall to the ground.
  const double abut_len = std::min(1.5, span / 4.0);
  if (std::optional<manifold::Manifold> a =
          abutment_at(network, road, s0, abut_len, extent, params)) {
    parts.push_back(std::move(*a));
  }
  if (std::optional<manifold::Manifold> a =
          abutment_at(network, road, s1 - abut_len, abut_len, extent, params)) {
    parts.push_back(std::move(*a));
  }

  manifold::Manifold solid = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
  if (solid.Status() != manifold::Manifold::Error::NoError || solid.NumTri() == 0) {
    return std::nullopt;
  }
  SubMesh mesh = bridge::to_submesh(solid);
  if (mesh.indices.empty()) {
    return std::nullopt;
  }
  mesh.name = fmt::format("bridge {} on road {}", bridge.odr_id, road.odr_id);
  return mesh;
}

void remesh_bridges(const RoadNetwork& network, NetworkMesh& mesh, const MeshOptions& options) {
  mesh.bridges.clear();
  if (!options.bridges.enabled) {
    return;
  }
  network.for_each_road([&](RoadId road_id, const Road& road) {
    for (std::size_t i = 0; i < road.bridges.size(); ++i) {
      std::optional<SubMesh> solid =
          build_bridge_solid(network, road, road.bridges[i], options.bridges);
      if (solid.has_value()) {
        mesh.bridges.push_back(BridgeMesh{.road = road_id, .index = i, .mesh = std::move(*solid)});
      }
    }
  });
}

} // namespace roadmaker
