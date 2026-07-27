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

#include "roadmaker/mesh/prop_obstructions.hpp"

#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include "../road/junction_adjacency.hpp"
#include "mesh_detail.hpp"
#include "object_placement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace roadmaker {

namespace {

using Point = std::array<double, 2>;

// --- plain 2D helpers --------------------------------------------------------

struct Aabb {
  double lo_x = std::numeric_limits<double>::max();
  double lo_y = std::numeric_limits<double>::max();
  double hi_x = std::numeric_limits<double>::lowest();
  double hi_y = std::numeric_limits<double>::lowest();

  void add(double x, double y) {
    lo_x = std::min(lo_x, x);
    lo_y = std::min(lo_y, y);
    hi_x = std::max(hi_x, x);
    hi_y = std::max(hi_y, y);
  }
  [[nodiscard]] bool valid() const { return lo_x <= hi_x && lo_y <= hi_y; }
  [[nodiscard]] bool overlaps(const Aabb& other) const {
    return valid() && other.valid() && lo_x <= other.hi_x && other.lo_x <= hi_x &&
           lo_y <= other.hi_y && other.lo_y <= hi_y;
  }
  void grow(double by) {
    lo_x -= by;
    lo_y -= by;
    hi_x += by;
    hi_y += by;
  }
};

double dot2(const Point& a, const Point& b) {
  return (a[0] * b[0]) + (a[1] * b[1]);
}

/// Closest point to `p` on segment a->b, and the squared distance to it.
std::pair<Point, double> closest_on_segment(const Point& a, const Point& b, const Point& p) {
  const Point ab{b[0] - a[0], b[1] - a[1]};
  const double len2 = dot2(ab, ab);
  double f = 0.0;
  if (len2 > 0.0) {
    f = std::clamp(dot2(Point{p[0] - a[0], p[1] - a[1]}, ab) / len2, 0.0, 1.0);
  }
  const Point q{a[0] + (ab[0] * f), a[1] + (ab[1] * f)};
  const double dx = p[0] - q[0];
  const double dy = p[1] - q[1];
  return {q, (dx * dx) + (dy * dy)};
}

/// Intersection point of segments p1->p2 and p3->p4, or nullopt. Same form as
/// grade_separation.cpp's, which is the repo's other segment-crossing test.
std::optional<Point>
segment_crossing(const Point& p1, const Point& p2, const Point& p3, const Point& p4) {
  const Point d1{p2[0] - p1[0], p2[1] - p1[1]};
  const Point d2{p4[0] - p3[0], p4[1] - p3[1]};
  const double denom = (d1[0] * d2[1]) - (d1[1] * d2[0]);
  if (std::abs(denom) < 1e-12) {
    return std::nullopt; // parallel or degenerate
  }
  const Point r{p3[0] - p1[0], p3[1] - p1[1]};
  const double t = ((r[0] * d2[1]) - (r[1] * d2[0])) / denom;
  const double u = ((r[0] * d1[1]) - (r[1] * d1[0])) / denom;
  if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) {
    return std::nullopt;
  }
  return Point{p1[0] + (d1[0] * t), p1[1] + (d1[1] * t)};
}

/// Even-odd ray cast, in plan-view METRES.
///
/// Deliberately NOT `Clipper2Lib::PointInPolygon`, which is unsound on a
/// double path: its CrossProductSign casts the edge deltas to __int128_t, so
/// every sub-metre difference truncates to zero and interior points report
/// "on the edge" (#442, found via #402). This is the same hand-rolled cast
/// fill_backend::inside_path uses, on a plain ring rather than a PathD.
bool inside_ring(const std::vector<Point>& ring, const Point& pt) {
  if (ring.size() < 3) {
    return false;
  }
  bool inside = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const Point& a = ring[i];
    const Point& b = ring[j];
    if ((a[1] > pt[1]) != (b[1] > pt[1]) &&
        pt[0] < (((b[0] - a[0]) * (pt[1] - a[1])) / (b[1] - a[1])) + a[0]) {
      inside = !inside;
    }
  }
  return inside;
}

// --- the prop's own volume ---------------------------------------------------

/// One instance's bounding volume in world space: a circle (@radius) or an
/// oriented box (@length x @width about the instance heading), plus the
/// vertical span the 2.5D gate uses.
struct Volume {
  bool circular = true;
  Point center{};
  double radius = 0.0; ///< circular
  double half_l = 0.0; ///< box, along the heading (§13.1 local u)
  double half_w = 0.0; ///< box, across it (local v)
  double cos_h = 1.0;
  double sin_h = 0.0;
  double z_lo = 0.0;
  double z_hi = 0.0;
  Aabb bounds;
  std::array<Point, 4> corners{}; ///< box only, CCW

  [[nodiscard]] bool overlaps_height(double z, double clearance) const {
    if (clearance <= 0.0) {
      return true; // vertical gate disabled: pure plan view
    }
    return z > z_lo - clearance && z < z_hi + clearance;
  }
};

/// True when the object is paint rather than a solid: §13.1 says an <outline>
/// supersedes the bounding volume, and every author of one in this product
/// (core/src/edit/markings.cpp) writes a crosswalk, marking curve or stencil —
/// coplanar with the road by construction. Testing their bounding boxes would
/// flag every zebra crossing on every save.
bool is_paint(const Object& object) {
  return !object.outlines.empty() || object.crosswalk.has_value() ||
         object.marking_curve.has_value() || object.stencil.has_value() ||
         object.type == ObjectType::Crosswalk;
}

/// The declared bounding volume of one placement, or nullopt when the object
/// declares none. NOTHING is ever invented: the prop library is not consulted,
/// and a missing @height only flattens the vertical span, it does not skip.
std::optional<Volume> volume_of(const Object& object,
                                const object_placement::PlacedInstance& placed) {
  Volume out;
  out.center = {placed.position[0], placed.position[1]};
  out.cos_h = std::cos(placed.heading);
  out.sin_h = std::sin(placed.heading);
  out.z_lo = placed.position[2];
  out.z_hi = placed.position[2] + std::max(0.0, object.height.value_or(0.0));

  if (object.radius.value_or(0.0) > 0.0) {
    // Carrying both channels is spec-illegal (road.object.circular_vs_angular);
    // the circle wins by decision, because @radius is the channel RoadMaker
    // itself writes (edit::set_object_model seeds it from the prop library).
    out.circular = true;
    out.radius = *object.radius;
    out.bounds.add(out.center[0] - out.radius, out.center[1] - out.radius);
    out.bounds.add(out.center[0] + out.radius, out.center[1] + out.radius);
    return out;
  }

  if (object.length.value_or(0.0) > 0.0 && object.width.value_or(0.0) > 0.0) {
    out.circular = false;
    out.half_l = *object.length * 0.5;
    out.half_w = *object.width * 0.5;
    // §13.1 Table 85: @length runs along local u, @width along local v. The
    // origin is taken as the CENTRE in u/v — the spec settles that in a figure
    // rather than in text, so the assumption is named in the header and pinned
    // by PropObstructions.ARectangularPropIsCentredOnItsOrigin.
    const Point along{out.cos_h * out.half_l, out.sin_h * out.half_l};
    const Point across{-out.sin_h * out.half_w, out.cos_h * out.half_w};
    out.corners = {Point{out.center[0] - along[0] - across[0], out.center[1] - along[1] - across[1]},
                   Point{out.center[0] + along[0] - across[0], out.center[1] + along[1] - across[1]},
                   Point{out.center[0] + along[0] + across[0], out.center[1] + along[1] + across[1]},
                   Point{out.center[0] - along[0] + across[0], out.center[1] - along[1] + across[1]}};
    for (const Point& corner : out.corners) {
      out.bounds.add(corner[0], corner[1]);
    }
    return out;
  }

  return std::nullopt; // no usable bounding volume — not obstruction-checked
}

/// A witness point inside both `volume` and `ring`, or nullopt.
///
/// COMPLETE, not sampled. For a circle: the centre inside the ring, or any ring
/// edge within the radius. For a box: any corner inside the ring, any ring
/// vertex inside the box, or any edge pair crossing. The last clause is the one
/// that matters — a long prop lying ACROSS a carriageway has no corner inside
/// the band and no band vertex inside it, and a centre-point or corner-sampling
/// test misses it entirely while passing every obvious case.
std::optional<Point> ring_witness(const Volume& volume, const std::vector<Point>& ring) {
  if (ring.size() < 3) {
    return std::nullopt;
  }

  if (volume.circular) {
    if (inside_ring(ring, volume.center)) {
      return volume.center;
    }
    const double r2 = volume.radius * volume.radius;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const auto [point, distance2] = closest_on_segment(ring[j], ring[i], volume.center);
      if (distance2 <= r2) {
        return point;
      }
    }
    return std::nullopt;
  }

  for (const Point& corner : volume.corners) {
    if (inside_ring(ring, corner)) {
      return corner;
    }
  }
  const std::vector<Point> box(volume.corners.begin(), volume.corners.end());
  for (const Point& vertex : ring) {
    if (inside_ring(box, vertex)) {
      return vertex;
    }
  }
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    for (std::size_t k = 0; k < 4; ++k) {
      const std::optional<Point> hit =
          segment_crossing(ring[j], ring[i], volume.corners[k], volume.corners[(k + 1) % 4]);
      if (hit.has_value()) {
        return hit;
      }
    }
  }
  return std::nullopt;
}

/// A witness point inside both volumes, or nullopt. Exact for all three pair
/// kinds: circle/circle by distance, circle/box by closest point on the box,
/// box/box by reusing the ring test (a box IS a 4-vertex ring).
std::optional<Point> volume_witness(const Volume& a, const Volume& b) {
  if (a.circular && b.circular) {
    const double dx = b.center[0] - a.center[0];
    const double dy = b.center[1] - a.center[1];
    const double reach = a.radius + b.radius;
    if ((dx * dx) + (dy * dy) > reach * reach) {
      return std::nullopt;
    }
    // Midpoint weighted by the radii: inside both discs whenever they meet.
    const double f = reach > 0.0 ? a.radius / reach : 0.5;
    return Point{a.center[0] + (dx * f), a.center[1] + (dy * f)};
  }
  if (a.circular) {
    const std::vector<Point> box(b.corners.begin(), b.corners.end());
    return ring_witness(a, box);
  }
  if (b.circular) {
    const std::vector<Point> box(a.corners.begin(), a.corners.end());
    return ring_witness(b, box);
  }
  const std::vector<Point> box(b.corners.begin(), b.corners.end());
  return ring_witness(a, box);
}

// --- the surfaces a prop can obstruct ---------------------------------------

/// A drivable ring plus the heights of its border samples, so the 2.5D gate can
/// ask "how high is the surface where they meet".
struct Band {
  std::vector<Point> ring;
  std::vector<std::array<double, 3>> border;
  Aabb bounds;

  [[nodiscard]] bool empty() const { return ring.size() < 3; }

  /// Height of the surface nearest (x, y). Approximate by construction — the
  /// band is sampled, not a continuous field — which is why the gate carries a
  /// slack rather than comparing exactly.
  [[nodiscard]] double height_at(const Point& at) const {
    double best = 0.0;
    double best_distance = std::numeric_limits<double>::max();
    for (const std::array<double, 3>& sample : border) {
      const double dx = sample[0] - at[0];
      const double dy = sample[1] - at[1];
      const double distance = (dx * dx) + (dy * dy);
      if (distance < best_distance) {
        best_distance = distance;
        best = sample[2];
      }
    }
    return best;
  }
};

/// The DRIVING band of one road: the ring bounded by the outermost driving lane
/// edge on each side, not the full cross section.
///
/// build_contribution's footprint runs offsets.front() to offsets.back() —
/// sidewalks, shoulders and borders included — and testing against THAT would
/// flag every prop standing on a neighbouring road's verge, which is where
/// props belong.
///
/// The cursor walk is signal_facing.cpp's, and it is not interchangeable with
/// zipping the two vectors index-for-index: lane_boundary_offsets covers only
/// the width-bearing lanes, because the centre lane 0 is a boundary and not a
/// span. Advancing the edge cursor for lane 0 too puts the band's edge one lane
/// out, on a road with a centre lane — which is every road this editor authors.
Band build_driving_band(const RoadNetwork& network,
                        RoadId road_id,
                        const Road& road,
                        const SamplingOptions& sampling) {
  Band out;
  if (road.plan_view.empty() || road.sections.empty()) {
    return out;
  }
  // Same station set fill_backend::road_stations builds, so the band's samples
  // land on the road mesh's own vertices rather than between them.
  SamplingOptions stationing = sampling;
  const std::vector<double> extra = mesh_detail::mandatory_stations(network, road);
  stationing.extra_stations.insert(stationing.extra_stations.end(), extra.begin(), extra.end());
  const std::vector<double> stations = sample_stations(road.plan_view, stationing);

  std::vector<std::array<double, 3>> left;
  std::vector<std::array<double, 3>> right;
  left.reserve(stations.size());
  right.reserve(stations.size());

  for (const double station : stations) {
    const LaneSectionId section_id = section_at(network, road_id, station);
    const LaneSection* section = network.lane_section(section_id);
    if (section == nullptr) {
      continue;
    }
    const std::vector<double> boundaries =
        lane_boundary_offsets(network, road, *section, station);
    if (boundaries.size() < 2) {
      continue;
    }

    bool any = false;
    double lo = 0.0;
    double hi = 0.0;
    std::size_t edge = 0;
    for (const LaneId lane_id : section->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane == nullptr) {
        break; // a stale lane id desyncs the edge cursor: stop rather than skip
      }
      if (lane->odr_id == 0) {
        continue; // lane 0 owns no span, so it does not consume an edge
      }
      if (edge + 1 >= boundaries.size()) {
        break; // cross section and edge list disagree: stop rather than guess
      }
      const double outer = boundaries[edge];
      const double inner = boundaries[edge + 1];
      ++edge;
      if (lane->type != LaneType::Driving) {
        continue;
      }
      const double span_lo = std::min(inner, outer);
      const double span_hi = std::max(inner, outer);
      lo = any ? std::min(lo, span_lo) : span_lo;
      hi = any ? std::max(hi, span_hi) : span_hi;
      any = true;
    }
    if (!any || hi - lo <= tol::kLength) {
      continue; // no driving surface at this station, or it has tapered away
    }

    const mesh_detail::StationFrame frame = mesh_detail::make_frame(road, station);
    left.push_back(mesh_detail::lateral_point(frame, hi));
    right.push_back(mesh_detail::lateral_point(frame, lo));
  }

  if (left.size() < 2) {
    return out;
  }
  out.ring.reserve(left.size() * 2);
  out.border.reserve(left.size() * 2);
  for (const std::array<double, 3>& p : left) {
    out.ring.push_back({p[0], p[1]});
    out.border.push_back(p);
    out.bounds.add(p[0], p[1]);
  }
  for (auto it = right.rbegin(); it != right.rend(); ++it) {
    out.ring.push_back({(*it)[0], (*it)[1]});
    out.border.push_back(*it);
    out.bounds.add((*it)[0], (*it)[1]);
  }
  return out;
}

// --- the gathered inputs -----------------------------------------------------

struct PropEntry {
  ObjectId id;
  RoadId anchor;
  std::size_t instance = 0;
  Volume volume;
  road_detail::TouchedJunctions anchor_junctions{};
};

struct RoadEntry {
  RoadId id;
  const Road* road = nullptr;
  road_detail::TouchedJunctions junctions{};
  Aabb reach; ///< reference-line bounds grown by a generous half-width
  bool built = false;
  Band band;
};

/// Every checkable prop instance in the network. Empty (the common case for a
/// scene with no props, or only paint) short-circuits the whole query before a
/// single band is built — the same early-out bridge_anchors uses.
std::vector<PropEntry> gather_props(const RoadNetwork& network) {
  std::vector<PropEntry> out;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (is_paint(object)) {
      return;
    }
    const Road* road = network.road(object.road);
    if (road == nullptr || road->plan_view.empty()) {
      return;
    }
    for (const object_placement::PlacedInstance& placed :
         object_placement::placed_instances(*road, object)) {
      std::optional<Volume> volume = volume_of(object, placed);
      if (!volume.has_value()) {
        continue; // no declared bounding volume: not obstruction-checked
      }
      out.push_back(PropEntry{.id = id,
                              .anchor = object.road,
                              .instance = placed.index,
                              .volume = *volume,
                              .anchor_junctions = {}});
    }
  });
  return out;
}

bool contains(std::span<const RoadId> roads, RoadId road) {
  return std::find(roads.begin(), roads.end(), road) != roads.end();
}

std::vector<PropObstruction> find_impl(const RoadNetwork& network,
                                       std::span<const RoadId> touching,
                                       bool narrowed,
                                       const PropObstructionOptions& options) {
  std::vector<PropObstruction> out;

  std::vector<PropEntry> props = gather_props(network);
  if (props.empty()) {
    return out;
  }

  Aabb props_reach;
  for (PropEntry& prop : props) {
    const Road* road = network.road(prop.anchor);
    prop.anchor_junctions = road_detail::touched_junctions(network, prop.anchor, *road);
    if (!narrowed || contains(touching, prop.anchor)) {
      props_reach.add(prop.volume.bounds.lo_x, prop.volume.bounds.lo_y);
      props_reach.add(prop.volume.bounds.hi_x, prop.volume.bounds.hi_y);
    }
  }

  // --- roads ---------------------------------------------------------------
  std::vector<RoadEntry> roads;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.plan_view.empty() || road.sections.empty()) {
      return;
    }
    if (road.junction.is_valid()) {
      // A junction's connecting roads are represented by its floor, which is
      // what the user sees and what the report should name. Testing them too
      // would report the same overlap once per turn.
      return;
    }
    RoadEntry entry{.id = id,
                    .road = &road,
                    .junctions = road_detail::touched_junctions(network, id, road),
                    .reach = {},
                    .built = false,
                    .band = {}};
    for (const double s : sample_stations(road.plan_view)) {
      const PathPoint p = road.plan_view.evaluate(s);
      entry.reach.add(p.x, p.y);
    }
    // Reference-line bounds say nothing about the cross section; grow them by a
    // generous half-width so the cheap reject cannot drop a real overlap.
    entry.reach.grow(30.0);
    roads.push_back(std::move(entry));
  });

  for (const PropEntry& prop : props) {
    for (RoadEntry& road : roads) {
      if (road.id == prop.anchor) {
        continue; // R1: never against its own anchor road
      }
      if (road_detail::junction_connected(prop.anchor_junctions, road.junctions)) {
        continue; // R2: a junction connects them — they meet by design
      }
      if (narrowed && !contains(touching, prop.anchor) && !contains(touching, road.id)) {
        continue;
      }
      if (!prop.volume.bounds.overlaps(road.reach)) {
        continue;
      }
      if (!road.built) {
        road.band = build_driving_band(network, road.id, *road.road, options.sampling);
        road.built = true;
      }
      if (road.band.empty() || !prop.volume.bounds.overlaps(road.band.bounds)) {
        continue;
      }
      const std::optional<Point> at = ring_witness(prop.volume, road.band.ring);
      if (!at.has_value()) {
        continue;
      }
      if (!prop.volume.overlaps_height(road.band.height_at(*at), options.vertical_clearance)) {
        continue;
      }
      out.push_back(PropObstruction{.object = prop.id,
                                    .instance = prop.instance,
                                    .kind = ObstructionKind::RoadSurface,
                                    .road = road.id,
                                    .junction = {},
                                    .other = {},
                                    .other_instance = 0,
                                    .at = *at});
    }
  }

  // --- junction floors ------------------------------------------------------
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    // A floor moves when any of its arms does — the whole junction is
    // regenerated around them (cascade-s2).
    const bool arm_moved =
        narrowed && std::any_of(junction.arms.begin(), junction.arms.end(), [&](const RoadEnd& arm) {
          return contains(touching, arm.road);
        });
    std::vector<Band> floor;
    bool built = false;
    for (const PropEntry& prop : props) {
      if (road_detail::touches_junction(prop.anchor_junctions, junction_id)) {
        continue; // R3: the anchor road is an arm of this junction
      }
      if (narrowed && !arm_moved && !contains(touching, prop.anchor)) {
        continue;
      }
      if (!built) {
        for (const JunctionSurfaceSpanInfo& span :
             junction_surface_spans(network, junction_id, options.sampling)) {
          Band band;
          band.ring.reserve(span.footprint.size());
          for (const std::array<double, 2>& point : span.footprint) {
            band.ring.push_back(point);
            band.bounds.add(point[0], point[1]);
          }
          band.border = span.border;
          if (!band.empty()) {
            floor.push_back(std::move(band));
          }
        }
        built = true;
      }
      for (const Band& band : floor) {
        if (!prop.volume.bounds.overlaps(band.bounds)) {
          continue;
        }
        const std::optional<Point> at = ring_witness(prop.volume, band.ring);
        if (!at.has_value()) {
          continue;
        }
        if (!prop.volume.overlaps_height(band.height_at(*at), options.vertical_clearance)) {
          continue;
        }
        out.push_back(PropObstruction{.object = prop.id,
                                      .instance = prop.instance,
                                      .kind = ObstructionKind::JunctionFloor,
                                      .road = {},
                                      .junction = junction_id,
                                      .other = {},
                                      .other_instance = 0,
                                      .at = *at});
        break; // one report per (prop, junction): the floor is one surface
      }
    }
  });

  // --- prop vs prop ---------------------------------------------------------
  for (std::size_t i = 0; i < props.size(); ++i) {
    for (std::size_t j = i + 1; j < props.size(); ++j) {
      const PropEntry& a = props[i];
      const PropEntry& b = props[j];
      if (a.id == b.id) {
        continue; // R5: a repeat series tighter than its own diameter is a hedge
      }
      if (narrowed && !contains(touching, a.anchor) && !contains(touching, b.anchor)) {
        continue;
      }
      if (!a.volume.bounds.overlaps(b.volume.bounds)) {
        continue;
      }
      if (a.volume.z_hi < b.volume.z_lo || b.volume.z_hi < a.volume.z_lo) {
        continue; // stacked, not overlapping — one stands on a deck above the other
      }
      const std::optional<Point> at = volume_witness(a.volume, b.volume);
      if (!at.has_value()) {
        continue;
      }
      out.push_back(PropObstruction{.object = a.id,
                                    .instance = a.instance,
                                    .kind = ObstructionKind::Prop,
                                    .road = {},
                                    .junction = {},
                                    .other = b.id,
                                    .other_instance = b.instance,
                                    .at = *at});
    }
  }

  // Traversal order must never leak into the result: a caller diffing two runs
  // (the cascade stage does exactly that) would see phantom changes.
  std::sort(out.begin(), out.end(), [](const PropObstruction& a, const PropObstruction& b) {
    return std::tie(a.object.index, a.instance, a.kind, a.road.index, a.junction.index,
                    a.other.index, a.other_instance) <
           std::tie(b.object.index, b.instance, b.kind, b.road.index, b.junction.index,
                    b.other.index, b.other_instance);
  });
  return out;
}

} // namespace

std::vector<PropObstruction> find_prop_obstructions(const RoadNetwork& network,
                                                    const PropObstructionOptions& options) {
  return find_impl(network, {}, /*narrowed=*/false, options);
}

std::vector<PropObstruction> find_prop_obstructions(const RoadNetwork& network,
                                                    std::span<const RoadId> touching,
                                                    const PropObstructionOptions& options) {
  return find_impl(network, touching, /*narrowed=*/true, options);
}

} // namespace roadmaker
