// The signal-controller arena (p4-s7, issue #228).
//
// ASAM OpenDRIVE 1.9.0 §14.6 makes `<controller>` a child of `<OpenDRIVE>`
// (Table 128) whose `<control>` children (Table 129) name signals by STRING
// @signalId. The arena therefore takes no owner, and a control that outlives
// its signal is a dangling reference — an expected state, never a cascade.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
using roadmaker::SignalId;
using roadmaker::Waypoint;

namespace {

Controller group(const char* id, const char* signal_odr_id) {
  return Controller{.odr_id = id, .controls = {Control{.signal_odr_id = signal_odr_id}}};
}

RoadId straight(RoadNetwork& network, const char* odr_id) {
  const std::vector<Waypoint> waypoints{Waypoint{0.0, 0.0}, Waypoint{60.0, 0.0}};
  return *roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
}

} // namespace

TEST(ControllerArena, AddsWithoutAnOwnerAndIteratesInCreationOrder) {
  RoadNetwork network;
  EXPECT_EQ(network.controller_count(), 0U);

  const ControllerId first = network.add_controller(group("10", "1"));
  const ControllerId second = network.add_controller(group("11", "2"));
  ASSERT_TRUE(first.is_valid());
  ASSERT_TRUE(second.is_valid());
  EXPECT_EQ(network.controller_count(), 2U);
  EXPECT_EQ(network.controller(first)->odr_id, "10");
  EXPECT_EQ(network.controller(second)->controls.at(0).signal_odr_id, "2");

  std::vector<std::string> seen;
  network.for_each_controller(
      [&](ControllerId, const Controller& controller) { seen.push_back(controller.odr_id); });
  EXPECT_EQ(seen, (std::vector<std::string>{"10", "11"}));
}

TEST(ControllerArena, EraseInvalidatesTheIdAndIsIdempotentlyFalse) {
  RoadNetwork network;
  const ControllerId id = network.add_controller(group("10", "1"));
  EXPECT_TRUE(network.erase_controller(id));
  EXPECT_EQ(network.controller(id), nullptr);
  EXPECT_EQ(network.controller_count(), 0U);
  EXPECT_FALSE(network.erase_controller(id));
  EXPECT_FALSE(network.erase_controller(ControllerId{}));
}

TEST(ControllerArena, RestoreInPlaceKeepsTheGenerationSoHeldIdsSurvive) {
  RoadNetwork network;
  const ControllerId id = network.add_controller(group("10", "1"));
  const std::size_t slots = network.controller_slot_count();

  ASSERT_TRUE(network.erase_controller_exact(id).has_value());
  EXPECT_EQ(network.controller(id), nullptr);
  EXPECT_EQ(network.controller_count(), 0U);
  EXPECT_EQ(network.controller_slot_count(), slots) << "the slot stays reserved";

  const auto restored = network.restore_controller(id, group("10", "1"));
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, id) << "restore-in-place must not bump the generation";
  ASSERT_NE(network.controller(id), nullptr);
  EXPECT_EQ(network.controller(id)->odr_id, "10");
  EXPECT_EQ(network.controller_slot_count(), slots);
}

TEST(ControllerArena, ReleaseReservedRecyclesADiscardedSlotAndGuardsMisuse) {
  RoadNetwork network;
  const ControllerId id = network.add_controller(group("10", "1"));
  // An occupied slot may not be released.
  EXPECT_FALSE(network.release_controller_reserved(id).has_value());

  ASSERT_TRUE(network.erase_controller_exact(id).has_value());
  EXPECT_TRUE(network.release_controller_reserved(id).has_value());
  // Releasing twice is a safe error, not a corruption.
  EXPECT_FALSE(network.release_controller_reserved(id).has_value());
}

TEST(ControllerArena, ErasingASignalLeavesTheControlDangling) {
  // delete_signal stays a leaf op: nothing cascades into controllers, so the
  // <control> keeps naming the gone signal. validate_network reports it and
  // clear_signalization (p4-s7 WP2) cleans it — the model never mutates itself.
  RoadNetwork network;
  const RoadId road = straight(network, "1");
  const SignalId signal =
      network.add_signal(road, Signal{.odr_id = "7", .s = 20.0, .type = "1000001"});
  ASSERT_TRUE(signal.is_valid());
  const ControllerId controller = network.add_controller(group("10", "7"));

  EXPECT_TRUE(network.erase_signal(signal));
  ASSERT_NE(network.controller(controller), nullptr);
  ASSERT_EQ(network.controller(controller)->controls.size(), 1U);
  EXPECT_EQ(network.controller(controller)->controls[0].signal_odr_id, "7");
}

TEST(ControllerArena, ErasingARoadDoesNotTouchControllers) {
  RoadNetwork network;
  const RoadId road = straight(network, "1");
  network.add_signal(road, Signal{.odr_id = "7", .s = 20.0, .type = "1000001"});
  const ControllerId controller = network.add_controller(group("10", "7"));

  EXPECT_TRUE(network.erase_road(road));
  EXPECT_EQ(network.signal_count(), 0U) << "signals are owned by their road";
  EXPECT_EQ(network.controller_count(), 1U) << "controllers are not";
  EXPECT_EQ(network.controller(controller)->controls[0].signal_odr_id, "7");
}
