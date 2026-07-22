// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/geometry/reference_line.hpp"

#include "roadmaker/tol.hpp"

// Clothoids (and its UtilsLite headers, which embed fmt 11) are an
// implementation detail — they must never leak into RoadMaker headers.
#include <Clothoids.hh>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace roadmaker {

/// One prebuilt evaluator per record; only spiral records carry a curve.
struct SpiralEvalCache {
  std::vector<std::optional<G2lib::ClothoidCurve>> curves;
};

namespace {

/// Local (u, v) frame of a record: u along the start heading, v to its left.
PathPoint
to_inertial(const GeometryRecord& record, double u, double v, double local_hdg, double curvature) {
  const double cos_h = std::cos(record.hdg);
  const double sin_h = std::sin(record.hdg);
  return PathPoint{
      .x = record.x + (u * cos_h) - (v * sin_h),
      .y = record.y + (u * sin_h) + (v * cos_h),
      .hdg = record.hdg + local_hdg,
      .curvature = curvature,
  };
}

PathPoint eval_line(const GeometryRecord& record, double ds) {
  return to_inertial(record, ds, 0.0, 0.0, 0.0);
}

PathPoint eval_arc(const GeometryRecord& record, const ArcGeom& arc, double ds) {
  const double k = arc.curvature;
  if (std::abs(k) < tol::kCurvatureEpsilon) {
    return eval_line(record, ds);
  }
  // Circle of radius 1/k tangent to the start heading at the record origin.
  const double angle = k * ds;
  const double u = std::sin(angle) / k;
  const double v = (1.0 - std::cos(angle)) / k;
  return to_inertial(record, u, v, angle, k);
}

/// Builds a spiral record's evaluator in the record-local frame (origin,
/// zero heading) — to_inertial applies the rotation once at evaluation.
G2lib::ClothoidCurve build_spiral_curve(const GeometryRecord& record, const SpiralGeom& spiral) {
  const double dk = (spiral.curv_end - spiral.curv_start) / record.length;
  G2lib::ClothoidCurve curve("rm_eval");
  curve.build(0.0, 0.0, 0.0, spiral.curv_start, dk, record.length);
  return curve;
}

PathPoint eval_spiral(const GeometryRecord& record, const SpiralGeom& spiral, double ds) {
  const G2lib::ClothoidCurve curve = build_spiral_curve(record, spiral);
  return to_inertial(record, curve.X(ds), curve.Y(ds), curve.theta(ds), curve.kappa(ds));
}

PathPoint eval_param_poly3(const GeometryRecord& record, const ParamPoly3Geom& poly, double ds) {
  const double p = poly.normalized ? ds / record.length : ds;
  const double u = poly.au + (p * (poly.bu + (p * (poly.cu + (p * poly.du)))));
  const double v = poly.av + (p * (poly.bv + (p * (poly.cv + (p * poly.dv)))));
  const double du1 = poly.bu + (p * ((2.0 * poly.cu) + (p * 3.0 * poly.du)));
  const double dv1 = poly.bv + (p * ((2.0 * poly.cv) + (p * 3.0 * poly.dv)));
  const double du2 = (2.0 * poly.cu) + (6.0 * poly.du * p);
  const double dv2 = (2.0 * poly.cv) + (6.0 * poly.dv * p);

  const double local_hdg = std::atan2(dv1, du1);
  // Parametric curvature — invariant under the s vs. normalized-p scaling.
  const double speed_sq = (du1 * du1) + (dv1 * dv1);
  const double denominator = speed_sq * std::sqrt(speed_sq);
  const double curvature =
      denominator < tol::kCurvatureEpsilon ? 0.0 : ((du1 * dv2) - (dv1 * du2)) / denominator;
  return to_inertial(record, u, v, local_hdg, curvature);
}

PathPoint eval_record(const GeometryRecord& record, double ds) {
  return std::visit(
      [&](const auto& shape) -> PathPoint {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, LineGeom>) {
          return eval_line(record, ds);
        } else if constexpr (std::is_same_v<Shape, ArcGeom>) {
          return eval_arc(record, shape, ds);
        } else if constexpr (std::is_same_v<Shape, SpiralGeom>) {
          return eval_spiral(record, shape, ds);
        } else {
          return eval_param_poly3(record, shape, ds);
        }
      },
      record.shape);
}

} // namespace

void ReferenceLine::append(GeometryRecord record) {
  if (record.length <= 0.0) {
    return;
  }
  record.s = length_;
  length_ += record.length;
  records_.push_back(record);
  spiral_cache_.reset(); // derived from records_; rebuilt lazily
}

const SpiralEvalCache& ReferenceLine::spiral_cache() const {
  if (spiral_cache_ == nullptr) {
    auto cache = std::make_shared<SpiralEvalCache>();
    cache->curves.resize(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i) {
      if (const auto* spiral = std::get_if<SpiralGeom>(&records_[i].shape)) {
        cache->curves[i].emplace(build_spiral_curve(records_[i], *spiral));
      }
    }
    spiral_cache_ = std::move(cache);
  }
  return *spiral_cache_;
}

PathPoint ReferenceLine::evaluate(double s) const {
  if (records_.empty()) {
    return {};
  }
  const double clamped = std::clamp(s, 0.0, length_);
  // Last record whose start is <= s.
  auto it = std::ranges::upper_bound(records_, clamped, {}, &GeometryRecord::s);
  if (it != records_.begin()) {
    --it;
  }
  const double ds = clamped - it->s;
  if (std::holds_alternative<SpiralGeom>(it->shape)) {
    // Hot path during meshing/drags: reuse the prebuilt Clothoids state
    // instead of rebuilding Fresnel setup per call.
    const std::size_t index = static_cast<std::size_t>(it - records_.begin());
    const G2lib::ClothoidCurve& curve = *spiral_cache().curves[index];
    return to_inertial(*it, curve.X(ds), curve.Y(ds), curve.theta(ds), curve.kappa(ds));
  }
  return eval_record(*it, ds);
}

std::vector<double> sample_stations(const ReferenceLine& line, const SamplingOptions& options) {
  if (line.empty()) {
    return {};
  }
  const double total = line.length();

  std::vector<double> mandatory{0.0, total};
  for (const GeometryRecord& record : line.records()) {
    mandatory.push_back(record.s);
  }
  for (const double s : options.extra_stations) {
    mandatory.push_back(std::clamp(s, 0.0, total));
  }
  std::ranges::sort(mandatory);
  const auto duplicates =
      std::ranges::unique(mandatory, [](double a, double b) { return b - a < tol::kLength; });
  mandatory.erase(duplicates.begin(), duplicates.end());

  std::vector<double> stations;
  for (std::size_t i = 0; i + 1 < mandatory.size(); ++i) {
    const double begin = mandatory[i];
    const double end = mandatory[i + 1];
    stations.push_back(begin);
    double cursor = begin;
    while (true) {
      const double k = std::abs(line.evaluate(cursor).curvature);
      double step = options.max_step;
      if (k > tol::kCurvatureEpsilon) {
        step = std::clamp(
            std::sqrt(8.0 * options.chord_tolerance / k), options.min_step, options.max_step);
      }
      // Stop when the remainder fits in ~1.5 steps: emitting `end` (a
      // mandatory station) then keeps every gap <= 1.5·step, without
      // creating a sliver segment shorter than step/2.
      if (end - cursor <= 1.5 * step) {
        break;
      }
      cursor += step;
      stations.push_back(cursor);
    }
  }
  stations.push_back(total);
  return stations;
}

} // namespace roadmaker
