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

// The junction-timeline -> TrafficSignalController decomposition (p8-s1 PR-D,
// issue #245) — ADR-0014 §8.
//
// THE SUBJECT IS A REAL SIGNALIZED JUNCTION, not hand-built structs. Every
// defect this file exists to catch is a defect in reading a live network, and
// a fixture assembled by hand would be testing the assembly. So the network is
// authored and signalized through the same command factories the editor and
// the fixture script use.
//
// The three silent corruptions, each invisible to a whole-document assertion:
//
//   * RED BY OMISSION. Skipping a Red state as "the default" is a natural size
//     optimization and reproduces exactly what the plan structure exists to
//     prevent: a signal RoadMaker shows as red becomes a signal that is NEVER
//     red. Red states appear in OTHER phases, so a global count still passes.
//     Only a PER-PHASE count catches it. (`EverySignalCarriesAStateInEveryPhase`,
//     `AnAllRedPhaseStillNamesEverySignalAsRed`)
//   * A HANDLE REACHING THE FILE. `SignalId` is a generational arena handle.
//     Written out, it produces a file that looks entirely right and references
//     nothing. (`TrafficSignalIdIsTheSignalOdrIdAndNeverAHandle`)
//   * ARENA ORDER REACHING THE FILE. `for_each_controller` walks slots, so a
//     freed-and-reused slot reorders the output and two networks holding the
//     same signals write two different documents. The sync group is authored
//     in REVERSE id order below precisely so the sort is not vacuous.
//     (`ControllerOrderIsSortedByIdNotByTimelineRow`)

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/osc/decompose.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Control;
using roadmaker::Controller;
using roadmaker::ControllerId;
using roadmaker::Junction;
using roadmaker::junction_phases;
using roadmaker::JunctionController;
using roadmaker::JunctionId;
using roadmaker::JunctionPhaseInfo;
using roadmaker::JunctionPhasePlan;
using roadmaker::LaneProfile;
using roadmaker::PhaseSignalState;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Signal;
using roadmaker::SignalId;
using roadmaker::SignalState;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::edit::signalize_junction;
using roadmaker::edit::SignalizeTemplate;
using roadmaker::osc::decompose_junction_signals;
using roadmaker::osc::JunctionSignalDecomposition;
using roadmaker::osc::Phase;
using roadmaker::osc::state_token;
using roadmaker::osc::TrafficSignalController;
using roadmaker::osc::TrafficSignalState;

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

void run(RoadNetwork& network, const std::unique_ptr<Command>& command) {
  if (command == nullptr) {
    throw std::runtime_error("factory returned nullptr");
  }
  const auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error(applied.error().message);
  }
}

/// A four-arm crossing signalized from the two-phase template — the same shape
/// `scripts/gen_xosc_fixtures.py` builds, so what is asserted here is what the
/// tracked esmini fixture is made of.
struct SignalizedCross {
  RoadNetwork network;
  JunctionId junction;

  explicit SignalizedCross(SignalizeTemplate tmpl = SignalizeTemplate::TwoPhase) {
    const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    const RoadId north = author(network, {Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south), end_of(north)};
    run(network, roadmaker::edit::create_junction(network, ends));
    network.for_each_junction([this](JunctionId id, const Junction&) { junction = id; });
    run(network, signalize_junction(network, junction, {.tmpl = tmpl}));
  }

  /// The live `<control>` mapping, read straight off the network rather than
  /// off the thing under test.
  [[nodiscard]] std::set<std::string> heads_of(const std::string& controller_odr_id) const {
    std::set<std::string> ids;
    network.for_each_controller([&](ControllerId, const Controller& controller) {
      if (controller.odr_id != controller_odr_id) {
        return;
      }
      for (const Control& control : controller.controls) {
        ids.insert(control.signal_odr_id);
      }
    });
    return ids;
  }
};

std::vector<std::string> names_of(const std::vector<TrafficSignalController>& controllers) {
  std::vector<std::string> names;
  names.reserve(controllers.size());
  for (const TrafficSignalController& controller : controllers) {
    names.push_back(controller.name);
  }
  return names;
}

} // namespace

// --- the token table ---------------------------------------------------------

TEST(XoscStateToken, EveryStateHasItsOwnDistinctToken) {
  // Not a spelling assertion for its own sake: the four must be DISTINCT, or a
  // scenario cannot express the difference between stop and go at all.
  const std::set<std::string_view> tokens{state_token(SignalState::Red),
                                          state_token(SignalState::Yellow),
                                          state_token(SignalState::Green),
                                          state_token(SignalState::Off)};
  EXPECT_EQ(tokens.size(), 4U);
  EXPECT_EQ(state_token(SignalState::Red), "red");
  EXPECT_EQ(state_token(SignalState::Yellow), "yellow");
  EXPECT_EQ(state_token(SignalState::Green), "green");
  EXPECT_EQ(state_token(SignalState::Off), "off");
}

TEST(XoscStateToken, NoTokenIsEmpty) {
  // An empty @state writes state="" — schema-valid, meaningless, and the kind
  // of thing esmini v3.5.0 accepts in silence (p8-s1 PR-C).
  for (const SignalState state :
       {SignalState::Red, SignalState::Yellow, SignalState::Green, SignalState::Off}) {
    EXPECT_FALSE(state_token(state).empty());
  }
}

// --- shape -------------------------------------------------------------------

TEST(XoscDecompose, OneControllerPerMemberController) {
  const SignalizedCross fixture;
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  ASSERT_FALSE(plan.controller_odr_ids.empty()) << "the fixture must be genuinely signalized";

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);

  EXPECT_EQ(out.controllers.size(), plan.controller_odr_ids.size());
  std::set<std::string> expected(plan.controller_odr_ids.begin(), plan.controller_odr_ids.end());
  const std::vector<std::string> got = names_of(out.controllers);
  EXPECT_EQ(std::set<std::string>(got.begin(), got.end()), expected);
}

TEST(XoscDecompose, ControllerNameIsTheOpenDriveControllerIdNotItsLabel) {
  SignalizedCross fixture;
  // Give every controller a human-readable name. Emitting THAT is the failure
  // §10.10 warns about: "The ASAM OpenDRIVE controller ID is used as the name
  // of the TrafficSignalController to reference it", so a label produces a
  // file that references nothing.
  std::set<std::string> ids;
  fixture.network.for_each_controller([&ids](ControllerId, Controller& controller) {
    controller.name = "friendly_" + controller.odr_id;
    ids.insert(controller.odr_id);
  });

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_FALSE(out.controllers.empty());
  for (const TrafficSignalController& controller : out.controllers) {
    EXPECT_TRUE(ids.contains(controller.name)) << controller.name;
    EXPECT_FALSE(controller.name.starts_with("friendly_")) << controller.name;
  }
}

TEST(XoscDecompose, ControllerOrderIsSortedByIdNotByTimelineRow) {
  SignalizedCross fixture;
  // Reverse the sync group, so the plan's TIMELINE ROW order is the reverse of
  // id order and a decomposition that simply follows the plan would emit the
  // reverse. Without this the sort would be vacuous — the template happens to
  // create controllers in ascending id order.
  Junction* record = fixture.network.junction(fixture.junction);
  ASSERT_NE(record, nullptr);
  ASSERT_GE(record->junction_controllers.size(), 2U) << "need two rows for order to mean anything";
  std::reverse(record->junction_controllers.begin(), record->junction_controllers.end());

  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  std::vector<std::string> rows = plan.controller_odr_ids;
  ASSERT_FALSE(std::is_sorted(rows.begin(), rows.end()))
      << "the reversal did not make the row order differ from id order";

  const std::vector<std::string> got =
      names_of(decompose_junction_signals(fixture.network, fixture.junction).controllers);
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
  std::sort(rows.begin(), rows.end());
  EXPECT_EQ(got, rows);
}

TEST(XoscDecompose, DurationsAndPhaseOrderAreIdenticalAcrossControllers) {
  const SignalizedCross fixture;
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_GE(out.controllers.size(), 2U);

  const std::vector<Phase>& reference = out.controllers.front().phases;
  ASSERT_FALSE(reference.empty());
  for (const TrafficSignalController& controller : out.controllers) {
    ASSERT_EQ(controller.phases.size(), reference.size()) << controller.name;
    for (std::size_t i = 0; i < reference.size(); ++i) {
      EXPECT_DOUBLE_EQ(controller.phases[i].duration, reference[i].duration) << controller.name;
      EXPECT_EQ(controller.phases[i].name, reference[i].name) << controller.name;
    }
  }
}

TEST(XoscDecompose, PhaseDurationsMatchTheJunctionCycle) {
  const SignalizedCross fixture;
  const JunctionPhasePlan plan = junction_phases(fixture.network, fixture.junction);
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_FALSE(out.controllers.empty());

  const std::vector<Phase>& phases = out.controllers.front().phases;
  ASSERT_EQ(phases.size(), plan.phases.size());
  for (std::size_t i = 0; i < phases.size(); ++i) {
    EXPECT_DOUBLE_EQ(phases[i].duration, plan.phases[i].duration);
  }
}

// --- the Red-by-omission guard ----------------------------------------------

TEST(XoscDecompose, EverySignalCarriesAStateInEveryPhase) {
  const SignalizedCross fixture;
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_FALSE(out.controllers.empty());

  for (const TrafficSignalController& controller : out.controllers) {
    // Read the expected head count off the NETWORK, not off the output.
    std::set<std::string> heads = fixture.heads_of(controller.name);
    ASSERT_FALSE(heads.empty()) << controller.name;

    for (const Phase& phase : controller.phases) {
      // PER-PHASE. A global count over the controller passes on Red-by-omission
      // because the omitted reds are present in the phases that are green.
      std::set<std::string> named;
      for (const TrafficSignalState& state : phase.signal_states) {
        named.insert(state.traffic_signal_id);
      }
      EXPECT_EQ(named, heads) << controller.name << " phase '" << phase.name << "'";
    }
  }
}

TEST(XoscDecompose, AnAllRedPhaseStillNamesEverySignalAsRed) {
  const SignalizedCross fixture;
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);

  // A two-phase cycle gives every controller at least one phase in which it is
  // wholly red — the clearance phase, or the other axis's green. That phase is
  // where Red-by-omission would produce an EMPTY state list.
  std::size_t all_red_phases = 0;
  for (const TrafficSignalController& controller : out.controllers) {
    for (const Phase& phase : controller.phases) {
      const bool all_red =
          !phase.signal_states.empty() &&
          std::all_of(phase.signal_states.begin(),
                      phase.signal_states.end(),
                      [](const TrafficSignalState& s) { return s.state == "red"; });
      if (all_red) {
        ++all_red_phases;
        EXPECT_EQ(phase.signal_states.size(), fixture.heads_of(controller.name).size());
      }
    }
  }
  EXPECT_GT(all_red_phases, 0U) << "the fixture never goes all-red, so this gate is vacuous";
}

// --- identity ----------------------------------------------------------------

TEST(XoscDecompose, TrafficSignalIdIsTheSignalOdrIdAndNeverAHandle) {
  const SignalizedCross fixture;
  std::set<std::string> live;
  fixture.network.for_each_signal(
      [&live](SignalId, const Signal& signal) { live.insert(signal.odr_id); });
  ASSERT_FALSE(live.empty());

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  std::size_t states = 0;
  for (const TrafficSignalController& controller : out.controllers) {
    for (const Phase& phase : controller.phases) {
      for (const TrafficSignalState& state : phase.signal_states) {
        ++states;
        EXPECT_FALSE(state.traffic_signal_id.empty());
        EXPECT_TRUE(live.contains(state.traffic_signal_id)) << state.traffic_signal_id;
      }
    }
  }
  EXPECT_GT(states, 0U);
}

TEST(XoscDecompose, StatesWithinAPhaseAreSortedBySignalId) {
  SignalizedCross fixture;
  // ★ THE CONTROL ORDER IS REVERSED FIRST, AND WITHOUT THAT THIS TEST IS
  // VACUOUS. A sabotage run proved it: deleting the sort outright changed
  // nothing, because `junction_phases` walks each controller's <control>
  // children in order and the template happens to create them with ascending
  // ids — so the states were already sorted by construction and the sort was
  // never doing anything observable. Reversing the controls makes the natural
  // order DESCENDING, so an unsorted decomposition emits them that way and two
  // networks holding the same signals write two different files.
  fixture.network.for_each_controller([](ControllerId, Controller& controller) {
    std::reverse(controller.controls.begin(), controller.controls.end());
  });

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_FALSE(out.controllers.empty());

  std::size_t multi_head_phases = 0;
  for (const TrafficSignalController& controller : out.controllers) {
    for (const Phase& phase : controller.phases) {
      if (phase.signal_states.size() > 1) {
        ++multi_head_phases;
      }
      EXPECT_TRUE(std::is_sorted(phase.signal_states.begin(),
                                 phase.signal_states.end(),
                                 [](const TrafficSignalState& a, const TrafficSignalState& b) {
                                   return a.traffic_signal_id < b.traffic_signal_id;
                                 }))
          << controller.name << " phase '" << phase.name << "'";
    }
  }
  // Sortedness over a one-element list is a tautology; this is what makes the
  // assertion above mean something.
  EXPECT_GT(multi_head_phases, 0U)
      << "no phase has two heads, so nothing above can be out of order";
}

TEST(XoscDecompose, AControllerOnlyCarriesItsOwnHeads) {
  const SignalizedCross fixture;
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_GE(out.controllers.size(), 2U);

  for (const TrafficSignalController& controller : out.controllers) {
    const std::set<std::string> heads = fixture.heads_of(controller.name);
    for (const Phase& phase : controller.phases) {
      for (const TrafficSignalState& state : phase.signal_states) {
        EXPECT_TRUE(heads.contains(state.traffic_signal_id))
            << controller.name << " carries " << state.traffic_signal_id
            << ", which belongs to another signal group";
      }
    }
  }
}

TEST(XoscDecompose, AnErasedSignalHeadSimplyLeavesTheCycleAndBreaksNothing) {
  SignalizedCross fixture;
  // ★ WHAT THIS DOES AND DOES NOT PROVE. `junction_phases` builds its
  // `signal_states` by resolving each `<control>` through a map of LIVE
  // signals (`mesh/junction_phases.cpp:178-181`), so an erased head never
  // reaches the plan as a dangling handle — it simply stops appearing. That is
  // why the null-handle branch in the decomposition is defence in depth rather
  // than a path this test can drive; see `AnEmptySignalIdIsReportedAndOmitted`
  // for the guard that IS reachable.
  //
  // What is worth pinning is that the rest still decomposes: erasing one head
  // must not empty a phase, drop a controller, or leave an empty id behind.
  SignalId victim;
  fixture.network.for_each_signal([&victim](SignalId id, const Signal&) {
    if (!victim.is_valid()) {
      victim = id;
    }
  });
  ASSERT_TRUE(victim.is_valid());
  const std::string erased_id = fixture.network.signal(victim)->odr_id;
  ASSERT_TRUE(fixture.network.erase_signal(victim));

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  ASSERT_FALSE(out.controllers.empty()) << "one dead head must not lose the whole cycle";
  for (const TrafficSignalController& controller : out.controllers) {
    for (const Phase& phase : controller.phases) {
      for (const TrafficSignalState& state : phase.signal_states) {
        EXPECT_FALSE(state.traffic_signal_id.empty());
        EXPECT_NE(state.traffic_signal_id, erased_id);
      }
    }
  }
}

TEST(XoscDecompose, AnEmptySignalIdIsReportedAndOmitted) {
  SignalizedCross fixture;
  // The reachable half of the identity guard. A LIVE signal whose @id is empty
  // resolves through the plan perfectly well (the map is keyed by @id, so ""
  // is a key like any other) — and writing it would emit
  // trafficSignalId="", the "looks right, references nothing" failure
  // ADR-0014 §5 exists to prevent. Both ends are blanked, because the plan
  // reaches the head through its controller's <control>.
  SignalId victim;
  fixture.network.for_each_signal([&victim](SignalId id, const Signal&) {
    if (!victim.is_valid()) {
      victim = id;
    }
  });
  ASSERT_TRUE(victim.is_valid());
  const std::string blanked = fixture.network.signal(victim)->odr_id;
  fixture.network.signal(victim)->odr_id.clear();
  fixture.network.for_each_controller([&blanked](ControllerId, Controller& controller) {
    for (Control& control : controller.controls) {
      if (control.signal_odr_id == blanked) {
        control.signal_odr_id.clear();
      }
    }
  });

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  bool reported = false;
  for (const auto& finding : out.findings) {
    reported = reported || finding.message.find("empty @id") != std::string::npos;
  }
  EXPECT_TRUE(reported) << "an omitted state must never be silent";
  for (const TrafficSignalController& controller : out.controllers) {
    for (const Phase& phase : controller.phases) {
      for (const TrafficSignalState& state : phase.signal_states) {
        EXPECT_FALSE(state.traffic_signal_id.empty());
      }
    }
  }
}

TEST(XoscDecompose, AHeadControlledByTwoGroupsAppearsUnderBothWithOneColour) {
  // A KNOWN LIMIT, pinned rather than left to surprise someone. RoadMaker's
  // plan resolves a head that two controllers claim ONCE, first-wins
  // (`mesh/junction_phases.cpp:182-187`), because a physical head shows one
  // colour. The decomposition follows: the head appears in BOTH groups'
  // rows carrying that single resolved state, rather than vanishing from the
  // second group and leaving it with a phase that names fewer heads than it
  // controls. Sparse would break the dense contract; a second colour would be
  // a lie about the hardware.
  SignalizedCross fixture;
  std::string donor_head;
  std::string first_controller;
  std::string second_controller;
  fixture.network.for_each_controller([&](ControllerId, const Controller& controller) {
    if (first_controller.empty()) {
      first_controller = controller.odr_id;
      if (!controller.controls.empty()) {
        donor_head = controller.controls.front().signal_odr_id;
      }
    } else if (second_controller.empty()) {
      second_controller = controller.odr_id;
    }
  });
  ASSERT_FALSE(donor_head.empty());
  ASSERT_FALSE(second_controller.empty());

  fixture.network.for_each_controller([&](ControllerId, Controller& controller) {
    if (controller.odr_id == second_controller) {
      controller.controls.push_back(Control{.signal_odr_id = donor_head, .type = ""});
    }
  });

  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  for (const TrafficSignalController& controller : out.controllers) {
    if (controller.name != first_controller && controller.name != second_controller) {
      continue;
    }
    for (const Phase& phase : controller.phases) {
      const bool names_it = std::any_of(
          phase.signal_states.begin(),
          phase.signal_states.end(),
          [&donor_head](const TrafficSignalState& s) { return s.traffic_signal_id == donor_head; });
      EXPECT_TRUE(names_it) << controller.name << " phase '" << phase.name
                            << "' dropped the shared head";
    }
  }
}

// --- the junctions with nothing to say ---------------------------------------

TEST(XoscDecompose, AStaleJunctionIdYieldsNoControllersAndSaysWhy) {
  const SignalizedCross fixture;
  const JunctionSignalDecomposition out = decompose_junction_signals(fixture.network, JunctionId{});
  EXPECT_TRUE(out.controllers.empty());
  EXPECT_FALSE(out.findings.empty()) << "an empty result must never be silent";
}

TEST(XoscDecompose, AnUnsignalizedJunctionYieldsNoControllersAndSaysWhy) {
  RoadNetwork network;
  const RoadId west = author(network, {Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
  const RoadId east = author(network, {Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
  const RoadId south = author(network, {Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
  const std::vector<RoadEnd> ends{end_of(west), end_of(east), end_of(south)};
  run(network, roadmaker::edit::create_junction(network, ends));
  JunctionId junction;
  network.for_each_junction([&junction](JunctionId id, const Junction&) { junction = id; });

  const JunctionSignalDecomposition out = decompose_junction_signals(network, junction);
  EXPECT_TRUE(out.controllers.empty());
  ASSERT_FALSE(out.findings.empty());
  EXPECT_EQ(out.findings.front().severity, roadmaker::Severity::Warning);
}

TEST(XoscDecompose, AStaticTemplateHasNoCycleAndIsReportedRatherThanExported) {
  // A static template places signs, not controllers — there is no cycle to
  // time, and the export must say so rather than emit an empty document half.
  const SignalizedCross fixture(SignalizeTemplate::AllWayStop);
  const JunctionSignalDecomposition out =
      decompose_junction_signals(fixture.network, fixture.junction);
  EXPECT_TRUE(out.controllers.empty());
  EXPECT_FALSE(out.findings.empty());
}

TEST(XoscDecompose, NoFindingIsAnError) {
  // Findings describe what the export COPED with. A decomposition that cannot
  // proceed returns nothing and says why; it never returns half a cycle plus
  // an error, because a caller would have no way to act on that.
  const SignalizedCross fixture;
  for (const auto& finding :
       decompose_junction_signals(fixture.network, fixture.junction).findings) {
    EXPECT_NE(finding.severity, roadmaker::Severity::Error) << finding.message;
  }
}
