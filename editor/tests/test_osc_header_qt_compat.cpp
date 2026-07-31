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

// The scenario model parses from a translation unit that has seen Qt
// (p8-s1, issue #245).
//
// ADR-0014 §2 rules that `signals` can never be a member name anywhere in a
// kernel header, because <QObject> does `#define signals public` and a member
// of that name makes the STRUCT DECLARATION ITSELF fail to parse — the trap is
// already documented twice in mesh/junction_phases.hpp and
// mesh/junction_signals.hpp. `osc::Phase` therefore names its member
// `signal_states`, and `<TrafficSignals>` gets no struct at all.
//
// Nothing in the kernel's own test suite can prove that, because the kernel
// never includes Qt. This file is the gate: the QObject include below comes
// FIRST on purpose, and if a future member is ever named `signals` this
// translation unit stops compiling. p8-s2 puts scenario UI in the editor, so
// the day that matters is close.
//
// The assertions are incidental — a compiling TU is the whole test. They exist
// so the file is a real test rather than a compile-only artifact, and so the
// macro is demonstrably in effect rather than merely assumed.

// clang-format off
// INCLUDE ORDER IS THE TEST. <QObject> must be seen BEFORE scenario.hpp, or
// `#define signals public` is not yet in effect when the struct is parsed and
// this whole file silently stops gating anything.
//
// `IncludeBlocks: Regroup` in .clang-format sorts <QObject> down below the
// roadmaker/ block, which is exactly that vacuity — verified: CI's formatter
// reordered it, and with the reordering in place the `signals` sabotage
// COMPILES CLEANLY. Hence the guard; do not remove it.
#include <QObject>

#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/osc/writer.hpp"
// clang-format on

#include <gtest/gtest.h>

#include <string>

namespace roadmaker::osc {
namespace {

TEST(OscHeaderQtCompat, TheQtSignalsMacroIsActuallyInEffectHere) {
  // Without this, the test above it would pass in a build where <QObject>
  // silently failed to define the macro, and the gate would be vacuous.
#ifndef signals
  FAIL() << "Qt's `signals` macro is not defined in this translation unit, so "
            "this file does not actually gate anything";
#endif
  SUCCEED();
}

TEST(OscHeaderQtCompat, TheModelIsUsableFromATranslationUnitThatHasSeenQt) {
  Scenario scenario;
  scenario.road_network.logic_file = FileRef{.filepath = "town.xodr", .preserved = {}};

  TrafficSignalController controller;
  controller.name = "17";

  Phase phase;
  phase.name = "go";
  phase.duration = 20.0;
  // The member whose name the ADR constrains. Naming it `signals` would make
  // the declaration in scenario.hpp unparseable, so this line is the payload
  // of the whole file.
  phase.signal_states.push_back(
      TrafficSignalState{.traffic_signal_id = "17251", .state = "green", .preserved = {}});
  controller.phases.push_back(phase);
  scenario.road_network.traffic_signal_controllers.push_back(controller);

  const auto text = write_xosc(scenario);
  ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().message);
  EXPECT_NE(text->find(R"(<TrafficSignalController name="17")"), std::string::npos) << *text;
}

} // namespace
} // namespace roadmaker::osc
