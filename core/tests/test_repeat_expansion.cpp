// Tests for expand_repeat — the §13.4 <repeat> section expander that turns one
// ObjectRepeat into the discrete instances it places. Semantics under test:
// round-down count, inclusive exact-fit endpoint, linear vs cubic t, linear
// z_offset, and the continuous (distance == 0) no-instance case.

#include "roadmaker/road/object.hpp"
#include "roadmaker/road/repeat_expansion.hpp"

#include <gtest/gtest.h>

namespace roadmaker {
namespace {

TEST(RepeatExpansion, CountRoundsDown) {
  // §13.4: "as many time as ... @distance and @length allow, rounded down."
  // floor(110/15) = 7 -> 8 instances (ds = 0, 15, ... 105); the 110 m end has
  // no incomplete instance.
  ObjectRepeat repeat;
  repeat.s = 5.0;
  repeat.length = 110.0;
  repeat.distance = 15.0;
  repeat.t_start = repeat.t_end = 8.0;

  const std::vector<RepeatInstance> instances = expand_repeat(repeat);
  ASSERT_EQ(instances.size(), 8U);
  EXPECT_DOUBLE_EQ(instances.front().s, 5.0);
  EXPECT_DOUBLE_EQ(instances.back().s, 5.0 + 105.0); // last origin inside the section
}

TEST(RepeatExpansion, ExactFitKeepsEndpointInstance) {
  // length == k*distance: the origin at ds=length is "inside that section
  // (including its border)" and must survive float rounding via tol::kLength.
  ObjectRepeat repeat;
  repeat.s = 0.0;
  repeat.length = 30.0;
  repeat.distance = 10.0;

  const std::vector<RepeatInstance> instances = expand_repeat(repeat);
  ASSERT_EQ(instances.size(), 4U); // ds = 0, 10, 20, 30
  EXPECT_DOUBLE_EQ(instances.back().s, 30.0);
}

TEST(RepeatExpansion, LinearTAndZOffsetInterpolate) {
  // No cubic coefficients -> t is a linear tStart->tEnd lerp; z_offset is always
  // a linear lerp.
  ObjectRepeat repeat;
  repeat.s = 0.0;
  repeat.length = 20.0;
  repeat.distance = 10.0;
  repeat.t_start = 2.0;
  repeat.t_end = 6.0;
  repeat.z_offset_start = 1.0;
  repeat.z_offset_end = 5.0;

  const std::vector<RepeatInstance> instances = expand_repeat(repeat);
  ASSERT_EQ(instances.size(), 3U); // ds = 0, 10, 20
  EXPECT_DOUBLE_EQ(instances[0].t, 2.0);
  EXPECT_DOUBLE_EQ(instances[1].t, 4.0); // halfway
  EXPECT_DOUBLE_EQ(instances[2].t, 6.0);
  EXPECT_DOUBLE_EQ(instances[0].z_offset, 1.0);
  EXPECT_DOUBLE_EQ(instances[1].z_offset, 3.0);
  EXPECT_DOUBLE_EQ(instances[2].z_offset, 5.0);
}

TEST(RepeatExpansion, CubicTCoefficientsOverrideLinear) {
  // §13.4: any of bT/cT/dT present selects the cubic t(ds) = tStart + bT*ds +
  // cT*ds^2 + dT*ds^3; tEnd is then ignored. 1.8.1 §13.2 has no such branch.
  ObjectRepeat repeat;
  repeat.s = 0.0;
  repeat.length = 20.0;
  repeat.distance = 10.0;
  repeat.t_start = 1.0;
  repeat.t_end = 99.0; // ignored under the cubic
  repeat.c_t = 0.25;   // only cT set -> bT, dT treated as 0

  const std::vector<RepeatInstance> instances = expand_repeat(repeat);
  ASSERT_EQ(instances.size(), 3U);
  EXPECT_DOUBLE_EQ(instances[0].t, 1.0);                // 1 + 0.25*0
  EXPECT_DOUBLE_EQ(instances[1].t, 1.0 + 0.25 * 100.0); // ds=10 -> 1 + 25
  EXPECT_DOUBLE_EQ(instances[2].t, 1.0 + 0.25 * 400.0); // ds=20 -> 1 + 100
}

TEST(RepeatExpansion, ContinuousDistanceZeroYieldsNoInstances) {
  // §13.4: @distance == 0 extrudes one continuous shape, not a series.
  ObjectRepeat repeat;
  repeat.s = 0.0;
  repeat.length = 50.0;
  repeat.distance = 0.0;

  EXPECT_TRUE(expand_repeat(repeat).empty());
}

} // namespace
} // namespace roadmaker
