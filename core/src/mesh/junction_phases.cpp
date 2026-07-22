// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/mesh/junction_phases.hpp"

#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace roadmaker {

namespace {

/// The member controllers of `junction`'s synchronization group, in sync-group
/// order, filtered to those still live and deduplicated — the timeline's rows.
std::vector<std::string> live_member_controllers(
    const Junction& junction,
    const std::unordered_map<std::string, const Controller*>& live_controllers) {
  std::vector<std::string> members;
  for (const JunctionController& reference : junction.junction_controllers) {
    if (!live_controllers.contains(reference.controller_odr_id)) {
      continue; // a deleted controller lies dormant, never a phantom row
    }
    if (std::ranges::find(members, reference.controller_odr_id) == members.end()) {
      members.push_back(reference.controller_odr_id);
    }
  }
  return members;
}

} // namespace

JunctionPhasePlan junction_phases(const RoadNetwork& network, JunctionId junction_id) {
  JunctionPhasePlan plan;

  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr || !junction->spans.empty()) {
    // A stale id, or a §12.7 span junction — which "shall not have controllers
    // and therefore no traffic lights" — has no cycle to time.
    return plan;
  }

  // Live top-level controllers, by odr id (§14.6). A dangling sync-group
  // reference simply never resolves here.
  std::unordered_map<std::string, const Controller*> live_controllers;
  network.for_each_controller([&](ControllerId, const Controller& controller) {
    if (!controller.odr_id.empty()) {
      live_controllers.emplace(controller.odr_id, &controller);
    }
  });

  const std::vector<std::string> members = live_member_controllers(*junction, live_controllers);
  plan.controller_odr_ids = members;

  // Live signals by odr id, first-wins (odr ids are unique in a well-formed
  // network), so a controller's <control> can be resolved to a head.
  std::unordered_map<std::string, SignalId> signal_by_odr;
  network.for_each_signal(
      [&](SignalId id, const Signal& signal) { signal_by_odr.emplace(signal.odr_id, id); });

  // The signal odr ids each live controller drives, for the head resolution and
  // the protected-left partition below.
  std::unordered_map<std::string, std::set<std::string>> controls_of;
  for (const auto& [odr_id, controller] : live_controllers) {
    std::set<std::string>& set = controls_of[odr_id];
    for (const Control& control : controller->controls) {
      set.insert(control.signal_odr_id);
    }
  }

  // The approaches and their maneuvers drive `moving` (GW-4 step 6). Both are
  // derived, so a foreign junction still resolves them off its connections.
  const std::vector<JunctionApproachInfo> approaches = junction_signals(network, junction_id);
  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction_id);
  const auto effective_turn = [&](RoadId road) {
    const auto entry = std::ranges::find_if(
        maneuvers, [&](const JunctionManeuverInfo& info) { return info.road == road; });
    return entry == maneuvers.end() ? TurnType::Straight : entry->effective;
  };

  // Per approach, partition its MEMBER controllers into *left* and *other*. A
  // controller is *left* on an approach when every head it drives there is a
  // p4-s7 protected-left arrow (`Signal::name == "protected_left"`, the only
  // carrier — §14.1 has no protected-left catalog code). Phase-independent, so
  // it is computed once and only the green test below varies per phase.
  //
  // A FOREIGN junction never uses the name convention, so no controller is ever
  // *left* there; left-turn roads then fall through to the *other* rule below,
  // which degrades to the intended "any green moves all".
  struct ApproachMovers {
    const JunctionApproachInfo* approach = nullptr;
    std::vector<std::string> left;
    std::vector<std::string> other;
  };

  std::vector<ApproachMovers> movers;
  for (const JunctionApproachInfo& approach : approaches) {
    ApproachMovers entry;
    entry.approach = &approach;
    for (const std::string& controller : approach.controller_odr_ids) {
      if (std::ranges::find(members, controller) == members.end()) {
        continue; // controls a head here but is not in the sync group
      }
      const auto controlled = controls_of.find(controller);
      bool has_head = false;
      bool all_protected_left = true;
      if (controlled != controls_of.end()) {
        for (const SignalId head : approach.signal_ids) {
          const Signal* signal = network.signal(head);
          if (signal == nullptr || !controlled->second.contains(signal->odr_id)) {
            continue;
          }
          has_head = true;
          all_protected_left = all_protected_left && signal->name == "protected_left";
        }
      }
      if (has_head && all_protected_left) {
        entry.left.push_back(controller);
      } else {
        entry.other.push_back(controller);
      }
    }
    movers.push_back(std::move(entry));
  }

  // Builds one fully-resolved phase from a per-controller state lookup. Shared
  // by the derived and authored paths so they cannot disagree about resolution.
  const auto build_phase = [&](std::string name, double duration, auto&& state_of) {
    JunctionPhaseInfo info;
    info.name = std::move(name);
    info.duration = duration;

    // states: EVERY member controller, Red-filled, in row order.
    std::unordered_map<std::string, SignalState> member_state;
    info.states.reserve(members.size());
    for (const std::string& controller : members) {
      const SignalState state = state_of(controller);
      info.states.push_back(PhaseControllerState{.controller_odr_id = controller, .state = state});
      member_state.emplace(controller, state);
    }
    const auto state_for = [&](const std::string& controller) {
      const auto entry = member_state.find(controller);
      return entry == member_state.end() ? SignalState::Red : entry->second;
    };

    // signal_states: member order, then control order, first-wins dedup by head.
    for (const std::string& controller : members) {
      const auto controlled = live_controllers.find(controller);
      if (controlled == live_controllers.end()) {
        continue;
      }
      const SignalState state = state_for(controller);
      for (const Control& control : controlled->second->controls) {
        const auto head = signal_by_odr.find(control.signal_odr_id);
        if (head == signal_by_odr.end()) {
          continue;
        }
        const bool seen = std::ranges::any_of(info.signal_states, [&](const PhaseSignalState& p) {
          return p.signal == head->second;
        });
        if (!seen) {
          info.signal_states.push_back(PhaseSignalState{.signal = head->second, .state = state});
        }
      }
    }

    // moving: a gated road may proceed when its controlling group shows Green.
    for (const ApproachMovers& approach : movers) {
      const bool any_left_green = std::ranges::any_of(
          approach.left, [&](const std::string& c) { return state_for(c) == SignalState::Green; });
      const bool any_other_green = std::ranges::any_of(
          approach.other, [&](const std::string& c) { return state_for(c) == SignalState::Green; });
      const bool has_left = !approach.left.empty();
      for (const RoadId road : approach.approach->gated) {
        const bool is_left = effective_turn(road) == TurnType::Left;
        const bool moves =
            is_left ? (has_left ? any_left_green : any_other_green) : any_other_green;
        if (moves && std::ranges::find(info.moving, road) == info.moving.end()) {
          info.moving.push_back(road);
        }
      }
    }

    return info;
  };

  if (junction->phases.empty()) {
    // --- DERIVED path: synthesize the default red-yellow-green cycle. ---
    if (members.empty()) {
      return plan; // no live member controller ⇒ no cycle (a static template)
    }
    const std::vector<std::vector<std::size_t>> axes = cluster_signal_axes(approaches);
    for (std::size_t n = 0; n < axes.size(); ++n) {
      // The member controllers active on this axis: those driving a head on any
      // of the axis's approaches. protected_left is permissive by default —
      // through and left controllers of the axis simply go green together.
      std::vector<std::string> axis_controllers;
      for (const std::string& controller : members) {
        const bool on_axis = std::ranges::any_of(axes[n], [&](std::size_t index) {
          return std::ranges::find(approaches[index].controller_odr_ids, controller) !=
                 approaches[index].controller_odr_ids.end();
        });
        if (on_axis) {
          axis_controllers.push_back(controller);
        }
      }
      if (axis_controllers.empty()) {
        continue; // an axis with no controller contributes no phase
      }
      const auto on_axis = [&](const std::string& controller) {
        return std::ranges::find(axis_controllers, controller) != axis_controllers.end();
      };
      plan.phases.push_back(build_phase(
          "axis" + std::to_string(n), kDefaultPhaseGreenSeconds, [&](const std::string& c) {
            return on_axis(c) ? SignalState::Green : SignalState::Red;
          }));
      plan.phases.push_back(build_phase("axis" + std::to_string(n) + "_clear",
                                        kDefaultPhaseYellowSeconds,
                                        [&](const std::string& c) {
                                          return on_axis(c) ? SignalState::Yellow
                                                            : SignalState::Red;
                                        }));
    }
  } else {
    // --- AUTHORED path: resolve the stored cycle. ---
    plan.authored = true;
    const std::set<std::string> member_set(members.begin(), members.end());
    for (const SignalPhase& phase : junction->phases) {
      // Stored states naming a non-member controller are dormant: ignored for
      // resolution, reported for the writer to prune and the validator to advise.
      for (const PhaseState& state : phase.states) {
        if (!member_set.contains(state.controller_odr_id) &&
            std::ranges::find(plan.dormant_controller_odr_ids, state.controller_odr_id) ==
                plan.dormant_controller_odr_ids.end()) {
          plan.dormant_controller_odr_ids.push_back(state.controller_odr_id);
        }
      }
      plan.phases.push_back(build_phase(phase.name, phase.duration, [&](const std::string& c) {
        const auto entry = std::ranges::find_if(
            phase.states, [&](const PhaseState& s) { return s.controller_odr_id == c; });
        return entry == phase.states.end() ? SignalState::Red : entry->state;
      }));
    }
  }

  // Cumulative starts and the cycle length the playhead wraps over.
  double cursor = 0.0;
  for (JunctionPhaseInfo& phase : plan.phases) {
    phase.start = cursor;
    cursor += phase.duration;
  }
  plan.cycle_duration = cursor;
  return plan;
}

std::size_t phase_index_at(const JunctionPhasePlan& plan, double t) {
  if (plan.phases.empty()) {
    return static_cast<std::size_t>(-1); // SIZE_MAX — nothing to index
  }
  const double cycle = plan.cycle_duration;
  if (!(cycle > 0.0)) {
    return 0; // a degenerate all-zero-duration cycle: pin to the first phase
  }
  double m = std::fmod(t, cycle);
  if (m < 0.0) {
    m += cycle; // negative time wraps into the cycle
  }
  for (std::size_t i = 0; i < plan.phases.size(); ++i) {
    const double end = plan.phases[i].start + plan.phases[i].duration;
    if (m < end) {
      return i;
    }
  }
  return plan.phases.size() - 1; // floating-point guard at the very end of the cycle
}

} // namespace roadmaker
