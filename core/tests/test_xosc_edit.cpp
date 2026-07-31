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

// The scenario command layer (p8-s1 PR-D, issue #245) — osc/edit.hpp.
//
// THE ORACLE IS `write_xosc`, NOT FIELD COMPARISON. The contract every command
// is held to is that apply -> revert leaves the document's SERIALIZED FORM
// byte-identical, which is the same invariant `edit::Command` carries against
// `write_xodr` and the same one both golden-workflow replays fingerprint with.
// Comparing fields instead would pass on a command that restored the values
// but reordered a vector, moved a preserved fragment, or dropped an optional —
// none of which a struct-wise check sees and all of which change the file.
//
// So `fingerprint()` below is the assertion in nearly every test here, and the
// undo x10 / redo x10 loop is the strongest form of it.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
using roadmaker::Waypoint;
using roadmaker::edit::signalize_junction;
using roadmaker::edit::SignalizeTemplate;
using roadmaker::osc::Phase;
using roadmaker::osc::Private;
using roadmaker::osc::PrivateAction;
using roadmaker::osc::Scenario;
using roadmaker::osc::ScenarioObject;
using roadmaker::osc::TeleportAction;
using roadmaker::osc::TrafficSignalController;
using roadmaker::osc::TrafficSignalState;
using roadmaker::osc::Vehicle;
using roadmaker::osc::WorldPosition;
using roadmaker::osc::write_xosc;
using roadmaker::osc::edit::add_scenario_object;
using roadmaker::osc::edit::Command;
using roadmaker::osc::edit::remove_scenario_object;
using roadmaker::osc::edit::ScenarioStack;
using roadmaker::osc::edit::set_entity_init_position;
using roadmaker::osc::edit::set_logic_file;
using roadmaker::osc::edit::sync_traffic_signals;

namespace {

/// The document's serialized form — the only thing the byte-identity contract
/// is defined over.
std::string fingerprint(const Scenario& scenario) {
  auto written = write_xosc(scenario);
  if (!written.has_value()) {
    throw std::runtime_error("write_xosc: " + written.error().message);
  }
  return *written;
}

ScenarioObject vehicle_named(std::string name) {
  Vehicle car;
  car.name = name;
  car.bounding_box.width = 2.0;
  car.bounding_box.length = 5.0;
  car.bounding_box.height = 1.5;
  ScenarioObject object;
  object.name = std::move(name);
  object.entity_object = std::move(car);
  return object;
}

/// A scenario with one entity, which is the minimum most factories need.
Scenario one_actor() {
  Scenario scenario;
  scenario.entities.scenario_objects.push_back(vehicle_named("Ego"));
  return scenario;
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

struct SignalizedCross {
  RoadNetwork network;
  JunctionId junction;

  SignalizedCross() {
    const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
    auto junction_command = roadmaker::edit::create_junction(network, ends);
    if (!junction_command->apply(network).has_value()) {
      throw std::runtime_error("create_junction failed");
    }
    network.for_each_junction([this](JunctionId id, const Junction&) { junction = id; });
    auto signalize = signalize_junction(network, junction, {.tmpl = SignalizeTemplate::TwoPhase});
    if (!signalize->apply(network).has_value()) {
      throw std::runtime_error("signalize_junction failed");
    }
  }
};

/// apply -> revert leaves the SERIALIZED document untouched. The contract, in
/// one helper, so no test can accidentally assert something weaker.
void expect_round_trips(Scenario& scenario, std::unique_ptr<Command> command) {
  ASSERT_NE(command, nullptr) << "a factory must never return nullptr";
  const std::string before = fingerprint(scenario);

  const auto applied = command->apply(scenario);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_NE(fingerprint(scenario), before) << "the command changed nothing — the test is vacuous";

  const auto reverted = command->revert(scenario);
  ASSERT_TRUE(reverted.has_value()) << reverted.error().message;
  EXPECT_EQ(fingerprint(scenario), before);
}

/// A refused command must leave the document EXACTLY as it was — a factory
/// that validated and then half-mutated is the failure this catches.
void expect_refused(Scenario& scenario, std::unique_ptr<Command> command) {
  ASSERT_NE(command, nullptr) << "a refusal is a Command, never nullptr";
  const std::string before = fingerprint(scenario);
  const auto applied = command->apply(scenario);
  EXPECT_FALSE(applied.has_value()) << "the command was expected to refuse";
  EXPECT_EQ(fingerprint(scenario), before);
}

} // namespace

// --- the round trip, per factory --------------------------------------------

TEST(XoscEdit, SetLogicFileRoundTrips) {
  Scenario scenario = one_actor();
  expect_round_trips(scenario, set_logic_file(scenario, "town.xodr"));
}

TEST(XoscEdit, AddScenarioObjectRoundTrips) {
  Scenario scenario = one_actor();
  expect_round_trips(scenario, add_scenario_object(scenario, vehicle_named("Target")));
}

TEST(XoscEdit, RemoveScenarioObjectRoundTrips) {
  Scenario scenario = one_actor();
  scenario.entities.scenario_objects.push_back(vehicle_named("Target"));
  expect_round_trips(scenario, remove_scenario_object(scenario, "Ego"));
}

TEST(XoscEdit, SetEntityInitPositionRoundTrips) {
  Scenario scenario = one_actor();
  WorldPosition position;
  position.x = -40.0;
  position.y = -1.75;
  position.h = 0.0;
  expect_round_trips(scenario, set_entity_init_position(scenario, "Ego", position));
}

TEST(XoscEdit, SyncTrafficSignalsRoundTrips) {
  const SignalizedCross fixture;
  Scenario scenario = one_actor();
  expect_round_trips(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction));
}

// --- the round trip when the edit OVERWRITES rather than adds ---------------

TEST(XoscEdit, SetEntityInitPositionRoundTripsWhenThePrivateAlreadyExists) {
  // The first placement CREATES the <Private>; the second OVERWRITES a
  // position. Those are two different code paths and only the second is
  // covered by a naive "append then pop" undo.
  Scenario scenario = one_actor();
  ScenarioStack stack;
  WorldPosition first;
  first.x = 1.0;
  ASSERT_TRUE(stack.push(scenario, set_entity_init_position(scenario, "Ego", first)).has_value());

  WorldPosition second;
  second.x = 2.0;
  second.h = 1.57;
  expect_round_trips(scenario, set_entity_init_position(scenario, "Ego", second));
}

TEST(XoscEdit, SyncTrafficSignalsRoundTripsWhenControllersAlreadyExist) {
  // Syncing twice overwrites in place rather than appending; the second sync's
  // undo has to restore the FIRST sync's list, not an empty one.
  const SignalizedCross fixture;
  Scenario scenario = one_actor();
  ScenarioStack stack;
  ASSERT_TRUE(
      stack.push(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction))
          .has_value());
  const std::size_t after_first = scenario.road_network.traffic_signal_controllers.size();
  ASSERT_GT(after_first, 0U);

  const std::string before = fingerprint(scenario);
  auto again = sync_traffic_signals(scenario, fixture.network, fixture.junction);
  ASSERT_TRUE(again->apply(scenario).has_value());
  EXPECT_EQ(scenario.road_network.traffic_signal_controllers.size(), after_first)
      << "a second sync of the same junction must overwrite, never duplicate";
  EXPECT_EQ(fingerprint(scenario), before) << "syncing the same junction twice is idempotent";
  ASSERT_TRUE(again->revert(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
}

TEST(XoscEdit, SyncTrafficSignalsKeepsControllersItDoesNotOwn) {
  // A scenario may reference several junctions. A wholesale replace would make
  // syncing the second delete the first — silently, and only visible in the
  // written file.
  const SignalizedCross fixture;
  Scenario scenario = one_actor();
  TrafficSignalController foreign;
  foreign.name = "zzz_someone_elses_junction";
  Phase phase;
  phase.name = "go";
  phase.duration = 10.0;
  TrafficSignalState state;
  state.traffic_signal_id = "9001";
  state.state = "green";
  phase.signal_states.push_back(state);
  foreign.phases.push_back(std::move(phase));
  scenario.road_network.traffic_signal_controllers.push_back(std::move(foreign));

  ScenarioStack stack;
  ASSERT_TRUE(
      stack.push(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction))
          .has_value());

  bool kept = false;
  for (const TrafficSignalController& controller :
       scenario.road_network.traffic_signal_controllers) {
    kept = kept || controller.name == "zzz_someone_elses_junction";
  }
  EXPECT_TRUE(kept) << "the sync deleted a controller belonging to another junction";
  EXPECT_GT(scenario.road_network.traffic_signal_controllers.size(), 1U);
}

TEST(XoscEdit, SyncTrafficSignalsCarriesOverDelayAndReferenceItDoesNotProduce) {
  // @delay and @reference are authored RELATIONSHIPS between controllers. The
  // decomposition does not produce them, so overwriting a controller with a
  // fresh one would drop an authored fact on every re-sync.
  const SignalizedCross fixture;
  Scenario scenario = one_actor();
  ScenarioStack stack;
  ASSERT_TRUE(
      stack.push(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction))
          .has_value());
  ASSERT_GE(scenario.road_network.traffic_signal_controllers.size(), 2U);

  const std::string anchor = scenario.road_network.traffic_signal_controllers.front().name;
  auto& dependent = scenario.road_network.traffic_signal_controllers.back();
  dependent.reference = anchor;
  dependent.delay = 4.5;
  const std::string dependent_name = dependent.name;

  ASSERT_TRUE(
      stack.push(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction))
          .has_value());
  for (const TrafficSignalController& controller :
       scenario.road_network.traffic_signal_controllers) {
    if (controller.name == dependent_name) {
      EXPECT_EQ(controller.reference, anchor);
      ASSERT_TRUE(controller.delay.has_value());
      EXPECT_DOUBLE_EQ(*controller.delay, 4.5);
    }
  }
}

// --- refusals ----------------------------------------------------------------

TEST(XoscEdit, AddScenarioObjectRefusesADuplicateName) {
  Scenario scenario = one_actor();
  expect_refused(scenario, add_scenario_object(scenario, vehicle_named("Ego")));
}

TEST(XoscEdit, AddScenarioObjectRefusesAnEmptyName) {
  Scenario scenario = one_actor();
  expect_refused(scenario, add_scenario_object(scenario, ScenarioObject{}));
}

TEST(XoscEdit, RemoveScenarioObjectRefusesANameNoEntityCarries) {
  Scenario scenario = one_actor();
  expect_refused(scenario, remove_scenario_object(scenario, "NoSuchActor"));
}

TEST(XoscEdit, SetEntityInitPositionRefusesAnUnknownEntity) {
  Scenario scenario = one_actor();
  expect_refused(scenario, set_entity_init_position(scenario, "NoSuchActor", WorldPosition{}));
}

TEST(XoscEdit, SetLogicFileRefusesAnEmptyFilepath) {
  Scenario scenario = one_actor();
  expect_refused(scenario, set_logic_file(scenario, ""));
}

TEST(XoscEdit, SyncTrafficSignalsRefusesAJunctionWithNoCycle) {
  // Refusing rather than syncing NOTHING is the whole point: an unsignalized
  // junction interpreted as an empty controller list would delete an authored
  // one, and the caller would see success.
  RoadNetwork network;
  Scenario scenario = one_actor();
  expect_refused(scenario, sync_traffic_signals(scenario, network, JunctionId{}));
}

TEST(XoscEdit, ARefusedCommandIsNotRecordedOnTheStack) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  const auto pushed = stack.push(scenario, add_scenario_object(scenario, vehicle_named("Ego")));
  EXPECT_FALSE(pushed.has_value());
  EXPECT_EQ(stack.size(), 0U);
  EXPECT_FALSE(stack.can_undo());
}

// --- removal takes the init actions with it ---------------------------------

TEST(XoscEdit, RemovingAnActorAlsoRemovesItsInitPrivate) {
  // Left behind, the <Private> is a dangling entityRef, which write_xosc
  // refuses — so a removal that reported success would leave a document that
  // cannot be saved at all.
  Scenario scenario = one_actor();
  ScenarioStack stack;
  WorldPosition position;
  position.x = 5.0;
  ASSERT_TRUE(
      stack.push(scenario, set_entity_init_position(scenario, "Ego", position)).has_value());
  ASSERT_EQ(scenario.storyboard.init.actions.privates.size(), 1U);

  ASSERT_TRUE(stack.push(scenario, remove_scenario_object(scenario, "Ego")).has_value());
  EXPECT_TRUE(scenario.storyboard.init.actions.privates.empty());
  EXPECT_TRUE(write_xosc(scenario).has_value()) << "the document must stay writable";

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(scenario.storyboard.init.actions.privates.size(), 1U);
}

TEST(XoscEdit, RemovingAnActorRestoresItAtItsOriginalIndex) {
  // Restoring at the END would reorder <Entities>, which is a different file:
  // the writer emits vectors in vector order and has no sort anywhere.
  Scenario scenario;
  scenario.entities.scenario_objects.push_back(vehicle_named("A"));
  scenario.entities.scenario_objects.push_back(vehicle_named("B"));
  scenario.entities.scenario_objects.push_back(vehicle_named("C"));
  const std::string before = fingerprint(scenario);

  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, remove_scenario_object(scenario, "A")).has_value());
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
  EXPECT_EQ(scenario.entities.scenario_objects.front().name, "A");
}

// --- the stack ---------------------------------------------------------------

TEST(XoscEdit, UndoTenTimesRedoTenTimesIsAFixedPoint) {
  // GW-6's fingerprint discipline, and the reason this layer exists at all:
  // a scenario must survive the same undo/redo hammering `write_xodr` does.
  const SignalizedCross fixture;
  Scenario scenario;
  ScenarioStack stack;

  const std::string empty = fingerprint(scenario);
  ASSERT_TRUE(
      stack.push(scenario, add_scenario_object(scenario, vehicle_named("Ego"))).has_value());
  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "town.xodr")).has_value());
  WorldPosition position;
  position.x = -40.0;
  position.y = -1.75;
  ASSERT_TRUE(
      stack.push(scenario, set_entity_init_position(scenario, "Ego", position)).has_value());
  ASSERT_TRUE(
      stack.push(scenario, sync_traffic_signals(scenario, fixture.network, fixture.junction))
          .has_value());
  const std::string full = fingerprint(scenario);
  ASSERT_NE(full, empty);

  for (int round = 0; round < 10; ++round) {
    while (stack.can_undo()) {
      ASSERT_TRUE(stack.undo(scenario).has_value());
    }
    EXPECT_EQ(fingerprint(scenario), empty) << "round " << round;
    while (stack.can_redo()) {
      ASSERT_TRUE(stack.redo(scenario).has_value());
    }
    EXPECT_EQ(fingerprint(scenario), full) << "round " << round;
  }
}

TEST(XoscEdit, PushingTruncatesTheRedoTail) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "a.xodr")).has_value());
  ASSERT_TRUE(stack.push(scenario, add_scenario_object(scenario, vehicle_named("B"))).has_value());
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_TRUE(stack.can_redo());

  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "c.xodr")).has_value());
  EXPECT_FALSE(stack.can_redo());
  EXPECT_EQ(stack.size(), 2U);
}

TEST(XoscEdit, DepthLimitDropsOldestAndClampsAtOne) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  stack.set_depth_limit(0);
  EXPECT_EQ(stack.depth_limit(), 1U);

  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "a.xodr")).has_value());
  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "b.xodr")).has_value());
  EXPECT_EQ(stack.size(), 1U);
  // The dropped command's edit STAYS APPLIED — it just becomes un-undoable.
  ASSERT_TRUE(scenario.road_network.logic_file.has_value());
  EXPECT_EQ(scenario.road_network.logic_file->filepath, "b.xodr");
  ASSERT_TRUE(stack.undo(scenario).has_value());
  ASSERT_TRUE(scenario.road_network.logic_file.has_value());
  EXPECT_EQ(scenario.road_network.logic_file->filepath, "a.xodr");
  EXPECT_FALSE(stack.can_undo());
}

TEST(XoscEdit, ClearForgetsHistoryWithoutTouchingTheDocument) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "a.xodr")).has_value());
  const std::string applied = fingerprint(scenario);

  stack.clear();
  EXPECT_EQ(stack.size(), 0U);
  EXPECT_FALSE(stack.can_undo());
  EXPECT_EQ(fingerprint(scenario), applied);
}

TEST(XoscEdit, UndoAndRedoWithNothingToDoAreRefusedNotIgnored) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  EXPECT_FALSE(stack.undo(scenario).has_value());
  EXPECT_FALSE(stack.redo(scenario).has_value());
}

TEST(XoscEdit, PushingANullCommandIsRefused) {
  Scenario scenario = one_actor();
  ScenarioStack stack;
  EXPECT_FALSE(stack.push(scenario, nullptr).has_value());
}

// --- findings ----------------------------------------------------------------

TEST(XoscEdit, LastFindingsIsHowAHeadlessCallerLearnsWhatTheSyncCopedWith) {
  // push() takes OWNERSHIP of the command, so this accessor is the only way a
  // headless caller can read the decomposition's findings at all — the same
  // problem EditStack grew last_follow_records() for. A clean sync says
  // nothing; a sync that had to omit something must say so.
  SignalizedCross clean;
  Scenario scenario = one_actor();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, sync_traffic_signals(scenario, clean.network, clean.junction))
                  .has_value());
  EXPECT_TRUE(stack.last_findings().empty()) << "a clean junction must not invent findings";

  // Now one whose head carries an empty @id, which the decomposition omits and
  // reports (see XoscDecompose.AnEmptySignalIdIsReportedAndOmitted).
  SignalizedCross damaged;
  std::string blanked;
  damaged.network.for_each_signal([&blanked](roadmaker::SignalId, Signal& signal) {
    if (blanked.empty()) {
      blanked = signal.odr_id;
      signal.odr_id.clear();
    }
  });
  ASSERT_FALSE(blanked.empty());
  damaged.network.for_each_controller(
      [&blanked](roadmaker::ControllerId, roadmaker::Controller& controller) {
        for (roadmaker::Control& control : controller.controls) {
          if (control.signal_odr_id == blanked) {
            control.signal_odr_id.clear();
          }
        }
      });

  Scenario second = one_actor();
  ScenarioStack damaged_stack;
  ASSERT_TRUE(
      damaged_stack.push(second, sync_traffic_signals(second, damaged.network, damaged.junction))
          .has_value());
  EXPECT_FALSE(damaged_stack.last_findings().empty())
      << "the sync omitted a signal state and said nothing";

  // Findings survive an undo: they describe what applying does, and a redo
  // does it again (the follow_records lifetime).
  ASSERT_TRUE(damaged_stack.undo(second).has_value());
  EXPECT_TRUE(damaged_stack.last_findings().empty()) << "nothing is applied, so nothing to report";
  ASSERT_TRUE(damaged_stack.redo(second).has_value());
  EXPECT_FALSE(damaged_stack.last_findings().empty());
}

// --- an unapplied command must not write a default snapshot -----------------

TEST(XoscEdit, RevertingACommandThatWasNeverAppliedIsANoOp) {
  Scenario scenario = one_actor();
  scenario.road_network.logic_file =
      roadmaker::osc::FileRef{.filepath = "town.xodr", .preserved = {}};
  const std::string before = fingerprint(scenario);

  auto command = set_logic_file(scenario, "other.xodr");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->revert(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before)
      << "reverting an unapplied command wrote its default snapshot into the document";
}

TEST(XoscEdit, EveryCommandHasANonEmptyName) {
  const SignalizedCross fixture;
  Scenario scenario = one_actor();
  EXPECT_FALSE(set_logic_file(scenario, "a.xodr")->name().empty());
  EXPECT_FALSE(add_scenario_object(scenario, vehicle_named("B"))->name().empty());
  EXPECT_FALSE(remove_scenario_object(scenario, "Ego")->name().empty());
  EXPECT_FALSE(set_entity_init_position(scenario, "Ego", WorldPosition{})->name().empty());
  EXPECT_FALSE(sync_traffic_signals(scenario, fixture.network, fixture.junction)->name().empty());
  // A refusal is a Command too, and its name is what an undo menu would show.
  EXPECT_FALSE(set_logic_file(scenario, "")->name().empty());
}
