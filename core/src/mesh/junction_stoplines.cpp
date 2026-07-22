// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
#include "junction_stoplines_detail.hpp"
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

/// The lateral extent [t_min, t_max] the band covers over `lanes`, or nullopt
/// when the direction carries no driving lane wide enough to paint.
std::optional<std::array<double, 2>> lane_band(const std::vector<edit::ContactLane>& lanes) {
  double t_min = 0.0;
  double t_max = 0.0;
  bool any = false;
  for (const edit::ContactLane& lane : lanes) {
    const double outer = lane.inner_t + (lane.odr_id > 0 ? lane.width : -lane.width);
    const double lo = std::min(lane.inner_t, outer);
    const double hi = std::max(lane.inner_t, outer);
    t_min = any ? std::min(t_min, lo) : lo;
    t_max = any ? std::max(t_max, hi) : hi;
    any = true;
  }
  if (!any || (t_max - t_min) < tol::kLength) {
    return std::nullopt;
  }
  return std::array<double, 2>{t_min, t_max};
}

/// Everything one face of a span needs before the record is merged in.
struct SpanFace {
  double edge = 0.0;   ///< the span edge this face guards [m], clamped to the road
  double length = 0.0; ///< the road's plan-view length [m]

  /// The road end whose travel sense APPROACHES `edge` — the opposite contact
  /// of the face key. Traffic reaching the `s_start` face runs toward +s, the
  /// same sense in which traffic reaches a road's End; only `contact` is read
  /// (by driving_lanes_at, to decide which side leads in).
  RoadEnd approach;

  /// A synthetic contact state pinned to the span edge rather than to a road
  /// end: driving_lanes_at reads only `section` and `station` off it, so the
  /// lane widths and the lane set come from the section the span edge lies in.
  edit::ContactState sample;
};

/// Resolves one face of `span`. Empty when the road is stale, has no geometry,
/// or is too short to carry a band at all.
std::optional<SpanFace>
resolve_span_face(const RoadNetwork& network, const SpanArm& span, ContactPoint face) {
  const Road* road = network.road(span.road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  const double length = road->plan_view.length();
  if (length <= kStopLineThickness) {
    return std::nullopt;
  }
  // A span whose road later shortened is clamped, not rejected: the junction
  // outlives the edit exactly as a dormant StopLine record does.
  const double s_start = std::clamp(span.s_start, 0.0, length);
  const double s_end = std::clamp(std::max(span.s_end, s_start), 0.0, length);

  SpanFace out;
  out.length = length;
  out.edge = face == ContactPoint::Start ? s_start : s_end;
  out.approach =
      RoadEnd{.road = span.road,
              .contact = face == ContactPoint::Start ? ContactPoint::End : ContactPoint::Start};
  out.sample.section = section_at(network, span.road, out.edge);
  out.sample.station = out.edge;
  if (network.lane_section(out.sample.section) == nullptr) {
    return std::nullopt;
  }
  return out;
}

/// Appends the (at most two) faces of one SpanArm to `out`.
void append_span_faces(const RoadNetwork& network,
                       const Junction& junction,
                       const SpanArm& span,
                       std::vector<JunctionStopLineInfo>& out) {
  const Road* road = network.road(span.road);
  for (const ContactPoint face : {ContactPoint::Start, ContactPoint::End}) {
    const std::optional<SpanFace> geometry = resolve_span_face(network, span, face);
    if (!geometry.has_value()) {
      return; // the road itself is unusable — neither face exists
    }
    const RoadEnd key{.road = span.road, .contact = face};
    const StopLine* record = authored_record(junction, key);
    const bool flipped = record != nullptr && record->flipped;

    const std::optional<std::array<double, 2>> band = lane_band(edit::driving_lanes_at(
        network, geometry->approach, geometry->sample, /*incoming=*/!flipped));
    if (!band.has_value()) {
      continue; // a one-way road guards only the edge its traffic approaches
    }

    JunctionStopLineInfo info;
    info.arm = key;
    info.span_face = true;
    // The band sits OUTSIDE the span, so the room it has is the stretch of road
    // between the span edge and the near end of the road.
    const double room =
        face == ContactPoint::Start ? geometry->edge : geometry->length - geometry->edge;
    info.max_distance = std::max(0.0, room - kStopLineThickness);
    info.distance_authored = record != nullptr && record->distance.has_value();
    info.distance =
        std::clamp(info.distance_authored ? *record->distance : kStopLineDefaultDistance,
                   0.0,
                   info.max_distance);
    info.flipped = flipped;
    info.authored = record != nullptr;
    info.crosswalk_odr_id = record != nullptr ? record->crosswalk_odr_id : std::string{};
    info.span = (*band)[1] - (*band)[0];
    info.thickness = kStopLineThickness;
    info.t_center = ((*band)[0] + (*band)[1]) / 2.0;
    const double half = kStopLineThickness / 2.0;
    // Upstream of s_start, downstream of s_end. max_distance already keeps the
    // band whole on the road; the clamp only catches a span that begins (or
    // ends) inside the first half-thickness of it.
    info.s_center = face == ContactPoint::Start ? geometry->edge - info.distance - half
                                                : geometry->edge + info.distance + half;
    info.s_center = std::clamp(info.s_center, half, geometry->length - half);

    const mesh_detail::StationFrame frame = mesh_detail::make_frame(*road, info.s_center);
    const std::array<double, 3> left = mesh_detail::lateral_point(frame, (*band)[1]);
    const std::array<double, 3> right = mesh_detail::lateral_point(frame, (*band)[0]);
    info.left = {left[0], left[1]};
    info.right = {right[0], right[1]};

    out.push_back(std::move(info));
  }
}

} // namespace

namespace stopline_detail {

bool stopline_direction_has_lanes(const RoadNetwork& network,
                                  const Junction& junction,
                                  const RoadEnd& arm,
                                  bool flipped) {
  const auto span = std::ranges::find_if(
      junction.spans, [&](const SpanArm& entry) { return entry.road == arm.road; });
  if (span != junction.spans.end()) {
    const std::optional<SpanFace> geometry = resolve_span_face(network, *span, arm.contact);
    return geometry.has_value() &&
           lane_band(edit::driving_lanes_at(
                         network, geometry->approach, geometry->sample, /*incoming=*/!flipped))
               .has_value();
  }
  const Expected<edit::ContactState> contact = edit::contact_state(network, arm);
  return contact.has_value() &&
         lane_band(edit::driving_lanes_at(network, arm, *contact, /*incoming=*/!flipped))
             .has_value();
}

} // namespace stopline_detail

std::vector<JunctionStopLineInfo> junction_stoplines(const RoadNetwork& network,
                                                     JunctionId junction_id) {
  std::vector<JunctionStopLineInfo> out;
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return out;
  }

  // arms-xor-spans: a span (virtual) junction has neither arms nor connections,
  // so it solves faces instead of arm lines and the loop below never runs.
  if (!junction->spans.empty()) {
    for (const SpanArm& span : junction->spans) {
      append_span_faces(network, *junction, span, out);
    }
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
    const std::optional<std::array<double, 2>> band =
        lane_band(edit::driving_lanes_at(network, arm, *contact, /*incoming=*/!flipped));
    if (!band.has_value()) {
      continue; // no driving lanes in this direction — no line to draw
    }
    const double t_min = (*band)[0];
    const double t_max = (*band)[1];

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
