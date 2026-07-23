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

// Kernel tests for the SIGNALIZATION ENGINE (p4-s7, issue #228): the
// junction_signals() approach query and the two commands built on it,
// edit::signalize_junction and edit::clear_signalization.
//
// Two things are being pinned here.
//
// 1. The query is DERIVED all the way down. An approach's gated movements are
//    exactly the maneuvers leaving that arm; its anchor is the stop line the
//    stop-line query already solved; its controller groups come from scanning
//    the top-level <controller>s for a <control> naming one of its signals.
//    Nothing about gating is stored, so nothing about it can go stale — and a
//    FOREIGN junction (no arm list) still reads, because the walk goes through
//    the connections.
//
// 2. Both commands satisfy the §8 oracle: apply changes the document, revert
//    restores it BYTE-identically, and a command that would change nothing is
//    rejected rather than applied (re-applying the same template is the exact
//    case p4-s6 hit with rebuild_maneuvers).
//
// The four-way fixture is deliberately ROOMY — arms stop 20 m short of the
// centre — because a tight junction clamps its stop lines and the placement
// anchors would then all coincide (the p4-s1 trap). The 3-arm T-junction is
// here to stop the templates from quietly hard-coding four arms: the axis
// clustering must give it one two-arm axis and one single-arm axis.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::Junction;
using roadmaker::junction_maneuvers;
using roadmaker::junction_phases;
using roadmaker::junction_signals;
using roadmaker::JunctionApproachInfo;
using roadmaker::JunctionId;
using roadmaker::JunctionManeuverInfo;
using roadmaker::JunctionPhasePlan;
using roadmaker::kSignalApproachWindow;
using roadmaker::LaneProfile;
using roadmaker::ObjectId;
using roadmaker::ObjectOrientation;
using roadmaker::PhaseState;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
using roadmaker::SignalId;
using roadmaker::SignalPhase;
using roadmaker::SignalState;
using roadmaker::SpanArm;
using roadmaker::TurnType;
using roadmaker::Waypoint;
using roadmaker::edit::clear_signalization;
using roadmaker::edit::Command;
using roadmaker::edit::set_phase_duration;
using roadmaker::edit::signalize_junction;
using roadmaker::edit::SignalizeOptions;
using roadmaker::edit::SignalizeTemplate;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// The §8 command oracle: apply changes the doc, revert restores it
/// byte-identically, re-apply reproduces, final revert is pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after); // a command that changes nothing is a bug
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
  ASSERT_TRUE(command.apply(network).has_value());
  expect_network_matches(network, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
}

/// A failed apply must leave the serialized network untouched.
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

JunctionId sole_junction(const RoadNetwork& network) {
  JunctionId found;
  network.for_each_junction([&](JunctionId id, const Junction&) { found = id; });
  return found;
}

/// A roomy four-way: every arm runs from 80 m out to 20 m short of the centre,
/// so its stop line sits well inside the road and never clamps.
struct CrossFixture {
  RoadNetwork network;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;
  JunctionId junction;

  CrossFixture() {
    west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    make_junction({end_of(west), end_of(east), end_of(south), end_of(north)});
  }

protected:
  CrossFixture(int /*tag*/) {}

  void make_junction(const std::vector<RoadEnd>& ends) {
    auto command = roadmaker::edit::create_junction(network, ends);
    if (command == nullptr) {
      throw std::runtime_error("create_junction: null command");
    }
    auto applied = command->apply(network);
    if (!applied.has_value()) {
      throw std::runtime_error("create_junction: " + applied.error().message);
    }
    junction = sole_junction(network);
  }
};

/// The 3-arm T: the west/east through road plus a southern stem. The stem has
/// no opposite partner, so it must form an axis of its own.
struct TeeFixture : CrossFixture {
  TeeFixture() : CrossFixture(0) {
    west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    make_junction({end_of(west), end_of(east), end_of(south)});
  }
};

std::size_t signal_count(const RoadNetwork& network) {
  return network.signal_count();
}

std::size_t controller_count(const RoadNetwork& network) {
  return network.controller_count();
}

const JunctionApproachInfo* approach_for(const std::vector<JunctionApproachInfo>& approaches,
                                         RoadId road) {
  const auto entry = std::ranges::find_if(
      approaches, [&](const JunctionApproachInfo& info) { return info.arm.road == road; });
  return entry == approaches.end() ? nullptr : &*entry;
}

/// Applies `command`, failing the test (fatally for the caller's flow) if it
/// does not apply.
void apply_command(RoadNetwork& network, const std::unique_ptr<Command>& command) {
  ASSERT_NE(command, nullptr);
  const auto applied = command->apply(network);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
}

} // namespace

// --- the query ---------------------------------------------------------------

TEST(JunctionSignals, DerivesOneApproachPerArmWithItsGatedMovements) {
  CrossFixture fixture;
  const std::vector<JunctionApproachInfo> approaches =
      junction_signals(fixture.network, fixture.junction);
  ASSERT_EQ(approaches.size(), 4U);

  const std::vector<JunctionManeuverInfo> maneuvers =
      junction_maneuvers(fixture.network, fixture.junction);
  std::size_t gated_total = 0;
  for (const JunctionApproachInfo& approach : approaches) {
    EXPECT_EQ(approach.arm.contact, ContactPoint::End);
    EXPECT_FALSE(approach.gated.empty());
    gated_total += approach.gated.size();
    // Gating is derived: every listed movement really leaves this arm.
    for (const RoadId turn : approach.gated) {
      const auto entry = std::ranges::find_if(
          maneuvers, [&](const JunctionManeuverInfo& info) { return info.road == turn; });
      ASSERT_NE(entry, maneuvers.end());
      EXPECT_EQ(entry->from, approach.arm);
    }
    // The anchor is the stop line's, not a second derivation: it sits inside
    // the roomy arm rather than clamped to its end.
    const double length = fixture.network.road(approach.arm.road)->plan_view.length();
    EXPECT_GT(approach.s_stop, 0.0);
    EXPECT_LT(approach.s_stop, length);
    // Nothing is signalized yet.
    EXPECT_TRUE(approach.signal_ids.empty());
    EXPECT_TRUE(approach.controller_odr_ids.empty());
    EXPECT_FALSE(approach.dynamic);
  }
  // Every maneuver is gated by exactly one approach.
  EXPECT_EQ(gated_total, maneuvers.size());
}

TEST(JunctionSignals, StaleAndSpanJunctionsYieldNothing) {
  CrossFixture fixture;
  EXPECT_TRUE(junction_signals(fixture.network, JunctionId{}).empty());

  RoadNetwork network;
  const RoadId through = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const std::vector<SpanArm> spans{SpanArm{.road = through, .s_start = 50.0, .s_end = 56.5}};
  auto command = roadmaker::edit::create_span_junction(network, spans);
  apply_command(network, command);
  // A §12.7 span junction cuts no road, so it has no connections and therefore
  // no approaches.
  EXPECT_TRUE(junction_signals(network, sole_junction(network)).empty());
}

TEST(JunctionSignals, ForeignJunctionStillReadsThroughItsConnections) {
  CrossFixture fixture;
  // Simulate a junction loaded from someone else's file: connections survive,
  // the RoadMaker-only arm list does not.
  fixture.network.junction(fixture.junction)->arms.clear();

  const std::vector<JunctionApproachInfo> approaches =
      junction_signals(fixture.network, fixture.junction);
  EXPECT_EQ(approaches.size(), 4U);
  for (const JunctionApproachInfo& approach : approaches) {
    EXPECT_FALSE(approach.gated.empty());
  }
  // ...but it cannot be authored.
  expect_command_rejected(fixture.network,
                          signalize_junction(fixture.network, fixture.junction, {}));
}

TEST(JunctionSignals, ResolvesSignalsInsideTheApproachWindowOnly) {
  CrossFixture fixture;
  const double length = fixture.network.road(fixture.west)->plan_view.length();

  const auto place = [&](double s, ObjectOrientation orientation, const char* odr_id) {
    Signal signal;
    signal.odr_id = odr_id;
    signal.s = s;
    signal.t = -5.0;
    signal.orientation = orientation;
    signal.dynamic = true;
    signal.type = "1000001";
    signal.subtype = "-1";
    signal.country = "OpenDRIVE";
    return fixture.network.add_signal(fixture.west, signal);
  };

  // Exactly at the window edge: inside (the bound is inclusive).
  const SignalId edge = place(length - kSignalApproachWindow, ObjectOrientation::Plus, "10");
  // Just beyond it: out.
  place(length - kSignalApproachWindow - 1.0, ObjectOrientation::Plus, "11");
  // Inside the window but facing the wrong way: out.
  place(length - 1.0, ObjectOrientation::Minus, "12");
  // Inside and valid in both directions: in (§14.1 e_orientation "none").
  const SignalId both = place(length - 2.0, ObjectOrientation::None, "13");

  const std::vector<JunctionApproachInfo> approaches =
      junction_signals(fixture.network, fixture.junction);
  const JunctionApproachInfo* west = approach_for(approaches, fixture.west);
  ASSERT_NE(west, nullptr);
  ASSERT_EQ(west->signal_ids.size(), 2U);
  EXPECT_EQ(west->signal_ids[0], edge);
  EXPECT_EQ(west->signal_ids[1], both);
  EXPECT_TRUE(west->dynamic);
  EXPECT_TRUE(west->controller_odr_ids.empty());

  // A controller naming one of them puts the approach in its group; a dangling
  // <control> matches nothing and is not an error.
  Controller controller;
  controller.odr_id = "ctrl";
  controller.controls.push_back(Control{.signal_odr_id = "10", .type = {}});
  controller.controls.push_back(Control{.signal_odr_id = "does-not-exist", .type = {}});
  fixture.network.add_controller(controller);

  const std::vector<JunctionApproachInfo> grouped =
      junction_signals(fixture.network, fixture.junction);
  const JunctionApproachInfo* regrouped = approach_for(grouped, fixture.west);
  ASSERT_NE(regrouped, nullptr);
  ASSERT_EQ(regrouped->controller_odr_ids.size(), 1U);
  EXPECT_EQ(regrouped->controller_odr_ids.front(), "ctrl");
}

// --- templates ---------------------------------------------------------------

TEST(Signalize, FourWayProtectedLeftGroupsThroughAndLeftPerAxis) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::FourWayProtectedLeft});
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Signalize Junction");
  apply_command(fixture.network, command);

  // Four approaches, each with a through head; every approach of a plain cross
  // has a left turn, so each also gets a protected-left head.
  EXPECT_EQ(signal_count(fixture.network), 8U);
  // Two axes x (through + left) = four controllers.
  EXPECT_EQ(controller_count(fixture.network), 4U);

  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.signalization.tmpl, "protected_left");
  EXPECT_TRUE(junction.signalization.mount_model.empty());
  EXPECT_EQ(junction.junction_controllers.size(), 4U);
  EXPECT_TRUE(junction.signal_mounts.empty());

  // Every sync-group reference names a real top-level controller, and every
  // controller drives at least one signal
  // (asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals).
  std::size_t matched = 0;
  fixture.network.for_each_controller([&](ControllerId, const Controller& controller) {
    EXPECT_FALSE(controller.controls.empty());
    const auto entry = std::ranges::find_if(
        junction.junction_controllers, [&](const roadmaker::JunctionController& reference) {
          return reference.controller_odr_id == controller.odr_id;
        });
    if (entry != junction.junction_controllers.end()) {
      ++matched;
    }
  });
  EXPECT_EQ(matched, 4U);

  // Every head is dynamic, faces the junction, and carries the OpenDRIVE
  // traffic-light catalog code.
  fixture.network.for_each_signal([&](SignalId, const Signal& signal) {
    EXPECT_TRUE(signal.dynamic.value_or(false));
    EXPECT_EQ(signal.orientation, ObjectOrientation::Plus); // every arm meets at its End
    EXPECT_EQ(signal.type, "1000001");
    EXPECT_EQ(signal.subtype, "-1");
    EXPECT_EQ(signal.country, "OpenDRIVE");
  });
}

TEST(Signalize, TwoPhaseGroupsOneControllerPerAxis) {
  CrossFixture fixture;
  auto command =
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase});
  apply_command(fixture.network, command);

  EXPECT_EQ(signal_count(fixture.network), 4U); // permissive lefts: one head per approach
  EXPECT_EQ(controller_count(fixture.network), 2U);
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.signalization.tmpl, "two_phase");
  EXPECT_EQ(junction.junction_controllers.size(), 2U);

  // Each axis controller drives the two opposite arms.
  fixture.network.for_each_controller([&](ControllerId, const Controller& controller) {
    EXPECT_EQ(controller.controls.size(), 2U);
  });
}

TEST(Signalize, StaticTemplatesCreateNoPhaseData) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::AllWayStop});
  apply_command(fixture.network, command);

  EXPECT_EQ(signal_count(fixture.network), 4U);
  // GW-4 step 3: an all-way stop has no phases, so no phase data is created.
  EXPECT_EQ(controller_count(fixture.network), 0U);
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.signalization.tmpl, "all_way_stop");
  EXPECT_TRUE(junction.junction_controllers.empty());

  fixture.network.for_each_signal([&](SignalId, const Signal& signal) {
    EXPECT_FALSE(signal.dynamic.value_or(true));
    EXPECT_EQ(signal.type, "206");
    EXPECT_EQ(signal.country, "DE");
  });
}

TEST(Signalize, TwoWayStopSignsTheMinorAxisOnly) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoWayStop});
  apply_command(fixture.network, command);

  EXPECT_EQ(signal_count(fixture.network), 2U);
  EXPECT_EQ(controller_count(fixture.network), 0U);
  EXPECT_EQ(fixture.network.junction(fixture.junction)->signalization.tmpl, "two_way_stop");

  // Both signs sit on ONE axis — i.e. on two arms whose approach headings are
  // opposite.
  std::vector<RoadId> signed_roads;
  fixture.network.for_each_signal(
      [&](SignalId, const Signal& signal) { signed_roads.push_back(signal.road); });
  ASSERT_EQ(signed_roads.size(), 2U);
  EXPECT_NE(signed_roads[0], signed_roads[1]);
  const std::vector<JunctionApproachInfo> approaches =
      junction_signals(fixture.network, fixture.junction);
  const JunctionApproachInfo* a = approach_for(approaches, signed_roads[0]);
  const JunctionApproachInfo* b = approach_for(approaches, signed_roads[1]);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NEAR(std::abs(std::remainder(a->heading - b->heading, 2.0 * std::numbers::pi)),
              std::numbers::pi,
              0.6);
}

TEST(Signalize, HeadsSitOnTheApproachAnchorOutboardOfTheCarriageway) {
  CrossFixture fixture;
  auto command =
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase});
  const std::vector<JunctionApproachInfo> before =
      junction_signals(fixture.network, fixture.junction);
  apply_command(fixture.network, command);

  fixture.network.for_each_signal([&](SignalId, const Signal& signal) {
    const JunctionApproachInfo* approach = approach_for(before, signal.road);
    ASSERT_NE(approach, nullptr);
    EXPECT_NEAR(signal.s, approach->s_stop, 1e-9);
    // Every arm meets the junction at its End, so travel runs toward +s and the
    // driver's right is negative t — outboard of the incoming lanes.
    EXPECT_LT(signal.t, -3.0);
    EXPECT_GT(signal.z_offset, 2.0);
  });

  // The query now sees them, and reports the junction as light-controlled.
  for (const JunctionApproachInfo& approach : junction_signals(fixture.network, fixture.junction)) {
    EXPECT_EQ(approach.signal_ids.size(), 1U);
    EXPECT_TRUE(approach.dynamic);
    EXPECT_EQ(approach.controller_odr_ids.size(), 1U);
  }
}

// --- the T-junction ----------------------------------------------------------

TEST(Signalize, ThreeArmTeeGetsATwoArmAxisAndASingleArmAxis) {
  TeeFixture fixture;
  ASSERT_EQ(junction_signals(fixture.network, fixture.junction).size(), 3U);

  auto command =
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase});
  apply_command(fixture.network, command);

  EXPECT_EQ(signal_count(fixture.network), 3U);
  // One axis for the through road, one for the stem — two controllers, not one
  // and not three.
  EXPECT_EQ(controller_count(fixture.network), 2U);
  std::vector<std::size_t> group_sizes;
  fixture.network.for_each_controller([&](ControllerId, const Controller& controller) {
    group_sizes.push_back(controller.controls.size());
  });
  std::ranges::sort(group_sizes);
  EXPECT_EQ(group_sizes, (std::vector<std::size_t>{1U, 2U}));
}

TEST(Signalize, ThreeArmTeeTakesAnAllWayStop) {
  TeeFixture fixture;
  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::AllWayStop});
  apply_command(fixture.network, command);
  EXPECT_EQ(signal_count(fixture.network), 3U);
  EXPECT_EQ(controller_count(fixture.network), 0U);
}

// --- rejections --------------------------------------------------------------

TEST(Signalize, RejectsStaleSpanAndAlreadySignalizedJunctions) {
  CrossFixture fixture;
  expect_command_rejected(fixture.network, signalize_junction(fixture.network, JunctionId{}, {}));

  // A span (virtual) junction "shall not have controllers and therefore no
  // traffic lights" (asam.net:xodr:1.9.0:junctions.virtual.no_controllers).
  RoadNetwork network;
  const RoadId through = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const std::vector<SpanArm> spans{SpanArm{.road = through, .s_start = 50.0, .s_end = 56.5}};
  auto span = roadmaker::edit::create_span_junction(network, spans);
  apply_command(network, span);
  const JunctionId span_junction = sole_junction(network);
  auto rejected = signalize_junction(network, span_junction, {});
  ASSERT_NE(rejected, nullptr);
  const auto applied = rejected->apply(network);
  ASSERT_FALSE(applied.has_value());
  EXPECT_NE(applied.error().message.find("junctions.virtual.no_controllers"), std::string::npos);

  // An unknown mount model is refused before anything is placed.
  expect_command_rejected(
      fixture.network,
      signalize_junction(fixture.network, fixture.junction, {.mount_model = "not_a_prop"}));
}

TEST(Signalize, ReapplyingTheSameTemplateIsInvalid) {
  CrossFixture fixture;
  auto first =
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase});
  apply_command(fixture.network, first);

  // The round-trip oracle forbids a command that changes nothing, so the
  // factory refuses instead of authoring a duplicate.
  expect_command_rejected(
      fixture.network,
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase}));

  // A DIFFERENT template is accepted, and it replaces rather than accumulates.
  auto second = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::AllWayStop});
  apply_command(fixture.network, second);
  EXPECT_EQ(signal_count(fixture.network), 4U);
  EXPECT_EQ(controller_count(fixture.network), 0U);
  EXPECT_EQ(fixture.network.junction(fixture.junction)->signalization.tmpl, "all_way_stop");
}

TEST(Signalize, ClearingAnUnsignalizedJunctionIsInvalid) {
  CrossFixture fixture;
  expect_command_rejected(fixture.network, clear_signalization(fixture.network, fixture.junction));
  expect_command_rejected(fixture.network, clear_signalization(fixture.network, JunctionId{}));
}

// --- clearing and mounts -----------------------------------------------------

TEST(Signalize, ClearRemovesEverySignalizationArtifactIncludingStaticSigns) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::AllWayStop});
  apply_command(fixture.network, command);
  ASSERT_EQ(signal_count(fixture.network), 4U);

  auto cleared = clear_signalization(fixture.network, fixture.junction);
  ASSERT_NE(cleared, nullptr);
  EXPECT_EQ(cleared->name(), "Clear Signalization");
  apply_command(fixture.network, cleared);

  // A static template creates no controllers and no mount records, so clearing
  // it can only work off the DERIVED catalog match — that is what is pinned.
  EXPECT_EQ(signal_count(fixture.network), 0U);
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_TRUE(junction.signalization.tmpl.empty());
  EXPECT_TRUE(junction.junction_controllers.empty());
  EXPECT_TRUE(junction.signal_mounts.empty());
}

TEST(Signalize, ClearLeavesAHandPlacedSignOfAnotherTypeAlone) {
  CrossFixture fixture;
  Signal plate;
  plate.odr_id = "speed";
  plate.s = fixture.network.road(fixture.west)->plan_view.length() - 5.0;
  plate.t = -6.0;
  plate.orientation = ObjectOrientation::Plus;
  plate.dynamic = false;
  plate.type = "274";
  plate.subtype = "50";
  plate.country = "DE";
  fixture.network.add_signal(fixture.west, plate);

  auto command = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::AllWayStop});
  apply_command(fixture.network, command);
  auto cleared = clear_signalization(fixture.network, fixture.junction);
  apply_command(fixture.network, cleared);

  ASSERT_EQ(signal_count(fixture.network), 1U);
  fixture.network.for_each_signal(
      [&](SignalId, const Signal& signal) { EXPECT_EQ(signal.odr_id, "speed"); });
}

TEST(Signalize, MountPropsArePlacedPairedAndErased) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network,
      fixture.junction,
      {.tmpl = SignalizeTemplate::TwoPhase, .mount_model = "streetlight_single"});
  apply_command(fixture.network, command);

  EXPECT_EQ(signal_count(fixture.network), 4U);
  EXPECT_EQ(fixture.network.object_count(), 4U);
  const Junction& junction = *fixture.network.junction(fixture.junction);
  EXPECT_EQ(junction.signalization.mount_model, "streetlight_single");
  ASSERT_EQ(junction.signal_mounts.size(), 4U);
  for (const roadmaker::SignalMount& mount : junction.signal_mounts) {
    ASSERT_EQ(mount.object_odr_ids.size(), 1U);
    // The pairing names live objects, or the writer would drop the record.
    bool found_object = false;
    fixture.network.for_each_object([&](ObjectId, const roadmaker::Object& object) {
      found_object = found_object || object.odr_id == mount.object_odr_ids.front();
    });
    EXPECT_TRUE(found_object);
    bool found_signal = false;
    fixture.network.for_each_signal([&](SignalId, const Signal& signal) {
      found_signal = found_signal || signal.odr_id == mount.signal_odr_id;
    });
    EXPECT_TRUE(found_signal);
  }

  auto cleared = clear_signalization(fixture.network, fixture.junction);
  apply_command(fixture.network, cleared);
  EXPECT_EQ(signal_count(fixture.network), 0U);
  EXPECT_EQ(fixture.network.object_count(), 0U);
}

// --- the oracle --------------------------------------------------------------

TEST(Signalize, SignalizeRoundTrips) {
  for (const SignalizeTemplate tmpl : {SignalizeTemplate::FourWayProtectedLeft,
                                       SignalizeTemplate::TwoPhase,
                                       SignalizeTemplate::AllWayStop,
                                       SignalizeTemplate::TwoWayStop}) {
    CrossFixture fixture;
    SCOPED_TRACE(static_cast<int>(tmpl));
    auto command = signalize_junction(fixture.network, fixture.junction, {.tmpl = tmpl});
    ASSERT_NE(command, nullptr);
    EXPECT_TRUE(command->dirty().junctions_are_current);
    expect_command_round_trip(fixture.network, *command);
  }
}

TEST(Signalize, SignalizeWithMountsRoundTrips) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network,
      fixture.junction,
      {.tmpl = SignalizeTemplate::FourWayProtectedLeft, .mount_model = "streetlight_single"});
  ASSERT_NE(command, nullptr);
  expect_command_round_trip(fixture.network, *command);
}

TEST(Signalize, ClearRoundTrips) {
  CrossFixture fixture;
  auto command = signalize_junction(
      fixture.network,
      fixture.junction,
      {.tmpl = SignalizeTemplate::TwoPhase, .mount_model = "streetlight_single"});
  apply_command(fixture.network, command);

  auto cleared = clear_signalization(fixture.network, fixture.junction);
  ASSERT_NE(cleared, nullptr);
  expect_command_round_trip(fixture.network, *cleared);
}

TEST(Signalize, RetemplatingRoundTrips) {
  CrossFixture fixture;
  auto first = signalize_junction(
      fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::FourWayProtectedLeft});
  apply_command(fixture.network, first);

  // The interesting case: the command both creates (new heads, new groups) and
  // erases (the previous generation), so revert has to restore in place.
  auto second =
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase});
  ASSERT_NE(second, nullptr);
  expect_command_round_trip(fixture.network, *second);
}

// --- phase interaction (p4-s8) -----------------------------------------------

TEST(Signalize, RetemplatingClearsTheAuthoredCycle) {
  CrossFixture fixture;
  apply_command(fixture.network,
                signalize_junction(fixture.network,
                                   fixture.junction,
                                   {.tmpl = SignalizeTemplate::FourWayProtectedLeft}));
  // Author a cycle (materializes the derived phases into the junction).
  apply_command(fixture.network, set_phase_duration(fixture.network, fixture.junction, 0, 24.0));
  ASSERT_TRUE(junction_phases(fixture.network, fixture.junction).authored);

  // Re-templating mints fresh controller ids, so the authored cycle — which
  // named the old ones — is dropped rather than left all-dormant.
  apply_command(
      fixture.network,
      signalize_junction(fixture.network, fixture.junction, {.tmpl = SignalizeTemplate::TwoPhase}));
  EXPECT_TRUE(fixture.network.junction(fixture.junction)->phases.empty());
  EXPECT_FALSE(junction_phases(fixture.network, fixture.junction).authored);
}

TEST(Signalize, ClearSignalizationClearsAPhaseOnlyJunction) {
  CrossFixture fixture;
  // A junction whose ONLY signalization artifact is an authored cycle (no
  // template, controllers or mounts): the `records` guard must still admit it.
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.phases = {SignalPhase{.name = "solo", .duration = 20.0, .states = {}}};

  auto cleared = clear_signalization(fixture.network, fixture.junction);
  ASSERT_NE(cleared, nullptr);
  apply_command(fixture.network, cleared);
  EXPECT_TRUE(fixture.network.junction(fixture.junction)->phases.empty());
}
