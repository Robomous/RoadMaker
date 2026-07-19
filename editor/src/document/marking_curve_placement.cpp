#include "document/marking_curve_placement.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

#include "render/material_catalog.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// Centreline sample step [m]: the ~0.5 m spacing rm:markingCurve stores, so the
/// projected polyline tracks the road frame closely without over-sampling.
constexpr double kCurveSampleStep = 0.5;

/// Lowest positive integer odr id not already used by an object (id-unique in
/// class), mirroring the stencil/tree placement paths.
std::string next_object_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

/// The anchor road's (s, t) centreline projected from the clicked points, with
/// each sample required to stay on the anchor when `strict`. When lenient (the
/// preview) an off-road sample keeps its (possibly large) t so the ghost shows
/// the snap; s is always clamped to [0, length]. nullopt on a failed fit or, in
/// strict mode, a sample that leaves the anchor.
std::optional<std::vector<std::array<double, 2>>>
project_centerline(const RoadNetwork& network,
                   RoadId anchor,
                   std::span<const Waypoint> world_points,
                   bool strict) {
  const Road* road = network.road(anchor);
  if (road == nullptr || road->plan_view.empty() || world_points.size() < 2) {
    return std::nullopt;
  }
  const Expected<ReferenceLine> line = fit_clothoid_path(world_points);
  if (!line.has_value()) {
    return std::nullopt;
  }
  SamplingOptions options;
  options.max_step = kCurveSampleStep;
  const std::vector<double> stations = sample_stations(*line, options);
  const double length = road->plan_view.length();

  std::vector<std::array<double, 2>> centerline;
  centerline.reserve(stations.size());
  for (const double s : stations) {
    const PathPoint point = line->evaluate(s);
    if (strict) {
      const std::optional<StationCoord> st =
          station_within(road->plan_view, point.x, point.y, kObjectSnapThreshold);
      if (!st.has_value()) {
        return std::nullopt; // the sample left the anchor road (single-road rule)
      }
      centerline.push_back({std::clamp(st->s, 0.0, length), st->t});
    } else {
      const StationCoord st = find_station(road->plan_view, point.x, point.y);
      centerline.push_back({std::clamp(st.s, 0.0, length), st.t});
    }
  }
  return centerline;
}

} // namespace

bool is_marking_curve_asset(const LibraryItem& item) {
  return item.kind == LibraryItem::Kind::Crosswalk || item.kind == LibraryItem::Kind::Marking;
}

edit::MarkingCurveParams marking_curve_params_from_item(const LibraryItem& item,
                                                        const MaterialCatalog& /*materials*/) {
  edit::MarkingCurveParams params;
  params.asset = item.key.toStdString();
  params.color = "white"; // road markings are white paint by default
  if (item.kind == LibraryItem::Kind::Crosswalk) {
    // A crosswalk asset paints a striped band: the walking depth is the band
    // width across the curve, the stripe geometry its dash pattern.
    params.striped = true;
    params.width_m = item.crosswalk_width;
    params.dash_length_m = item.crosswalk_dash;
    params.dash_gap_m = item.crosswalk_gap;
    params.material = item.crosswalk_material.toStdString();
    params.category = item.crosswalk_segmentation.toStdString();
  } else {
    // A plain marking asset paints a solid line at its painted width.
    params.striped = false;
    params.width_m = item.mark_width;
    params.dash_length_m = 0.0;
    params.dash_gap_m = 0.0;
    params.category = "road_marking";
  }
  return params;
}

std::optional<RoadId> anchor_road_at(const RoadNetwork& network, double x, double y, double max_t) {
  std::optional<RoadId> best;
  double best_abs_t = max_t;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.plan_view.empty()) {
      return;
    }
    const std::optional<StationCoord> station = station_within(road.plan_view, x, y, max_t);
    if (station.has_value() && std::abs(station->t) < best_abs_t) {
      best_abs_t = std::abs(station->t);
      best = id;
    }
  });
  return best;
}

Expected<MarkingCurveResult> build_marking_curve(const RoadNetwork& network,
                                                 RoadId anchor,
                                                 std::span<const Waypoint> world_points,
                                                 const edit::MarkingCurveParams& params) {
  if (world_points.size() < 2) {
    return make_error(ErrorCode::InvalidArgument, "a marking curve needs at least two points");
  }
  const std::optional<std::vector<std::array<double, 2>>> centerline =
      project_centerline(network, anchor, world_points, /*strict=*/true);
  if (!centerline.has_value()) {
    return make_error(ErrorCode::InvalidArgument, "the marking curve must stay on one road");
  }

  Object object;
  object.odr_id = next_object_odr_id(network);
  // apply_marking_curve_asset authors the outline + <markings> + rm:markingCurve
  // userData and sets @s/@t to the first sample; it may refuse a bend tighter
  // than width/2 (the offset band would self-intersect).
  if (const Expected<void> authored = edit::apply_marking_curve_asset(object, *centerline, params);
      !authored.has_value()) {
    return make_error(authored.error().code, authored.error().message, authored.error().context);
  }
  return MarkingCurveResult{.road = anchor, .object = std::move(object)};
}

std::vector<std::array<double, 2>> marking_curve_preview_polyline(
    const RoadNetwork& network, RoadId anchor, std::span<const Waypoint> world_points) {
  const std::optional<std::vector<std::array<double, 2>>> centerline =
      project_centerline(network, anchor, world_points, /*strict=*/false);
  if (!centerline.has_value()) {
    return {};
  }
  const Road* road = network.road(anchor);
  if (road == nullptr) {
    return {};
  }
  std::vector<std::array<double, 2>> world;
  world.reserve(centerline->size());
  for (const std::array<double, 2>& st : *centerline) {
    world.push_back(station_to_world(road->plan_view, st[0], st[1]));
  }
  return world;
}

} // namespace roadmaker::editor
