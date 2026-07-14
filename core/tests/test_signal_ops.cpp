// Tests for the signal edit-command factories (add/delete/move_signal). Same M2
// contract as the object commands: apply→revert is byte-identical, a failed
// apply leaves the network untouched, and restore-in-place keeps the SignalId.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "support/network_compare.hpp"

namespace roadmaker {
namespace {

using test::snapshot_xodr;

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 120.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

/// A valid static sign (speed limit 50) — type/subtype/country satisfy the
/// signal validation rules so write_xodr accepts the network.
Signal make_sign(std::string odr_id, double s, double t) {
  Signal sign;
  sign.odr_id = std::move(odr_id);
  sign.type = "274";
  sign.subtype = "50";
  sign.country = "DE";
  sign.dynamic = false;
  sign.s = s;
  sign.t = t;
  return sign;
}

TEST(SignalOps, AddApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const std::string before = snapshot_xodr(network);

  auto command = edit::add_signal(network, road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.signal_count(), 1U);
  EXPECT_NE(snapshot_xodr(network), before) << "adding a signal must change the output";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.signal_count(), 0U);
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.signal_count(), 1U); // redo resurrects it
}

TEST(SignalOps, AddSetsTheRoadBackReference) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::add_signal(network, road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(command->apply(network).has_value());
  SignalId added;
  network.for_each_signal([&](SignalId id, const Signal&) { added = id; });
  ASSERT_NE(network.signal(added), nullptr);
  EXPECT_EQ(network.signal(added)->road, road);
}

TEST(SignalOps, AddRejectsStaleRoadWithoutMutating) {
  RoadNetwork network;
  author_street(network);
  const std::string before = snapshot_xodr(network);
  auto command = edit::add_signal(network, RoadId{}, make_sign("1", 10.0, -5.0));
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, AddRejectsStationOutsideRoad) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::add_signal(network, road, make_sign("1", 10000.0, -5.0));
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.signal_count(), 0U);
}

TEST(SignalOps, DeleteRestoresExactOnUndo) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::delete_signal(network, id);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.signal_count(), 0U);

  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.signal(id), nullptr) << "undo must restore the same SignalId";
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, MoveApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::move_signal(network, id, 40.0, -6.0, 1.5);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.signal(id)->s, 40.0);
  EXPECT_DOUBLE_EQ(network.signal(id)->t, -6.0);
  EXPECT_DOUBLE_EQ(network.signal(id)->h_offset, 1.5);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

} // namespace
} // namespace roadmaker
