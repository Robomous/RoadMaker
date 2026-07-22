// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace roadmaker {

/// Default duration [s] of a derived AXIS-green phase (a full through-and-left
/// permissive green). The editor is how a shorter or protected phase is
/// authored; these are only the shape of the DERIVED cycle.
inline constexpr double kDefaultPhaseGreenSeconds = 20.0;

/// Default duration [s] of a derived yellow CLEARANCE phase between two axes.
inline constexpr double kDefaultPhaseYellowSeconds = 3.0;

/// Default duration [s] of a phase the editor ADDS (`edit::add_signal_phase`) —
/// short, all-red, so the author immediately sets it to something meaningful.
inline constexpr double kDefaultAddedPhaseSeconds = 5.0;

/// One member controller's resolved state within one phase (p4-s8, issue #229).
/// Every member controller appears, so a phase's `states` is Red-filled — the
/// sparse storage of `PhaseState` is expanded here for the consumer.
struct PhaseControllerState {
  std::string controller_odr_id;
  SignalState state = SignalState::Red;
};

/// One signal HEAD's resolved state within one phase (p4-s8, issue #229): a
/// live signal and the color its controlling group shows. This is what the
/// viewport lights up.
///
/// NOT named `signals`: Qt's <QObject> defines `signals` as a macro
/// (`#define signals public`), so a member of that name makes this header
/// impossible to include from any editor translation unit. The member is
/// `signal`, echoing `JunctionApproachInfo::signal_ids`.
struct PhaseSignalState {
  SignalId signal;
  SignalState state = SignalState::Red;
};

/// One phase of the effective cycle, fully resolved for a consumer (p4-s8,
/// issue #229). Derived phases and authored phases are reported identically;
/// only `JunctionPhasePlan::authored` tells them apart.
struct JunctionPhaseInfo {
  /// The phase label (may be empty). Derived phases are `axis{n}` / `axis{n}_clear`.
  std::string name;

  /// Duration [s] of this phase.
  double duration = 0.0;

  /// Cumulative offset [s] of this phase's start within the cycle (phase 0 = 0).
  double start = 0.0;

  /// EVERY member controller and the state it shows this phase, in timeline row
  /// order (`JunctionPhasePlan::controller_odr_ids`). Red-filled: a controller
  /// the stored/derived phase does not name explicitly shows Red.
  std::vector<PhaseControllerState> states;

  /// The live signal heads and their state this phase, resolved through each
  /// member controller's `<control>` children (first-wins dedup across the
  /// phase). Complete — scrubbing needs no time-parameterized kernel call, only
  /// `plan.phases[phase_index_at(plan, t)].signal_states`.
  std::vector<PhaseSignalState> signal_states;

  /// The connecting roads whose traffic MAY proceed this phase (GW-4 step 6):
  /// a gated movement whose controlling group is Green. Deduplicated, in
  /// approach-then-gated order.
  std::vector<RoadId> moving;
};

/// The effective signal CYCLE of a junction — the single source shared by the
/// phase editor, the viewport overlay, the Python bindings and the validator
/// (p4-s8, issue #229). Derived when the junction stores no phases, resolved
/// from `Junction::phases` when it does; either way the phases are reported the
/// same, so no consumer can disagree about the cycle.
///
/// Plain data, no out-of-line member functions: a member defined in the .cpp
/// would need its own RM_API under RM_BUILD_SHARED=ON, and exporting the struct
/// wholesale would trip MSVC C4251 on its std::string / std::vector members
/// (the junction_signals.hpp rule).
struct JunctionPhasePlan {
  /// False ⇒ the cycle is DERIVED and the junction stores no `rm:phases` bytes.
  /// True ⇒ the cycle is read from `Junction::phases`. The first edit flips
  /// this by materializing the derived cycle into the junction.
  bool authored = false;

  /// Sum [s] of every phase's duration — the cycle length `phase_index_at`
  /// wraps over.
  double cycle_duration = 0.0;

  /// The member controllers, in TIMELINE ROW order (the junction's sync-group /
  /// `junction_controllers` order), filtered to controllers that are still live.
  /// One row per entry.
  std::vector<std::string> controller_odr_ids;

  /// Controller ids named by AUTHORED phase states that are NOT live members of
  /// this junction's sync group — dormant references the writer prunes and the
  /// validator advises on. Deduplicated. Always empty on a derived plan.
  std::vector<std::string> dormant_controller_odr_ids;

  /// The effective phases in cycle order. Empty ⇒ nothing to time (a junction
  /// with no live member controller: a static template, an unsignalized or a
  /// span junction), which the editor shows as "signalize first".
  std::vector<JunctionPhaseInfo> phases;
};

/// The effective cycle of `junction_id` — derived from the sync group when the
/// junction stores no phases, resolved from `Junction::phases` when it does.
///
/// Returns an empty plan (`authored=false`, no phases) for a stale id, a span
/// (virtual) junction, an unsignalized junction, and a junction whose only
/// signalization is a STATIC template (no controllers) — none of those has a
/// cycle to time. A FOREIGN dynamic junction (loaded from another file, no arm
/// list) still resolves, because member controllers come off the sync-group
/// references and the top-level `<controller>`s, not off the arm list.
[[nodiscard]] RM_API JunctionPhasePlan junction_phases(const RoadNetwork& network,
                                                       JunctionId junction_id);

/// The index of the phase active at cycle time `t` [s], wrapping over
/// `plan.cycle_duration` (negative `t` and `t >= cycle` handled by modulo).
/// Returns `SIZE_MAX` when the plan has no phases.
[[nodiscard]] RM_API std::size_t phase_index_at(const JunctionPhasePlan& plan, double t);

} // namespace roadmaker
