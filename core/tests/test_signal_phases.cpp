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
using roadmaker::edit::Command;
using roadmaker::edit::signalize_junction;
using roadmaker::edit::SignalizeTemplate;

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
