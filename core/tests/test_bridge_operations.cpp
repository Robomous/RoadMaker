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

// Phase 5 (WP5) — the bridge commands. Unlike the terrain command (whose
// round-trip oracle is vacuous because write_xodr carries only a sidecar
// reference), the <bridge> SPAN serializes, so the standard apply->revert
// byte-identical oracle is meaningful and used directly here.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace roadmaker {
namespace {

using edit::author_bridge;
using edit::remove_bridge;
using edit::set_bridge_span;

RoadId make_road(RoadNetwork& network) {
  const std::array<Waypoint, 2> waypoints{Waypoint{0, 0}, Waypoint{120, 0}};
  return author_clothoid_road(network, waypoints, LaneProfile::two_lane_rural(), "", "r").value();
}

const std::vector<Bridge>& bridges(RoadNetwork& network, RoadId road) {
  return network.road(road)->bridges;
}

TEST(BridgeOperations, AuthorAddsASpanWithAGeneratedId) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  ASSERT_TRUE(author_bridge(network, road, 40.0, 24.0)->apply(network).has_value());
  ASSERT_EQ(bridges(network, road).size(), 1U);
  EXPECT_EQ(bridges(network, road)[0].odr_id, "bridge1");
  EXPECT_DOUBLE_EQ(bridges(network, road)[0].s, 40.0);
  EXPECT_DOUBLE_EQ(bridges(network, road)[0].length, 24.0);
  EXPECT_EQ(bridges(network, road)[0].type, "concrete");
}

TEST(BridgeOperations, AuthorRejectsShortPastEndAndDuplicateSpans) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  EXPECT_FALSE(author_bridge(network, road, 40.0, 0.5)->apply(network).has_value());   // too short
  EXPECT_FALSE(author_bridge(network, road, 110.0, 40.0)->apply(network).has_value()); // past end
  ASSERT_TRUE(author_bridge(network, road, 40.0, 24.0)->apply(network).has_value());
  EXPECT_FALSE(author_bridge(network, road, 40.0, 24.0)->apply(network).has_value()); // duplicate
}

TEST(BridgeOperations, RemoveDropsTheNamedSpan) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  ASSERT_TRUE(author_bridge(network, road, 10.0, 20.0)->apply(network).has_value());
  ASSERT_TRUE(author_bridge(network, road, 50.0, 20.0)->apply(network).has_value());
  ASSERT_TRUE(remove_bridge(network, road, 0)->apply(network).has_value());
  ASSERT_EQ(bridges(network, road).size(), 1U);
  EXPECT_DOUBLE_EQ(bridges(network, road)[0].s, 50.0);
  EXPECT_FALSE(remove_bridge(network, road, 5)->apply(network).has_value()); // out of range
}

TEST(BridgeOperations, SetSpanInflatesTheExtent) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  ASSERT_TRUE(author_bridge(network, road, 40.0, 24.0)->apply(network).has_value());
  ASSERT_TRUE(set_bridge_span(network, road, 0, 30.0, 44.0)->apply(network).has_value());
  EXPECT_DOUBLE_EQ(bridges(network, road)[0].s, 30.0);
  EXPECT_DOUBLE_EQ(bridges(network, road)[0].length, 44.0);
  EXPECT_FALSE(set_bridge_span(network, road, 0, 30.0, 44.0)->apply(network).has_value()); // no-op
}

TEST(BridgeOperations, UndoRestoresTheOriginalState) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  auto add = author_bridge(network, road, 40.0, 24.0);
  ASSERT_TRUE(add->apply(network).has_value());
  auto resize = set_bridge_span(network, road, 0, 20.0, 60.0);
  ASSERT_TRUE(resize->apply(network).has_value());
  auto drop = remove_bridge(network, road, 0);
  ASSERT_TRUE(drop->apply(network).has_value());
  EXPECT_TRUE(bridges(network, road).empty());

  ASSERT_TRUE(drop->revert(network).has_value());
  ASSERT_TRUE(resize->revert(network).has_value());
  ASSERT_TRUE(add->revert(network).has_value());
  EXPECT_TRUE(bridges(network, road).empty()); // back to no bridge
}

TEST(BridgeOperations, ApplyThenRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  const auto baseline = write_xodr(network, "t");
  ASSERT_TRUE(baseline.has_value());

  auto command = author_bridge(network, road, 40.0, 24.0);
  ASSERT_TRUE(command->apply(network).has_value());
  const auto with_bridge = write_xodr(network, "t");
  ASSERT_TRUE(with_bridge.has_value());
  EXPECT_NE(with_bridge->find("<bridge"), std::string::npos);

  ASSERT_TRUE(command->revert(network).has_value());
  const auto reverted = write_xodr(network, "t");
  ASSERT_TRUE(reverted.has_value());
  EXPECT_EQ(*reverted, *baseline); // apply -> revert leaves write_xodr untouched
}

TEST(BridgeOperations, DirtySetNamesTheRoadOnly) {
  RoadNetwork network;
  const RoadId road = make_road(network);
  const edit::DirtySet dirty = author_bridge(network, road, 40.0, 24.0)->dirty();
  ASSERT_EQ(dirty.roads.size(), 1U);
  EXPECT_EQ(dirty.roads[0], road);
  EXPECT_FALSE(dirty.terrain);
  EXPECT_TRUE(dirty.junctions.empty());
}

} // namespace
} // namespace roadmaker
