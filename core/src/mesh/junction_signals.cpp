#include "roadmaker/mesh/junction_signals.hpp"

#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace roadmaker {

namespace {

/// The @orientation a signal must carry to apply to traffic reaching the
/// junction on `arm` (§14.1: "+" applies to traffic travelling in the positive
/// reference-line direction). Traffic reaching a road's End runs toward +s;
/// traffic reaching its Start runs toward -s.
ObjectOrientation approach_orientation(const RoadEnd& arm) {
  return arm.contact == ContactPoint::Start ? ObjectOrientation::Minus : ObjectOrientation::Plus;
}

/// The station [m] of the junction mouth on `arm`, i.e. where the arm road ends
/// at the junction.
double mouth_station(const Road& road, const RoadEnd& arm) {
  return arm.contact == ContactPoint::Start ? 0.0 : road.plan_view.length();
}

} // namespace

std::vector<JunctionApproachInfo> junction_signals(const RoadNetwork& network,
                                                   JunctionId junction_id) {
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return {};
  }

  // Gating is DERIVED from the maneuvers, which is also what makes a foreign
  // junction readable: they come off `connections`, not off `arms`.
  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction_id);
  std::vector<JunctionApproachInfo> out;
  for (const JunctionManeuverInfo& maneuver : maneuvers) {
    const auto entry = std::ranges::find_if(
        out, [&](const JunctionApproachInfo& info) { return info.arm == maneuver.from; });
    if (entry == out.end()) {
      JunctionApproachInfo info;
      info.arm = maneuver.from;
      // The tangent leaving the arm into the junction — the maneuver query
      // already solved it, so the two cannot drift.
      info.heading = maneuver.start_heading;
      info.gated.push_back(maneuver.road);
      out.push_back(std::move(info));
    } else {
      entry->gated.push_back(maneuver.road);
    }
  }
  if (out.empty()) {
    return out;
  }

  // The placement anchor comes from the stop-line solve, never from a second
  // derivation of the same station.
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction_id);

  for (JunctionApproachInfo& info : out) {
    const Road* road = network.road(info.arm.road);
    if (road == nullptr || road->plan_view.empty()) {
      continue; // maneuver query already vetted the contact; be defensive anyway
    }
    const double length = road->plan_view.length();
    const double mouth = mouth_station(*road, info.arm);

    const auto line = std::ranges::find_if(lines, [&](const JunctionStopLineInfo& candidate) {
      return !candidate.span_face && candidate.arm == info.arm;
    });
    if (line != lines.end()) {
      info.s_stop = line->s_center;
      info.t_center = line->t_center;
    } else {
      // No solvable line (a one-way arm, or a legacy line the file paints
      // itself): fall back to the same default setback the line would have
      // used, on the reference line.
      info.s_stop = info.arm.contact == ContactPoint::Start
                        ? std::min(kStopLineDefaultDistance, length)
                        : std::max(0.0, length - kStopLineDefaultDistance);
      info.t_center = 0.0;
    }

    const ObjectOrientation toward = approach_orientation(info.arm);
    for (const SignalId signal_id : signals_of(network, info.arm.road)) {
      const Signal* signal = network.signal(signal_id);
      if (signal == nullptr) {
        continue;
      }
      if (std::abs(signal->s - mouth) > kSignalApproachWindow) {
        continue;
      }
      if (signal->orientation != toward && signal->orientation != ObjectOrientation::None) {
        continue;
      }
      info.signal_ids.push_back(signal_id);
      info.dynamic = info.dynamic || signal->dynamic.value_or(false);
    }
  }

  // Signal group membership (§14.6): a controller belongs to an approach when
  // one of its <control> children names a signal resolved onto that approach.
  // Dangling references simply never match.
  network.for_each_controller([&](ControllerId, const Controller& controller) {
    if (controller.controls.empty() || controller.odr_id.empty()) {
      return;
    }
    std::set<std::string> controlled;
    for (const Control& control : controller.controls) {
      controlled.insert(control.signal_odr_id);
    }
    for (JunctionApproachInfo& info : out) {
      const bool matches = std::ranges::any_of(info.signal_ids, [&](SignalId signal_id) {
        const Signal* signal = network.signal(signal_id);
        return signal != nullptr && controlled.contains(signal->odr_id);
      });
      if (matches && std::ranges::find(info.controller_odr_ids, controller.odr_id) ==
                         info.controller_odr_ids.end()) {
        info.controller_odr_ids.push_back(controller.odr_id);
      }
    }
  });

  return out;
}

} // namespace roadmaker
