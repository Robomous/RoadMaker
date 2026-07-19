#include "document/prop_placement.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// The lowest positive integer odr id not in `taken`, then reserve it. A bake
/// mints many ids in one pass: growing `taken` in place keeps every prop unique
/// (a per-prop next_object_odr_id would hand out the same lowest id N times).
std::string mint_object_odr_id(std::set<std::string>& taken) {
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  std::string id = std::to_string(candidate);
  taken.insert(id);
  return id;
}

} // namespace

std::optional<RoadStation>
nearest_road_station(const RoadNetwork& network, double x, double y, double max_t) {
  std::optional<RoadStation> best;
  double best_abs_t = max_t;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.plan_view.empty()) {
      return;
    }
    const StationCoord station = find_station(road.plan_view, x, y);
    if (std::abs(station.t) < best_abs_t) {
      best_abs_t = std::abs(station.t);
      best = RoadStation{.road = id, .s = station.s, .t = station.t};
    }
  });
  return best;
}

std::string next_object_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  return mint_object_odr_id(taken);
}

bool is_prop_asset(const LibraryItem& item) {
  return item.kind == LibraryItem::Kind::Tree;
}

Object make_prop_object(const LibraryItem& item, std::string odr_id, double s, double t) {
  Object prop;
  prop.odr_id = std::move(odr_id);
  prop.name = item.model.toStdString();
  // A shrub is Vegetation; every other bundled model is a Tree (the Library
  // tree-drop classification, kept identical so a click and a drop agree).
  prop.type = item.model == QStringLiteral("shrub") ? ObjectType::Vegetation : ObjectType::Tree;
  prop.s = s;
  prop.t = t;
  if (const props::PropModel* model = props::model(prop.name)) {
    prop.radius = model->radius;
    prop.height = model->height;
  }
  return prop;
}

Expected<PropCurveDistribution> distribute_props_along_curve(const RoadNetwork& network,
                                                             RoadId anchor,
                                                             std::span<const Waypoint> world_points,
                                                             const LibraryItem& item,
                                                             double spacing_m) {
  if (world_points.size() < 2) {
    return make_error(ErrorCode::InvalidArgument, "a prop curve needs at least two points");
  }
  if (spacing_m <= 0.0) {
    return make_error(ErrorCode::InvalidArgument, "prop spacing must be positive");
  }
  const Road* road = network.road(anchor);
  if (road == nullptr || road->plan_view.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the prop curve has no anchor road");
  }
  const Expected<ReferenceLine> line = fit_clothoid_path(world_points);
  if (!line.has_value()) {
    return make_error(line.error().code, line.error().message, line.error().context);
  }

  // The taken-set is built ONCE and grows as props are minted, so the batch never
  // hands out a duplicate odr id (the id-unique-in-class contract, WP1 gotcha).
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });

  const double curve_length = line->length();
  const double road_length = road->plan_view.length();

  PropCurveDistribution result;
  // Sample s = 0, spacing, 2·spacing … including the final station within a
  // tolerance of the end, so a curve exactly N·spacing long places N+1 props.
  for (std::size_t i = 0;; ++i) {
    const double s = static_cast<double>(i) * spacing_m;
    if (s > curve_length + tol::kLength) {
      break;
    }
    const PathPoint point = line->evaluate(s);
    const std::optional<StationCoord> station =
        station_within(road->plan_view, point.x, point.y, kObjectSnapThreshold);
    if (!station.has_value()) {
      ++result.skipped; // the sample left the anchor road (single-road rule)
      continue;
    }
    const double clamped_s = std::clamp(station->s, 0.0, road_length);
    result.props.emplace_back(
        anchor, make_prop_object(item, mint_object_odr_id(taken), clamped_s, station->t));
    result.preview_points.push_back(station_to_world(road->plan_view, clamped_s, station->t));
  }

  if (result.props.empty()) {
    return make_error(ErrorCode::InvalidArgument, "no props landed on the anchor road");
  }
  return result;
}

} // namespace roadmaker::editor
