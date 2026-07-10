#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using roadmaker::ArcGeom;
using roadmaker::GeometryRecord;
using roadmaker::LineGeom;
using roadmaker::ParamPoly3Geom;
using roadmaker::PathPoint;
using roadmaker::ReferenceLine;
using roadmaker::SamplingOptions;
using roadmaker::SpiralGeom;

namespace {

constexpr double kPi = std::numbers::pi;

ReferenceLine straight(double length, double hdg = 0.0) {
  ReferenceLine line;
  line.append(GeometryRecord{.hdg = hdg, .length = length, .shape = LineGeom{}});
  return line;
}

} // namespace

TEST_CASE("line evaluates along its heading", "[geometry]") {
  ReferenceLine line;
  line.append(
      GeometryRecord{.x = 1.0, .y = 2.0, .hdg = kPi / 2.0, .length = 10.0, .shape = LineGeom{}});

  const PathPoint mid = line.evaluate(4.0);
  REQUIRE_THAT(mid.x, WithinAbs(1.0, 1e-12));
  REQUIRE_THAT(mid.y, WithinAbs(6.0, 1e-12));
  REQUIRE_THAT(mid.hdg, WithinAbs(kPi / 2.0, 1e-12));
  REQUIRE(mid.curvature == 0.0);
}

TEST_CASE("evaluate clamps out-of-range stations", "[geometry]") {
  const ReferenceLine line = straight(10.0);
  REQUIRE_THAT(line.evaluate(-5.0).x, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(line.evaluate(50.0).x, WithinAbs(10.0, 1e-12));
  REQUIRE(ReferenceLine{}.evaluate(1.0).x == 0.0);
}

TEST_CASE("arc traces a quarter circle", "[geometry]") {
  // Left turn, radius 10: quarter circle from the origin heading +X.
  const double radius = 10.0;
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = kPi * radius / 2.0, .shape = ArcGeom{.curvature = 1.0 / radius}});

  const PathPoint end = line.evaluate(line.length());
  REQUIRE_THAT(end.x, WithinAbs(radius, 1e-9));
  REQUIRE_THAT(end.y, WithinAbs(radius, 1e-9));
  REQUIRE_THAT(end.hdg, WithinAbs(kPi / 2.0, 1e-9));
  REQUIRE_THAT(end.curvature, WithinAbs(0.1, 1e-12));

  // Midpoint at 45°.
  const PathPoint mid = line.evaluate(line.length() / 2.0);
  REQUIRE_THAT(mid.x, WithinAbs(radius * std::sin(kPi / 4.0), 1e-9));
  REQUIRE_THAT(mid.y, WithinAbs(radius * (1.0 - std::cos(kPi / 4.0)), 1e-9));
}

TEST_CASE("right-turning arc bends negative", "[geometry]") {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 5.0, .shape = ArcGeom{.curvature = -0.2}});
  const PathPoint end = line.evaluate(5.0);
  REQUIRE(end.y < 0.0);
  REQUIRE_THAT(end.hdg, WithinAbs(-1.0, 1e-9)); // hdg = κ·s = -0.2·5
}

TEST_CASE("spiral curvature is linear in s and heading integrates it", "[geometry]") {
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = 10.0, .shape = SpiralGeom{.curv_start = 0.0, .curv_end = 0.1}});

  REQUIRE_THAT(line.evaluate(0.0).curvature, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(line.evaluate(5.0).curvature, WithinAbs(0.05, 1e-9));
  REQUIRE_THAT(line.evaluate(10.0).curvature, WithinAbs(0.1, 1e-9));

  // θ(L) = ∫₀ᴸ κ(s) ds = ½(κ₀+κ₁)L = 0.5 rad.
  REQUIRE_THAT(line.evaluate(10.0).hdg, WithinAbs(0.5, 1e-9));
}

TEST_CASE("spiral respects the record start pose", "[geometry]") {
  ReferenceLine line;
  line.append(GeometryRecord{.x = 3.0,
                             .y = -2.0,
                             .hdg = kPi / 6.0,
                             .length = 8.0,
                             .shape = SpiralGeom{.curv_start = 0.02, .curv_end = 0.06}});
  const PathPoint start = line.evaluate(0.0);
  REQUIRE_THAT(start.x, WithinAbs(3.0, 1e-12));
  REQUIRE_THAT(start.y, WithinAbs(-2.0, 1e-12));
  REQUIRE_THAT(start.hdg, WithinAbs(kPi / 6.0, 1e-12));
  REQUIRE_THAT(start.curvature, WithinAbs(0.02, 1e-12));
}

TEST_CASE("paramPoly3 reproduces a straight line and a known cubic", "[geometry]") {
  // u = p, v = 0 → straight line (arcLength parameterization).
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = ParamPoly3Geom{.bu = 1.0}});
  const PathPoint straight_end = line.evaluate(10.0);
  REQUIRE_THAT(straight_end.x, WithinAbs(10.0, 1e-12));
  REQUIRE_THAT(straight_end.hdg, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(straight_end.curvature, WithinAbs(0.0, 1e-12));

  // v = 0.001·p³: heading and curvature from analytic derivatives.
  const double d = 0.001;
  ReferenceLine cubic;
  cubic.append(GeometryRecord{.length = 10.0, .shape = ParamPoly3Geom{.bu = 1.0, .dv = d}});
  const double p = 5.0;
  const PathPoint at = cubic.evaluate(p); // p == local s for u = p (approx exact here)
  REQUIRE_THAT(at.y, WithinAbs(d * p * p * p, 1e-12));
  const double slope = 3.0 * d * p * p;
  REQUIRE_THAT(at.hdg, WithinAbs(std::atan2(slope, 1.0), 1e-12));
  const double expected_k = (6.0 * d * p) / std::pow(1.0 + slope * slope, 1.5);
  REQUIRE_THAT(at.curvature, WithinAbs(expected_k, 1e-12));
}

TEST_CASE("normalized paramPoly3 maps p to s/L", "[geometry]") {
  // u = L·p over p ∈ [0,1] — identical to a straight line of length L.
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = 20.0, .shape = ParamPoly3Geom{.bu = 20.0, .normalized = true}});
  REQUIRE_THAT(line.evaluate(10.0).x, WithinAbs(10.0, 1e-12));
  REQUIRE_THAT(line.evaluate(20.0).x, WithinAbs(20.0, 1e-12));
}

TEST_CASE("multi-record lines are contiguous across joints", "[geometry]") {
  // Straight 10 m, then a left quarter-circle of radius 10 starting where
  // the line ends.
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.x = 10.0,
                             .y = 0.0,
                             .hdg = 0.0,
                             .length = kPi * 10.0 / 2.0,
                             .shape = ArcGeom{.curvature = 0.1}});

  REQUIRE_THAT(line.length(), WithinAbs(10.0 + (kPi * 5.0), 1e-12));

  // Approaching the joint from both sides gives the same pose (G1).
  const PathPoint before = line.evaluate(10.0 - 1e-9);
  const PathPoint after = line.evaluate(10.0 + 1e-9);
  REQUIRE_THAT(before.x, WithinAbs(after.x, 1e-6));
  REQUIRE_THAT(before.y, WithinAbs(after.y, 1e-6));
  REQUIRE_THAT(before.hdg, WithinAbs(after.hdg, 1e-6));

  const PathPoint end = line.evaluate(line.length());
  REQUIRE_THAT(end.x, WithinAbs(20.0, 1e-9));
  REQUIRE_THAT(end.y, WithinAbs(10.0, 1e-9));

  // Records got contiguous stations.
  REQUIRE(line.records()[0].s == 0.0);
  REQUIRE_THAT(line.records()[1].s, WithinAbs(10.0, 1e-12));
}

TEST_CASE("append ignores degenerate records", "[geometry]") {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 0.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.length = -1.0, .shape = LineGeom{}});
  REQUIRE(line.empty());
}

TEST_CASE("sampler: straight lines use max_step", "[sampler]") {
  const ReferenceLine line = straight(20.0);
  const auto stations =
      roadmaker::sample_stations(line, SamplingOptions{.min_step = 0.1, .max_step = 5.0});

  REQUIRE(stations.front() == 0.0);
  REQUIRE(stations.back() == 20.0);
  REQUIRE(stations.size() <= 6); // ~5 m spacing, no curvature refinement
  for (std::size_t i = 1; i < stations.size(); ++i) {
    REQUIRE(stations[i] > stations[i - 1]);
  }
}

TEST_CASE("sampler: curvature tightens the step", "[sampler]") {
  // R = 10 → step = sqrt(8·0.01·10) ≈ 0.894 m ≪ max_step.
  ReferenceLine arc;
  arc.append(GeometryRecord{.length = kPi * 5.0, .shape = ArcGeom{.curvature = 0.1}});

  const auto stations = roadmaker::sample_stations(arc, SamplingOptions{.chord_tolerance = 0.01});
  const double expected_step = std::sqrt(8.0 * 0.01 / 0.1);
  for (std::size_t i = 1; i < stations.size(); ++i) {
    REQUIRE(stations[i] - stations[i - 1] <= 1.5 * expected_step + 1e-9);
  }
  // Sanity: dense enough to matter.
  REQUIRE(stations.size() >= 12);
}

TEST_CASE("sampler: record boundaries and extra stations are mandatory", "[sampler]") {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.x = 10.0, .length = 10.0, .shape = ArcGeom{.curvature = 0.05}});

  SamplingOptions options;
  options.extra_stations = {2.5, 17.75, 500.0}; // 500 clamps to length
  const auto stations = roadmaker::sample_stations(line, options);

  auto contains = [&](double s) {
    return std::ranges::any_of(stations, [&](double v) { return std::abs(v - s) < 1e-9; });
  };
  REQUIRE(contains(0.0));
  REQUIRE(contains(10.0)); // record joint
  REQUIRE(contains(2.5));
  REQUIRE(contains(17.75));
  REQUIRE(stations.back() == 20.0);
}

TEST_CASE("sampler: empty line yields no stations", "[sampler]") {
  REQUIRE(roadmaker::sample_stations(ReferenceLine{}).empty());
}
