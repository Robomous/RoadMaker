#include "document/prop_placement.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/repeat_expansion.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
  if (item.kind == LibraryItem::Kind::Tree) {
    return true;
  }
  if (item.kind == LibraryItem::Kind::PropSet) {
    // A set is placeable only with at least one entry and every model bundled —
    // an empty or dangling set would draw nothing to author.
    if (item.prop_entries.empty()) {
      return false;
    }
    for (const LibraryItem::PropSetEntry& entry : item.prop_entries) {
      if (props::model(entry.model.toStdString()) == nullptr) {
        return false;
      }
    }
    return true;
  }
  return false;
}

LibraryItem resolve_prop_asset(const LibraryItem& item, std::mt19937& rng) {
  // A plain tree needs no draw (and must not advance the RNG, so a Tree curve
  // stays byte-deterministic regardless of seed).
  if (item.kind != LibraryItem::Kind::PropSet || item.prop_entries.empty()) {
    return item;
  }
  std::vector<double> weights;
  weights.reserve(item.prop_entries.size());
  for (const LibraryItem::PropSetEntry& entry : item.prop_entries) {
    weights.push_back(entry.portion);
  }
  std::discrete_distribution<std::size_t> draw(weights.begin(), weights.end());
  const LibraryItem::PropSetEntry& chosen = item.prop_entries[draw(rng)];
  // A synthetic Tree so make_prop_object classifies (tree vs vegetation) and
  // dimensions the drawn model exactly as a first-class tree drop would.
  LibraryItem tree;
  tree.kind = LibraryItem::Kind::Tree;
  tree.model = chosen.model;
  tree.key = item.key;
  tree.label = item.label;
  return tree;
}

Object make_prop_object(const LibraryItem& item, std::string odr_id, double s, double t) {
  Object prop;
  prop.odr_id = std::move(odr_id);
  prop.name = item.model.toStdString();
  // The bundled model is the single source of truth for the object class (Tree,
  // Vegetation, Pole, Building) and dimensions — a click and a drop agree
  // because both read it here. An unknown model falls back to Tree.
  prop.type = ObjectType::Tree;
  prop.s = s;
  prop.t = t;
  if (const props::PropModel* model = props::model(prop.name)) {
    prop.type = model->type;
    prop.radius = model->radius;
    prop.height = model->height;
  }
  return prop;
}

Expected<PropCurveDistribution> distribute_props_along_curve(const RoadNetwork& network,
                                                             RoadId anchor,
                                                             std::span<const Waypoint> world_points,
                                                             const LibraryItem& item,
                                                             double spacing_m,
                                                             std::uint32_t seed) {
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

  // Seeded so a PropSet scatter (one draw per placed instance) is reproducible.
  std::mt19937 rng(seed);

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
    // One draw per instance: a mixed PropSet scatters models along the curve; a
    // plain Tree resolves to itself.
    const LibraryItem drawn = resolve_prop_asset(item, rng);
    result.props.emplace_back(
        anchor, make_prop_object(drawn, mint_object_odr_id(taken), clamped_s, station->t));
    result.preview_points.push_back(station_to_world(road->plan_view, clamped_s, station->t));
  }

  if (result.props.empty()) {
    return make_error(ErrorCode::InvalidArgument, "no props landed on the anchor road");
  }
  return result;
}

ObjectRepeat make_span_repeat(double s_a, double s_b, double t, double distance) {
  ObjectRepeat repeat;
  repeat.s = std::min(s_a, s_b);
  repeat.length = std::abs(s_a - s_b);
  repeat.distance = distance;
  // t and z are held constant across the section (a straight avenue at a fixed
  // offset): equal start/end and no cubic coefficients, so expand_repeat takes
  // the linear branch and every instance sits at (t, 0).
  repeat.t_start = t;
  repeat.t_end = t;
  repeat.z_offset_start = 0.0;
  repeat.z_offset_end = 0.0;
  return repeat;
}

Expected<Object> make_prop_span_object(const LibraryItem& item,
                                       std::string odr_id,
                                       double s_a,
                                       double s_b,
                                       double t,
                                       double distance) {
  if (!is_prop_asset(item)) {
    return make_error(ErrorCode::InvalidArgument, "a prop span needs a tree or shrub asset");
  }
  if (distance <= 0.0) {
    return make_error(ErrorCode::InvalidArgument, "prop span spacing must be positive");
  }
  if (std::abs(s_a - s_b) < tol::kLength) {
    return make_error(ErrorCode::InvalidArgument, "a prop span needs two distinct stations");
  }
  // The base prop carries the model, type and dimensions; the repeat overrides
  // its single placement with the series (§13.4, "repeat attrs overrule object
  // attrs" — mesh_builder suppresses the base instance when a series repeat is
  // present). hdg stays 0: a repeat orients each instance to the reference line.
  Object object = make_prop_object(item, std::move(odr_id), std::min(s_a, s_b), t);
  object.hdg = 0.0;
  object.repeats.push_back(make_span_repeat(s_a, s_b, t, distance));
  return object;
}

std::vector<std::array<double, 2>>
span_preview_points(const RoadNetwork& network, RoadId road, const ObjectRepeat& repeat) {
  std::vector<std::array<double, 2>> points;
  const Road* r = network.road(road);
  if (r == nullptr || r->plan_view.empty()) {
    return points;
  }
  const double road_length = r->plan_view.length();
  for (const RepeatInstance& inst : expand_repeat(repeat)) {
    // Match mesh_builder: an instance origin past the road end is not placed.
    if (inst.s > road_length + tol::kLength) {
      continue;
    }
    points.push_back(station_to_world(r->plan_view, inst.s, inst.t));
  }
  return points;
}

namespace {

/// Twice the signed shoelace area of `polygon` — sign drops out, we take the
/// magnitude for the region area.
double polygon_area(std::span<const Waypoint> polygon) {
  double twice = 0.0;
  const std::size_t n = polygon.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    twice += (polygon[j].x * polygon[i].y) - (polygon[i].x * polygon[j].y);
  }
  return std::abs(twice) * 0.5;
}

/// Crossing-number point-in-polygon: a horizontal ray from (x, y) crosses the
/// polygon edges an odd number of times iff the point is inside.
bool point_in_polygon(std::span<const Waypoint> polygon, double x, double y) {
  bool inside = false;
  const std::size_t n = polygon.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Waypoint& a = polygon[i];
    const Waypoint& b = polygon[j];
    if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) {
      inside = !inside;
    }
  }
  return inside;
}

} // namespace

Expected<PropCurveDistribution> distribute_props_in_polygon(const RoadNetwork& network,
                                                            std::span<const Waypoint> polygon,
                                                            const LibraryItem& item,
                                                            const PropScatterParams& params) {
  if (polygon.size() < 3) {
    return make_error(ErrorCode::InvalidArgument, "a prop region needs at least three vertices");
  }
  if (!is_prop_asset(item)) {
    return make_error(ErrorCode::InvalidArgument, "a prop region needs a tree or shrub asset");
  }
  const double area = polygon_area(polygon);
  if (area < tol::kLength) {
    return make_error(ErrorCode::InvalidArgument, "the prop region has no area");
  }

  // Target count from the requested density; at least one prop for any real
  // region, rounded from the area.
  const auto target = static_cast<std::size_t>(
      std::max<long long>(1, std::llround(area * params.density_per_100m2 / 100.0)));

  double min_x = polygon[0].x;
  double max_x = polygon[0].x;
  double min_y = polygon[0].y;
  double max_y = polygon[0].y;
  for (const Waypoint& v : polygon) {
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
    min_y = std::min(min_y, v.y);
    max_y = std::max(max_y, v.y);
  }

  // The taken-set is built ONCE and grows as ids are minted (id-unique-in-class).
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });

  // One RNG drives BOTH the position sampling and each PropSet draw, in a fixed
  // call order, so the whole scatter is reproducible for a given seed.
  std::mt19937 rng(params.seed);
  std::uniform_real_distribution<double> pick_x(min_x, max_x);
  std::uniform_real_distribution<double> pick_y(min_y, max_y);

  PropCurveDistribution result;
  const std::size_t max_attempts = 100 * target;
  std::size_t accepted = 0;
  for (std::size_t attempt = 0; accepted < target && attempt < max_attempts; ++attempt) {
    const double x = pick_x(rng);
    const double y = pick_y(rng);
    if (!point_in_polygon(polygon, x, y)) {
      continue; // outside the region: not an accepted sample, retry
    }
    ++accepted;
    const std::optional<RoadStation> station =
        nearest_road_station(network, x, y, kPolygonAnchorMaxT);
    if (!station.has_value()) {
      ++result.skipped; // no road in reach — the sample can't become a road-relative prop
      continue;
    }
    const Road* road = network.road(station->road);
    if (road == nullptr || road->plan_view.empty()) {
      ++result.skipped;
      continue;
    }
    const double clamped_s = std::clamp(station->s, 0.0, road->plan_view.length());
    // One draw per sample: a mixed PropSet scatters models across the region.
    const LibraryItem drawn = resolve_prop_asset(item, rng);
    result.props.emplace_back(
        station->road, make_prop_object(drawn, mint_object_odr_id(taken), clamped_s, station->t));
    result.preview_points.push_back(station_to_world(road->plan_view, clamped_s, station->t));
  }

  if (result.props.empty()) {
    return make_error(ErrorCode::InvalidArgument, "no props landed on a road");
  }
  return result;
}

} // namespace roadmaker::editor
