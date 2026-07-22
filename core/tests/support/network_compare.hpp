// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace roadmaker::test {

/// Compares two roads' plan-view geometry by dense sampling. Non-fatal
/// expectations so a failure pinpoints every diverging station.
inline void expect_same_geometry(const Road& a, const Road& b) {
  EXPECT_NEAR(a.plan_view.length(), b.plan_view.length(), tol::kRoundTripPosition);
  const double length = a.plan_view.length();
  constexpr int kSamples = 200;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = length * i / kSamples;
    SCOPED_TRACE("s=" + std::to_string(s));
    const auto pa = a.plan_view.evaluate(s);
    const auto pb = b.plan_view.evaluate(s);
    EXPECT_NEAR(pa.x, pb.x, tol::kRoundTripPosition);
    EXPECT_NEAR(pa.y, pb.y, tol::kRoundTripPosition);
    EXPECT_NEAR(
        std::remainder(pa.hdg - pb.hdg, 2.0 * std::numbers::pi), 0.0, tol::kRoundTripHeading);
  }
}

/// The command round-trip oracle (docs/m2/01_editing_framework.md §1.1):
/// write_xodr is deterministic, so two networks are equal iff their
/// serialized documents are byte-identical. Both must be writable.
inline void expect_networks_equal(const RoadNetwork& a, const RoadNetwork& b) {
  const auto text_a = write_xodr(a, "compare");
  const auto text_b = write_xodr(b, "compare");
  ASSERT_TRUE(text_a.has_value()) << text_a.error().message;
  ASSERT_TRUE(text_b.has_value()) << text_b.error().message;
  EXPECT_EQ(*text_a, *text_b);
}

/// Byte-compares a network against a serialization captured earlier (the
/// apply→revert form of the oracle, where "before" no longer exists).
inline void expect_network_matches(const RoadNetwork& network, const std::string& expected_xml) {
  const auto text = write_xodr(network, "compare");
  ASSERT_TRUE(text.has_value()) << text.error().message;
  EXPECT_EQ(*text, expected_xml);
}

/// Serializes for later expect_network_matches; setup failure throws (GTest
/// reports uncaught exceptions as test failures).
inline std::string snapshot_xodr(const RoadNetwork& network) {
  auto text = write_xodr(network, "compare");
  if (!text.has_value()) {
    throw std::runtime_error("snapshot_xodr: " + text.error().message);
  }
  return std::move(*text);
}

} // namespace roadmaker::test
