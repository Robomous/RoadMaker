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
#include "roadmaker/osc/catalog.hpp"
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
#include <string_view>
#include <utility>
#include <variant>
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
using roadmaker::osc::BoundingBox;
using roadmaker::osc::LanePosition;
using roadmaker::osc::LongitudinalAction;
using roadmaker::osc::Phase;
using roadmaker::osc::Position;
using roadmaker::osc::Private;
using roadmaker::osc::PrivateAction;
using roadmaker::osc::RouteWaypoint;
using roadmaker::osc::Scenario;
using roadmaker::osc::ScenarioObject;
using roadmaker::osc::SpeedAction;
using roadmaker::osc::TeleportAction;
using roadmaker::osc::TrafficSignalController;
using roadmaker::osc::TrafficSignalState;
using roadmaker::osc::Vehicle;
using roadmaker::osc::WorldPosition;
using roadmaker::osc::write_xosc;
using roadmaker::osc::edit::add_scenario_object;
using roadmaker::osc::edit::assign_route;
using roadmaker::osc::edit::clear_route;
using roadmaker::osc::edit::Command;
using roadmaker::osc::edit::insert_route_waypoint;
using roadmaker::osc::edit::place_scenario_object;
using roadmaker::osc::edit::remove_route_waypoint;
using roadmaker::osc::edit::remove_scenario_object;
using roadmaker::osc::edit::rename_scenario_object;
using roadmaker::osc::edit::ScenarioStack;
using roadmaker::osc::edit::set_entity_init_pose;
using roadmaker::osc::edit::set_entity_init_position;
using roadmaker::osc::edit::set_entity_init_speed;
using roadmaker::osc::edit::set_logic_file;
using roadmaker::osc::edit::set_route_waypoint;
using roadmaker::osc::edit::set_scenario_object_bounding_box;
using roadmaker::osc::edit::sync_traffic_signals;

namespace osc = roadmaker::osc;

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

/// A lane position on road "1", lane "-1" — the shape the Actor tool authors.
LanePosition lane_at(double s) {
  LanePosition lane;
  lane.road_id = "1";
  lane.lane_id = "-1";
  lane.s = s;
  lane.offset = 0.0;
  return lane;
}

BoundingBox box_of(double width, double length, double height) {
  BoundingBox box;
  box.width = width;
  box.length = length;
  box.height = height;
  box.center_z = height / 2.0;
  return box;
}

/// A scenario with one entity, which is the minimum most factories need.
Scenario one_actor() {
  Scenario scenario;
  scenario.entities.scenario_objects.push_back(vehicle_named("Ego"));
  return scenario;
}

/// `one_actor()` plus a `<LogicFile>`.
///
/// ★ REQUIRED FOR ANY TEST THAT USES A LANE OR ROAD POSITION.
/// `asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network` is
/// a "shall not": a `<LanePosition>` in a scenario that links no road network
/// names a `roadId` nothing can resolve, so `write_xosc` refuses it — and
/// `fingerprint()` throws rather than returning, which is what makes the
/// omission loud instead of subtle.
Scenario one_actor_on_a_network() {
  Scenario scenario = one_actor();
  scenario.road_network.logic_file =
      roadmaker::osc::FileRef{.filepath = "town.xodr", .preserved = {}};
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
  // p8-s2 (#246).
  EXPECT_FALSE(set_entity_init_pose(scenario, "Ego", Position{lane_at(12.5)})->name().empty());
  EXPECT_FALSE(
      place_scenario_object(scenario, vehicle_named("C"), Position{lane_at(1.0)})->name().empty());
  EXPECT_FALSE(set_entity_init_speed(scenario, "Ego", 13.89)->name().empty());
  EXPECT_FALSE(rename_scenario_object(scenario, "Ego", "Hero")->name().empty());
  EXPECT_FALSE(
      set_scenario_object_bounding_box(scenario, "Ego", box_of(2, 5, 1.5))->name().empty());
  // A refusal is a Command too, and its name is what an undo menu would show.
  EXPECT_FALSE(set_logic_file(scenario, "")->name().empty());
}

// --- p8-s2 (#246): lane placement, speed, rename, resize ---------------------

TEST(XoscEdit, PlaceScenarioObjectRoundTrips) {
  Scenario scenario = one_actor_on_a_network();
  expect_round_trips(
      scenario, place_scenario_object(scenario, vehicle_named("Target"), Position{lane_at(20.0)}));
}

TEST(XoscEdit, SetEntityInitPoseOnALaneRoundTrips) {
  Scenario scenario = one_actor_on_a_network();
  expect_round_trips(scenario, set_entity_init_pose(scenario, "Ego", Position{lane_at(12.5)}));
}

TEST(XoscEdit, SetEntityInitSpeedRoundTrips) {
  Scenario scenario = one_actor_on_a_network();
  expect_round_trips(scenario, set_entity_init_speed(scenario, "Ego", 13.89));
}

TEST(XoscEdit, SetEntityInitSpeedRoundTripsWhenASpeedIsAlreadySet) {
  Scenario scenario = one_actor_on_a_network();
  ASSERT_TRUE(set_entity_init_speed(scenario, "Ego", 10.0)->apply(scenario).has_value());
  expect_round_trips(scenario, set_entity_init_speed(scenario, "Ego", 25.0));
}

TEST(XoscEdit, RenameScenarioObjectRoundTrips) {
  Scenario scenario = one_actor_on_a_network();
  ASSERT_TRUE(
      set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)})->apply(scenario).has_value());
  expect_round_trips(scenario, rename_scenario_object(scenario, "Ego", "Hero"));
}

TEST(XoscEdit, SetBoundingBoxRoundTrips) {
  Scenario scenario = one_actor_on_a_network();
  expect_round_trips(scenario,
                     set_scenario_object_bounding_box(scenario, "Ego", box_of(3, 8, 2.5)));
}

TEST(XoscEdit, PlacingAnActorIsONEUndoEntryAndNotTwo) {
  // ★ The reason place_scenario_object exists at all. Placing an actor is one
  // gesture; if it were add + set_pose it would take TWO undos to put the
  // document back, and the user's single click would need two Ctrl+Z.
  Scenario scenario;
  ScenarioStack stack;
  const std::string before = fingerprint(scenario);

  ASSERT_TRUE(
      stack
          .push(scenario,
                place_scenario_object(scenario, vehicle_named("Car1"), Position{lane_at(30.0)}))
          .has_value());
  EXPECT_EQ(stack.size(), 1U) << "placing an actor recorded more than one command";
  EXPECT_EQ(scenario.entities.scenario_objects.size(), 1U);
  EXPECT_EQ(scenario.storyboard.init.actions.privates.size(), 1U)
      << "the entity was added without its placement";

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before)
      << "ONE undo did not fully unwind a placement — it is two commands wearing one coat";
}

TEST(XoscEdit, RenamingAnActorRewritesEveryEntityRef) {
  // ★ THE TRAP. @name is the key <Private> resolves through, so renaming the
  // entity alone leaves a dangling entityRef — and write_xosc REFUSES that, so
  // a rename that appeared to succeed would make the document unsavable.
  Scenario scenario = one_actor_on_a_network();
  ASSERT_TRUE(
      set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)})->apply(scenario).has_value());

  auto command = rename_scenario_object(scenario, "Ego", "Hero");
  ASSERT_TRUE(command->apply(scenario).has_value());

  EXPECT_EQ(scenario.entities.scenario_objects[0].name, "Hero");
  EXPECT_EQ(scenario.storyboard.init.actions.privates[0].entity_ref, "Hero")
      << "the entityRef still names the old entity, which write_xosc refuses";

  // The real assertion: the document is still writable. A field check alone
  // would pass on a rename that broke a reference somewhere else.
  const auto written = write_xosc(scenario);
  EXPECT_TRUE(written.has_value())
      << "the renamed document is unwritable: " << written.error().message;
}

TEST(XoscEdit, RenamingDoesNotDisturbAReferenceThatAlreadyHadTheNewName) {
  // The revert must not sweep to_ -> from_ blindly: a <Private> that already
  // said "Hero" before the rename was never this command's to touch, and
  // rewriting it on undo would silently re-point a second actor's placement.
  Scenario scenario = one_actor_on_a_network();
  scenario.entities.scenario_objects.push_back(vehicle_named("Hero"));
  ASSERT_TRUE(
      set_entity_init_pose(scenario, "Hero", Position{lane_at(1.0)})->apply(scenario).has_value());
  ASSERT_TRUE(
      set_entity_init_pose(scenario, "Ego", Position{lane_at(2.0)})->apply(scenario).has_value());
  ASSERT_TRUE(remove_scenario_object(scenario, "Hero")->apply(scenario).has_value());

  // "Hero" is free again but a Private for "Ego" exists; rename Ego -> Hero.
  const std::string before = fingerprint(scenario);
  auto command = rename_scenario_object(scenario, "Ego", "Hero");
  ASSERT_TRUE(command->apply(scenario).has_value());
  ASSERT_TRUE(command->revert(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
}

TEST(XoscEdit, PlaceScenarioObjectRefusesADuplicateName) {
  Scenario scenario = one_actor_on_a_network();
  expect_refused(scenario,
                 place_scenario_object(scenario, vehicle_named("Ego"), Position{lane_at(1.0)}));
}

TEST(XoscEdit, ALanePositionThatNamesNoRoadIsRefusedAtPlacementNotAtSave) {
  // validate_scenario would catch this too — but only when the user tries to
  // save, an hour after the placement that caused it. Refusing in the factory
  // is what makes the message about the placement.
  Scenario scenario = one_actor_on_a_network();
  osc::LanePosition nowhere = lane_at(10.0);
  nowhere.road_id.clear();
  expect_refused(scenario, set_entity_init_pose(scenario, "Ego", Position{nowhere}));
}

TEST(XoscEdit, ALanePositionThatNamesNoLaneIsRefused) {
  Scenario scenario = one_actor_on_a_network();
  osc::LanePosition nowhere = lane_at(10.0);
  nowhere.lane_id.clear();
  expect_refused(scenario, set_entity_init_pose(scenario, "Ego", Position{nowhere}));
}

TEST(XoscEdit, ANegativeStationIsRefused) {
  Scenario scenario = one_actor_on_a_network();
  expect_refused(scenario, set_entity_init_pose(scenario, "Ego", Position{lane_at(-1.0)}));
}

TEST(XoscEdit, ANegativeSpeedIsRefusedAndNotClamped) {
  // Clamping to 0 would hide the slip. The user asked for something impossible
  // and is told so.
  Scenario scenario = one_actor_on_a_network();
  expect_refused(scenario, set_entity_init_speed(scenario, "Ego", -5.0));
}

TEST(XoscEdit, RenameRefusesAnEmptyOrTakenName) {
  Scenario scenario = one_actor_on_a_network();
  scenario.entities.scenario_objects.push_back(vehicle_named("Hero"));
  expect_refused(scenario, rename_scenario_object(scenario, "Ego", ""));
  expect_refused(scenario, rename_scenario_object(scenario, "Ego", "Hero"));
  expect_refused(scenario, rename_scenario_object(scenario, "Nobody", "Someone"));
}

TEST(XoscEdit, ANonPositiveBoundingBoxIsRefused) {
  Scenario scenario = one_actor_on_a_network();
  expect_refused(scenario, set_scenario_object_bounding_box(scenario, "Ego", box_of(0, 5, 1.5)));
  expect_refused(scenario, set_scenario_object_bounding_box(scenario, "Ego", box_of(2, -5, 1.5)));
  expect_refused(scenario, set_scenario_object_bounding_box(scenario, "Ego", box_of(2, 5, 0)));
}

TEST(XoscEdit, RetypingAPositionReportsTheDroppedPreservedTierRatherThanDroppingItSilently) {
  // ★ A <WorldPosition>'s foreign attributes name a DIFFERENT element and
  // cannot ride onto a <LanePosition>. Dropping them is correct; dropping them
  // in silence is the failure ADR-0014 §6 exists to prevent.
  Scenario scenario = one_actor_on_a_network();
  WorldPosition world;
  world.preserved.attributes.emplace_back("vendorFlag", "1");
  ASSERT_TRUE(set_entity_init_position(scenario, "Ego", world)->apply(scenario).has_value());

  auto command = set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)});
  ASSERT_TRUE(command->apply(scenario).has_value());

  ASSERT_FALSE(command->findings().empty()) << "a preserved tier was dropped and nothing said so";
  EXPECT_NE(command->findings()[0].message.find("WorldPosition"), std::string::npos)
      << command->findings()[0].message;
  EXPECT_EQ(write_xosc(scenario)->find("vendorFlag"), std::string::npos)
      << "the dropped attribute was re-emitted onto an element that never had it";
}

TEST(XoscEdit, RetypingAPositionWithNothingPreservedSaysNothing) {
  // The counterpart: findings() must not fire on every retype, only on one that
  // actually costs something. A warning nobody can act on is noise.
  Scenario scenario = one_actor_on_a_network();
  ASSERT_TRUE(
      set_entity_init_position(scenario, "Ego", WorldPosition{})->apply(scenario).has_value());

  auto command = set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)});
  ASSERT_TRUE(command->apply(scenario).has_value());
  EXPECT_TRUE(command->findings().empty());
}

TEST(XoscEdit, SettingASpeedIsASECONDPrivateActionAndNotAThirdArmOfTheTeleports) {
  // <PrivateAction> is a per-element CHOICE, so a file holds one action per
  // arm. Building the model the reader would have produced from the same file
  // is what keeps write -> read -> write idempotent.
  Scenario scenario = one_actor_on_a_network();
  ASSERT_TRUE(
      set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)})->apply(scenario).has_value());
  ASSERT_TRUE(set_entity_init_speed(scenario, "Ego", 13.89)->apply(scenario).has_value());

  const std::vector<PrivateAction>& actions = scenario.storyboard.init.actions.privates[0].actions;
  ASSERT_EQ(actions.size(), 2U);
  EXPECT_TRUE(actions[0].teleport.has_value());
  EXPECT_FALSE(actions[0].longitudinal.has_value())
      << "the speed rode on the teleport's action, producing an invalid <PrivateAction>";
  EXPECT_TRUE(actions[1].longitudinal.has_value());

  const auto written = write_xosc(scenario);
  ASSERT_TRUE(written.has_value()) << written.error().message;
}

TEST(XoscEdit, SettingASpeedOverARelativeTargetIsRefusedRatherThanDroppingIt) {
  // <SpeedActionTarget> is a 1..1 union: writing an <AbsoluteTargetSpeed>
  // beside a preserved <RelativeTargetSpeed> emits BOTH and produces a file no
  // parser accepts, while deleting the relative one is the silent drop
  // ADR-0014 §6 forbids. Refusing is the only honest third option.
  Scenario scenario = one_actor_on_a_network();
  SpeedAction relative;
  relative.target_preserved.children.push_back(
      R"(<RelativeTargetSpeed entityRef="Other" value="5" speedTargetValueType="delta"/>)");
  LongitudinalAction longitudinal;
  longitudinal.speed = std::move(relative);
  PrivateAction action;
  action.longitudinal = std::move(longitudinal);
  Private entry;
  entry.entity_ref = "Ego";
  entry.actions.push_back(std::move(action));
  scenario.storyboard.init.actions.privates.push_back(std::move(entry));

  const std::string before = fingerprint(scenario);
  auto command = set_entity_init_speed(scenario, "Ego", 20.0);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before)
      << "the refusal half-mutated the document before giving up";
}

TEST(XoscEdit, AnActorFromTheCatalogPlacesOnALaneAndWrites) {
  // The whole p8-s2 kernel path in one test: catalogue -> command -> file.
  Scenario scenario;
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, set_logic_file(scenario, "town.xodr")).has_value());
  ASSERT_TRUE(stack
                  .push(scenario,
                        place_scenario_object(scenario,
                                              osc::make_actor(osc::ActorKind::Car, "Car1"),
                                              Position{lane_at(42.5)}))
                  .has_value());
  ASSERT_TRUE(stack.push(scenario, set_entity_init_speed(scenario, "Car1", 13.89)).has_value());

  const auto written = write_xosc(scenario);
  ASSERT_TRUE(written.has_value()) << written.error().message;
  EXPECT_NE(written->find(R"(<LanePosition roadId="1" laneId="-1" s="42.5")"), std::string::npos)
      << *written;
  EXPECT_NE(written->find(R"(<AbsoluteTargetSpeed value="13.89" />)"), std::string::npos)
      << *written;

  // And every step of it undoes to nothing.
  const std::string empty = fingerprint(Scenario{});
  ASSERT_TRUE(stack.undo(scenario).has_value());
  ASSERT_TRUE(stack.undo(scenario).has_value());
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), empty);
}

// --- routes (p8-s3, #247) -----------------------------------------------------

namespace {

RouteWaypoint waypoint_at(double s) {
  return RouteWaypoint{.route_strategy = std::string(roadmaker::osc::kDefaultRouteStrategy),
                       .position = Position{lane_at(s)},
                       .preserved = {}};
}

roadmaker::osc::Route route_named(std::string name, std::size_t waypoints) {
  roadmaker::osc::Route route;
  route.name = std::move(name);
  for (std::size_t index = 0; index < waypoints; ++index) {
    route.waypoints.push_back(waypoint_at(10.0 * static_cast<double>(index + 1)));
  }
  return route;
}

const roadmaker::osc::Route* route_of(const Scenario& scenario, std::string_view entity) {
  for (const Private& entry : scenario.storyboard.init.actions.privates) {
    if (entry.entity_ref != entity) {
      continue;
    }
    for (const PrivateAction& action : entry.actions) {
      if (action.routing.has_value() && action.routing->assign_route.has_value() &&
          action.routing->assign_route->route.has_value()) {
        return &*action.routing->assign_route->route;
      }
    }
  }
  return nullptr;
}

const roadmaker::osc::Route* route_of(Scenario&&, std::string_view) = delete;

} // namespace

TEST(XoscEdit, AssignRouteCreatesThePrivateAndTheAction) {
  Scenario scenario = one_actor_on_a_network();
  const std::string before = fingerprint(scenario);
  ScenarioStack stack;

  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());
  const roadmaker::osc::Route* route = route_of(scenario, "Ego");
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->name, "R");
  EXPECT_EQ(route->waypoints.size(), 2U);

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
}

// The routing arm is its OWN <PrivateAction>, beside the teleport rather than
// on it — the model the reader would have produced from the same file.
TEST(XoscEdit, AssignRouteAppendsASecondPrivateActionBesideTheTeleport) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)}))
                  .has_value());
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());

  ASSERT_EQ(scenario.storyboard.init.actions.privates.size(), 1U);
  const std::vector<PrivateAction>& actions = scenario.storyboard.init.actions.privates[0].actions;
  ASSERT_EQ(actions.size(), 2U);
  EXPECT_TRUE(actions[0].teleport.has_value());
  EXPECT_FALSE(actions[0].routing.has_value()) << "the route rode the teleport's action";
  EXPECT_TRUE(actions[1].routing.has_value());
}

TEST(XoscEdit, AssignRouteReplacesAnExistingOneRatherThanStackingASecond) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("A", 2))).has_value());
  const std::string one_route = fingerprint(scenario);
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("B", 3))).has_value());

  ASSERT_EQ(scenario.storyboard.init.actions.privates[0].actions.size(), 1U)
      << "a second RoutingAction was stacked on";
  EXPECT_EQ(route_of(scenario, "Ego")->name, "B");

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), one_route);
}

TEST(XoscEdit, AssignRouteRefusesAnUnknownEntityAnEmptyNameAndAShortRoute) {
  const Scenario scenario = one_actor_on_a_network();
  Scenario target = scenario;
  ScenarioStack stack;

  EXPECT_FALSE(
      stack.push(target, assign_route(scenario, "Nobody", route_named("R", 2))).has_value());
  EXPECT_FALSE(stack.push(target, assign_route(scenario, "Ego", route_named("", 2))).has_value());
  EXPECT_FALSE(stack.push(target, assign_route(scenario, "Ego", route_named("R", 1))).has_value());
  // Every refusal left the document untouched and pushed nothing.
  EXPECT_EQ(fingerprint(target), fingerprint(scenario));
  EXPECT_EQ(stack.size(), 0U);
}

TEST(XoscEdit, MovingAWaypointKeepsItsStrategyAndIsOneUndo) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 3))).has_value());
  const std::string before = fingerprint(scenario);

  ASSERT_TRUE(stack.push(scenario, set_route_waypoint(scenario, "Ego", 1, Position{lane_at(77.0)}))
                  .has_value());
  const roadmaker::osc::Route* route = route_of(scenario, "Ego");
  ASSERT_NE(route, nullptr);
  ASSERT_EQ(route->waypoints.size(), 3U) << "moving a waypoint changed how many there are";
  EXPECT_DOUBLE_EQ(std::get<LanePosition>(route->waypoints[1].position).s, 77.0);
  // @routeStrategy belongs to the waypoint, not to the position being moved.
  EXPECT_EQ(route->waypoints[1].route_strategy, std::string(roadmaker::osc::kDefaultRouteStrategy));
  // ...and its siblings did not move.
  EXPECT_DOUBLE_EQ(std::get<LanePosition>(route->waypoints[0].position).s, 10.0);
  EXPECT_DOUBLE_EQ(std::get<LanePosition>(route->waypoints[2].position).s, 30.0);

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
}

TEST(XoscEdit, InsertingAndRemovingWaypointsAreEachOneUndo) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());
  const std::string two = fingerprint(scenario);

  ASSERT_TRUE(stack.push(scenario, insert_route_waypoint(scenario, "Ego", 1, waypoint_at(15.0)))
                  .has_value());
  ASSERT_EQ(route_of(scenario, "Ego")->waypoints.size(), 3U);
  EXPECT_DOUBLE_EQ(std::get<LanePosition>(route_of(scenario, "Ego")->waypoints[1].position).s,
                   15.0);

  ASSERT_TRUE(stack.push(scenario, remove_route_waypoint(scenario, "Ego", 1)).has_value());
  EXPECT_EQ(fingerprint(scenario), two);

  ASSERT_TRUE(stack.undo(scenario).has_value());
  ASSERT_EQ(route_of(scenario, "Ego")->waypoints.size(), 3U);
  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), two);
}

// ★ Removing the second-to-last waypoint would leave a route the writer
// refuses — a deletion that appeared to succeed and then made the scenario
// unsavable. Refused instead.
TEST(XoscEdit, RemovingAWaypointFromATwoWaypointRouteIsRefused) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());
  const std::string before = fingerprint(scenario);
  const std::size_t pushed = stack.size();

  EXPECT_FALSE(stack.push(scenario, remove_route_waypoint(scenario, "Ego", 0)).has_value());
  EXPECT_EQ(fingerprint(scenario), before);
  EXPECT_EQ(stack.size(), pushed);
  // ...and the document is still writable, which is the point of the refusal.
  EXPECT_TRUE(write_xosc(scenario).has_value());
}

TEST(XoscEdit, AnOutOfRangeWaypointIndexIsRefusedRatherThanClamped) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 3))).has_value());
  const std::string before = fingerprint(scenario);

  EXPECT_FALSE(stack.push(scenario, set_route_waypoint(scenario, "Ego", 3, Position{lane_at(1.0)}))
                   .has_value());
  EXPECT_FALSE(stack.push(scenario, insert_route_waypoint(scenario, "Ego", 4, waypoint_at(1.0)))
                   .has_value());
  EXPECT_FALSE(stack.push(scenario, remove_route_waypoint(scenario, "Ego", 3)).has_value());
  // A clamp would have written waypoint 2, which the caller did not name.
  EXPECT_EQ(fingerprint(scenario), before);
}

TEST(XoscEdit, ClearRouteRemovesTheWholeActionAndUndoRestoresIt) {
  Scenario scenario = one_actor_on_a_network();
  ScenarioStack stack;
  ASSERT_TRUE(stack.push(scenario, set_entity_init_pose(scenario, "Ego", Position{lane_at(5.0)}))
                  .has_value());
  const std::string placed_only = fingerprint(scenario);
  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());
  const std::string routed = fingerprint(scenario);

  ASSERT_TRUE(stack.push(scenario, clear_route(scenario, "Ego")).has_value());
  EXPECT_EQ(route_of(scenario, "Ego"), nullptr);
  // The teleport survived, and the emptied PrivateAction did not.
  EXPECT_EQ(fingerprint(scenario), placed_only);

  ASSERT_TRUE(stack.undo(scenario).has_value());
  EXPECT_EQ(fingerprint(scenario), routed);
}

TEST(XoscEdit, ClearRouteRefusesAnEntityWithNoRoute) {
  const Scenario scenario = one_actor_on_a_network();
  Scenario target = scenario;
  ScenarioStack stack;
  EXPECT_FALSE(stack.push(target, clear_route(scenario, "Ego")).has_value());
  EXPECT_EQ(fingerprint(target), fingerprint(scenario));
}

// The strongest form of the byte-identity contract, applied to the route
// commands as a group: ten mutations, ten undos, ten redos.
TEST(XoscEdit, RouteCommandsSurviveUndoRedoByteIdentically) {
  Scenario scenario = one_actor_on_a_network();
  const std::string empty = fingerprint(scenario);
  ScenarioStack stack;

  ASSERT_TRUE(stack.push(scenario, assign_route(scenario, "Ego", route_named("R", 2))).has_value());
  for (std::size_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(stack.push(scenario, insert_route_waypoint(scenario, "Ego", 1, waypoint_at(20.0)))
                    .has_value());
  }
  for (std::size_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(
        stack.push(scenario, set_route_waypoint(scenario, "Ego", index, Position{lane_at(3.0)}))
            .has_value());
  }
  ASSERT_TRUE(stack.push(scenario, remove_route_waypoint(scenario, "Ego", 2)).has_value());
  const std::string applied = fingerprint(scenario);

  while (stack.can_undo()) {
    ASSERT_TRUE(stack.undo(scenario).has_value());
  }
  EXPECT_EQ(fingerprint(scenario), empty);
  while (stack.can_redo()) {
    ASSERT_TRUE(stack.redo(scenario).has_value());
  }
  EXPECT_EQ(fingerprint(scenario), applied);
}
