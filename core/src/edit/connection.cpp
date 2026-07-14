#include "roadmaker/edit/connection.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <variant>

namespace roadmaker::edit {

namespace {

/// dz/ds of a piecewise-cubic profile at station s (0 for an empty profile).
/// Moved verbatim from operations.cpp (was profile_grade).
double profile_grade(std::span<const Poly3> profile, double s) {
  if (profile.empty()) {
    return 0.0;
  }
  const Poly3* covering = &profile.front();
  for (const Poly3& poly : profile) {
    if (poly.s <= s + tol::kLength) {
      covering = &poly;
    }
  }
  const double ds = s - covering->s;
  return covering->b + (2.0 * covering->c * ds) + (3.0 * covering->d * ds * ds);
}

/// Width [m] of `lane` at road station `road_s` (widths are section-local,
/// evaluated on the last record starting at or before the station). Moved
/// verbatim from operations.cpp (was lane_width_at).
double lane_width_at(const Lane& lane, const LaneSection& section, double road_s) {
  if (lane.widths.empty()) {
    return 0.0;
  }
  const double local = road_s - section.s0;
  const Poly3* width = &lane.widths.front();
  for (const Poly3& poly : lane.widths) {
    if (poly.s <= local + tol::kLength) {
      width = &poly;
    }
  }
  const double ds = local - width->s;
  return width->a + (width->b * ds) + (width->c * ds * ds) + (width->d * ds * ds * ds);
}

/// Guide waypoints (with locked headings) approximating a turn as
/// straight-leg + circular-fillet + straight-leg. Moved verbatim from
/// operations.cpp (was fillet_guides / TurnGuide); the exact clothoid-peak
/// control the junction connectors have always used.
struct TurnGuide {
  std::vector<Waypoint> points;
  std::vector<double> headings;
};

TurnGuide fillet_guides(const std::array<double, 2>& a,
                        double heading_a,
                        const std::array<double, 2>& b,
                        double heading_b) {
  constexpr double kPi = std::numbers::pi;
  const double deflection = std::abs(std::remainder(heading_b - heading_a, 2.0 * kPi));
  constexpr double kMinDeflection = 10.0 * kPi / 180.0;
  constexpr double kMaxDeflection = 170.0 * kPi / 180.0;
  if (deflection < kMinDeflection || deflection > kMaxDeflection) {
    return {};
  }
  const double d1x = std::cos(heading_a), d1y = std::sin(heading_a);
  const double d2x = std::cos(heading_b), d2y = std::sin(heading_b);
  const double det = (d1x * d2y) - (d1y * d2x);
  if (std::abs(det) < 1e-9) {
    return {};
  }
  const double rx = b[0] - a[0];
  const double ry = b[1] - a[1];
  const double t = ((rx * d2y) - (ry * d2x)) / det;
  const double u = -((rx * d1y) - (ry * d1x)) / det;
  if (t <= tol::kLength || u <= tol::kLength) {
    return {};
  }
  const std::array<double, 2> corner{a[0] + (t * d1x), a[1] + (t * d1y)};
  const double leg = std::min(t, u);
  const double half_angle = (kPi - deflection) / 2.0;
  const double radius = leg / std::tan(deflection / 2.0);
  double bx = -d1x + d2x;
  double by = -d1y + d2y;
  const double blen = std::hypot(bx, by);
  if (blen < 1e-9) {
    return {};
  }
  bx /= blen;
  by /= blen;

  TurnGuide guide;
  constexpr double kMinGuideSpacing = 0.5;
  if (t - leg > kMinGuideSpacing) {
    guide.points.push_back(Waypoint{corner[0] - (leg * d1x), corner[1] - (leg * d1y)});
    guide.headings.push_back(heading_a);
  }
  const double reach = radius * ((1.0 / std::sin(half_angle)) - 1.0);
  guide.points.push_back(Waypoint{corner[0] + (bx * reach), corner[1] + (by * reach)});
  guide.headings.push_back(std::atan2(d1y + d2y, d1x + d2x));
  if (u - leg > kMinGuideSpacing) {
    guide.points.push_back(Waypoint{corner[0] + (leg * d2x), corner[1] + (leg * d2y)});
    guide.headings.push_back(heading_b);
  }
  return guide;
}

/// Cubic Hermite elevation matching z and grade at both ends (the connecting-
/// road elevation formula, shared with operations.cpp's connecting_elevation).
/// Flat/degenerate → no profile (the OpenDRIVE default).
std::vector<Poly3> hermite_elevation(double z0, double g0, double z1, double g1, double length) {
  const bool flat = std::abs(z0) < tol::kLength && std::abs(z1) < tol::kLength &&
                    std::abs(g0) < tol::kLength && std::abs(g1) < tol::kLength;
  if (flat || length <= tol::kLength) {
    return {};
  }
  return {Poly3{.s = 0.0,
                .a = z0,
                .b = g0,
                .c = ((3.0 * (z1 - z0)) - (((2.0 * g0) + g1) * length)) / (length * length),
                .d = ((2.0 * (z0 - z1)) + ((g0 + g1) * length)) / (length * length * length)}};
}

/// Maximum |curvature| [1/m] over a reference line, sampled uniformly.
double max_abs_curvature(const ReferenceLine& line) {
  double worst = 0.0;
  constexpr int kSamples = 32;
  const double length = line.length();
  for (int i = 0; i <= kSamples; ++i) {
    const double s = length * (static_cast<double>(i) / kSamples);
    worst = std::max(worst, std::abs(line.evaluate(s).curvature));
  }
  return worst;
}

} // namespace

std::array<double, 2> contact_lateral(const ContactState& contact, double t) {
  return {contact.x - (t * std::sin(contact.road_hdg)),
          contact.y + (t * std::cos(contact.road_hdg))};
}

Expected<ContactState> contact_state(const RoadNetwork& network, const RoadEnd& end) {
  const Road* road = network.road(end.road);
  if (road == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale road id in road end");
  }
  if (road->sections.empty()) {
    return make_error(ErrorCode::InvalidArgument, "road has no lane sections", road->odr_id);
  }
  constexpr double kPi = std::numbers::pi;
  ContactState out;
  if (end.contact == ContactPoint::Start) {
    const PathPoint pose = road->plan_view.evaluate(0.0);
    out = ContactState{.x = pose.x,
                       .y = pose.y,
                       .into_hdg = pose.hdg + kPi, // travel toward decreasing s enters
                       .out_hdg = pose.hdg,        // body continues along +s
                       .road_hdg = pose.hdg,
                       // Curvature signed along travel INTO the junction: a Start
                       // end runs opposite +s, so the plan-view curvature flips.
                       .curvature = -pose.curvature,
                       .z = eval_profile(road->elevation, 0.0),
                       .grade = profile_grade(road->elevation, 0.0),
                       .section = road->sections.front(),
                       .station = 0.0};
  } else {
    const double station = road->plan_view.length();
    const PathPoint pose = road->plan_view.evaluate(station);
    out = ContactState{.x = pose.x,
                       .y = pose.y,
                       .into_hdg = pose.hdg,      // travel toward increasing s enters
                       .out_hdg = pose.hdg + kPi, // body continues along -s
                       .road_hdg = pose.hdg,
                       .curvature = pose.curvature,
                       .z = eval_profile(road->elevation, station),
                       .grade = profile_grade(road->elevation, station),
                       .section = road->sections.back(),
                       .station = station};
  }
  return out;
}

std::vector<ContactLane> driving_lanes_at(const RoadNetwork& network,
                                          const RoadEnd& end,
                                          const ContactState& contact,
                                          bool incoming) {
  const Road& road = *network.road(end.road);
  const LaneSection& section = *network.lane_section(contact.section);
  const bool positive_leads_in = end.contact == ContactPoint::Start;
  const bool want_positive = incoming ? positive_leads_in : !positive_leads_in;
  const double center_t = eval_profile(road.lane_offset, contact.station);
  std::vector<ContactLane> lanes;
  double left_cum = 0.0;
  double right_cum = 0.0;
  std::vector<LaneId> ordered(section.lanes.begin(), section.lanes.end());
  std::ranges::reverse(ordered); // now -N..-1, 0, +1..+N
  for (const LaneId lane_id : ordered) {
    const Lane& lane = *network.lane(lane_id);
    if (lane.odr_id <= 0) {
      continue;
    }
    const double width = lane_width_at(lane, section, contact.station);
    if (want_positive && lane.type == LaneType::Driving) {
      lanes.push_back(
          ContactLane{.odr_id = lane.odr_id, .width = width, .inner_t = center_t + left_cum});
    }
    left_cum += width;
  }
  if (!want_positive) {
    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id >= 0) {
        continue;
      }
      const double width = lane_width_at(lane, section, contact.station);
      if (lane.type == LaneType::Driving) {
        lanes.push_back(
            ContactLane{.odr_id = lane.odr_id, .width = width, .inner_t = center_t - right_cum});
      }
      right_cum += width;
    }
  }
  std::ranges::reverse(lanes); // curb-in (outermost first)
  return lanes;
}

Expected<Connector> fit_connector(const ConnectorEndpoint& a,
                                  const ConnectorEndpoint& b,
                                  const ConnectorParams& params) {
  std::vector<Waypoint> waypoints{Waypoint{a.x, a.y}};
  std::vector<double> headings{a.heading};
  const TurnGuide guide = fillet_guides({a.x, a.y}, a.heading, {b.x, b.y}, b.heading);
  waypoints.insert(waypoints.end(), guide.points.begin(), guide.points.end());
  headings.insert(headings.end(), guide.headings.begin(), guide.headings.end());
  waypoints.push_back(Waypoint{b.x, b.y});
  headings.push_back(b.heading);

  const double dist = std::hypot(b.x - a.x, b.y - a.y);
  // G2 matches the endpoint curvatures via the three-arc interpolant (no kink,
  // finding 3); G1 uses the straight-fillet-straight guide chain (byte-identical
  // to the junction connectors).
  auto line = params.g2 ? fit_g2_three_arc(Waypoint{a.x, a.y},
                                           a.heading,
                                           a.curvature,
                                           Waypoint{b.x, b.y},
                                           b.heading,
                                           b.curvature)
                        : fit_clothoid_path(waypoints, headings);
  if (!line.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      params.g2 ? "G2 connector fit failed" : "clothoid fit failed");
  }
  if (params.max_loop_factor > 0.0 && line->length() > params.max_loop_factor * dist) {
    return make_error(ErrorCode::InvalidArgument, "fitted turn loops");
  }
  if (params.min_turn_radius_m > 0.0) {
    const double kappa = max_abs_curvature(*line);
    if (kappa > tol::kCurvatureEpsilon && (1.0 / kappa) < params.min_turn_radius_m) {
      return make_error(ErrorCode::InvalidArgument, "turn radius below the minimum");
    }
  }
  const double length = line->length();
  return Connector{.line = std::move(*line),
                   .elevation = hermite_elevation(a.z, a.grade, b.z, b.grade, length)};
}

Expected<Pose2D> aligned_pose_on_road(const RoadNetwork& network,
                                      RoadId road,
                                      double s,
                                      std::optional<Side> branch) {
  const Road* r = network.road(road);
  if (r == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale road id");
  }
  const double length = r->plan_view.length();
  if (s < -tol::kLength || s > length + tol::kLength) {
    return make_error(ErrorCode::InvalidArgument, "station is outside the road", r->odr_id);
  }
  const double clamped = std::clamp(s, 0.0, length);
  const PathPoint pose = r->plan_view.evaluate(clamped);
  double heading = pose.hdg;
  if (branch.has_value()) {
    heading += (*branch == Side::Left ? 1.0 : -1.0) * (std::numbers::pi / 2.0);
  }
  return Pose2D{.x = pose.x, .y = pose.y, .heading = heading};
}

std::optional<JunctionId> junction_at_end(const RoadNetwork& network, const RoadEnd& end) {
  std::optional<JunctionId> found;
  network.for_each_junction([&](JunctionId id, const Junction& junction) {
    if (found.has_value()) {
      return;
    }
    if (std::ranges::find(junction.arms, end) != junction.arms.end()) {
      found = id;
    }
  });
  return found;
}

std::optional<JunctionId> matching_junction(const RoadNetwork& network,
                                            std::span<const RoadEnd> ends) {
  std::optional<JunctionId> found;
  network.for_each_junction([&](JunctionId id, const Junction& junction) {
    if (found.has_value() || junction.arms.size() != ends.size() || ends.empty()) {
      return;
    }
    // Order-free exact set match: every arm is in `ends` and vice versa (arms
    // are unique within a junction, so equal sizes + one-way containment suffice).
    const bool all_present = std::ranges::all_of(junction.arms, [&](const RoadEnd& arm) {
      return std::ranges::find(ends, arm) != ends.end();
    });
    if (all_present) {
      found = id;
    }
  });
  return found;
}

Expected<WeldReport> verify_junction_welds(const RoadNetwork& network, JunctionId junction_id) {
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale junction id");
  }
  WeldReport report;
  const auto arm_of = [&](const RoadLink& link) -> std::optional<RoadEnd> {
    if (const RoadId* road = std::get_if<RoadId>(&link.target)) {
      return RoadEnd{.road = *road, .contact = link.contact};
    }
    return std::nullopt;
  };
  // The connecting road anchors its reference line on the LINKED lane's inner
  // boundary (contact_lateral at that lane's inner_t) — the same anchor
  // plan_junction lays it on — so the weld point is the boundary, not the arm's
  // reference point.
  const auto inner_t_of = [&](const std::vector<ContactLane>& lanes,
                              int odr_id) -> std::optional<double> {
    for (const ContactLane& lane : lanes) {
      if (lane.odr_id == odr_id) {
        return lane.inner_t;
      }
    }
    return std::nullopt;
  };
  const auto accumulate =
      [&](const PathPoint& tip, double expected_heading, const ContactState& arm, double inner_t) {
        const std::array<double, 2> weld = contact_lateral(arm, inner_t);
        report.max_position_gap =
            std::max(report.max_position_gap, std::hypot(tip.x - weld[0], tip.y - weld[1]));
        report.max_heading_gap =
            std::max(report.max_heading_gap,
                     std::abs(std::remainder(tip.hdg - expected_heading, 2.0 * std::numbers::pi)));
        report.max_curvature_gap =
            std::max(report.max_curvature_gap, std::abs(tip.curvature - arm.curvature));
      };
  for (const JunctionConnection& connection : junction->connections) {
    const Road* road = network.road(connection.connecting_road);
    if (road == nullptr || !road->predecessor.has_value() || !road->successor.has_value() ||
        road->sections.empty()) {
      continue;
    }
    const double length = road->plan_view.length();
    const int from_lane = connection.lane_links.empty() ? 0 : connection.lane_links.front().first;
    // to_lane is recorded on the connecting road's single driving lane (-1).
    int to_lane = 0;
    for (const LaneId lane_id : network.lane_section(road->sections.front())->lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id == -1 && lane.successor.has_value()) {
        to_lane = *lane.successor;
      }
    }
    // Start end touches the incoming arm (into_hdg = the connector's +s start).
    if (const auto from = arm_of(*road->predecessor)) {
      if (auto arm = contact_state(network, *from); arm.has_value()) {
        const auto inner_t = inner_t_of(driving_lanes_at(network, *from, *arm, true), from_lane);
        if (inner_t.has_value()) {
          accumulate(road->plan_view.evaluate(0.0), arm->into_hdg, *arm, *inner_t);
        }
      }
    }
    // End end touches the outgoing arm; the connector arrives heading out_hdg.
    if (const auto to = arm_of(*road->successor)) {
      if (auto arm = contact_state(network, *to); arm.has_value()) {
        const auto inner_t = inner_t_of(driving_lanes_at(network, *to, *arm, false), to_lane);
        if (inner_t.has_value()) {
          accumulate(road->plan_view.evaluate(length), arm->out_hdg, *arm, *inner_t);
        }
      }
    }
  }
  // A G1 weld matches position and heading but legitimately leaves a curvature
  // step (only a G2 close_gap drives that to zero), so `breaches` is
  // position/heading only; max_curvature_gap is reported for information.
  report.breaches =
      report.max_position_gap > tol::kWeldPosition || report.max_heading_gap > tol::kWeldHeading;
  return report;
}

Expected<void> check_linkable(const RoadNetwork& network,
                              const RoadEnd& a,
                              const RoadEnd& b,
                              const CloseGapOptions& options) {
  if (a == b) {
    return make_error(ErrorCode::InvalidArgument, "an end cannot link to itself");
  }
  if (a.road == b.road) {
    return make_error(ErrorCode::InvalidArgument, "both ends are on the same road");
  }
  const auto contact_a = contact_state(network, a);
  if (!contact_a.has_value()) {
    return tl::unexpected<Error>(contact_a.error());
  }
  const auto contact_b = contact_state(network, b);
  if (!contact_b.has_value()) {
    return tl::unexpected<Error>(contact_b.error());
  }
  for (const RoadEnd& end : {a, b}) {
    if (junction_at_end(network, end).has_value()) {
      const Road* road = network.road(end.road);
      return make_error(ErrorCode::InvalidArgument,
                        "an end already belongs to a junction",
                        road != nullptr ? road->odr_id : std::string{});
    }
    const Road& road = *network.road(end.road);
    const auto& slot = end.contact == ContactPoint::Start ? road.predecessor : road.successor;
    if (slot.has_value()) {
      return make_error(ErrorCode::InvalidArgument, "an end is already linked", road.odr_id);
    }
  }
  const double gap = std::hypot(contact_a->x - contact_b->x, contact_a->y - contact_b->y);
  if (gap > options.max_gap_m + tol::kLength) {
    return make_error(ErrorCode::InvalidArgument, "ends are too far apart to link");
  }
  return {};
}

} // namespace roadmaker::edit
