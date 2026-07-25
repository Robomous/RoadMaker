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

#include <limits>
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

/// A valid static sign — type/subtype/country satisfy the signal validation
/// rules so write_xodr accepts the network. This is deliberately a LEGACY
/// German StVO identity: these tests are about the command layer, and using an
/// identity no shipped pack claims keeps them independent of what the US
/// catalogue happens to contain.
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

// --- set_signal_z_offset (p6-s16, #418) --------------------------------------

TEST(SignalOps, SetZOffsetApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const double before_z = network.signal(id)->z_offset;
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_z_offset(network, id, 3.2);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.signal(id)->z_offset, 3.2);
  EXPECT_NE(snapshot_xodr(network), before) << "@zOffset is required, so it always writes";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_DOUBLE_EQ(network.signal(id)->z_offset, before_z);
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

TEST(SignalOps, SetZOffsetAcceptsANegativeHeight) {
  // A signal below the reference line is legal (§14.1 puts no bound on
  // @zOffset) — think of one hung under an overpass deck.
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));

  auto command = edit::set_signal_z_offset(network, id, -1.5);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.signal(id)->z_offset, -1.5);
}

TEST(SignalOps, NoOpZOffsetIsRejected) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sign = make_sign("1", 10.0, -5.0);
  sign.z_offset = 2.1;
  const SignalId id = network.add_signal(road, sign);
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_z_offset(network, id, 2.1);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "a rejected no-op must not mutate";
}

TEST(SignalOps, SetZOffsetRejectsNonFiniteWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_z_offset(network, id, std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, SetZOffsetRejectsStaleSignalWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  network.erase_signal(id);
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_z_offset(network, id, 3.2);
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

// --- set_signal_value (US sign pack, #414) ----------------------------------
//
// §14.1 Table 122 binds @value to @unit: "value of the signal, if value is
// given, unit is mandatory". The command therefore edits the pair, never one
// half of it — that invariant is what these tests pin.

TEST(SignalOps, SetValueApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_value(network, id, 25.0, "mph");
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_TRUE(network.signal(id)->value.has_value());
  EXPECT_DOUBLE_EQ(*network.signal(id)->value, 25.0);
  EXPECT_EQ(network.signal(id)->unit, "mph");
  EXPECT_NE(snapshot_xodr(network), before) << "posting a speed must change the output";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_FALSE(network.signal(id)->value.has_value());
  EXPECT_EQ(network.signal(id)->unit, "");
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(*network.signal(id)->value, 25.0); // redo
}

TEST(SignalOps, ClearingTheValueClearsTheUnitWithIt) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sign = make_sign("1", 10.0, -5.0);
  sign.value = 25.0;
  sign.unit = "mph";
  const SignalId id = network.add_signal(road, sign);

  auto command = edit::set_signal_value(network, id, std::nullopt, "");
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_FALSE(network.signal(id)->value.has_value());
  EXPECT_EQ(network.signal(id)->unit, "") << "a unit without a value is meaningless";
}

TEST(SignalOps, HalfAPairIsRejectedWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  // A value with no unit, and a unit with no value: both violate §14.1.
  EXPECT_FALSE(edit::set_signal_value(network, id, 25.0, "")->apply(network).has_value());
  EXPECT_FALSE(
      edit::set_signal_value(network, id, std::nullopt, "mph")->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "a rejected command must not mutate";
}

TEST(SignalOps, NoOpValueIsRejected) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sign = make_sign("1", 10.0, -5.0);
  sign.value = 25.0;
  sign.unit = "mph";
  const SignalId id = network.add_signal(road, sign);
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_value(network, id, 25.0, "mph");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, SetValueRejectsStaleSignalWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  network.erase_signal(id);
  const std::string before = snapshot_xodr(network);

  auto command = edit::set_signal_value(network, id, 25.0, "mph");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, PostedSpeedRoundTripsThroughXodr) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(edit::set_signal_value(network, id, 45.0, "mph")->apply(network).has_value());

  const auto written = write_xodr(network, "speed-limit");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse_xodr(*written, "speed-limit");
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->network.signal_count(), 1U);

  const Signal* again = nullptr;
  reparsed->network.for_each_signal([&](SignalId /*sid*/, const Signal& sig) { again = &sig; });
  ASSERT_NE(again, nullptr);
  ASSERT_TRUE(again->value.has_value());
  EXPECT_DOUBLE_EQ(*again->value, 45.0);
  EXPECT_EQ(again->unit, "mph");
}

// --- auto_orient_signal (p6-s14, #416) ---------------------------------------
//
// The explicit "auto" action. Same command contract as the rest of this file;
// what makes it interesting is the OTHER half of its job — being one of only
// two places a facing is ever derived, so that a hand-set heading survives.

TEST(SignalOps, AutoOrientApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::auto_orient_signal(network, id);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NE(snapshot_xodr(network), before);
  const Signal* aimed = network.signal(id);
  ASSERT_NE(aimed, nullptr);
  EXPECT_EQ(aimed->orientation, ObjectOrientation::Plus);
  EXPECT_NE(aimed->h_offset, 0.0);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

TEST(SignalOps, AutoOrientRejectsASignalAlreadyFacingItsTraffic) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(edit::auto_orient_signal(network, id)->apply(network).has_value());
  const std::string before = snapshot_xodr(network);

  auto again = edit::auto_orient_signal(network, id);
  EXPECT_FALSE(again->apply(network).has_value())
      << "a second auto must be rejected as a no-op, not pushed as an empty undo step";
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(SignalOps, AutoOrientRejectsStaleSignalWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  network.erase_signal(id);
  const std::string before = snapshot_xodr(network);

  auto command = edit::auto_orient_signal(network, id);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
}

// The override rule, stated as a test: NOTHING but the auto action re-derives
// a facing. Moving a signal — the edit most likely to want to "helpfully"
// re-aim it — must leave a hand-set heading exactly where the user put it.
TEST(SignalOps, MovingASignalNeverRecomputesAHandSetHeading) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(edit::move_signal(network, id, 10.0, -5.0, 1.234)->apply(network).has_value());

  // Move it far along the road and across to the other side, which is exactly
  // where auto-orientation would answer differently.
  ASSERT_TRUE(edit::move_signal(network, id, 90.0, 5.0)->apply(network).has_value());

  const Signal* moved = network.signal(id);
  ASSERT_NE(moved, nullptr);
  EXPECT_DOUBLE_EQ(moved->h_offset, 1.234) << "a user's heading is an override, not a suggestion";
}

TEST(SignalOps, AHandSetHeadingSurvivesARoundTrip) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_sign("1", 10.0, -5.0));
  ASSERT_TRUE(edit::move_signal(network, id, 10.0, -5.0, 1.234)->apply(network).has_value());

  const auto written = write_xodr(network, "override");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse_xodr(*written, "override");
  ASSERT_TRUE(reparsed.has_value());

  const Signal* again = nullptr;
  reparsed->network.for_each_signal([&](SignalId /*sid*/, const Signal& sig) { again = &sig; });
  ASSERT_NE(again, nullptr);
  EXPECT_DOUBLE_EQ(again->h_offset, 1.234);
}

} // namespace
} // namespace roadmaker
