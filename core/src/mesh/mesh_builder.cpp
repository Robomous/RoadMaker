// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/mesh/mesh_builder.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/repeat_expansion.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fill_backend.hpp"
#include "junction_surface.hpp"
#include "mesh_detail.hpp"
#include "surface_fill.hpp"

namespace roadmaker {

namespace {

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::mandatory_stations;
using mesh_detail::StationFrame;
using mesh_detail::surface_normal;

// M1 marking pattern for RoadMarkType::Broken: 3 m dash / 6 m gap.
constexpr double kDashLength = 3.0;
constexpr double kDashCycle = 9.0;

// Lift markings slightly above the surface to avoid z-fighting.
constexpr double kMarkingLift = 0.002;

/// The outer boundary offset of a lane (its t at the edge away from the
/// reference line) — where its marking is painted.
double outer_boundary_offset(const RoadNetwork& network,
                             const Road& road,
                             const LaneSection& section,
                             const Lane& lane,
                             double s) {
  const std::vector<double> offsets = boundary_offsets(network, road, section, s);
  const std::size_t center_index = static_cast<std::size_t>(std::ranges::count_if(
      section.lanes, [&](LaneId id) { return network.lane(id)->odr_id > 0; }));
  if (lane.odr_id == 0) {
    return offsets[center_index];
  }
  // p-th lane out from the center on its side.
  std::size_t p = 0;
  for (const LaneId lane_id : section.lanes) {
    const Lane& other = *network.lane(lane_id);
    if ((lane.odr_id > 0) == (other.odr_id > 0) && other.odr_id != 0 &&
        std::abs(other.odr_id) <= std::abs(lane.odr_id)) {
      ++p;
    }
  }
  return lane.odr_id > 0 ? offsets[center_index - p] : offsets[center_index + p];
}

void emit_marking_strip(const Road& road,
                        double s_begin,
                        double s_end,
                        double t,
                        double width,
                        std::vector<double> section_stations,
                        SubMesh& out) {
  // Clamp the strip's sampling to [s_begin, s_end].
  std::erase_if(section_stations,
                [&](double s) { return s < s_begin - tol::kLength || s > s_end + tol::kLength; });
  if (section_stations.empty() || section_stations.front() > s_begin + tol::kLength) {
    section_stations.insert(section_stations.begin(), s_begin);
  }
  if (section_stations.back() < s_end - tol::kLength) {
    section_stations.push_back(s_end);
  }
  const double half = width / 2.0;
  const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
  for (const double s : section_stations) {
    const StationFrame frame = make_frame(road, s);
    for (const double offset : {t + half, t - half}) {
      const auto p = lateral_point(frame, offset);
      out.positions.insert(out.positions.end(), {p[0], p[1], p[2] + kMarkingLift});
      const auto n = surface_normal(frame);
      out.normals.insert(out.normals.end(), n.begin(), n.end());
    }
  }
  for (std::uint32_t row = 0; row + 1 < section_stations.size(); ++row) {
    const std::uint32_t a = base + (row * 2); // left edge of strip
    const std::uint32_t b = a + 1;            // right edge
    const std::uint32_t c = a + 3;            // next right
    const std::uint32_t d = a + 2;            // next left
    out.indices.insert(out.indices.end(), {a, b, c, a, c, d});
  }
}

/// The painted stripes a road mark decomposes into, resolved for meshing
/// (docs/design/m3a/02 §1/§4). Explicit <line> geometry wins; otherwise the
/// multi-line types synthesize two symmetric stripes so solid_solid renders
/// as true dual geometry, and single types stay one stripe (M2 behaviour).
std::vector<RoadMarkLine> resolve_stripes(const RoadMark& mark) {
  if (!mark.lines.empty()) {
    return mark.lines;
  }
  const double w = mark.width;
  const RoadMarkLine solid{.width = w};
  const RoadMarkLine broken{.width = w, .length = kDashLength, .space = kDashCycle - kDashLength};
  // Symmetric split for the *_family: two stripes one mark-width apart.
  const auto at = [](RoadMarkLine stripe, double t) {
    stripe.t_offset = t;
    return stripe;
  };
  switch (mark.type) {
  case RoadMarkType::Broken:
    return {broken};
  case RoadMarkType::SolidSolid:
    return {at(solid, +w), at(solid, -w)};
  case RoadMarkType::SolidBroken:
    return {at(solid, +w), at(broken, -w)};
  case RoadMarkType::BrokenSolid:
    return {at(broken, +w), at(solid, -w)};
  case RoadMarkType::BrokenBroken:
    return {at(broken, +w), at(broken, -w)};
  case RoadMarkType::Solid:
  case RoadMarkType::Other:
  case RoadMarkType::None:
    break;
  }
  return {solid};
}

void build_markings(const RoadNetwork& network,
                    const Road& road,
                    const RoadId road_id,
                    const std::vector<double>& stations,
                    RoadMesh& mesh) {
  for (std::size_t si = 0; si < road.sections.size(); ++si) {
    const LaneSection& section = *network.lane_section(road.sections[si]);
    const double section_end = si + 1 < road.sections.size()
                                   ? network.lane_section(road.sections[si + 1])->s0
                                   : road.plan_view.length();

    std::vector<double> section_stations;
    for (const double s : stations) {
      if (s >= section.s0 - tol::kLength && s <= section_end + tol::kLength) {
        section_stations.push_back(s);
      }
    }

    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      for (std::size_t mi = 0; mi < lane.road_marks.size(); ++mi) {
        const RoadMark& mark = lane.road_marks[mi];
        if (mark.type == RoadMarkType::None || mark.width <= 0.0) {
          continue;
        }
        const double mark_begin = section.s0 + mark.s_offset;
        const double mark_end = mi + 1 < lane.road_marks.size()
                                    ? section.s0 + lane.road_marks[mi + 1].s_offset
                                    : section_end;
        if (mark_end <= mark_begin) {
          continue;
        }

        SubMesh strip;
        strip.material = lane.type;
        strip.mark_color = mark.color;
        strip.name = fmt::format("road {} lane {} marking", road.odr_id, lane.odr_id);

        // One quad run per painted stripe. Multi-line marks emit two strips at
        // their lateral offsets; dashed stripes (space > 0) tessellate as
        // segment runs of `length` on / (length + space) cycle.
        for (const RoadMarkLine& stripe : resolve_stripes(mark)) {
          const double begin = std::min(mark_begin + stripe.s_offset, mark_end);
          if (stripe.width <= 0.0) {
            continue;
          }
          if (stripe.space > tol::kLength && stripe.length > tol::kLength) {
            const double cycle = stripe.length + stripe.space;
            for (double dash = begin; dash < mark_end; dash += cycle) {
              const double dash_end = std::min(dash + stripe.length, mark_end);
              const double t =
                  outer_boundary_offset(network, road, section, lane, dash) + stripe.t_offset;
              emit_marking_strip(road, dash, dash_end, t, stripe.width, section_stations, strip);
            }
          } else {
            const double t =
                outer_boundary_offset(network, road, section, lane, begin) + stripe.t_offset;
            emit_marking_strip(road, begin, mark_end, t, stripe.width, section_stations, strip);
          }
        }
        if (!strip.indices.empty()) {
          mesh.markings.push_back(std::move(strip));
        }
      }
    }
    (void)road_id;
  }
}

// --- object markings (crosswalks, stop lines, lane arrows) — §13.8/§13.14 ---

/// A flat local frame at an object's origin for painted-marking geometry: the
/// world centre (lifted off the surface) plus planar u (forward = road tangent
/// rotated by the object heading) and v (leftward) axes.
struct ObjectFrame {
  std::array<double, 3> center;
  std::array<double, 3> normal;
  double ux, uy; // forward axis in world XY
  double vx, vy; // leftward axis in world XY
};

/// The paint frame at a road-relative pose. Split out of object_frame so a
/// DERIVED marking (one with no Object behind it, e.g. a junction stop line)
/// gets a frame from exactly the same math a placed object would.
ObjectFrame pose_frame(const Road& road, double s, double t, double hdg) {
  const StationFrame f = make_frame(road, s);
  std::array<double, 3> center = lateral_point(f, t);
  center[2] += kMarkingLift;
  const double heading = std::atan2(f.sin_h, f.cos_h) + hdg;
  return ObjectFrame{.center = center,
                     .normal = surface_normal(f),
                     .ux = std::cos(heading),
                     .uy = std::sin(heading),
                     .vx = -std::sin(heading),
                     .vy = std::cos(heading)};
}

ObjectFrame object_frame(const Road& road, const Object& object) {
  return pose_frame(road, object.s, object.t, object.hdg);
}

/// Appends a convex polygon (local u/v points, CCW) as a triangle fan.
void emit_object_polygon(const ObjectFrame& frame,
                         std::span<const std::array<double, 2>> points,
                         SubMesh& out) {
  if (points.size() < 3) {
    return;
  }
  const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
  for (const std::array<double, 2>& p : points) {
    out.positions.insert(out.positions.end(),
                         {frame.center[0] + (p[0] * frame.ux) + (p[1] * frame.vx),
                          frame.center[1] + (p[0] * frame.uy) + (p[1] * frame.vy),
                          frame.center[2]});
    out.normals.insert(out.normals.end(), frame.normal.begin(), frame.normal.end());
  }
  for (std::uint32_t i = 1; i + 1 < static_cast<std::uint32_t>(points.size()); ++i) {
    out.indices.insert(out.indices.end(), {base, base + i, base + i + 1});
  }
}

/// A filled rectangle centred at the object, `length` along u, `width` along v.
void emit_object_quad(const ObjectFrame& frame, double length, double width, SubMesh& out) {
  const double hu = length / 2.0;
  const double hv = width / 2.0;
  const std::array<std::array<double, 2>, 4> quad{{{-hu, -hv}, {hu, -hv}, {hu, hv}, {-hu, hv}}};
  emit_object_polygon(frame, quad, out);
}

// Zebra stripe geometry (§13.14.3): paint band + gap repeated along u.
constexpr double kZebraStripe = 0.5;
constexpr double kZebraGap = 0.5;

/// Alternating painted bars across the crossing: `length` along u (crossing
/// direction), `width` along v. Deterministic bar layout from -length/2. Used
/// as the fallback for foreign crosswalks that carry no CrosswalkData.
void emit_zebra(const ObjectFrame& frame, double length, double width, SubMesh& out) {
  const double hv = width / 2.0;
  const double cycle = kZebraStripe + kZebraGap;
  for (double u = -length / 2.0; u < length / 2.0 - tol::kLength; u += cycle) {
    const double u_end = std::min(u + kZebraStripe, length / 2.0);
    const std::array<std::array<double, 2>, 4> bar{{{u, -hv}, {u_end, -hv}, {u_end, hv}, {u, hv}}};
    emit_object_polygon(frame, bar, out);
  }
}

/// A parametric crosswalk from its CrosswalkData (p3-s2). `length` is the
/// crossing span along u, `width` the walking depth along v. dash_length<=0
/// paints one solid quad; otherwise bars of `dash_length` repeat every
/// dash_length+dash_gap. With border_width>0, two solid border quads frame the
/// two road-parallel edges (the u-extremes). Matches the outline/markings the
/// generator authors (edit::author_crosswalk).
void emit_crosswalk(const ObjectFrame& frame,
                    double length,
                    double width,
                    const CrosswalkData& data,
                    SubMesh& out) {
  const double hv = width / 2.0;
  const double hu = length / 2.0;
  if (data.dash_length <= tol::kLength) {
    emit_object_quad(frame, length, width, out); // solid crossing
  } else {
    const double cycle = data.dash_length + data.dash_gap;
    for (double u = -hu; u < hu - tol::kLength; u += cycle) {
      const double u_end = std::min(u + data.dash_length, hu);
      const std::array<std::array<double, 2>, 4> bar{
          {{u, -hv}, {u_end, -hv}, {u_end, hv}, {u, hv}}};
      emit_object_polygon(frame, bar, out);
    }
  }
  if (data.border_width > tol::kLength) {
    const double bw = std::min(data.border_width, length); // clamp to the span
    for (const double u_center : {-hu + (bw / 2.0), hu - (bw / 2.0)}) {
      const std::array<std::array<double, 2>, 4> border{{{u_center - (bw / 2.0), -hv},
                                                         {u_center + (bw / 2.0), -hv},
                                                         {u_center + (bw / 2.0), hv},
                                                         {u_center - (bw / 2.0), hv}}};
      emit_object_polygon(frame, border, out);
    }
  }
}

/// Parametric arrow glyph (generated, no asset — GS-1 checklist row 5): a
/// shaft rectangle plus a triangular head, `length` along u (travel), sized to
/// `width`. The head tip shifts laterally for the left/right turn variants.
void emit_arrow(
    const ObjectFrame& frame, double length, double width, std::string_view subtype, SubMesh& out) {
  const double neck = length * 0.2;       // where the head starts (from centre)
  const double shaft_half = width * 0.15; // shaft half-width
  const double head_half = width * 0.5;   // head base half-width
  // Shaft: tail (-length/2) to neck.
  const std::array<std::array<double, 2>, 4> shaft{{{-length / 2.0, -shaft_half},
                                                    {neck, -shaft_half},
                                                    {neck, shaft_half},
                                                    {-length / 2.0, shaft_half}}};
  emit_object_polygon(frame, shaft, out);
  // Head: base at neck spanning ±head_half, tip forward (+u) with a lateral
  // bias for turn arrows.
  double tip_v = 0.0;
  if (subtype == "arrowLeft") {
    tip_v = head_half;
  } else if (subtype == "arrowRight") {
    tip_v = -head_half;
  }
  const std::array<std::array<double, 2>, 3> head{
      {{neck, -head_half}, {neck, head_half}, {length / 2.0, tip_v}}};
  emit_object_polygon(frame, head, out);
}

/// A free-form marking curve from its MarkingCurveData (p3-s4): a band of the
/// stored width painted along the (s,t) centreline, walked by arc length so a
/// dash pattern (dash_length>0) breaks into runs. Each sample maps through the
/// road reference line to a world point; the band edges are the centreline
/// offset ±width/2 along the XY-perpendicular of the local tangent. Solid when
/// dash_length<=0. Meshed from the userData, mirroring emit_crosswalk (the
/// outline/markings are only the foreign-tool projection).
void emit_marking_curve(const Road& road, const MarkingCurveData& data, SubMesh& out) {
  const std::vector<std::array<double, 2>>& samples = data.samples;
  if (samples.size() < 2) {
    return;
  }
  const double half = std::max(data.width, tol::kLength) / 2.0;

  struct Node {
    std::array<double, 3> p;    // world point (surface-lifted)
    std::array<double, 2> left; // unit XY perpendicular, pointing left
    std::array<double, 3> n;    // surface normal
  };

  std::vector<Node> nodes(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const StationFrame f = make_frame(road, samples[i][0]);
    std::array<double, 3> p = lateral_point(f, samples[i][1]);
    p[2] += kMarkingLift;
    nodes[i].p = p;
    nodes[i].n = surface_normal(f);
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    double dx = 0.0;
    double dy = 0.0;
    if (i > 0) {
      dx += nodes[i].p[0] - nodes[i - 1].p[0];
      dy += nodes[i].p[1] - nodes[i - 1].p[1];
    }
    if (i + 1 < nodes.size()) {
      dx += nodes[i + 1].p[0] - nodes[i].p[0];
      dy += nodes[i + 1].p[1] - nodes[i].p[1];
    }
    const double len = std::hypot(dx, dy);
    nodes[i].left = len > tol::kLength ? std::array<double, 2>{-dy / len, dx / len}
                                       : std::array<double, 2>{0.0, 0.0};
  }

  // Push a band quad between the interpolated cross-sections at local arc
  // lengths la..lb of segment [A,B]; left/normal are lerped and re-normalized.
  const auto emit_span = [&](const Node& a, const Node& b, double seglen, double la, double lb) {
    const auto cross = [&](double frac) {
      std::array<double, 3> centre{a.p[0] + ((b.p[0] - a.p[0]) * frac),
                                   a.p[1] + ((b.p[1] - a.p[1]) * frac),
                                   a.p[2] + ((b.p[2] - a.p[2]) * frac)};
      double lx = a.left[0] + ((b.left[0] - a.left[0]) * frac);
      double ly = a.left[1] + ((b.left[1] - a.left[1]) * frac);
      const double llen = std::hypot(lx, ly);
      if (llen > tol::kLength) {
        lx /= llen;
        ly /= llen;
      }
      const std::array<double, 3> l{centre[0] - (half * lx), centre[1] - (half * ly), centre[2]};
      const std::array<double, 3> r{centre[0] + (half * lx), centre[1] + (half * ly), centre[2]};
      return std::pair{l, r};
    };
    const auto [la_l, la_r] = cross(la / seglen);
    const auto [lb_l, lb_r] = cross(lb / seglen);
    const std::array<double, 3> n{
        (a.n[0] + b.n[0]) / 2.0, (a.n[1] + b.n[1]) / 2.0, (a.n[2] + b.n[2]) / 2.0};
    const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
    for (const std::array<double, 3>& v : {la_r, lb_r, lb_l, la_l}) {
      out.positions.insert(out.positions.end(), v.begin(), v.end());
      out.normals.insert(out.normals.end(), n.begin(), n.end());
    }
    out.indices.insert(out.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  };

  const bool solid = data.dash_length <= tol::kLength;
  const double cycle = data.dash_length + data.dash_gap;
  double dist = 0.0;
  for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
    const Node& a = nodes[i];
    const Node& b = nodes[i + 1];
    const double seglen = std::hypot(b.p[0] - a.p[0], b.p[1] - a.p[1]);
    if (seglen <= tol::kLength) {
      continue;
    }
    if (solid) {
      emit_span(a, b, seglen, 0.0, seglen);
    } else if (cycle > tol::kLength) {
      const long kstart = static_cast<long>(std::floor(dist / cycle));
      for (long k = kstart; static_cast<double>(k) * cycle < dist + seglen; ++k) {
        const double pa = std::max(dist, static_cast<double>(k) * cycle);
        const double pb =
            std::min(dist + seglen, (static_cast<double>(k) * cycle) + data.dash_length);
        if (pb > pa) {
          emit_span(a, b, seglen, pa - dist, pb - dist);
        }
      }
    }
    dist += seglen;
  }
}

/// Tessellates a closed cornerLocal outline (u,v) with the in-tree CDT and
/// emits the triangles in the object's frame — the concave-glyph path for
/// authored arrow stencils (p3-s4). No-op when CDT refuses the polygon.
void emit_tessellated_outline(const ObjectFrame& frame,
                              const ObjectOutline& outline,
                              SubMesh& out) {
  std::vector<std::array<double, 2>> polygon;
  polygon.reserve(outline.corners.size());
  for (const OutlineCorner& corner : outline.corners) {
    polygon.push_back({corner.a, corner.b}); // (u, v) local
  }
  const std::optional<fill_backend::CompactMesh> mesh = fill_backend::tessellate_polygon(polygon);
  if (!mesh.has_value()) {
    return;
  }
  const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
  for (const fill_backend::Vec3& v : mesh->vertices) {
    out.positions.insert(out.positions.end(),
                         {frame.center[0] + (v.x * frame.ux) + (v.y * frame.vx),
                          frame.center[1] + (v.x * frame.uy) + (v.y * frame.vy),
                          frame.center[2]});
    out.normals.insert(out.normals.end(), frame.normal.begin(), frame.normal.end());
  }
  for (const std::array<std::uint32_t, 3>& tri : mesh->triangles) {
    out.indices.insert(out.indices.end(), {base + tri[0], base + tri[1], base + tri[2]});
  }
}

/// True when `object` carries an authored stencil outline (one closed cornerLocal
/// loop) the mesher should tessellate instead of drawing the parametric glyph.
bool has_local_glyph_outline(const Object& object) {
  return !object.outlines.empty() && !object.outlines.front().road_coords &&
         object.outlines.front().closed.value_or(false) &&
         object.outlines.front().corners.size() >= 3;
}

/// Object-based road markings a road owns (crosswalks, stop lines, arrows —
/// docs/design/m3a/02 §3). Emitted as paint submeshes; unresolved object
/// types are left to the render-side placeholder box (phase 4).
void build_object_markings(const RoadNetwork& network,
                           const Road& road,
                           RoadId road_id,
                           RoadMesh& mesh) {
  for (const ObjectId object_id : objects_of(network, road_id)) {
    const Object& object = *network.object(object_id);
    const ObjectFrame frame = object_frame(road, object);
    SubMesh marking;
    marking.material = LaneType::None;

    if (object.marking_curve.has_value()) {
      // Free-form curve: render exactly from MarkingCurveData (dispatched before
      // the crosswalk branch — a striped curve is typed as a crosswalk).
      marking.name = fmt::format("road {} object {} marking curve", road.odr_id, object.odr_id);
      emit_marking_curve(road, *object.marking_curve, marking);
      marking.surface = object.marking_curve->material;
    } else if (object.type == ObjectType::Crosswalk) {
      const double length = object.length.value_or(4.0);
      const double width = object.width.value_or(2.0);
      marking.name = fmt::format("road {} object {} crosswalk", road.odr_id, object.odr_id);
      if (object.crosswalk.has_value()) {
        // Parametric asset instance: render exactly from CrosswalkData and
        // carry its material code so the viewport can tint the paint.
        emit_crosswalk(frame, length, width, *object.crosswalk, marking);
        marking.surface = object.crosswalk->material;
      } else {
        emit_zebra(frame, length, width, marking); // foreign crosswalk fallback
      }
    } else if (object.type_str == "roadMark" && object.subtype == "signalLines") {
      const double length = object.length.value_or(0.3);
      const double width = object.width.value_or(3.5);
      marking.name = fmt::format("road {} object {} stop line", road.odr_id, object.odr_id);
      emit_object_quad(frame, length, width, marking);
    } else if (object.type_str == "roadMark" && object.subtype.starts_with("arrow")) {
      marking.name = fmt::format("road {} object {} arrow", road.odr_id, object.odr_id);
      if (has_local_glyph_outline(object)) {
        // Authored stencil: tessellate the object's own concave glyph outline.
        emit_tessellated_outline(frame, object.outlines.front(), marking);
      } else {
        const double length = object.length.value_or(4.0);
        const double width = object.width.value_or(1.75);
        emit_arrow(frame, length, width, object.subtype, marking); // legacy/merge fallback
      }
      if (object.stencil.has_value()) {
        marking.surface = object.stencil->material; // tint from the asset material
      }
    } else {
      continue; // not a painted marking — render-side placeholder (phase 4)
    }
    if (!marking.indices.empty()) {
      mesh.markings.push_back(std::move(marking));
    }
  }
}

/// Junction stop lines painted on THIS road (p4-s3, #318). Unlike every other
/// marking these have no Object behind them: they are derived from the junction
/// at each end of the road (plus any authored StopLine record) and materialized
/// into the .xodr only on write. junction_stoplines() is the single geometry
/// source, so what the viewport paints and what the writer exports cannot drift.
///
/// The quad is emitted through the same emit_object_quad the placed-object
/// branch uses, with a synthetic frame at (s_center, t_center, hdg = 0) and
/// length/width taken from the solve — pixel-identical to the objects the
/// pre-p4-s3 generator used to create. An arm already carrying a legacy or
/// foreign signalLines object is suppressed inside junction_stoplines(), so
/// that object's own branch above draws it exactly once.
void build_stopline_markings(const RoadNetwork& network,
                             const Road& road,
                             RoadId road_id,
                             RoadMesh& mesh) {
  const auto paint = [&](const JunctionStopLineInfo& info) {
    const ObjectFrame frame = pose_frame(road, info.s_center, info.t_center, 0.0);
    SubMesh marking;
    marking.material = LaneType::None;
    marking.name = fmt::format("road {} stopline {}",
                               road.odr_id,
                               info.arm.contact == ContactPoint::End ? "end" : "start");
    emit_object_quad(frame, info.thickness, info.span, marking);
    if (!marking.indices.empty()) {
      mesh.markings.push_back(std::move(marking));
    }
  };

  for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
    const RoadEnd arm{.road = road_id, .contact = contact};
    const std::optional<JunctionId> junction = edit::junction_at_end(network, arm);
    if (!junction.has_value()) {
      continue;
    }
    for (const JunctionStopLineInfo& info : junction_stoplines(network, *junction)) {
      if (info.arm == arm) {
        paint(info);
      }
    }
  }

  // Span (virtual) junction faces (p4-s4, issue #319): the road is never cut,
  // so junction_at_end finds nothing and the owning junction has to be looked
  // up through its span list instead. A road can carry several span junctions
  // (two crosswalks), so every one of them is asked.
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    const bool spans_this_road =
        std::any_of(junction.spans.begin(), junction.spans.end(), [&](const SpanArm& span) {
          return span.road == road_id;
        });
    if (!spans_this_road) {
      return;
    }
    for (const JunctionStopLineInfo& info : junction_stoplines(network, junction_id)) {
      if (info.arm.road == road_id) {
        paint(info);
      }
    }
  });
}

/// Placed props (trees/vegetation) a road owns, as INSTANCES of bundled prop
/// models. An object contributes an instance only when it is a Tree/Vegetation
/// whose @name resolves to a prop_library model; markings and foreign objects
/// are skipped here (they round-trip in xodr and, for markings, mesh as paint
/// in build_object_markings). The instance transform is the object's world
/// pose from its road-relative s/t (no marking z-lift — a prop sits on the
/// surface, offset only by its z_offset).
void build_object_instances(const RoadNetwork& network,
                            const Road& road,
                            RoadId road_id,
                            std::vector<ObjectInstance>& out) {
  for (const ObjectId object_id : objects_of(network, road_id)) {
    const Object& object = *network.object(object_id);
    // Instanced prop classes: trees, vegetation, poles (streetlights), and
    // buildings all resolve their @name to a bundled prop model. Crosswalks are
    // excluded — they mesh as paint in build_object_markings, not as props.
    if (object.type != ObjectType::Tree && object.type != ObjectType::Vegetation &&
        object.type != ObjectType::Pole && object.type != ObjectType::Building) {
      continue;
    }
    const props::PropModel* prop_model = props::model(object.name);
    if (prop_model == nullptr) {
      continue;
    }
    // One derivation per object: every instance it emits (single or repeated)
    // renders at the size the object declares.
    const double scale = props::instance_scale(object, prop_model);

    // §13.4: a <repeat> with @distance > 0 places a SERIES of instances and
    // "the <repeat> element takes precedence" over the object's own s/t/hdg, so
    // the base single instance is suppressed once any such repeat is present. A
    // continuous repeat (@distance == 0) expands to no discrete instance
    // (expand_repeat returns empty), and an object with only continuous — or no
    // — repeats falls through to the single-instance path below.
    const bool has_series_repeat =
        std::any_of(object.repeats.begin(), object.repeats.end(), [](const ObjectRepeat& repeat) {
          return repeat.distance > 0.0;
        });
    if (has_series_repeat) {
      const double road_length = road.plan_view.length();
      for (const ObjectRepeat& repeat : object.repeats) {
        // §13.4: detachFromReferenceLine draws the section "in a straight line
        // from its start to its end position" — the chord between the section's
        // start/end anchors — instead of following the reference-line curvature.
        std::array<double, 3> chord_start{};
        std::array<double, 3> chord_end{};
        double chord_heading = 0.0;
        if (repeat.detach_from_reference_line) {
          const StationFrame start_frame = make_frame(road, repeat.s);
          const StationFrame end_frame = make_frame(road, repeat.s + repeat.length);
          chord_start = lateral_point(start_frame, repeat.t_start);
          chord_start[2] += repeat.z_offset_start;
          chord_end = lateral_point(end_frame, repeat.t_end);
          chord_end[2] += repeat.z_offset_end;
          chord_heading =
              std::atan2(chord_end[1] - chord_start[1], chord_end[0] - chord_start[0]) + object.hdg;
        }

        for (const RepeatInstance& inst : expand_repeat(repeat)) {
          // Repeat s is absolute along the road; skip origins past the road end
          // (the section may legally overshoot, but no instance origin should
          // fall off the reference line).
          if (inst.s > road_length + tol::kLength) {
            continue;
          }
          std::array<double, 3> position{};
          double heading = 0.0;
          if (repeat.detach_from_reference_line) {
            const double ratio = repeat.length > 0.0 ? (inst.s - repeat.s) / repeat.length : 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
              position[axis] = chord_start[axis] + (chord_end[axis] - chord_start[axis]) * ratio;
            }
            heading = chord_heading;
          } else {
            const StationFrame frame = make_frame(road, inst.s);
            position = lateral_point(frame, inst.t);
            position[2] += inst.z_offset;
            heading = std::atan2(frame.sin_h, frame.cos_h) + object.hdg;
          }
          out.push_back(ObjectInstance{.object = object_id,
                                       .road = road_id,
                                       .model_id = object.name,
                                       .position = position,
                                       .heading = heading,
                                       .scale = scale});
        }
      }
      continue;
    }

    const StationFrame frame = make_frame(road, object.s);
    std::array<double, 3> position = lateral_point(frame, object.t);
    position[2] += object.z_offset;
    const double heading = std::atan2(frame.sin_h, frame.cos_h) + object.hdg;
    out.push_back(ObjectInstance{.object = object_id,
                                 .road = road_id,
                                 .model_id = object.name,
                                 .position = position,
                                 .heading = heading,
                                 .scale = scale});
  }
}

/// The bundled signal model a <signal> renders as: a dynamic signal (traffic
/// light) is "signal_light", a static one (sign) is "sign_generic". @dynamic is
/// optional in the schema; an absent value is treated as static (a sign) — the
/// conservative default, and the reader already warns on the missing attribute.
std::string_view signal_model_id(const Signal& signal) {
  if (signal.dynamic.value_or(false)) {
    return "signal_light";
  }
  // A couple of common German StVO (VzKat) regulatory plates render as their own
  // bundled silhouette; every other static sign falls back to the generic plate.
  if (signal.type == "206") { // StVO 206: Halt! Vorfahrt gewähren — STOP
    return "sign_stop";
  }
  if (signal.type == "205") { // StVO 205: Vorfahrt gewähren — yield/give way
    return "sign_yield";
  }
  if (signal.type == "310") { // StVO 310: Ortstafel — town-entrance text plate
    return "sign_plate";
  }
  return "sign_generic";
}

/// A model-space text-face quad for a sign plate: a rectangle +0.005 m in front
/// of the plate's +x face, spanning y (−half_w→+half_w) and z (centred on
/// plate.z). Normals point +x (front-facing); UVs are [0,1] with v=0 at the TOP
/// (z=+half_h) so the row-0-top rasterised bitmap drapes with no flip. Winding is
/// CCW seen from +x. The bitmap is not baked here — consumers raster `text`.
SignalFaceOverlay make_face_overlay(const std::string& text, const props::FacePlate& plate) {
  const double x = plate.x + 0.005; // just in front of the plate face
  const double z_top = plate.z + plate.half_h;
  const double z_bot = plate.z - plate.half_h;
  const double y_left = -plate.half_w; // u = 0
  const double y_right = plate.half_w; // u = 1
  SignalFaceOverlay face;
  face.text = text;
  // Vertices: 0 top-left, 1 bottom-left, 2 bottom-right, 3 top-right.
  face.positions = {x, y_left, z_top, x, y_left, z_bot, x, y_right, z_bot, x, y_right, z_top};
  face.normals = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
  face.uvs = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0};
  face.indices = {0, 1, 2, 0, 2, 3};
  return face;
}

/// Placed signals a road owns, as INSTANCES of bundled signal models — the
/// signal analogue of build_object_instances. The pole base sits on the road
/// surface at the signal's s/t, lifted by z_offset; the model's +x front faces
/// the road tangent rotated by hOffset (so a signal reads as facing across or
/// along the road per its authored orientation).
void build_signal_instances(const RoadNetwork& network,
                            const Road& road,
                            RoadId road_id,
                            std::vector<SignalInstance>& out) {
  for (const SignalId signal_id : signals_of(network, road_id)) {
    const Signal& signal = *network.signal(signal_id);
    const StationFrame frame = make_frame(road, signal.s);
    std::array<double, 3> position = lateral_point(frame, signal.t);
    position[2] += signal.z_offset;
    const double heading = std::atan2(frame.sin_h, frame.cos_h) + signal.h_offset;
    SignalInstance instance{.signal = signal_id,
                            .road = road_id,
                            .model_id = std::string(signal_model_id(signal)),
                            .position = position,
                            .heading = heading};
    // Editable text face: only a STATIC sign with non-empty @text on a model
    // that carries a face plate. Dynamic signals (traffic lights) never do.
    if (!signal.dynamic.value_or(false) && !signal.text.empty()) {
      const props::PropModel* model = props::model(instance.model_id);
      if (model != nullptr && model->face_plate.has_value()) {
        instance.face = make_face_overlay(signal.text, *model->face_plate);
      }
    }
    out.push_back(std::move(instance));
  }
}

/// Sampling stations for one road (mandatory profile knots + adaptive fill).
std::vector<double>
road_stations(const RoadNetwork& network, const Road& road, const MeshOptions& options) {
  SamplingOptions sampling = options.sampling;
  const std::vector<double> extra = mandatory_stations(network, road);
  sampling.extra_stations.insert(sampling.extra_stations.end(), extra.begin(), extra.end());
  return sample_stations(road.plan_view, sampling);
}

/// One road's tessellation; empty (no lanes, no markings) for degenerate
/// roads — callers drop empty results.
RoadMesh build_one_road(const RoadNetwork& network,
                        RoadId road_id,
                        const Road& road,
                        const MeshOptions& options) {
  RoadMesh mesh;
  mesh.road = road_id;
  if (road.plan_view.empty() || road.sections.empty()) {
    return mesh; // parser already diagnosed these
  }

  const std::vector<double> stations = road_stations(network, road, options);

  mesh.name = road.name.empty() ? fmt::format("road {}", road.odr_id) : road.name;

  // A connecting road's surface IS the junction floor (the floor is built
  // from the union of exactly these footprints and stitched to the arms) —
  // emitting its lane grid too would draw two coplanar surfaces that
  // z-fight across the whole junction interior (issue #103). Markings still
  // emit below (they are lifted above the surface and the floor carries no
  // marks of its own).
  const bool surface_is_floor = options.junction_floors && road.junction.is_valid();

  for (std::size_t si = 0; !surface_is_floor && si < road.sections.size(); ++si) {
    const LaneSection& section = *network.lane_section(road.sections[si]);
    const double section_end = si + 1 < road.sections.size()
                                   ? network.lane_section(road.sections[si + 1])->s0
                                   : road.plan_view.length();

    std::vector<double> rows;
    for (const double s : stations) {
      if (s >= section.s0 - tol::kLength && s <= section_end + tol::kLength) {
        rows.push_back(std::clamp(s, section.s0, section_end));
      }
    }
    if (rows.size() < 2) {
      continue;
    }

    const std::uint32_t grid_base = static_cast<std::uint32_t>(mesh.positions.size() / 3);
    std::size_t columns = 0;

    // Vertex grid: one row per station, one column per lane boundary.
    std::vector<std::vector<double>> row_offsets;
    row_offsets.reserve(rows.size());
    for (const double s : rows) {
      const StationFrame frame = make_frame(road, s);
      std::vector<double> offsets = boundary_offsets(network, road, section, s);
      columns = offsets.size();
      for (const double t : offsets) {
        const auto p = lateral_point(frame, t);
        mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
        const auto n = surface_normal(frame);
        mesh.normals.insert(mesh.normals.end(), n.begin(), n.end());
        // Planar texture coordinates in meters: u follows s along the road, v
        // follows t across it, so textures tile continuously across lane
        // boundaries and welded seams (adjacent roads share s/t at the joint).
        mesh.uvs.insert(mesh.uvs.end(), {s, t});
      }
      row_offsets.push_back(std::move(offsets));
    }

    // One patch per non-center lane; triangles skip degenerate quads.
    const std::size_t center_col = static_cast<std::size_t>(std::ranges::count_if(
        section.lanes, [&](LaneId id) { return network.lane(id)->odr_id > 0; }));
    for (const LaneId lane_id : section.lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id == 0) {
        continue;
      }
      std::size_t p = 0;
      for (const LaneId other_id : section.lanes) {
        const Lane& other = *network.lane(other_id);
        if ((lane.odr_id > 0) == (other.odr_id > 0) && other.odr_id != 0 &&
            std::abs(other.odr_id) <= std::abs(lane.odr_id)) {
          ++p;
        }
      }
      // Columns of this lane's left and right boundaries in the grid.
      const std::size_t col_left = lane.odr_id > 0 ? center_col - p : center_col + p - 1;
      const std::size_t col_right = col_left + 1;

      // Surface code covering a section-local station: the <material> record
      // with the greatest sOffset <= s (§11.8.2, "valid until the next element
      // starts"). Empty when the lane has no record there — the renderer then
      // uses the lane-type palette. Material boundaries are mandatory stations,
      // so each quad resolves to a single code with an exact edge.
      const auto surface_at = [&lane](double s_local) -> std::string {
        const LaneMaterial* best = nullptr;
        for (const LaneMaterial& material : lane.materials) {
          if (material.s_offset <= s_local + tol::kLength &&
              (best == nullptr || material.s_offset > best->s_offset)) {
            best = &material;
          }
        }
        return best != nullptr ? best->surface.value_or(std::string{}) : std::string{};
      };
      const auto new_patch = [&](std::string surface) {
        RoadMesh::LanePatch patch;
        patch.lane = lane_id;
        patch.odr_lane_id = lane.odr_id;
        patch.material = lane.type;
        patch.surface = std::move(surface);
        return patch;
      };

      RoadMesh::LanePatch patch = new_patch(std::string{});
      bool patch_open = false;
      for (std::uint32_t row = 0; row + 1 < rows.size(); ++row) {
        const double width_here =
            std::abs(row_offsets[row][col_left] - row_offsets[row][col_right]);
        const double width_next =
            std::abs(row_offsets[row + 1][col_left] - row_offsets[row + 1][col_right]);
        if (width_here < tol::kLength && width_next < tol::kLength) {
          continue; // fully pinched-off quad
        }
        // Contiguous quads sharing a record merge into one patch; a change of
        // surface code flushes the current patch and opens a new one.
        std::string surface = surface_at(rows[row] - section.s0);
        if (patch_open && surface != patch.surface) {
          mesh.lanes.push_back(std::move(patch));
          patch = new_patch(std::move(surface));
        } else if (!patch_open) {
          patch.surface = std::move(surface);
          patch_open = true;
        }
        const std::uint32_t a = grid_base + (row * static_cast<std::uint32_t>(columns)) +
                                static_cast<std::uint32_t>(col_left);
        const std::uint32_t b = grid_base + (row * static_cast<std::uint32_t>(columns)) +
                                static_cast<std::uint32_t>(col_right);
        const std::uint32_t c = b + static_cast<std::uint32_t>(columns);
        const std::uint32_t d = a + static_cast<std::uint32_t>(columns);
        patch.indices.insert(patch.indices.end(), {a, b, c, a, c, d});
      }
      if (!patch.indices.empty()) {
        mesh.lanes.push_back(std::move(patch));
      }
    }
  }

  if (options.markings) {
    build_markings(network, road, road_id, stations, mesh);
    build_object_markings(network, road, road_id, mesh);
    build_stopline_markings(network, road, road_id, mesh);
  }
  return mesh;
}

/// One junction's blended 3D surface, keyed by id; empty mesh (no indices)
/// when the junction has no usable connecting-road footprints — callers drop
/// empty results. The pipeline itself lives in junction_surface.cpp.
JunctionFloor build_one_junction_floor(const RoadNetwork& network,
                                       JunctionId junction_id,
                                       const Junction& junction,
                                       const SamplingOptions& sampling) {
  // Sidewalked junctions (issue #357): the floor is split into its carriageway
  // core (kept as `mesh`, Driving) and per-corner sidewalk bands (Sidewalk),
  // which join the authored median-nose overlays in `details`. A rural junction
  // splits to itself, so `mesh` stays byte-identical to the un-split floor.
  const SubMesh floor = build_junction_surface(network, junction, sampling);
  JunctionFloorSplit split = split_junction_floor_sidewalks(network, junction, floor);
  std::vector<SubMesh> details = std::move(split.sidewalk_bands);
  for (SubMesh& overlay : build_junction_corner_details(network, junction, sampling)) {
    details.push_back(std::move(overlay));
  }
  return JunctionFloor{
      .junction = junction_id, .mesh = std::move(split.carriageway), .details = std::move(details)};
}

/// One enclosed-area ground surface, keyed by id; empty mesh (no indices) when
/// the ring encloses no area — callers drop empty results. The pipeline itself
/// lives in surface_fill.cpp.
SurfaceMesh build_one_surface(const RoadNetwork& network,
                              SurfaceId surface_id,
                              const Surface& surface,
                              const SamplingOptions& sampling) {
  SubMesh mesh = build_surface_mesh(network, surface, sampling);
  if (!mesh.indices.empty()) {
    mesh.name = fmt::format("surface {}", surface_id.index);
  }
  return SurfaceMesh{.surface = surface_id, .mesh = std::move(mesh)};
}

bool road_mesh_is_empty(const RoadMesh& mesh) {
  return mesh.lanes.empty() && mesh.markings.empty();
}

} // namespace

NetworkMesh build_network_mesh(const RoadNetwork& network, const MeshOptions& options) {
  NetworkMesh result;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    RoadMesh mesh = build_one_road(network, road_id, road, options);
    if (!road_mesh_is_empty(mesh)) {
      result.roads.push_back(std::move(mesh));
    }
    build_object_instances(network, road, road_id, result.objects);
    build_signal_instances(network, road, road_id, result.signal_instances);
  });
  if (options.junction_floors) {
    network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
      JunctionFloor floor =
          build_one_junction_floor(network, junction_id, junction, options.sampling);
      if (!floor.mesh.indices.empty()) {
        result.junction_floors.push_back(std::move(floor));
      }
    });
  }
  // Enclosed-area ground surfaces: mesh whatever surfaces already exist in the
  // arena (derive_surfaces owns them; meshing stays const on the network).
  network.for_each_surface([&](SurfaceId surface_id, const Surface& surface) {
    SurfaceMesh built = build_one_surface(network, surface_id, surface, options.sampling);
    if (!built.mesh.indices.empty()) {
      result.surfaces.push_back(std::move(built));
    }
  });
  return result;
}

void remesh_roads(const RoadNetwork& network,
                  NetworkMesh& mesh,
                  std::span<const RoadId> roads,
                  const MeshOptions& options) {
  for (const RoadId road_id : roads) {
    const auto existing = std::ranges::find(mesh.roads, road_id, &RoadMesh::road);
    const Road* road = network.road(road_id);
    RoadMesh rebuilt =
        road != nullptr ? build_one_road(network, road_id, *road, options) : RoadMesh{};
    if (road == nullptr || road_mesh_is_empty(rebuilt)) {
      if (existing != mesh.roads.end()) {
        // vector::erase move-assigns the tail entries; their heap buffers
        // are stolen, not copied, so untouched roads keep their data().
        mesh.roads.erase(existing);
      }
      continue;
    }
    if (existing != mesh.roads.end()) {
      *existing = std::move(rebuilt);
    } else {
      mesh.roads.push_back(std::move(rebuilt));
    }
  }
}

void remesh_objects(const RoadNetwork& network,
                    NetworkMesh& mesh,
                    std::span<const RoadId> roads,
                    const MeshOptions& options) {
  for (const RoadId road_id : roads) {
    const auto existing = std::ranges::find(mesh.roads, road_id, &RoadMesh::road);
    const Road* road = network.road(road_id);

    // Instanced-prop layer: drop this road's placed props and rebuild them
    // (an object was added, moved, or removed). Independent of the road's
    // surface mesh, so it runs before the marking-only fast path below.
    std::erase_if(mesh.objects, [&](const ObjectInstance& inst) { return inst.road == road_id; });
    std::erase_if(mesh.signal_instances,
                  [&](const SignalInstance& inst) { return inst.road == road_id; });
    if (road != nullptr) {
      build_object_instances(network, *road, road_id, mesh.objects);
      build_signal_instances(network, *road, road_id, mesh.signal_instances);
    }

    if (existing == mesh.roads.end()) {
      // Road not meshed yet — fall back to a full build so the object markings
      // still land (keeps the entry point total for a fresh network).
      if (road != nullptr) {
        RoadMesh rebuilt = build_one_road(network, road_id, *road, options);
        if (!road_mesh_is_empty(rebuilt)) {
          mesh.roads.push_back(std::move(rebuilt));
        }
      }
      continue;
    }
    // Object-layer re-mesh: regenerate ONLY the marking submeshes (lane marks
    // re-anchor with the object markings), leaving the road surface grid and
    // lane patches untouched — the editor re-uploads only the markings
    // (docs/design/m3a/01 §2.4, first consumer of DirtySet::objects).
    existing->markings.clear();
    if (road != nullptr && options.markings && !road->plan_view.empty() &&
        !road->sections.empty()) {
      const std::vector<double> stations = road_stations(network, *road, options);
      build_markings(network, *road, road_id, stations, *existing);
      build_object_markings(network, *road, road_id, *existing);
      build_stopline_markings(network, *road, road_id, *existing);
    }
  }
}

void remesh_junctions(const RoadNetwork& network,
                      NetworkMesh& mesh,
                      std::span<const JunctionId> junctions,
                      const MeshOptions& options) {
  for (const JunctionId junction_id : junctions) {
    const auto existing =
        std::ranges::find(mesh.junction_floors, junction_id, &JunctionFloor::junction);
    const Junction* junction = options.junction_floors ? network.junction(junction_id) : nullptr;
    JunctionFloor rebuilt =
        junction != nullptr
            ? build_one_junction_floor(network, junction_id, *junction, options.sampling)
            : JunctionFloor{.junction = junction_id, .mesh = {}};
    if (rebuilt.mesh.indices.empty()) {
      if (existing != mesh.junction_floors.end()) {
        mesh.junction_floors.erase(existing);
      }
      continue;
    }
    if (existing != mesh.junction_floors.end()) {
      *existing = std::move(rebuilt);
    } else {
      mesh.junction_floors.push_back(std::move(rebuilt));
    }
  }
}

void remesh_surfaces(const RoadNetwork& network,
                     NetworkMesh& mesh,
                     std::span<const SurfaceId> surfaces,
                     const MeshOptions& options) {
  for (const SurfaceId surface_id : surfaces) {
    const auto existing = std::ranges::find(mesh.surfaces, surface_id, &SurfaceMesh::surface);
    const Surface* surface = network.surface(surface_id);
    SurfaceMesh rebuilt = surface != nullptr
                              ? build_one_surface(network, surface_id, *surface, options.sampling)
                              : SurfaceMesh{.surface = surface_id, .mesh = {}};
    if (rebuilt.mesh.indices.empty()) {
      if (existing != mesh.surfaces.end()) {
        mesh.surfaces.erase(existing);
      }
      continue;
    }
    if (existing != mesh.surfaces.end()) {
      *existing = std::move(rebuilt);
    } else {
      mesh.surfaces.push_back(std::move(rebuilt));
    }
  }
}

} // namespace roadmaker
