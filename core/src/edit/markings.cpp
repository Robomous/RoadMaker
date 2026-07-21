#include "roadmaker/edit/markings.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "markings_detail.hpp"

namespace roadmaker::edit {

namespace {

/// The Table 117 subtype every approach lane gets when no chooser overrides it.
constexpr std::string_view kStraightArrow = "arrowStraight";

/// Paint thickness of an authored crosswalk marking above the road (§13.8
/// @zOffset), matching the ASAM crosswalk example.
constexpr double kMarkingZOffset = 0.005;

/// Paint-line width of the interop stripes marking [m]. The mesher renders the
/// zebra from CrosswalkData; this is a reasonable value for a foreign consumer
/// drawing the dashed outline ring.
constexpr double kStripePaintWidth = 0.12;

} // namespace

/// Authors the OpenDRIVE interop projection of a parametric crosswalk onto
/// `object`: a closed cornerRoad outline (ids 0..3, CCW, fillType="paint")
/// spanning s0..s1 along the road and t_min..t_max across it, plus the
/// <markings> (one dashed/solid stripes marking over the full ring, then two
/// solid border markings on the road-parallel edges when border_width>0) and
/// the rm:crosswalk userData that is the render-time source of truth. The
/// marking encoding is schema-valid but visually approximate — the mesher draws
/// from CrosswalkData (§13.8 line-marking geometry is underdefined for zebras).
void apply_crosswalk_asset(Object& object, const CrosswalkParams& params) {
  // Derive the outline extent from the object's placement: @s/@t are the
  // crosswalk centre, @length the crossing span across the road, params.depth_m
  // the walking depth along the road.
  const double half_span = object.length.value_or(0.0) / 2.0;
  const double t_min = object.t - half_span;
  const double t_max = object.t + half_span;
  const double s0 = object.s - (params.depth_m / 2.0);
  const double s1 = object.s + (params.depth_m / 2.0);

  object.type = ObjectType::Crosswalk;
  if (object.type_str.empty()) {
    object.type_str = "crosswalk";
  }
  if (object.subtype.empty()) {
    object.subtype = "zebra";
  }
  object.width = params.depth_m;
  object.outlines.clear();
  object.markings.clear();

  Object& crosswalk = object;
  ObjectOutline outline;
  outline.road_coords = true;
  outline.closed = true;
  outline.outer = true;
  outline.id = 0;
  outline.fill_type = "paint";
  outline.corners = {
      OutlineCorner{.a = s0, .b = t_min, .height = 0.0, .dz_or_z = 0.0, .id = 0},
      OutlineCorner{.a = s1, .b = t_min, .height = 0.0, .dz_or_z = 0.0, .id = 1},
      OutlineCorner{.a = s1, .b = t_max, .height = 0.0, .dz_or_z = 0.0, .id = 2},
      OutlineCorner{.a = s0, .b = t_max, .height = 0.0, .dz_or_z = 0.0, .id = 3},
  };

  const bool solid = params.dash_length_m <= tol::kLength;
  const double span = t_max - t_min;
  const double depth = s1 - s0;

  ObjectMarking stripes;
  stripes.color = params.color;
  stripes.width = kStripePaintWidth;
  stripes.z_offset = kMarkingZOffset;
  // A solid crossing is one visible run with no gap; lineLength must stay > 0
  // (t_grZero), so it takes the crossing span.
  stripes.line_length = solid ? span : params.dash_length_m;
  stripes.space_length = solid ? 0.0 : params.dash_gap_m;
  stripes.corner_refs = {0, 1, 2, 3};
  outline.markings.push_back(std::move(stripes));

  if (params.border_width_m > tol::kLength) {
    for (const auto& edge : {std::pair{0, 1}, std::pair{2, 3}}) {
      ObjectMarking border;
      border.color = params.color;
      border.width = params.border_width_m;
      border.z_offset = kMarkingZOffset;
      border.line_length = depth; // solid line along the road-parallel edge
      border.space_length = 0.0;
      border.corner_refs = {edge.first, edge.second};
      outline.markings.push_back(std::move(border));
    }
  }

  crosswalk.outlines.push_back(std::move(outline));
  crosswalk.crosswalk = CrosswalkData{.asset = params.asset,
                                      .border_width = params.border_width_m,
                                      .dash_length = params.dash_length_m,
                                      .dash_gap = params.dash_gap_m,
                                      .material = params.material,
                                      .material_override = false,
                                      .category = params.category};
}

namespace {

/// Circumradius of the triangle (p0,p1,p2) in the (s,t) plane, or +inf when the
/// three points are (near-)collinear. Used to reject a marking curve whose
/// tightest bend would fold the ±width/2 offset band onto itself.
double circumradius(const std::array<double, 2>& p0,
                    const std::array<double, 2>& p1,
                    const std::array<double, 2>& p2) {
  const auto dist = [](const std::array<double, 2>& a, const std::array<double, 2>& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1]);
  };
  const double a = dist(p1, p2);
  const double b = dist(p0, p2);
  const double c = dist(p0, p1);
  const double twice_area =
      std::abs(((p1[0] - p0[0]) * (p2[1] - p0[1])) - ((p1[1] - p0[1]) * (p2[0] - p0[0])));
  if (twice_area <= tol::kLength) {
    return std::numeric_limits<double>::infinity(); // collinear — no bend
  }
  return (a * b * c) / (2.0 * twice_area);
}

} // namespace

Expected<void> apply_marking_curve_asset(Object& object,
                                         std::span<const std::array<double, 2>> centerline,
                                         const MarkingCurveParams& params) {
  if (centerline.size() < 2) {
    return make_error(ErrorCode::InvalidArgument,
                      "marking curve needs at least two samples",
                      "apply_marking_curve_asset");
  }
  const double half = std::max(params.width_m, tol::kLength) / 2.0;
  for (std::size_t i = 1; i + 1 < centerline.size(); ++i) {
    if (circumradius(centerline[i - 1], centerline[i], centerline[i + 1]) < half) {
      return make_error(ErrorCode::InvalidArgument,
                        "marking curve bends tighter than its half-width — the band would "
                        "self-intersect; draw a gentler curve or reduce the width",
                        "apply_marking_curve_asset");
    }
  }

  // The left (+90°) unit normal of the (s,t) polyline at sample i, from the
  // averaged direction of the adjacent segments so the offset band stays smooth
  // across joints.
  const auto left_normal = [&](std::size_t i) -> std::array<double, 2> {
    std::array<double, 2> dir{0.0, 0.0};
    if (i > 0) {
      dir[0] += centerline[i][0] - centerline[i - 1][0];
      dir[1] += centerline[i][1] - centerline[i - 1][1];
    }
    if (i + 1 < centerline.size()) {
      dir[0] += centerline[i + 1][0] - centerline[i][0];
      dir[1] += centerline[i + 1][1] - centerline[i][1];
    }
    const double len = std::hypot(dir[0], dir[1]);
    if (len <= tol::kLength) {
      return {0.0, 0.0};
    }
    return {-dir[1] / len, dir[0] / len}; // rotate the unit tangent by +90°
  };

  const std::size_t n = centerline.size();
  object.outlines.clear();
  object.markings.clear();
  object.crosswalk.reset();
  object.s = centerline.front()[0];
  object.t = centerline.front()[1];
  object.hdg = 0.0;
  object.length.reset();
  object.width = params.width_m;
  if (params.striped) {
    object.type = ObjectType::Crosswalk;
    if (object.type_str.empty()) {
      object.type_str = "crosswalk";
    }
    if (object.subtype.empty()) {
      object.subtype = "zebra";
    }
  } else {
    object.type = ObjectType::None;
    object.type_str = "roadMark";
  }

  ObjectOutline outline;
  outline.road_coords = true;
  outline.closed = true;
  outline.outer = true;
  outline.id = 0;
  outline.fill_type = "paint";
  int corner_id = 0;
  std::vector<int> ring; // corner ids in ring order for the marking reference
  ring.reserve(2 * n);
  for (std::size_t i = 0; i < n; ++i) { // forward, left edge (+half)
    const std::array<double, 2> nrm = left_normal(i);
    outline.corners.push_back(OutlineCorner{.a = centerline[i][0] + (half * nrm[0]),
                                            .b = centerline[i][1] + (half * nrm[1]),
                                            .height = 0.0,
                                            .dz_or_z = 0.0,
                                            .id = corner_id});
    ring.push_back(corner_id);
    ++corner_id;
  }
  for (std::size_t k = 0; k < n; ++k) { // backward, right edge (-half)
    const std::size_t i = n - 1 - k;
    const std::array<double, 2> nrm = left_normal(i);
    outline.corners.push_back(OutlineCorner{.a = centerline[i][0] - (half * nrm[0]),
                                            .b = centerline[i][1] - (half * nrm[1]),
                                            .height = 0.0,
                                            .dz_or_z = 0.0,
                                            .id = corner_id});
    ring.push_back(corner_id);
    ++corner_id;
  }

  // The interop stripe marking spans the whole ring. Fill only for a striped
  // crosswalk band; a plain marking is a single line along the curve.
  const bool solid = params.dash_length_m <= tol::kLength;
  ObjectMarking stripes;
  stripes.color = params.color;
  stripes.width = params.striped ? kStripePaintWidth : params.width_m;
  stripes.z_offset = kMarkingZOffset;
  stripes.line_length = solid ? std::max(params.width_m, kStripePaintWidth) : params.dash_length_m;
  stripes.space_length = solid ? 0.0 : params.dash_gap_m;
  stripes.corner_refs = std::move(ring);
  outline.markings.push_back(std::move(stripes));

  object.outlines.push_back(std::move(outline));

  MarkingCurveData data;
  data.asset = params.asset;
  data.width = params.width_m;
  data.dash_length = params.dash_length_m;
  data.dash_gap = params.dash_gap_m;
  data.material = params.material;
  data.material_override = false;
  data.category = params.category;
  data.striped = params.striped;
  data.samples.assign(centerline.begin(), centerline.end());
  object.marking_curve = std::move(data);
  return {};
}

namespace {

/// The 6 core road-arrow glyphs as simple closed polygons in a normalized local
/// frame: u ∈ [-0.5, 0.5] along travel, v in fractions of the glyph width
/// (leftward positive). arrow_glyph_outline scales u by length and v by width.
/// left/right and straightLeft/straightRight are one authored shape mirrored in
/// v. Grounded in the ASAM road-arrow examples (OpenDRIVE 1.9.0 §13.14.8;
/// 1.8.1 Table 115 has the same subtypes).
std::vector<std::array<double, 2>> normalized_arrow(std::string_view subtype) {
  // A straight arrow: narrow shaft + triangular head pointing +u.
  const std::vector<std::array<double, 2>> straight{
      {-0.5, -0.15}, {0.1, -0.15}, {0.1, -0.5}, {0.5, 0.0}, {0.1, 0.5}, {0.1, 0.15}, {-0.5, 0.15}};
  // A turn arrow: shaft + head skewed fully to the turn side (+v = left).
  const std::vector<std::array<double, 2>> left_turn{
      {-0.5, -0.15}, {0.1, -0.15}, {0.1, -0.5}, {0.5, 0.5}, {0.1, 0.5}, {0.1, 0.15}, {-0.5, 0.15}};
  // A combined arrow: a straight head and a left barb branching off the shaft
  // (concave valley between the two heads).
  const std::vector<std::array<double, 2>> straight_left{{-0.5, -0.12},
                                                         {0.1, -0.12},
                                                         {0.1, -0.30},
                                                         {0.5, 0.0},
                                                         {0.2, 0.15},
                                                         {0.35, 0.45},
                                                         {0.12, 0.40},
                                                         {0.1, 0.12},
                                                         {-0.5, 0.12}};
  const auto mirror = [](const std::vector<std::array<double, 2>>& src) {
    std::vector<std::array<double, 2>> out;
    out.reserve(src.size());
    for (const std::array<double, 2>& p : src) {
      out.push_back({p[0], -p[1]});
    }
    return out;
  };
  if (subtype == "arrowStraight") {
    return straight;
  }
  if (subtype == "arrowLeft") {
    return left_turn;
  }
  if (subtype == "arrowRight") {
    return mirror(left_turn);
  }
  if (subtype == "arrowStraightLeft") {
    return straight_left;
  }
  if (subtype == "arrowStraightRight") {
    return mirror(straight_left);
  }
  if (subtype == "arrowLeftRight") {
    // A double turn arrow: barbs to both sides off a common shaft, with a small
    // forward nose keeping the loop simple (non-self-intersecting).
    return {{-0.5, -0.12},
            {0.1, -0.12},
            {0.12, -0.40},
            {0.35, -0.45},
            {0.2, -0.15},
            {0.4, 0.0},
            {0.2, 0.15},
            {0.35, 0.45},
            {0.12, 0.40},
            {0.1, 0.12},
            {-0.5, 0.12}};
  }
  return {}; // unknown subtype — caller falls back to the straight glyph
}

} // namespace

std::vector<OutlineCorner>
arrow_glyph_outline(std::string_view subtype, double length_m, double width_m) {
  const std::vector<std::array<double, 2>> shape = normalized_arrow(subtype);
  std::vector<OutlineCorner> corners;
  corners.reserve(shape.size());
  int id = 0;
  for (const std::array<double, 2>& p : shape) {
    corners.push_back(OutlineCorner{
        .a = p[0] * length_m, .b = p[1] * width_m, .height = 0.0, .dz_or_z = 0.0, .id = id++});
  }
  return corners;
}

Expected<void> apply_stencil_asset(Object& object, const StencilParams& params) {
  std::vector<OutlineCorner> corners =
      arrow_glyph_outline(params.subtype, params.length_m, params.width_m);
  if (corners.size() < 3) {
    return make_error(ErrorCode::InvalidArgument,
                      "unknown arrow stencil subtype '" + params.subtype + "'",
                      "apply_stencil_asset");
  }
  object.type = ObjectType::None;
  object.type_str = "roadMark";
  object.subtype = params.subtype;
  object.length = params.length_m;
  object.width = params.width_m;
  object.hdg = object.hdg; // heading is the caller's (travel direction)
  object.outlines.clear();
  object.markings.clear();
  object.crosswalk.reset();
  object.marking_curve.reset();

  ObjectOutline outline;
  outline.road_coords = false; // cornerLocal (u,v) — no mixed corner kinds
  outline.closed = true;
  outline.outer = true;
  outline.id = 0;
  outline.fill_type = "paint";
  outline.corners = std::move(corners);
  object.outlines.push_back(std::move(outline));

  // A <material roadMarkColor="..."/> child, preserved verbatim (the never-drop
  // slot round-trips it; a foreign viewer reads the paint colour).
  object.preserved.children.push_back("<material roadMarkColor=\"" + params.color + "\"/>");

  object.stencil = StencilData{.asset = params.asset,
                               .material = params.material,
                               .material_override = false,
                               .category = params.category};
  return {};
}

namespace {

using markings_detail::distinct_arms;
using markings_detail::facing_end;

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

    const double s_center = std::clamp(s, 0.0, length);
    Object crosswalk;
    crosswalk.odr_id = ids.next();
    crosswalk.type = ObjectType::Crosswalk;
    crosswalk.type_str = "crosswalk";
    crosswalk.subtype = "zebra";
    crosswalk.s = s_center;
    crosswalk.t = (t_min + t_max) / 2.0;
    crosswalk.hdg = std::numbers::pi / 2.0; // across the road (relative to +s)
    crosswalk.length = t_max - t_min;       // across span — zebra bars repeat here
    // Author the outline + <markings> + rm:crosswalk userData from the same
    // path the editor's re-materialization uses (reads s/t/length, writes @width
    // = depth).
    apply_crosswalk_asset(crosswalk, params);
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
