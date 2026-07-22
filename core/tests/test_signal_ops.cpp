// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Tests for the signal edit-command factories (add/delete/move_signal). Same M2
// contract as the object commands: apply→revert is byte-identical, a failed
// apply leaves the network untouched, and restore-in-place keeps the SignalId.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

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

// --- set_signal_text (p4-s9, #230) ------------------------------------------

TEST(SignalOps, SetTextApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_text(network, id, "City");
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.signal(id)->text, "City");
  EXPECT_NE(snapshot_xodr(network), before) << "setting text must change the output";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.signal(id)->text, "");
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

TEST(SignalOps, SetTextReapplyMatchesSingleApply) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));

  auto command = edit::set_signal_text(network, id, "City");
  ASSERT_TRUE(command->apply(network).has_value());
  const std::string once = snapshot_xodr(network);
  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), once) << "reapply must match the single apply";
}

TEST(SignalOps, NoOpTextIsRejected) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sign = make_sign("1", 10.0, -5.0);
  sign.text = "City";
  const SignalId id = network.add_signal(road, sign);
  const std::string before = snapshot_xodr(network);

  // Same text ⇒ invalid_command; a valid command would round-trip through the
  // EditStack, so a no-op must never produce one.
  auto command = edit::set_signal_text(network, id, "City");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "a rejected no-op must not mutate";
}

TEST(SignalOps, SetTextRejectsStaleSignalWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  network.erase_signal(id);
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_text(network, id, "City");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, MultiLineTextRoundTripsThroughXodr) {
  // §14 Table 122: a multi-line town name uses a literal '\n'; the writer must
  // escape it as &#10; and the reader must decode it, so the text survives a
  // full write→parse cycle unchanged. Locks the pugixml escaping both ways.
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));

  auto command = edit::set_signal_text(network, id, "City\nBadAibling");
  ASSERT_TRUE(command->apply(network).has_value());

  const auto written = write_xodr(network, "text-sign");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse_xodr(*written, "text-sign");
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->network.signal_count(), 1U);

  const Signal* again = nullptr;
  reparsed->network.for_each_signal([&](SignalId /*sid*/, const Signal& sig) { again = &sig; });
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again->text, "City\nBadAibling");
}

} // namespace
} // namespace roadmaker
