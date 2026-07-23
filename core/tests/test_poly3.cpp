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

// Direct unit for roadmaker/geometry/poly3.hpp (audit 2026-07 gap B5): the
// OpenDRIVE cubic record value(s) = a + b·ds + c·ds² + d·ds³ with ds = s − s₀
// (§8.4 elevation / §10 lateral profile records all share this shape), and the
// piecewise-profile lookup whose record selection is "last record whose start
// is <= s" — an upper_bound with a begin() clamp whose boundary behavior every
// elevation and width consumer inherits.

#include "roadmaker/geometry/poly3.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>

using roadmaker::eval_profile;
using roadmaker::eval_profile_derivative;
using roadmaker::Poly3;

namespace {

/// One record with every coefficient live and a non-zero start, so a wrong
/// ds = s_query - s shows up in every term: value(2 + h) = 1 + 2h + 3h² + 4h³.
constexpr Poly3 kCubic{.s = 2.0, .a = 1.0, .b = 2.0, .c = 3.0, .d = 4.0};

// The API is constexpr end to end; pin that contract at compile time.
static_assert(kCubic.eval(2.0) == 1.0);
static_assert(kCubic.eval(3.0) == 10.0);
static_assert(kCubic.eval_derivative(3.0) == 20.0);

/// Three-record profile with distinct slopes, sorted ascending by s as the
/// contract requires.
constexpr std::array<Poly3, 3> kProfile{
    Poly3{.s = 0.0, .a = 1.0, .b = 0.1},
    Poly3{.s = 10.0, .a = 2.0, .b = 0.5},
    Poly3{.s = 20.0, .a = 3.0, .b = -1.0},
};

} // namespace

// --- Poly3::eval / eval_derivative ------------------------------------------

TEST(Poly3, EvalAtRecordOriginIsA) {
  EXPECT_DOUBLE_EQ(kCubic.eval(2.0), 1.0);
}

TEST(Poly3, EvalAtOffsetMatchesHandComputedCubic) {
  // ds = 1: 1 + 2 + 3 + 4 = 10; ds = 0.5: 1 + 1 + 0.75 + 0.5 = 3.25.
  EXPECT_DOUBLE_EQ(kCubic.eval(3.0), 10.0);
  EXPECT_DOUBLE_EQ(kCubic.eval(2.5), 3.25);
}

TEST(Poly3, EvalDerivativeAtRecordOriginIsB) {
  EXPECT_DOUBLE_EQ(kCubic.eval_derivative(2.0), 2.0);
}

TEST(Poly3, EvalDerivativeAtOffsetMatchesHandComputedSlope) {
  // b + 2c·ds + 3d·ds²; ds = 1: 2 + 6 + 12 = 20; ds = 0.5: 2 + 3 + 3 = 8.
  EXPECT_DOUBLE_EQ(kCubic.eval_derivative(3.0), 20.0);
  EXPECT_DOUBLE_EQ(kCubic.eval_derivative(2.5), 8.0);
}

// --- eval_profile edges -------------------------------------------------------

TEST(Poly3, EvalProfileEmptyIsZero) {
  EXPECT_DOUBLE_EQ(eval_profile({}, 5.0), 0.0);
}

TEST(Poly3, EvalProfileBeforeFirstRecordEvaluatesTheFirstRecord) {
  // The documented clamp: a query before the first record extrapolates the
  // first record backwards, it does not return 0.
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, -5.0), 1.0 + (0.1 * -5.0));
}

TEST(Poly3, EvalProfileExactlyAtARecordStartSelectsThatRecord) {
  // upper_bound(10.0) points past the s=10 record, so the decrement lands ON
  // it — a query exactly at a record start belongs to the record it starts.
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 10.0), 2.0);
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 20.0), 3.0);
}

TEST(Poly3, EvalProfileJustBeforeARecordStartStaysOnThePriorRecord) {
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 9.5), 1.0 + (0.1 * 9.5));
}

TEST(Poly3, EvalProfileJustAfterARecordStartUsesTheNewRecord) {
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 10.5), 2.0 + (0.5 * 0.5));
}

TEST(Poly3, EvalProfilePastTheLastRecordExtrapolatesTheLastRecord) {
  EXPECT_DOUBLE_EQ(eval_profile(kProfile, 25.0), 3.0 + (-1.0 * 5.0));
}

// --- eval_profile_derivative analogues ---------------------------------------

TEST(Poly3, EvalProfileDerivativeEmptyIsZero) {
  EXPECT_DOUBLE_EQ(eval_profile_derivative({}, 5.0), 0.0);
}

TEST(Poly3, EvalProfileDerivativeBeforeFirstRecordUsesTheFirstRecord) {
  EXPECT_DOUBLE_EQ(eval_profile_derivative(kProfile, -5.0), 0.1);
}

TEST(Poly3, EvalProfileDerivativeSelectsTheSameRecordAsEvalProfile) {
  EXPECT_DOUBLE_EQ(eval_profile_derivative(kProfile, 9.5), 0.1);  // just before
  EXPECT_DOUBLE_EQ(eval_profile_derivative(kProfile, 10.0), 0.5); // exactly at
  EXPECT_DOUBLE_EQ(eval_profile_derivative(kProfile, 10.5), 0.5); // just after
  EXPECT_DOUBLE_EQ(eval_profile_derivative(kProfile, 25.0), -1.0);
}
