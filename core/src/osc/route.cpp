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

#include "roadmaker/osc/route.hpp"

#include "roadmaker/osc/rules.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace roadmaker::osc {
namespace {

Diagnostic
finding(Severity severity, std::string location, std::string message, std::string_view rule = {}) {
  return Diagnostic{.severity = severity,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = std::string(rule),
                    .road = {},
                    .lane = {}};
}

/// The road whose OpenDRIVE `@id` is `odr_id`.
///
/// `RoadNetwork::find_road` already does exactly this; it is named here only so
/// the resolution step reads as one operation.
RoadId road_by_odr_id(const RoadNetwork& network, const std::string& odr_id) {
  return network.find_road(odr_id);
}

/// The lane of `road` with OpenDRIVE `@id` `lane_odr_id` in the section
/// governing station `s`.
LaneId lane_at(const RoadNetwork& network, RoadId road, int lane_odr_id, double s) {
  const LaneSection* section = network.lane_section(section_at(network, road, s));
  if (section == nullptr) {
    return {};
  }
  for (const LaneId id : section->lanes) {
    const Lane* lane = network.lane(id);
    if (lane != nullptr && lane->odr_id == lane_odr_id) {
      return id;
    }
  }
  return {};
}

/// True when travelling this lane advances `s`.
///
/// The OpenDRIVE convention, and the same one `actor_world_pose` uses to aim an
/// actor: under RHT a right-hand lane (negative `@id`) travels with +s and a
/// left-hand lane against it, and under LHT the two swap (§11; the road's
/// `@rule` is what says which). `LaneDirection::Reversed` flips the answer —
/// it exists precisely to describe a lane whose traffic runs the other way (a
/// contraflow bus lane). `lane_travels_with_s` holds the whole convention so
/// signal facing and junction-arm selection cannot drift from it (#535).
bool travels_forward(const Lane& lane, TrafficRule rule) {
  return lane_travels_with_s(lane.odr_id, lane.direction, rule);
}

/// `travels_forward` for a lane reached through the network, so callers that
/// already hold the lane but not its road do not have to walk back up.
bool travels_forward(const RoadNetwork& network, const Lane& lane) {
  const LaneSection* section = network.lane_section(lane.section);
  const Road* road = section != nullptr ? network.road(section->road) : nullptr;
  return travels_forward(lane, road != nullptr ? road->rule : TrafficRule::RightHandTraffic);
}

/// The s-interval of `lane`'s section within its road: {s0, s_end}.
std::optional<std::pair<double, double>> section_span(const RoadNetwork& network,
                                                      const Lane& lane) {
  const LaneSection* section = network.lane_section(lane.section);
  if (section == nullptr) {
    return std::nullopt;
  }
  const Road* road = network.road(section->road);
  if (road == nullptr) {
    return std::nullopt;
  }
  const Expected<double> end = section_end(network, lane.section);
  return std::pair<double, double>{section->s0, end.has_value() ? *end : road->length};
}

/// The lane a route reaches by leaving `lane` in its direction of travel.
///
/// Three mechanisms, in the order OpenDRIVE composes them:
///   1. the next lane SECTION of the same road, via `Lane::successor`/
///      `predecessor` (§9.3);
///   2. the linked ROAD across the boundary, via the same lane link plus
///      `Road::successor`/`predecessor` (§9.2);
///   3. a JUNCTION, via `JunctionConnection::lane_links` (§12) — which is what
///      makes a solved path run along the junction's connecting roads, the
///      roads its authored maneuvers shape, rather than across it.
std::vector<LaneId> onward_lanes(const RoadNetwork& network, LaneId from) {
  std::vector<LaneId> out;
  const Lane* lane = network.lane(from);
  if (lane == nullptr) {
    return out;
  }
  const LaneSection* section = network.lane_section(lane->section);
  if (section == nullptr) {
    return out;
  }
  const Road* road = network.road(section->road);
  if (road == nullptr) {
    return out;
  }

  const bool forward = travels_forward(*lane, road->rule);
  // ALL the links, not the first: §11.6 mandates several where a lane splits
  // abruptly, and a route across a split genuinely has more than one onward
  // lane. `onward_lanes` already returns a vector, so this is the shape it
  // always wanted (#536).
  const std::vector<LaneLink>& linked = forward ? lane->successors : lane->predecessors;
  const auto links_to = [&linked](int odr_id) {
    return std::ranges::any_of(linked, [odr_id](const LaneLink& l) { return l.id == odr_id; });
  };

  // 1. Another section of the SAME road, when one lies ahead.
  const auto position = std::ranges::find(road->sections, lane->section);
  if (position != road->sections.end()) {
    const auto index = static_cast<std::size_t>(position - road->sections.begin());
    const bool has_next = forward ? index + 1 < road->sections.size() : index > 0;
    if (has_next) {
      const LaneSectionId next_id = road->sections[forward ? index + 1 : index - 1];
      const LaneSection* next = network.lane_section(next_id);
      // Within a road the kernel DOES author the lane link on every section
      // split (`edit::split_lane_section`), so requiring it here is not the
      // trap it is at a road boundary.
      if (next != nullptr && !linked.empty()) {
        for (const LaneId candidate : next->lanes) {
          const Lane* other = network.lane(candidate);
          if (other != nullptr && links_to(other->odr_id)) {
            out.push_back(candidate);
          }
        }
      }
      // A lane whose link is unset simply ends here; that is a topology
      // finding, not something to guess around.
      return out;
    }
  }

  // 2/3. The road boundary in the direction of travel.
  const std::optional<RoadLink>& link = forward ? road->successor : road->predecessor;
  if (!link.has_value()) {
    return out;
  }

  if (const auto* target = std::get_if<RoadId>(&link->target)) {
    const Road* other = network.road(*target);
    if (other == nullptr || other->sections.empty()) {
      return out;
    }
    // We arrive at the CONTACT end the link names, so the section that governs
    // the arrival is that road's first or last.
    const LaneSectionId arrival =
        link->contact == ContactPoint::Start ? other->sections.front() : other->sections.back();
    const LaneSection* arrival_section = network.lane_section(arrival);
    if (arrival_section == nullptr) {
      return out;
    }
    // ★ THE LANE LINK IS OFTEN ABSENT, AND THAT IS NOT A BROKEN NETWORK.
    // OpenDRIVE §9.3 defines lane continuation with `<lane><link>`, and this
    // kernel's model, reader and WRITER all carry it — but RoadMaker's own
    // authoring path never sets it across a plain road weld (only across a
    // section split and inside a junction). So every RoadMaker-authored chain
    // reaches here with `linked` unset, and a resolver that required the record
    // would report EVERY road-to-road route as unreachable while the roads are
    // visibly joined.
    //
    // The fallback matches by lane `@id`, which is exactly what a welded pair
    // means: `create_linked_road` welds identical cross-sections, so the
    // same-id lane IS the continuation. When the record IS present it wins,
    // because a foreign file may renumber across the joint and only the file
    // knows that. Tracked as a follow-up on the authoring side; the fallback is
    // not a substitute for emitting the records.
    for (const LaneId candidate : arrival_section->lanes) {
      const Lane* neighbour = network.lane(candidate);
      if (neighbour == nullptr) {
        continue;
      }
      // The records win when present — a foreign file may renumber across the
      // joint and only the file knows that — and a lane with several of them
      // reaches every one. Only a lane with NO record falls back to same-id.
      const bool reached =
          linked.empty() ? neighbour->odr_id == lane->odr_id : links_to(neighbour->odr_id);
      if (reached) {
        out.push_back(candidate);
      }
    }
    return out;
  }

  const auto* junction_id = std::get_if<JunctionId>(&link->target);
  if (junction_id == nullptr) {
    return out;
  }
  const Junction* junction = network.junction(*junction_id);
  if (junction == nullptr) {
    return out;
  }
  for (const JunctionConnection& connection : junction->connections) {
    if (connection.incoming_road != section->road) {
      continue;
    }
    const Road* connecting = network.road(connection.connecting_road);
    if (connecting == nullptr || connecting->sections.empty()) {
      continue;
    }
    // The connecting road is entered at its contact point, so the governing
    // section is its first or last — the same rule as a plain road link.
    const LaneSectionId entry = connection.contact_point == ContactPoint::Start
                                    ? connecting->sections.front()
                                    : connecting->sections.back();
    const LaneSection* entry_section = network.lane_section(entry);
    if (entry_section == nullptr) {
      continue;
    }
    for (const auto& [incoming_lane, connecting_lane] : connection.lane_links) {
      if (incoming_lane != lane->odr_id) {
        continue;
      }
      for (const LaneId candidate : entry_section->lanes) {
        const Lane* neighbour = network.lane(candidate);
        if (neighbour != nullptr && neighbour->odr_id == connecting_lane) {
          out.push_back(candidate);
        }
      }
    }
  }
  return out;
}

/// The lane chain from `from` to `to`, inclusive of both, or empty when no
/// drivable path exists within `kMaxRouteSearchLanes`.
std::vector<LaneId> shortest_lane_path(const RoadNetwork& network, LaneId from, LaneId to) {
  if (from == to) {
    return {from};
  }
  std::unordered_map<LaneId, LaneId> came_from;
  std::unordered_set<LaneId> seen{from};
  std::deque<LaneId> queue{from};

  while (!queue.empty() && seen.size() <= kMaxRouteSearchLanes) {
    const LaneId current = queue.front();
    queue.pop_front();
    for (const LaneId next : onward_lanes(network, current)) {
      if (!seen.insert(next).second) {
        continue;
      }
      came_from[next] = current;
      if (next == to) {
        std::vector<LaneId> path{to};
        for (LaneId step = to; step != from;) {
          step = came_from[step];
          path.push_back(step);
        }
        std::ranges::reverse(path);
        return path;
      }
      queue.push_back(next);
    }
  }
  return {};
}

/// A waypoint resolved to a lane, or the reason it could not be.
struct Anchor {
  LaneId lane;
  RoadId road;
  int lane_odr_id = 0;
  double s = 0.0;
};

/// True when `road` is a connecting road inside a junction.
bool inside_a_junction(const RoadNetwork& network, RoadId road) {
  const Road* value = network.road(road);
  return value != nullptr && value->junction.is_valid();
}

} // namespace

std::vector<AssignedRoute> assigned_routes(const Scenario& scenario) {
  std::vector<AssignedRoute> out;
  for (const Private& entry : scenario.storyboard.init.actions.privates) {
    for (const PrivateAction& action : entry.actions) {
      if (!action.routing.has_value() || !action.routing->assign_route.has_value()) {
        continue;
      }
      const AssignRouteAction& assign = *action.routing->assign_route;
      if (!assign.route.has_value()) {
        continue; // a catalog reference: nothing inline to resolve
      }
      out.push_back(AssignedRoute{.entity_ref = entry.entity_ref, .route = &*assign.route});
    }
  }
  return out;
}

ResolvedRoute resolve_route(const RoadNetwork& network, const Route& route) {
  ResolvedRoute out;
  const std::string location =
      fmt::format("Route[{}]", route.name.empty() ? "<unnamed>" : route.name);

  // --- resolve every waypoint first ----------------------------------------
  //
  // Separately from joining them, so a route with one dead waypoint reports
  // exactly that rather than "no path from waypoint 2 to waypoint 3", which
  // names the wrong problem.
  std::vector<std::optional<Anchor>> anchors(route.waypoints.size());
  bool every_waypoint_resolved = true;

  for (std::size_t index = 0; index < route.waypoints.size(); ++index) {
    const RouteWaypoint& waypoint = route.waypoints[index];
    const std::string waypoint_location = fmt::format("{}/Waypoint[{}]", location, index);

    const auto* lane_position = std::get_if<LanePosition>(&waypoint.position);
    if (lane_position == nullptr) {
      every_waypoint_resolved = false;
      const bool world = std::holds_alternative<WorldPosition>(waypoint.position);
      out.findings.push_back(finding(
          Severity::Warning,
          waypoint_location,
          world ? "the waypoint is a world position, which names no lane; the route cannot be "
                  "resolved against the network"
                : "the waypoint is a road position, which names a road but no lane; the route "
                  "cannot be resolved against the network",
          rules::kAmbiguousRouteWaypoints));
      continue;
    }

    const RoadId road = road_by_odr_id(network, lane_position->road_id);
    if (!road.is_valid()) {
      every_waypoint_resolved = false;
      out.findings.push_back(
          finding(Severity::Error,
                  waypoint_location,
                  fmt::format("road '{}' is not in this network — the route no longer resolves",
                              lane_position->road_id),
                  rules::kRoadLaneExists));
      continue;
    }

    int lane_odr_id = 0;
    try {
      lane_odr_id = std::stoi(lane_position->lane_id);
    } catch (...) {
      // A non-integer lane id is LEGAL (the temporary lane layer, §7.6) and
      // simply not resolvable against a network this version can read.
      every_waypoint_resolved = false;
      out.findings.push_back(
          finding(Severity::Warning,
                  waypoint_location,
                  fmt::format("lane id '{}' is not an integer, so it names a temporary lane layer "
                              "this version cannot resolve",
                              lane_position->lane_id)));
      continue;
    }

    const LaneId lane = lane_at(network, road, lane_odr_id, lane_position->s);
    if (!lane.is_valid()) {
      every_waypoint_resolved = false;
      out.findings.push_back(
          finding(Severity::Error,
                  waypoint_location,
                  fmt::format("road '{}' has no lane {} at s {} — the route no longer resolves",
                              lane_position->road_id,
                              lane_odr_id,
                              lane_position->s),
                  rules::kRoadLaneExists));
      continue;
    }

    if (inside_a_junction(network, road)) {
      // "Route waypoints should not be located in junctions to avoid
      // ambiguity." Reported, not refused: it resolves, it is merely a route
      // that two simulators may read differently.
      out.findings.push_back(
          finding(Severity::Warning,
                  waypoint_location,
                  fmt::format("the waypoint is on connecting road '{}', which is inside a "
                              "junction; a waypoint there does not say which manoeuvre is meant",
                              lane_position->road_id),
                  rules::kRouteWaypointsLocations));
    }

    anchors[index] =
        Anchor{.lane = lane, .road = road, .lane_odr_id = lane_odr_id, .s = lane_position->s};
  }

  // --- join consecutive anchors --------------------------------------------
  bool every_gap_joined = true;
  for (std::size_t index = 0; index + 1 < anchors.size(); ++index) {
    const std::optional<Anchor>& from = anchors[index];
    const std::optional<Anchor>& to = anchors[index + 1];
    if (!from.has_value() || !to.has_value()) {
      every_gap_joined = false;
      continue; // already reported above; do not name the same problem twice
    }

    const std::vector<LaneId> path = shortest_lane_path(network, from->lane, to->lane);
    if (path.empty()) {
      every_gap_joined = false;
      out.findings.push_back(finding(
          Severity::Error,
          fmt::format("{}/Waypoint[{}]", location, index),
          fmt::format("no drivable path from lane {} of road '{}' to lane {} of road '{}' — the "
                      "route is invalidated, and is reported rather than re-routed",
                      from->lane_odr_id,
                      network.road(from->road)->odr_id,
                      to->lane_odr_id,
                      network.road(to->road)->odr_id)));
      continue;
    }

    for (std::size_t step = 0; step < path.size(); ++step) {
      const Lane* lane = network.lane(path[step]);
      const LaneSection* section = lane == nullptr ? nullptr : network.lane_section(lane->section);
      if (lane == nullptr || section == nullptr) {
        continue;
      }
      const std::optional<std::pair<double, double>> span = section_span(network, *lane);
      if (!span.has_value()) {
        continue;
      }
      const bool forward = travels_forward(network, *lane);
      // The first leg starts at the origin waypoint's own station and the last
      // ends at the destination's; everything between spans its whole section,
      // in the direction of travel.
      const bool first = step == 0;
      const bool last = step + 1 == path.size();
      const double entry = forward ? span->first : span->second;
      const double exit = forward ? span->second : span->first;

      RouteLeg leg;
      leg.road = section->road;
      leg.lane = path[step];
      leg.lane_odr_id = lane->odr_id;
      leg.s_start = first ? from->s : entry;
      leg.s_end = last ? to->s : exit;

      // A path that re-enters the lane the previous leg ended on would draw the
      // same stretch twice; consecutive duplicates are merged instead.
      if (!out.legs.empty() && out.legs.back().lane == leg.lane) {
        out.legs.back().s_end = leg.s_end;
        continue;
      }
      out.legs.push_back(leg);
    }
  }

  // A single-waypoint route has nothing to join, and is not "complete" — the
  // schema needs two, and a caller must not read one waypoint as a solved path.
  out.complete = every_waypoint_resolved && every_gap_joined && route.waypoints.size() >= 2;
  if (!out.complete && out.findings.empty()) {
    out.findings.push_back(
        finding(Severity::Warning,
                location,
                fmt::format("the route has {} waypoint(s); at least two are needed to define one",
                            route.waypoints.size())));
  }
  return out;
}

std::vector<Diagnostic> validate_routes(const RoadNetwork& network, const Scenario& scenario) {
  std::vector<Diagnostic> findings;
  for (const AssignedRoute& assigned : assigned_routes(scenario)) {
    ResolvedRoute resolved = resolve_route(network, *assigned.route);
    for (Diagnostic& found : resolved.findings) {
      // The entity is what a user recognises; the route name alone leaves them
      // hunting for whose route it was.
      found.location = fmt::format("Entity[{}]/{}", assigned.entity_ref, found.location);
      findings.push_back(std::move(found));
    }
  }
  return findings;
}

} // namespace roadmaker::osc
