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

/// The Table 117 subtype every approach lane gets when no chooser overrides it.
constexpr std::string_view kStraightArrow = "arrowStraight";

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

/// Distinct arm (incoming) roads of a junction — one entry per road even when it
/// feeds several turns.
std::vector<RoadId> distinct_arms(const Junction& record) {
  std::vector<RoadId> arms;
  for (const JunctionConnection& connection : record.connections) {
    if (std::ranges::find(arms, connection.incoming_road) == arms.end()) {
      arms.push_back(connection.incoming_road);
    }
  }
  return arms;
}

/// A factory for object odr ids clear of every existing object, so a batch of
/// authored marks stays id_unique_in_class-valid.
class OdrIdReserver {
public:
  explicit OdrIdReserver(const RoadNetwork& network) {
    network.for_each_object([&](ObjectId, const Object& object) { taken_.insert(object.odr_id); });
  }

  std::string next() {
    while (taken_.contains(std::to_string(next_id_))) {
      ++next_id_;
    }
    std::string id = std::to_string(next_id_);
    taken_.insert(id);
    return id;
  }

private:
  std::set<std::string> taken_;
  int next_id_ = 1;
};

} // namespace

std::vector<std::pair<RoadId, Object>> junction_crosswalks(const RoadNetwork& network,
                                                           JunctionId junction,
                                                           const CrosswalkParams& params) {
  std::vector<std::pair<RoadId, Object>> out;
  const Junction* record = network.junction(junction);
  if (record == nullptr) {
    return out;
  }

  // One crosswalk per distinct arm, not one per turn.
  OdrIdReserver ids(network);
  for (const RoadId arm : distinct_arms(*record)) {
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
    crosswalk.odr_id = ids.next();
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

std::vector<std::pair<RoadId, Object>>
junction_stop_lines(const RoadNetwork& network, JunctionId junction, const StopLineParams& params) {
  std::vector<std::pair<RoadId, Object>> out;
  const Junction* record = network.junction(junction);
  if (record == nullptr) {
    return out;
  }

  OdrIdReserver ids(network);
  for (const RoadId arm : distinct_arms(*record)) {
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

    // Only the APPROACH lanes (leading into the junction) — one travel direction.
    double t_min = 0.0;
    double t_max = 0.0;
    bool any = false;
    for (const ContactLane& lane : driving_lanes_at(network, end, *contact, /*incoming=*/true)) {
      const double outer = lane.inner_t + (lane.odr_id > 0 ? lane.width : -lane.width);
      const double lo = std::min(lane.inner_t, outer);
      const double hi = std::max(lane.inner_t, outer);
      t_min = any ? std::min(t_min, lo) : lo;
      t_max = any ? std::max(t_max, hi) : hi;
      any = true;
    }
    if (!any || (t_max - t_min) < tol::kLength) {
      continue; // no approach lanes
    }

    const double length = road->plan_view.length();
    const double half = params.thickness_m / 2.0;
    const double s =
        *facing == ContactPoint::Start ? params.setback_m + half : length - params.setback_m - half;

    Object stop_line;
    stop_line.odr_id = ids.next();
    stop_line.type_str = "roadMark"; // a road-mark object (type stays None)
    stop_line.subtype = "signalLines";
    stop_line.s = std::clamp(s, 0.0, length);
    stop_line.t = (t_min + t_max) / 2.0;
    stop_line.length = params.thickness_m; // thin, along the road (u, hdg = 0)
    stop_line.width = t_max - t_min;       // across the approach lanes (v)
    out.emplace_back(arm, std::move(stop_line));
  }
  return out;
}

std::vector<std::pair<RoadId, Object>> junction_lane_arrows(const RoadNetwork& network,
                                                            JunctionId junction,
                                                            const LaneArrowParams& params) {
  std::vector<std::pair<RoadId, Object>> out;
  const Junction* record = network.junction(junction);
  if (record == nullptr) {
    return out;
  }

  OdrIdReserver ids(network);
  for (const RoadId arm : distinct_arms(*record)) {
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

    const double length = road->plan_view.length();
    const double s = *facing == ContactPoint::Start
                         ? params.setback_m + params.length_m / 2.0
                         : length - params.setback_m - params.length_m / 2.0;
    // The glyph points INTO the junction: +s for an End-facing arm (approach
    // lanes travel toward s = length), -s for a Start-facing one.
    const double hdg = *facing == ContactPoint::End ? 0.0 : std::numbers::pi;

    for (const ContactLane& lane : driving_lanes_at(network, end, *contact, /*incoming=*/true)) {
      const double center = lane.inner_t + ((lane.odr_id > 0 ? lane.width : -lane.width) / 2.0);
      Object arrow;
      arrow.odr_id = ids.next();
      arrow.type_str = "roadMark"; // a road-mark object (type stays None)
      arrow.subtype = params.glyph ? params.glyph(arm, lane) : std::string(kStraightArrow);
      if (arrow.subtype.empty()) {
        arrow.subtype = kStraightArrow; // a chooser that declines still writes a valid object
      }
      arrow.s = std::clamp(s, 0.0, length);
      arrow.t = center;
      arrow.hdg = hdg;
      arrow.length = params.length_m;               // along travel
      arrow.width = lane.width * params.width_frac; // narrower than the lane
      out.emplace_back(arm, std::move(arrow));
    }
  }
  return out;
}

std::vector<std::pair<LaneId, RoadMark>> junction_center_marks(const RoadNetwork& network,
                                                               JunctionId junction,
                                                               const CenterMarkParams& params) {
  std::vector<std::pair<LaneId, RoadMark>> out;
  const Junction* record = network.junction(junction);
  if (record == nullptr) {
    return out;
  }

  for (const RoadId arm : distinct_arms(*record)) {
    const Road* road = network.road(arm);
    if (road == nullptr) {
      continue;
    }
    // Every section, not just the first: the centre line runs the whole arm,
    // and a split or a lane-profile edit can leave an arm with several.
    for (const LaneSectionId section : road->sections) {
      for (const LaneId lane : network.lane_section(section)->lanes) {
        if (network.lane(lane)->odr_id != 0) {
          continue;
        }
        out.emplace_back(lane,
                         RoadMark{.s_offset = 0.0,
                                  .type = params.type,
                                  .width = params.width,
                                  .color = params.color});
      }
    }
  }
  return out;
}

} // namespace roadmaker::edit
