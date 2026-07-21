#include "roadmaker/mesh/junction_stoplines.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "../edit/markings_detail.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using edit::markings_detail::distinct_arms;
using edit::markings_detail::facing_end;

/// The authored record for `arm`, or nullptr when the arm is fully derived.
/// Records whose arm no longer matches any arm of the junction are simply never
/// looked up, which is what makes them dormant rather than an error.
const StopLine* authored_record(const Junction& junction, const RoadEnd& arm) {
  const auto entry = std::ranges::find_if(
      junction.stoplines, [&](const StopLine& record) { return record.arm == arm; });
  return entry == junction.stoplines.end() ? nullptr : &*entry;
}

/// True when a live, untagged `signalLines` object already sits on the
/// junction-facing half of `arm` — a stop line a legacy or foreign file painted
/// itself. Its object keeps rendering through the mesher's ordinary object
/// branch, so deriving a default line here as well would draw two bands on top
/// of each other. Objects RoadMaker itself materializes on write never appear
/// here: the reader absorbs the rm:stopline-tagged ones into records instead of
/// adding them to the arena.
bool legacy_stop_line_present(const RoadNetwork& network, const RoadEnd& arm, double road_length) {
  const double midpoint = road_length / 2.0;
  bool found = false;
  network.for_each_object([&](ObjectId, const Object& object) {
    if (found || object.road != arm.road || object.subtype != "signalLines") {
      return;
    }
    const bool near_half =
        arm.contact == ContactPoint::Start ? object.s <= midpoint : object.s >= midpoint;
    found = near_half;
  });
  return found;
}

} // namespace

std::vector<JunctionStopLineInfo> junction_stoplines(const RoadNetwork& network,
                                                     JunctionId junction_id) {
  std::vector<JunctionStopLineInfo> out;
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return out;
  }

  for (const RoadId arm_road : distinct_arms(*junction)) {
    const Road* road = network.road(arm_road);
    if (road == nullptr || road->plan_view.empty()) {
      continue;
    }
    const std::optional<ContactPoint> facing = facing_end(network, arm_road, junction_id);
    if (!facing.has_value()) {
      continue;
    }
    const RoadEnd arm{.road = arm_road, .contact = *facing};
    const Expected<edit::ContactState> contact = edit::contact_state(network, arm);
    if (!contact) {
      continue;
    }

    const double length = road->plan_view.length();
    if (length <= kStopLineThickness || legacy_stop_line_present(network, arm, length)) {
      continue;
    }

    const StopLine* record = authored_record(*junction, arm);
    const bool flipped = record != nullptr && record->flipped;

    // The lanes the band spans: by default those leading INTO the junction
    // (traffic that must stop), flipped to the outgoing side on request.
    double t_min = 0.0;
    double t_max = 0.0;
    bool any = false;
    for (const edit::ContactLane& lane :
         edit::driving_lanes_at(network, arm, *contact, /*incoming=*/!flipped)) {
      const double outer = lane.inner_t + (lane.odr_id > 0 ? lane.width : -lane.width);
      const double lo = std::min(lane.inner_t, outer);
      const double hi = std::max(lane.inner_t, outer);
      t_min = any ? std::min(t_min, lo) : lo;
      t_max = any ? std::max(t_max, hi) : hi;
      any = true;
    }
    if (!any || (t_max - t_min) < tol::kLength) {
      continue; // no driving lanes in this direction — no line to draw
    }

    JunctionStopLineInfo info;
    info.arm = arm;
    info.max_distance = length - kStopLineThickness;
    info.distance_authored = record != nullptr && record->distance.has_value();
    info.distance =
        std::clamp(info.distance_authored ? *record->distance : kStopLineDefaultDistance,
                   0.0,
                   info.max_distance);
    info.flipped = flipped;
    info.authored = record != nullptr;
    info.crosswalk_odr_id = record != nullptr ? record->crosswalk_odr_id : std::string{};
    info.span = t_max - t_min;
    info.thickness = kStopLineThickness;
    info.t_center = (t_min + t_max) / 2.0;
    // The setback is measured from the mouth inward, so which way it runs
    // depends on which end of the arm the junction is at; `distance` is already
    // clamped so the band always lands whole on the road.
    const double half = kStopLineThickness / 2.0;
    info.s_center =
        *facing == ContactPoint::Start ? info.distance + half : length - info.distance - half;

    const mesh_detail::StationFrame frame = mesh_detail::make_frame(*road, info.s_center);
    const std::array<double, 3> left = mesh_detail::lateral_point(frame, t_max);
    const std::array<double, 3> right = mesh_detail::lateral_point(frame, t_min);
    info.left = {left[0], left[1]};
    info.right = {right[0], right[1]};

    out.push_back(std::move(info));
  }
  return out;
}

} // namespace roadmaker
