#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

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

TEST(ReferenceLine, LineEvaluatesAlongItsHeading) {
  ReferenceLine line;
  line.append(
      GeometryRecord{.x = 1.0, .y = 2.0, .hdg = kPi / 2.0, .length = 10.0, .shape = LineGeom{}});

  const PathPoint mid = line.evaluate(4.0);
  EXPECT_NEAR(mid.x, 1.0, 1e-12);
  EXPECT_NEAR(mid.y, 6.0, 1e-12);
  EXPECT_NEAR(mid.hdg, kPi / 2.0, 1e-12);
  EXPECT_EQ(mid.curvature, 0.0);
}

TEST(ReferenceLine, EvaluateClampsOutOfRangeStations) {
  const ReferenceLine line = straight(10.0);
  EXPECT_NEAR(line.evaluate(-5.0).x, 0.0, 1e-12);
  EXPECT_NEAR(line.evaluate(50.0).x, 10.0, 1e-12);
  EXPECT_EQ(ReferenceLine{}.evaluate(1.0).x, 0.0);
}

TEST(ReferenceLine, ArcTracesAQuarterCircle) {
  // Left turn, radius 10: quarter circle from the origin heading +X.
  const double radius = 10.0;
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = kPi * radius / 2.0, .shape = ArcGeom{.curvature = 1.0 / radius}});

  const PathPoint end = line.evaluate(line.length());
  EXPECT_NEAR(end.x, radius, 1e-9);
  EXPECT_NEAR(end.y, radius, 1e-9);
  EXPECT_NEAR(end.hdg, kPi / 2.0, 1e-9);
  EXPECT_NEAR(end.curvature, 0.1, 1e-12);

  // Midpoint at 45°.
  const PathPoint mid = line.evaluate(line.length() / 2.0);
  EXPECT_NEAR(mid.x, radius * std::sin(kPi / 4.0), 1e-9);
  EXPECT_NEAR(mid.y, radius * (1.0 - std::cos(kPi / 4.0)), 1e-9);
}

TEST(ReferenceLine, RightTurningArcBendsNegative) {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 5.0, .shape = ArcGeom{.curvature = -0.2}});
  const PathPoint end = line.evaluate(5.0);
  EXPECT_LT(end.y, 0.0);
  EXPECT_NEAR(end.hdg, -1.0, 1e-9); // hdg = κ·s = -0.2·5
}

TEST(ReferenceLine, SpiralCurvatureLinearInSAndHeadingIntegratesIt) {
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = 10.0, .shape = SpiralGeom{.curv_start = 0.0, .curv_end = 0.1}});

  EXPECT_NEAR(line.evaluate(0.0).curvature, 0.0, 1e-12);
  EXPECT_NEAR(line.evaluate(5.0).curvature, 0.05, 1e-9);
  EXPECT_NEAR(line.evaluate(10.0).curvature, 0.1, 1e-9);

  // θ(L) = ∫₀ᴸ κ(s) ds = ½(κ₀+κ₁)L = 0.5 rad.
  EXPECT_NEAR(line.evaluate(10.0).hdg, 0.5, 1e-9);
}

TEST(ReferenceLine, SpiralRespectsTheRecordStartPose) {
  ReferenceLine line;
  line.append(GeometryRecord{.x = 3.0,
                             .y = -2.0,
                             .hdg = kPi / 6.0,
                             .length = 8.0,
                             .shape = SpiralGeom{.curv_start = 0.02, .curv_end = 0.06}});
  const PathPoint start = line.evaluate(0.0);
  EXPECT_NEAR(start.x, 3.0, 1e-12);
  EXPECT_NEAR(start.y, -2.0, 1e-12);
  EXPECT_NEAR(start.hdg, kPi / 6.0, 1e-12);
  EXPECT_NEAR(start.curvature, 0.02, 1e-12);
}

TEST(ReferenceLine, ParamPoly3ReproducesAStraightLineAndAKnownCubic) {
  // u = p, v = 0 → straight line (arcLength parameterization).
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = ParamPoly3Geom{.bu = 1.0}});
  const PathPoint straight_end = line.evaluate(10.0);
  EXPECT_NEAR(straight_end.x, 10.0, 1e-12);
  EXPECT_NEAR(straight_end.hdg, 0.0, 1e-12);
  EXPECT_NEAR(straight_end.curvature, 0.0, 1e-12);

  // v = 0.001·p³: heading and curvature from analytic derivatives.
  const double d = 0.001;
  ReferenceLine cubic;
  cubic.append(GeometryRecord{.length = 10.0, .shape = ParamPoly3Geom{.bu = 1.0, .dv = d}});
  const double p = 5.0;
  const PathPoint at = cubic.evaluate(p); // p == local s for u = p (approx exact here)
  EXPECT_NEAR(at.y, d * p * p * p, 1e-12);
  const double slope = 3.0 * d * p * p;
  EXPECT_NEAR(at.hdg, std::atan2(slope, 1.0), 1e-12);
  const double expected_k = (6.0 * d * p) / std::pow(1.0 + (slope * slope), 1.5);
  EXPECT_NEAR(at.curvature, expected_k, 1e-12);
}

TEST(ReferenceLine, NormalizedParamPoly3MapsPToSOverL) {
  // u = L·p over p ∈ [0,1] — identical to a straight line of length L.
  ReferenceLine line;
  line.append(
      GeometryRecord{.length = 20.0, .shape = ParamPoly3Geom{.bu = 20.0, .normalized = true}});
  EXPECT_NEAR(line.evaluate(10.0).x, 10.0, 1e-12);
  EXPECT_NEAR(line.evaluate(20.0).x, 20.0, 1e-12);
}

TEST(ReferenceLine, MultiRecordLinesAreContiguousAcrossJoints) {
  // Straight 10 m, then a left quarter-circle of radius 10 starting where
  // the line ends.
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.x = 10.0,
                             .y = 0.0,
                             .hdg = 0.0,
                             .length = kPi * 10.0 / 2.0,
                             .shape = ArcGeom{.curvature = 0.1}});

  EXPECT_NEAR(line.length(), 10.0 + (kPi * 5.0), 1e-12);

  // Approaching the joint from both sides gives the same pose (G1).
  const PathPoint before = line.evaluate(10.0 - 1e-9);
  const PathPoint after = line.evaluate(10.0 + 1e-9);
  EXPECT_NEAR(before.x, after.x, 1e-6);
  EXPECT_NEAR(before.y, after.y, 1e-6);
  EXPECT_NEAR(before.hdg, after.hdg, 1e-6);

  const PathPoint end = line.evaluate(line.length());
  EXPECT_NEAR(end.x, 20.0, 1e-9);
  EXPECT_NEAR(end.y, 10.0, 1e-9);

  // Records got contiguous stations.
  EXPECT_EQ(line.records()[0].s, 0.0);
  EXPECT_NEAR(line.records()[1].s, 10.0, 1e-12);
}

TEST(ReferenceLine, AppendIgnoresDegenerateRecords) {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 0.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.length = -1.0, .shape = LineGeom{}});
  EXPECT_TRUE(line.empty());
}

TEST(Sampler, StraightLinesUseMaxStep) {
  const ReferenceLine line = straight(20.0);
  const auto stations =
      roadmaker::sample_stations(line, SamplingOptions{.min_step = 0.1, .max_step = 5.0});

  ASSERT_FALSE(stations.empty());
  EXPECT_EQ(stations.front(), 0.0);
  EXPECT_EQ(stations.back(), 20.0);
  EXPECT_LE(stations.size(), 6U); // ~5 m spacing, no curvature refinement
  for (std::size_t i = 1; i < stations.size(); ++i) {
    EXPECT_GT(stations[i], stations[i - 1]);
  }
}

TEST(Sampler, CurvatureTightensTheStep) {
  // R = 10 → step = sqrt(8·0.01·10) ≈ 0.894 m ≪ max_step.
  ReferenceLine arc;
  arc.append(GeometryRecord{.length = kPi * 5.0, .shape = ArcGeom{.curvature = 0.1}});

  const auto stations = roadmaker::sample_stations(arc, SamplingOptions{.chord_tolerance = 0.01});
  const double expected_step = std::sqrt(8.0 * 0.01 / 0.1);
  for (std::size_t i = 1; i < stations.size(); ++i) {
    EXPECT_LE(stations[i] - stations[i - 1], (1.5 * expected_step) + 1e-9);
  }
  // Sanity: dense enough to matter.
  EXPECT_GE(stations.size(), 12U);
}

TEST(Sampler, RecordBoundariesAndExtraStationsAreMandatory) {
  ReferenceLine line;
  line.append(GeometryRecord{.length = 10.0, .shape = LineGeom{}});
  line.append(GeometryRecord{.x = 10.0, .length = 10.0, .shape = ArcGeom{.curvature = 0.05}});

  SamplingOptions options;
  options.extra_stations = {2.5, 17.75, 500.0}; // 500 clamps to length
  const auto stations = roadmaker::sample_stations(line, options);

  auto contains = [&](double s) {
    return std::ranges::any_of(stations, [&](double v) { return std::abs(v - s) < 1e-9; });
  };
  EXPECT_TRUE(contains(0.0));
  EXPECT_TRUE(contains(10.0)); // record joint
  EXPECT_TRUE(contains(2.5));
  EXPECT_TRUE(contains(17.75));
  EXPECT_EQ(stations.back(), 20.0);
}

TEST(Sampler, EmptyLineYieldsNoStations) {
  EXPECT_TRUE(roadmaker::sample_stations(ReferenceLine{}).empty());
}
