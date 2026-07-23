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

// Kernel tests for the SIGNAL PHASE query (p4-s8, issue #229): the derived
// default cycle and its resolution, `mesh::junction_phases()` and
// `phase_index_at()`. The command layer (edit::set_phase_* and friends) is
// WP3 and tested with the signalization commands; here the model is driven by
// signalize_junction and, for the authored path, by mutating Junction::phases
// directly (a pure query, so no command is needed to exercise resolution).
//
// What is pinned:
//   * DERIVED shape — a dynamic junction with no stored phases yields a
//     red-yellow-green cycle: one green + one yellow-clear phase per signal
//     axis, in sync-group row order, states Red-filled to every member.
//   * The protected-left partition of `moving`: a Left-turn road proceeds only
//     when a *left* controller is green, a through/right road only when an
//     *other* controller is.
//   * `phase_index_at` wraps over the cycle and handles negative/empty input.
//   * A junction with no live member controller (static template, span,
//     unsignalized) yields an empty plan — no cycle to time.

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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::Junction;
using roadmaker::junction_maneuvers;
using roadmaker::junction_phases;
using roadmaker::JunctionId;
using roadmaker::JunctionManeuverInfo;
using roadmaker::JunctionPhaseInfo;
using roadmaker::JunctionPhasePlan;
using roadmaker::LaneProfile;
using roadmaker::phase_index_at;
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
using roadmaker::edit::add_signal_phase;
using roadmaker::edit::clear_signal_phases;
using roadmaker::edit::Command;
using roadmaker::edit::duplicate_signal_phase;
using roadmaker::edit::remove_signal_phase;
using roadmaker::edit::set_phase_duration;
using roadmaker::edit::set_phase_state;
using roadmaker::edit::signalize_junction;
using roadmaker::edit::SignalizeTemplate;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

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

void apply_command(RoadNetwork& network, const std::unique_ptr<Command>& command) {
  ASSERT_NE(command, nullptr);
  const auto applied = command->apply(network);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
}

/// The roomy four-way from the signalization tests: arms stop 20 m short of the
/// centre so the stop lines never clamp.
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
    const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
    auto command = roadmaker::edit::create_junction(network, ends);
    auto applied = command->apply(network);
    if (!applied.has_value()) {
      throw std::runtime_error("create_junction: " + applied.error().message);
    }
    junction = sole_junction(network);
  }

  void signalize(SignalizeTemplate tmpl) {
    auto command = signalize_junction(network, junction, {.tmpl = tmpl});
    auto applied = command->apply(network);
    if (!applied.has_value()) {
      throw std::runtime_error("signalize: " + applied.error().message);
    }
  }
};

std::size_t count_states(const JunctionPhaseInfo& phase, SignalState want) {
  return static_cast<std::size_t>(
      std::ranges::count_if(phase.states, [&](const auto& s) { return s.state == want; }));
}

/// The controllers whose every controlled head is a protected-left arrow
/// (`Signal::name == "protected_left"`) versus the rest — the same partition
/// `junction_phases` makes internally, recovered here for the authoring tests.
struct Groups {
  std::vector<std::string> left;
  std::vector<std::string> through;
};

Groups classify_controllers(const RoadNetwork& network) {
  std::vector<std::pair<std::string, SignalId>> signals;
  network.for_each_signal(
      [&](SignalId id, const Signal& signal) { signals.emplace_back(signal.odr_id, id); });
  Groups groups;
  network.for_each_controller([&](ControllerId, const Controller& controller) {
    bool any = false;
    bool all_left = true;
    for (const Control& control : controller.controls) {
      const auto entry = std::ranges::find_if(
          signals, [&](const auto& pair) { return pair.first == control.signal_odr_id; });
      if (entry == signals.end()) {
        continue;
      }
      any = true;
      all_left = all_left && network.signal(entry->second)->name == "protected_left";
    }
    (any && all_left ? groups.left : groups.through).push_back(controller.odr_id);
  });
  return groups;
}

bool turns_left(const RoadNetwork& network, JunctionId junction, RoadId road) {
  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction);
  const auto entry = std::ranges::find_if(
      maneuvers, [&](const JunctionManeuverInfo& info) { return info.road == road; });
  return entry != maneuvers.end() && entry->effective == TurnType::Left;
}

/// The §8 command oracle (copied from test_corner_operations.cpp — the helper is
/// file-local per suite, not in support/): apply changes the doc, revert
/// restores it byte-identically, re-apply reproduces, final revert is pristine.
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

/// A rejected factory yields a command whose apply() fails and leaves the
/// serialized network untouched (the round-trip oracle forbids no-op commands).
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

std::size_t phase_count(const RoadNetwork& network, JunctionId junction) {
  return network.junction(junction)->phases.size();
}

std::string front_controller(const RoadNetwork& network, JunctionId junction) {
  return network.junction(junction)->junction_controllers.front().controller_odr_id;
}

std::string back_controller(const RoadNetwork& network, JunctionId junction) {
  return network.junction(junction)->junction_controllers.back().controller_odr_id;
}

} // namespace

// --- derived shape -----------------------------------------------------------

TEST(JunctionPhases, TwoPhaseFourWayDerivesFourPhaseCycle) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);

  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  EXPECT_FALSE(plan.authored); // no stored phases ⇒ derived
  EXPECT_TRUE(plan.dormant_controller_odr_ids.empty());

  // Two axes, so two rows, in sync-group order.
  const Junction& junction = *fixture.network.junction(fixture.junction);
  std::vector<std::string> expected_rows;
  for (const auto& reference : junction.junction_controllers) {
    expected_rows.push_back(reference.controller_odr_id);
  }
  ASSERT_EQ(expected_rows.size(), 2U);
  EXPECT_EQ(plan.controller_odr_ids, expected_rows);

  // Green(20) + Yellow(3) per axis ⇒ four phases, 46 s cycle.
  ASSERT_EQ(plan.phases.size(), 4U);
  EXPECT_NEAR(plan.cycle_duration, 46.0, 1e-9);

  const std::vector<std::pair<std::string, double>> want = {
      {"axis0", 20.0}, {"axis0_clear", 3.0}, {"axis1", 20.0}, {"axis1_clear", 3.0}};
  double expected_start = 0.0;
  for (std::size_t i = 0; i < want.size(); ++i) {
    SCOPED_TRACE(want[i].first);
    EXPECT_EQ(plan.phases[i].name, want[i].first);
    EXPECT_NEAR(plan.phases[i].duration, want[i].second, 1e-9);
    EXPECT_NEAR(plan.phases[i].start, expected_start, 1e-9);
    expected_start += want[i].second;
    // States are Red-filled to EVERY member controller.
    EXPECT_EQ(plan.phases[i].states.size(), 2U);
  }

  // axis0 phase: exactly one controller green, the rest red; its clear phase
  // turns that same controller yellow.
  EXPECT_EQ(count_states(plan.phases[0], SignalState::Green), 1U);
  EXPECT_EQ(count_states(plan.phases[0], SignalState::Red), 1U);
  EXPECT_EQ(count_states(plan.phases[1], SignalState::Yellow), 1U);
  EXPECT_EQ(count_states(plan.phases[2], SignalState::Green), 1U);
}

TEST(JunctionPhases, DerivedPhasesResolveSignalHeads) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  ASSERT_EQ(plan.phases.size(), 4U);

  // Four heads total; each green phase lights the two opposite heads of its axis.
  const std::size_t greens_in_axis0 = static_cast<std::size_t>(std::ranges::count_if(
      plan.phases[0].signal_states, [](const auto& s) { return s.state == SignalState::Green; }));
  EXPECT_EQ(greens_in_axis0, 2U);
  // The other axis's two heads are red this phase.
  const std::size_t reds_in_axis0 = static_cast<std::size_t>(std::ranges::count_if(
      plan.phases[0].signal_states, [](const auto& s) { return s.state == SignalState::Red; }));
  EXPECT_EQ(reds_in_axis0, 2U);
  EXPECT_EQ(plan.phases[0].signal_states.size(), 4U);
}

TEST(JunctionPhases, MovingDiffersBetweenAxisPhasesAndIsEmptyOnClearance) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  ASSERT_EQ(plan.phases.size(), 4U);

  EXPECT_FALSE(plan.phases[0].moving.empty());
  EXPECT_FALSE(plan.phases[2].moving.empty());
  // The two green phases move different connecting roads (opposite axes): no
  // road that moves in axis0's phase also moves in axis1's.
  for (const RoadId road : plan.phases[0].moving) {
    EXPECT_EQ(std::ranges::find(plan.phases[2].moving, road), plan.phases[2].moving.end());
  }
  EXPECT_NE(plan.phases[0].moving, plan.phases[2].moving);
  // A yellow clearance phase shows no green, so nothing may proceed.
  EXPECT_TRUE(plan.phases[1].moving.empty());
  EXPECT_TRUE(plan.phases[3].moving.empty());
}

// --- protected-left partition of `moving` ------------------------------------

TEST(JunctionPhases, ProtectedLeftPartitionGatesLeftTurnsSeparately) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::FourWayProtectedLeft);
  const Groups groups = classify_controllers(fixture.network);
  ASSERT_FALSE(groups.left.empty()); // every arm of a plain cross has a left turn
  ASSERT_FALSE(groups.through.empty());

  // Author a single phase that greens ONLY the through controllers. A left-turn
  // road must then be held; a straight/right road may proceed.
  Junction& junction = *fixture.network.junction(fixture.junction);
  SignalPhase through_green;
  through_green.name = "through";
  through_green.duration = 20.0;
  for (const std::string& controller : groups.through) {
    through_green.states.push_back(
        PhaseState{.controller_odr_id = controller, .state = SignalState::Green});
  }
  junction.phases = {through_green};

  JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  EXPECT_TRUE(plan.authored);
  ASSERT_EQ(plan.phases.size(), 1U);
  EXPECT_FALSE(plan.phases[0].moving.empty());
  bool any_left = false;
  for (const RoadId road : plan.phases[0].moving) {
    EXPECT_FALSE(turns_left(fixture.network, fixture.junction, road))
        << "a left turn proceeded on a through-only green";
    any_left = any_left || turns_left(fixture.network, fixture.junction, road);
  }
  EXPECT_FALSE(any_left);
  const std::size_t through_only_movers = plan.phases[0].moving.size();

  // Now green the protected-left controllers as well: the left turns join, so
  // strictly more connecting roads may proceed.
  SignalPhase all_green = through_green;
  for (const std::string& controller : groups.left) {
    all_green.states.push_back(
        PhaseState{.controller_odr_id = controller, .state = SignalState::Green});
  }
  junction.phases = {all_green};
  plan = junction_phases(fixture.network, fixture.junction);
  ASSERT_EQ(plan.phases.size(), 1U);
  const bool has_left_mover = std::ranges::any_of(plan.phases[0].moving, [&](RoadId road) {
    return turns_left(fixture.network, fixture.junction, road);
  });
  EXPECT_TRUE(has_left_mover);
  EXPECT_GT(plan.phases[0].moving.size(), through_only_movers);
}

TEST(JunctionPhases, AuthoredPhaseReportsDormantControllerReferences) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  Junction& junction = *fixture.network.junction(fixture.junction);
  const std::string live_member = junction.junction_controllers.front().controller_odr_id;

  SignalPhase phase;
  phase.name = "mixed";
  phase.duration = 15.0;
  phase.states.push_back(PhaseState{.controller_odr_id = live_member, .state = SignalState::Green});
  phase.states.push_back(PhaseState{.controller_odr_id = "ghost", .state = SignalState::Green});
  junction.phases = {phase};

  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  EXPECT_TRUE(plan.authored);
  ASSERT_EQ(plan.dormant_controller_odr_ids.size(), 1U);
  EXPECT_EQ(plan.dormant_controller_odr_ids.front(), "ghost");
  // The live member still resolves to a green head; the ghost lights nothing.
  ASSERT_EQ(plan.phases.size(), 1U);
  EXPECT_FALSE(plan.phases[0].signal_states.empty());
}

// --- phase_index_at ----------------------------------------------------------

TEST(JunctionPhases, PhaseIndexAtWrapsAndHandlesNegativeAndEmpty) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  ASSERT_EQ(plan.phases.size(), 4U); // starts 0, 20, 23, 43; cycle 46

  EXPECT_EQ(phase_index_at(plan, 0.0), 0U);
  EXPECT_EQ(phase_index_at(plan, 19.999), 0U);
  EXPECT_EQ(phase_index_at(plan, 20.0), 1U);
  EXPECT_EQ(phase_index_at(plan, 22.9), 1U);
  EXPECT_EQ(phase_index_at(plan, 23.0), 2U);
  EXPECT_EQ(phase_index_at(plan, 43.0), 3U);
  EXPECT_EQ(phase_index_at(plan, 45.9), 3U);
  EXPECT_EQ(phase_index_at(plan, 46.0), 0U); // wraps
  EXPECT_EQ(phase_index_at(plan, 46.0 + 20.5), 1U);
  EXPECT_EQ(phase_index_at(plan, -1.0), 3U); // -1 → 45 → last phase
  EXPECT_EQ(phase_index_at(plan, -46.0), 0U);

  const JunctionPhasePlan empty;
  EXPECT_EQ(phase_index_at(empty, 3.0), std::numeric_limits<std::size_t>::max());
}

// --- no-cycle junctions ------------------------------------------------------

TEST(JunctionPhases, StaticTemplateYieldsEmptyPlan) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::AllWayStop); // creates zero controllers
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  EXPECT_FALSE(plan.authored);
  EXPECT_TRUE(plan.phases.empty());
  EXPECT_TRUE(plan.controller_odr_ids.empty());
  EXPECT_NEAR(plan.cycle_duration, 0.0, 1e-12);
}

TEST(JunctionPhases, UnsignalizedJunctionYieldsEmptyPlan) {
  CrossFixture fixture;
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  EXPECT_FALSE(plan.authored);
  EXPECT_TRUE(plan.phases.empty());
  EXPECT_TRUE(plan.controller_odr_ids.empty());
}

TEST(JunctionPhases, SpanAndStaleJunctionsYieldEmptyPlan) {
  CrossFixture fixture;
  EXPECT_TRUE(junction_phases(fixture.network, JunctionId{}).phases.empty());

  RoadNetwork network;
  const RoadId through = author(network, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const std::vector<SpanArm> spans{SpanArm{.road = through, .s_start = 50.0, .s_end = 56.5}};
  auto command = roadmaker::edit::create_span_junction(network, spans);
  apply_command(network, command);
  const JunctionPhasePlan plan = junction_phases(network, sole_junction(network));
  EXPECT_FALSE(plan.authored);
  EXPECT_TRUE(plan.phases.empty());
}

// --- command layer (WP3): edits to the cycle ---------------------------------
//
// Each factory is a pure junction-value edit, so undo is byte-identical; the
// FIRST edit materializes the derived cycle sparsely into Junction::phases; the
// round-trip oracle forbids no-op commands, so every degenerate edit is
// rejected rather than applied.

TEST(SignalPhaseCommands, EachFactoryRoundTrips) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;
  const std::string ctrl1 = back_controller(net, jid); // Red in phase 0 (axis0)

  // set_phase_duration: change phase 0 from the derived 20 s.
  {
    auto command = set_phase_duration(net, jid, 0, 24.0);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
  // set_phase_state: green a controller that is Red this phase.
  {
    auto command = set_phase_state(net, jid, 0, ctrl1, SignalState::Green);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
  // add_signal_phase (append), duplicate_signal_phase, remove_signal_phase.
  {
    auto command = add_signal_phase(net, jid, 4);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
  {
    auto command = duplicate_signal_phase(net, jid, 0);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
  {
    auto command = remove_signal_phase(net, jid, 1);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
  // clear_signal_phases: needs an authored cycle first.
  {
    apply_command(net, set_phase_duration(net, jid, 0, 24.0));
    auto command = clear_signal_phases(net, jid);
    ASSERT_NE(command, nullptr);
    expect_command_round_trip(net, *command);
  }
}

TEST(SignalPhaseCommands, FirstEditMaterializesTheDerivedCycleSparsely) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  const JunctionPhasePlan derived = junction_phases(net, jid);
  ASSERT_FALSE(derived.authored);
  ASSERT_EQ(derived.phases.size(), 4U);
  ASSERT_TRUE(net.junction(jid)->phases.empty());

  apply_command(net, set_phase_duration(net, jid, 0, 24.0));

  // Stored sparsely: 4 phases, each green phase records only its one non-Red
  // controller, each yellow clearance one, and none is Red-padded.
  const Junction& junction = *net.junction(jid);
  ASSERT_EQ(junction.phases.size(), 4U);
  for (const SignalPhase& phase : junction.phases) {
    EXPECT_LE(phase.states.size(), 1U);
    for (const PhaseState& state : phase.states) {
      EXPECT_NE(state.state, SignalState::Red);
    }
  }

  // The query now reports it authored; the edited phase changed, every OTHER
  // phase still matches the derived shape exactly (name, duration, Red-filled
  // states).
  const JunctionPhasePlan authored = junction_phases(net, jid);
  EXPECT_TRUE(authored.authored);
  ASSERT_EQ(authored.phases.size(), 4U);
  EXPECT_NEAR(authored.phases[0].duration, 24.0, 1e-9);
  for (std::size_t i = 1; i < authored.phases.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(authored.phases[i].name, derived.phases[i].name);
    EXPECT_NEAR(authored.phases[i].duration, derived.phases[i].duration, 1e-9);
    ASSERT_EQ(authored.phases[i].states.size(), derived.phases[i].states.size());
    for (std::size_t r = 0; r < authored.phases[i].states.size(); ++r) {
      EXPECT_EQ(authored.phases[i].states[r].controller_odr_id,
                derived.phases[i].states[r].controller_odr_id);
      EXPECT_EQ(authored.phases[i].states[r].state, derived.phases[i].states[r].state);
    }
  }
}

TEST(SignalPhaseCommands, EveryNoOpAndInvalidEditIsRejected) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;
  const std::string ctrl0 = front_controller(net, jid); // Green in phase 0
  const std::string ctrl1 = back_controller(net, jid);  // Red in phase 0

  // set_phase_duration.
  expect_command_rejected(net, set_phase_duration(net, jid, 0, 20.0)); // equals derived
  expect_command_rejected(net, set_phase_duration(net, jid, 0, 0.0));  // <= 0
  expect_command_rejected(net, set_phase_duration(net, jid, 0, -4.0));
  expect_command_rejected(
      net, set_phase_duration(net, jid, 0, roadmaker::kMaxSignalPhaseDuration + 1.0));
  expect_command_rejected(net,
                          set_phase_duration(net, jid, 0, std::numeric_limits<double>::infinity()));
  expect_command_rejected(net, set_phase_duration(net, jid, 99, 24.0)); // index

  // set_phase_state.
  expect_command_rejected(net, set_phase_state(net, jid, 0, ctrl0, SignalState::Green)); // already
  expect_command_rejected(net, set_phase_state(net, jid, 0, ctrl1, SignalState::Red));   // already
  expect_command_rejected(net, set_phase_state(net, jid, 0, "ghost", SignalState::Green)); // member
  expect_command_rejected(net, set_phase_state(net, jid, 0, "bad id", SignalState::Green)); // token
  expect_command_rejected(net, set_phase_state(net, jid, 99, ctrl0, SignalState::Yellow));  // index

  // add / duplicate / remove: bad index.
  expect_command_rejected(net, add_signal_phase(net, jid, 99));
  expect_command_rejected(net, duplicate_signal_phase(net, jid, 99));
  expect_command_rejected(net, remove_signal_phase(net, jid, 99));

  // clear on a derived (unauthored) junction: nothing to clear.
  expect_command_rejected(net, clear_signal_phases(net, jid));

  // The whole junction is still purely derived — no edit slipped through.
  EXPECT_FALSE(junction_phases(net, jid).authored);
  EXPECT_TRUE(net.junction(jid)->phases.empty());
}

TEST(SignalPhaseCommands, StaleAndSpanJunctionsAreRejected) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  RoadNetwork& net = fixture.network;
  expect_command_rejected(net, set_phase_duration(net, JunctionId{}, 0, 24.0));
  expect_command_rejected(net, add_signal_phase(net, JunctionId{}, 0));
  expect_command_rejected(net, clear_signal_phases(net, JunctionId{}));

  // A span junction has no cycle at all.
  RoadNetwork span_net;
  const RoadId through = author(span_net, {Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}}, "1");
  const std::vector<SpanArm> spans{SpanArm{.road = through, .s_start = 50.0, .s_end = 56.5}};
  apply_command(span_net, roadmaker::edit::create_span_junction(span_net, spans));
  const JunctionId span = sole_junction(span_net);
  expect_command_rejected(span_net, set_phase_duration(span_net, span, 0, 24.0));
}

TEST(SignalPhaseCommands, RemovingTheLastRemainingPhaseIsRejected) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  apply_command(net, set_phase_duration(net, jid, 0, 24.0)); // materialize (4 phases)
  while (phase_count(net, jid) > 1) {
    apply_command(net, remove_signal_phase(net, jid, phase_count(net, jid) - 1));
  }
  ASSERT_EQ(phase_count(net, jid), 1U);
  // The last phase cannot be removed — a zero-phase authored cycle is
  // unrepresentable; clear_signal_phases is the way back to derivation.
  expect_command_rejected(net, remove_signal_phase(net, jid, 0));
}

TEST(SignalPhaseCommands, CannotGrowBeyondTheMaxPhaseCount) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  apply_command(net, set_phase_duration(net, jid, 0, 24.0)); // materialize
  while (phase_count(net, jid) < roadmaker::kMaxSignalPhases) {
    apply_command(net, add_signal_phase(net, jid, phase_count(net, jid)));
  }
  ASSERT_EQ(phase_count(net, jid), roadmaker::kMaxSignalPhases);
  expect_command_rejected(net, add_signal_phase(net, jid, 0));
  expect_command_rejected(net, duplicate_signal_phase(net, jid, 0));
}

TEST(SignalPhaseCommands, ClearReturnsToDerivedAndPreEditBytes) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  const std::string pristine = snapshot_xodr(net); // derived — no rm:phases bytes

  apply_command(net, set_phase_duration(net, jid, 0, 24.0));
  EXPECT_TRUE(junction_phases(net, jid).authored);
  EXPECT_NE(snapshot_xodr(net), pristine);

  apply_command(net, clear_signal_phases(net, jid));
  EXPECT_FALSE(junction_phases(net, jid).authored);
  EXPECT_TRUE(net.junction(jid)->phases.empty());
  expect_network_matches(net, pristine); // byte-identical to before the first edit
}

TEST(SignalPhaseCommands, SettingAControllerRedErasesItsSparsePair) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  apply_command(net, set_phase_duration(net, jid, 0, 24.0)); // materialize
  ASSERT_EQ(net.junction(jid)->phases[0].states.size(), 1U);
  const std::string green_ctrl = net.junction(jid)->phases[0].states.front().controller_odr_id;
  EXPECT_EQ(net.junction(jid)->phases[0].states.front().state, SignalState::Green);

  apply_command(net, set_phase_state(net, jid, 0, green_ctrl, SignalState::Red));
  // Red is the omitted default: the pair is erased, not stored as Red.
  EXPECT_TRUE(net.junction(jid)->phases[0].states.empty());
  // The query still reports it Red via Red-fill.
  const JunctionPhasePlan plan = junction_phases(net, jid);
  const auto& row = plan.phases[0].states;
  const auto entry =
      std::ranges::find_if(row, [&](const auto& s) { return s.controller_odr_id == green_ctrl; });
  ASSERT_NE(entry, row.end());
  EXPECT_EQ(entry->state, SignalState::Red);
}

TEST(SignalPhaseCommands, AuthoredStateSurvivesControllerEraseAndRestoreByString) {
  CrossFixture fixture;
  fixture.signalize(SignalizeTemplate::TwoPhase);
  const JunctionId jid = fixture.junction;
  RoadNetwork& net = fixture.network;

  // Materialize the derived cycle: phase 0 greens ctrl0, so an authored
  // PhaseState now names it by string.
  const std::string ctrl0 = front_controller(net, jid);
  apply_command(net, set_phase_duration(net, jid, 0, 24.0));
  ASSERT_EQ(net.junction(jid)->phases[0].states.front().controller_odr_id, ctrl0);

  // Erase that controller from the arena. The authored PhaseState is keyed by
  // STRING, so it stays put and goes dormant — reported, never resolved.
  ControllerId ctrl0_id;
  Controller ctrl0_value;
  net.for_each_controller([&](ControllerId id, const Controller& controller) {
    if (controller.odr_id == ctrl0) {
      ctrl0_id = id;
      ctrl0_value = controller;
    }
  });
  ASSERT_TRUE(ctrl0_id.is_valid());
  ASSERT_TRUE(net.erase_controller(ctrl0_id));

  JunctionPhasePlan plan = junction_phases(net, jid);
  EXPECT_EQ(std::ranges::find(plan.controller_odr_ids, ctrl0), plan.controller_odr_ids.end());
  EXPECT_NE(std::ranges::find(plan.dormant_controller_odr_ids, ctrl0),
            plan.dormant_controller_odr_ids.end());

  // Restoring a controller with the SAME odr id re-binds the authored state by
  // string match: it is a live member again and no longer dormant.
  net.add_controller(ctrl0_value);
  plan = junction_phases(net, jid);
  EXPECT_NE(std::ranges::find(plan.controller_odr_ids, ctrl0), plan.controller_odr_ids.end());
  EXPECT_TRUE(plan.dormant_controller_odr_ids.empty());
}
