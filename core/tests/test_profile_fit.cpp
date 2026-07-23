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

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/profile_fit.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

using roadmaker::eval_profile;
using roadmaker::fit_elevation_profile;
using roadmaker::Poly3;

namespace {

// Every fitted profile reproduces the node heights exactly at the node
// stations and comes out ascending in s
// (asam.net:xodr:1.4.0:road.elevation.elem_asc_order).
void expect_interpolates(const std::vector<Poly3>& profile,
                         const std::vector<double>& s,
                         const std::vector<double>& z) {
  ASSERT_EQ(profile.size(), s.size() - 1);
  for (std::size_t i = 0; i < s.size(); ++i) {
    EXPECT_NEAR(eval_profile(profile, s[i]), z[i], roadmaker::tol::kLength) << "node " << i;
  }
  EXPECT_TRUE(
      std::ranges::is_sorted(profile, [](const Poly3& a, const Poly3& b) { return a.s < b.s; }));
}

} // namespace

// A flat run carries no grade: every coefficient is zero.
TEST(FitElevationProfile, FlatIsAllZero) {
  const std::vector<double> s{0.0, 50.0, 100.0};
  const std::vector<double> z{0.0, 0.0, 0.0};
  const std::vector<Poly3> profile = fit_elevation_profile(s, z);
  ASSERT_EQ(profile.size(), 2U);
  for (const Poly3& p : profile) {
    EXPECT_DOUBLE_EQ(p.a, 0.0);
    EXPECT_DOUBLE_EQ(p.b, 0.0);
    EXPECT_DOUBLE_EQ(p.c, 0.0);
    EXPECT_DOUBLE_EQ(p.d, 0.0);
  }
  expect_interpolates(profile, s, z);
}

// A constant-grade ramp fits exactly as straight cubics (c = d = 0): the
// finite-difference tangents all equal the grade, so no curvature is
// introduced.
TEST(FitElevationProfile, ConstantGradeRampIsLinear) {
  const std::vector<double> s{0.0, 50.0, 100.0};
  const std::vector<double> z{0.0, 5.0, 10.0}; // grade 0.1
  const std::vector<Poly3> profile = fit_elevation_profile(s, z);
  ASSERT_EQ(profile.size(), 2U);
  EXPECT_DOUBLE_EQ(profile[0].s, 0.0);
  EXPECT_DOUBLE_EQ(profile[0].a, 0.0);
  EXPECT_DOUBLE_EQ(profile[0].b, 0.1);
  EXPECT_NEAR(profile[0].c, 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(profile[0].d, 0.0, roadmaker::tol::kLength);
  EXPECT_DOUBLE_EQ(profile[1].s, 50.0);
  EXPECT_DOUBLE_EQ(profile[1].b, 0.1);
  expect_interpolates(profile, s, z);
}

// A symmetric crest (0 → 10 → 0 over 0/50/100 m) fits to the hand-computed
// Hermite coefficients and meets C1 at the interior node (slope 0 there).
TEST(FitElevationProfile, SymmetricCrestGoldenCoefficients) {
  const std::vector<double> s{0.0, 50.0, 100.0};
  const std::vector<double> z{0.0, 10.0, 0.0};
  const std::vector<Poly3> profile = fit_elevation_profile(s, z);
  ASSERT_EQ(profile.size(), 2U);

  // Golden values: tangents m = {0.2, 0.0, -0.2} (finite differences).
  const std::array<Poly3, 2> expected{
      Poly3{.s = 0.0, .a = 0.0, .b = 0.2, .c = 0.004, .d = -0.00008},
      Poly3{.s = 50.0, .a = 10.0, .b = 0.0, .c = -0.008, .d = 0.00008},
  };
  for (std::size_t i = 0; i < profile.size(); ++i) {
    EXPECT_NEAR(profile[i].s, expected[i].s, roadmaker::tol::kLength) << "record " << i;
    EXPECT_NEAR(profile[i].a, expected[i].a, roadmaker::tol::kLength) << "record " << i;
    EXPECT_NEAR(profile[i].b, expected[i].b, roadmaker::tol::kLength) << "record " << i;
    EXPECT_NEAR(profile[i].c, expected[i].c, roadmaker::tol::kLength) << "record " << i;
    EXPECT_NEAR(profile[i].d, expected[i].d, roadmaker::tol::kLength) << "record " << i;
  }
  expect_interpolates(profile, s, z);

  // C1: the two pieces agree in value and slope at the shared node (s = 50).
  EXPECT_NEAR(profile[0].eval(50.0), profile[1].eval(50.0), roadmaker::tol::kLength);
  EXPECT_NEAR(
      profile[0].eval_derivative(50.0), profile[1].eval_derivative(50.0), roadmaker::tol::kLength);
}

// C1 continuity holds across every interior node of an irregular profile with
// uneven station spacing.
TEST(FitElevationProfile, C1ContinuousAtEveryInteriorNode) {
  const std::vector<double> s{0.0, 20.0, 75.0, 130.0, 160.0};
  const std::vector<double> z{1.0, 3.5, -2.0, 4.0, 4.0};
  const std::vector<Poly3> profile = fit_elevation_profile(s, z);
  ASSERT_EQ(profile.size(), 4U);
  expect_interpolates(profile, s, z);
  for (std::size_t i = 1; i + 1 < s.size(); ++i) {
    EXPECT_NEAR(profile[i - 1].eval(s[i]), profile[i].eval(s[i]), roadmaker::tol::kLength)
        << "value at node " << i;
    EXPECT_NEAR(profile[i - 1].eval_derivative(s[i]),
                profile[i].eval_derivative(s[i]),
                roadmaker::tol::kLength)
        << "slope at node " << i;
  }
}

TEST(FitElevationProfile, DegenerateInputs) {
  EXPECT_TRUE(fit_elevation_profile({}, {}).empty());

  const std::array<double, 1> one_s{7.0};
  const std::array<double, 1> one_z{2.5};
  const std::vector<Poly3> single = fit_elevation_profile(one_s, one_z);
  ASSERT_EQ(single.size(), 1U);
  EXPECT_DOUBLE_EQ(single[0].s, 7.0);
  EXPECT_DOUBLE_EQ(single[0].a, 2.5);

  // Size mismatch and non-ascending stations are rejected (empty result).
  const std::array<double, 2> two_s{0.0, 10.0};
  const std::array<double, 3> three_z{0.0, 1.0, 2.0};
  EXPECT_TRUE(fit_elevation_profile(two_s, three_z).empty());
  const std::array<double, 3> non_asc{0.0, 10.0, 10.0};
  const std::array<double, 3> any_z{0.0, 1.0, 2.0};
  EXPECT_TRUE(fit_elevation_profile(non_asc, any_z).empty());
}
