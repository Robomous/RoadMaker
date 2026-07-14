#include "roadmaker/edit/markings.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <numbers>
#include <optional>
#include <set>
#include <string>

namespace roadmaker::edit {

namespace {

/// The contact end of arm `road` that belongs to `junction` (Start or End).
std::optional<ContactPoint>
facing_end(const RoadNetwork& network, RoadId road, JunctionId junction) {
  for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
    if (junction_at_end(network, RoadEnd{.road = road, .contact = contact}) == junction) {
      return contact;
    }
  }
  return std::nullopt;
}

} // namespace

std::vector<std::pair<RoadId, Object>> junction_crosswalks(const RoadNetwork& network,
                                                           JunctionId junction,
                                                           const CrosswalkParams& params) {
  std::vector<std::pair<RoadId, Object>> out;
  const Junction* record = network.junction(junction);
  if (record == nullptr) {
    return out;
  }

  // Distinct arm roads — one crosswalk per arm, not one per turn (a junction has
  // several connections per incoming road for the left/straight/right turns).
  std::vector<RoadId> arms;
  for (const JunctionConnection& connection : record->connections) {
    if (std::ranges::find(arms, connection.incoming_road) == arms.end()) {
      arms.push_back(connection.incoming_road);
    }
  }

  // Reserve odr ids clear of every existing object; each authored crosswalk
  // takes the next free integer so the batch stays id_unique_in_class-valid.
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  int next_id = 1;
  const auto fresh_odr_id = [&]() {
    while (taken.contains(std::to_string(next_id))) {
      ++next_id;
    }
    const std::string id = std::to_string(next_id);
    taken.insert(id);
    return id;
  };

  for (const RoadId arm : arms) {
    const Road* road = network.road(arm);
    if (road == nullptr || road->plan_view.empty()) {
      continue;
    }
    const std::optional<ContactPoint> facing = facing_end(network, arm, junction);
    if (!facing.has_value()) {
      continue;
    }
    const RoadEnd end{.road = arm, .contact = *facing};
    const Expected<ContactState> contact = contact_state(network, end);
    if (!contact) {
      continue;
    }

    // Full driving span across the arm (both travel directions), from the
    // outermost driving-lane edges: inner_t is the lane's centre-side offset
    // (positive = left), the outer edge is width further from the centre.
    double t_min = 0.0;
    double t_max = 0.0;
    bool any = false;
    for (const bool incoming : {true, false}) {
      for (const ContactLane& lane : driving_lanes_at(network, end, *contact, incoming)) {
        const double outer = lane.inner_t + (lane.odr_id > 0 ? lane.width : -lane.width);
        const double lo = std::min(lane.inner_t, outer);
        const double hi = std::max(lane.inner_t, outer);
        t_min = any ? std::min(t_min, lo) : lo;
        t_max = any ? std::max(t_max, hi) : hi;
        any = true;
      }
    }
    if (!any || (t_max - t_min) < tol::kLength) {
      continue; // no driving lanes to cross
    }

    const double length = road->plan_view.length();
    const double half_depth = params.depth_m / 2.0;
    // Just inside the road from the junction edge, walking away from the arm's
    // near end (Start end is at s = 0, End end at s = length).
    const double s = *facing == ContactPoint::Start ? params.setback_m + half_depth
                                                    : length - params.setback_m - half_depth;

    Object crosswalk;
    crosswalk.odr_id = fresh_odr_id();
    crosswalk.type = ObjectType::Crosswalk;
    crosswalk.type_str = "crosswalk";
    crosswalk.subtype = "zebra";
    crosswalk.s = std::clamp(s, 0.0, length);
    crosswalk.t = (t_min + t_max) / 2.0;
    crosswalk.hdg = std::numbers::pi / 2.0; // across the road (relative to +s)
    crosswalk.length = t_max - t_min;       // across span — zebra bars repeat here
    crosswalk.width = params.depth_m;       // depth along the road
    out.emplace_back(arm, std::move(crosswalk));
  }
  return out;
}

} // namespace roadmaker::edit
