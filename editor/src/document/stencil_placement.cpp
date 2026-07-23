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

#include "document/stencil_placement.hpp"

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "document/stencil_item.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// One resolved lane band: the lane's OpenDRIVE odr id (never 0 — bands sit
/// between boundaries, never on the centre line) and its lateral extent [m].
struct LaneBand {
  int odr_id = 0;
  double t_lo = 0.0;
  double t_hi = 0.0;
};

/// The lane band whose lateral extent contains `cursor_t` at station `s` on
/// `road`, from lane_boundary_offsets (leftmost-first: left outer edges, the
/// centre boundary, then right edges). Band [i, i+1] is lane +(nleft - i) on the
/// left and lane -(i - nleft + 1) on the right. nullopt off the carriageway —
/// the same band arithmetic library_drop's material path uses, kept here so a
/// stencil lands on the lane it was aimed at.
std::optional<LaneBand>
lane_band_at(const RoadNetwork& network, RoadId road, double s, double cursor_t) {
  const std::vector<double> offsets = lane_boundary_offsets(network, road, s);
  if (offsets.size() < 2) {
    return std::nullopt;
  }
  const LaneSection* section = network.lane_section(section_at(network, road, s));
  if (section == nullptr) {
    return std::nullopt;
  }
  int nleft = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane != nullptr && lane->odr_id > 0) {
      ++nleft;
    }
  }
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
    const double hi = std::max(offsets[i], offsets[i + 1]);
    const double lo = std::min(offsets[i], offsets[i + 1]);
    if (cursor_t <= hi && cursor_t >= lo) {
      const int index = static_cast<int>(i);
      const int odr = index < nleft ? nleft - index : -(index - nleft + 1);
      return LaneBand{.odr_id = odr, .t_lo = lo, .t_hi = hi};
    }
  }
  return std::nullopt; // off the carriageway
}

/// The travel-direction heading of a lane relative to the reference tangent:
/// a right-of-reference lane (odr < 0) travels along +s (0); a left lane
/// (odr > 0) travels along -s (pi); the centre line (0) keeps +s.
double travel_heading(int odr_id) {
  return odr_id > 0 ? std::numbers::pi : 0.0;
}

/// Lowest positive integer odr id not already used by an object (id-unique in
/// class) — the OdrIdReserver behaviour, matching the Library drop path.
std::string next_object_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

/// The pose for a road-relative (s, t) on `road`: clamps t to the lane band and
/// reads the travel heading. Shared by the point and on-road resolvers.
std::optional<StencilPose>
pose_from_station(const RoadNetwork& network, RoadId road, double s, double t) {
  const std::optional<LaneBand> band = lane_band_at(network, road, s, t);
  if (!band.has_value()) {
    return std::nullopt;
  }
  return StencilPose{.road = road,
                     .s = s,
                     .t = std::clamp(t, band->t_lo, band->t_hi),
                     .hdg = travel_heading(band->odr_id),
                     .lane_width_m = std::abs(band->t_hi - band->t_lo)};
}

} // namespace

std::optional<StencilPose> stencil_pose_for_point(const RoadNetwork& network, double x, double y) {
  std::optional<StencilPose> best;
  double best_abs_t = kStencilSnapThreshold;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.plan_view.empty()) {
      return;
    }
    const std::optional<StationCoord> station =
        station_within(road.plan_view, x, y, kStencilSnapThreshold);
    if (!station.has_value() || std::abs(station->t) >= best_abs_t) {
      return;
    }
    const std::optional<StencilPose> pose = pose_from_station(network, id, station->s, station->t);
    if (pose.has_value()) {
      best_abs_t = std::abs(station->t);
      best = pose;
    }
  });
  return best;
}

std::optional<StencilPose>
stencil_pose_on_road(const RoadNetwork& network, RoadId road, double x, double y) {
  const Road* r = network.road(road);
  if (r == nullptr || r->plan_view.empty()) {
    return std::nullopt;
  }
  const std::optional<StationCoord> station =
      station_within(r->plan_view, x, y, kStencilSnapThreshold);
  if (!station.has_value()) {
    return std::nullopt;
  }
  return pose_from_station(network, road, station->s, station->t);
}

std::optional<std::pair<RoadId, Object>> stencil_for_point(const RoadNetwork& network,
                                                           double x,
                                                           double y,
                                                           const LibraryItem& item,
                                                           const MaterialCatalog& materials) {
  if (item.kind != LibraryItem::Kind::Stencil) {
    return std::nullopt;
  }
  const std::optional<StencilPose> pose = stencil_pose_for_point(network, x, y);
  if (!pose.has_value()) {
    return std::nullopt;
  }
  const edit::StencilParams params = stencil_params_from_item(item, pose->lane_width_m, materials);

  Object object;
  object.odr_id = next_object_odr_id(network);
  object.s = pose->s;
  object.t = pose->t;
  object.hdg = pose->hdg; // lane travel direction
  // apply_stencil_asset authors the glyph outline + <material> + rm:stencil
  // userData; it never fails for a manifest asset (one of the 6 core arrows).
  if (!edit::apply_stencil_asset(object, params).has_value()) {
    return std::nullopt;
  }
  return std::make_pair(pose->road, std::move(object));
}

} // namespace roadmaker::editor
