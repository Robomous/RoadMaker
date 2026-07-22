// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/mesh/junction_maneuvers.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace roadmaker {

namespace {

/// The road end a connecting road's link names, or nullopt when the link is
/// absent or points at a junction rather than a road.
std::optional<RoadEnd> linked_end(const std::optional<RoadLink>& link) {
  if (!link.has_value()) {
    return std::nullopt;
  }
  const RoadId* road = std::get_if<RoadId>(&link->target);
  if (road == nullptr) {
    return std::nullopt;
  }
  return RoadEnd{.road = *road, .contact = link->contact};
}

/// The outgoing lane id a connecting road links to — the successor of its
/// single right-hand driving lane. Mirrors retarget_junction's TurnKey read so
/// the query and the matcher cannot disagree about what a turn connects.
int outgoing_lane_of(const RoadNetwork& network, const Road& road) {
  if (road.sections.empty()) {
    return 0;
  }
  for (const LaneId lane_id : network.lane_section(road.sections.front())->lanes) {
    const Lane& lane = *network.lane(lane_id);
    if (lane.odr_id == -1 && lane.successor.has_value()) {
      return *lane.successor;
    }
  }
  return 0;
}

/// The slide constraint line for one face: the anchor lane's cross-section
/// segment, measured from its inner boundary along the arm's +t axis. A lane on
/// the right of the reference line (negative odr id) extends toward NEGATIVE t,
/// so its offsets run [-width, 0]; a left lane's run [0, +width].
ManeuverSlide make_slide(const edit::ContactState& contact, const edit::ContactLane& lane) {
  const double outer = (lane.odr_id < 0 ? -1.0 : 1.0) * lane.width;
  ManeuverSlide slide;
  slide.min_offset = std::min(0.0, outer);
  slide.max_offset = std::max(0.0, outer);
  slide.anchor = edit::contact_lateral(contact, lane.inner_t);
  slide.min_point = edit::contact_lateral(contact, lane.inner_t + slide.min_offset);
  slide.max_point = edit::contact_lateral(contact, lane.inner_t + slide.max_offset);
  return slide;
}

} // namespace

TurnType maneuver_turn_type(double delta, bool same_arm) {
  const double turned = std::remainder(delta, 2.0 * std::numbers::pi);
  if (same_arm || std::abs(turned) >= kManeuverUTurnThreshold) {
    return TurnType::UTurn;
  }
  if (std::abs(turned) <= kManeuverStraightThreshold) {
    return TurnType::Straight;
  }
  return turned > 0.0 ? TurnType::Left : TurnType::Right;
}

std::vector<JunctionManeuverInfo> junction_maneuvers(const RoadNetwork& network,
                                                     JunctionId junction_id,
                                                     const SamplingOptions& sampling) {
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return {};
  }
  std::vector<JunctionManeuverInfo> out;
  out.reserve(junction->connections.size());
  for (const JunctionConnection& connection : junction->connections) {
    const Road* road = network.road(connection.connecting_road);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    const std::optional<RoadEnd> from = linked_end(road->predecessor);
    const std::optional<RoadEnd> to = linked_end(road->successor);
    if (!from.has_value() || !to.has_value()) {
      continue;
    }
    const Expected<edit::ContactState> from_contact = edit::contact_state(network, *from);
    const Expected<edit::ContactState> to_contact = edit::contact_state(network, *to);
    if (!from_contact.has_value() || !to_contact.has_value()) {
      continue;
    }

    JunctionManeuverInfo info;
    info.road = connection.connecting_road;
    info.road_odr_id = road->odr_id;
    info.from = *from;
    info.to = *to;
    info.from_lane = connection.lane_links.empty() ? 0 : connection.lane_links.front().first;
    info.to_lane = outgoing_lane_of(network, *road);
    info.is_uturn_explicit = *from == *to;
    info.start_heading = from_contact->into_hdg;
    info.end_heading = to_contact->out_hdg;
    info.computed =
        maneuver_turn_type(info.end_heading - info.start_heading, info.is_uturn_explicit);
    info.effective = info.computed;

    const auto record = std::ranges::find_if(junction->maneuvers, [&](const Maneuver& entry) {
      return entry.road == connection.connecting_road;
    });
    if (record != junction->maneuvers.end()) {
      info.authored = true;
      info.locked = record->locked;
      info.overridden = record->turn_type.has_value();
      info.effective = record->turn_type.value_or(info.computed);
      info.start_offset = record->start_offset.value_or(0.0);
      info.end_offset = record->end_offset.value_or(0.0);
      info.control_points = record->control_points;
    }

    // Slide segments: the anchor lane's own cross section at each face. A lane
    // that has since been removed or retyped leaves a degenerate segment at the
    // connecting road's actual endpoint rather than dropping the maneuver — the
    // panel still lists it, the tool simply cannot drag it.
    const std::vector<edit::ContactLane> incoming =
        edit::driving_lanes_at(network, *from, *from_contact, /*incoming=*/true);
    const std::vector<edit::ContactLane> outgoing =
        edit::driving_lanes_at(network, *to, *to_contact, /*incoming=*/false);
    const auto from_lane = std::ranges::find_if(
        incoming, [&](const edit::ContactLane& lane) { return lane.odr_id == info.from_lane; });
    const auto to_lane = std::ranges::find_if(
        outgoing, [&](const edit::ContactLane& lane) { return lane.odr_id == info.to_lane; });
    const PathPoint head = road->plan_view.evaluate(0.0);
    const PathPoint tail = road->plan_view.evaluate(road->plan_view.length());
    info.start_slide = from_lane != incoming.end() ? make_slide(*from_contact, *from_lane)
                                                   : ManeuverSlide{.anchor = {head.x, head.y},
                                                                   .min_point = {head.x, head.y},
                                                                   .max_point = {head.x, head.y}};
    info.end_slide = to_lane != outgoing.end() ? make_slide(*to_contact, *to_lane)
                                               : ManeuverSlide{.anchor = {tail.x, tail.y},
                                                               .min_point = {tail.x, tail.y},
                                                               .max_point = {tail.x, tail.y}};

    for (const double s : sample_stations(road->plan_view, sampling)) {
      const PathPoint point = road->plan_view.evaluate(s);
      info.path.push_back({point.x, point.y, eval_profile(road->elevation, s)});
    }
    out.push_back(std::move(info));
  }
  return out;
}

} // namespace roadmaker
