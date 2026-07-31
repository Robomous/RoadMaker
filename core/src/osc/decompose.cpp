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

/// The junction-timeline -> TrafficSignalController decomposition (p8-s1 PR-D,
/// issue #245). See osc/decompose.hpp for why this exists at all.
///
/// EVERY SORT IN THIS FILE IS LOAD-BEARING. `osc/writer.cpp` states its own
/// discipline as "there is no sort in this file — if a future change
/// introduces an arena or map traversal, the sort belongs at the point of
/// construction". This IS that point. `RoadNetwork::for_each_controller` walks
/// arena SLOTS ascending and a freed slot is reused, so after any erase it is
/// not creation order; without the sorts below, two networks holding the same
/// signals could write two different files.

#include "roadmaker/osc/decompose.hpp"

#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace roadmaker::osc {
namespace {

/// Findings are document-scoped advisories about the NETWORK, so none carries
/// a rule id: the OpenSCENARIO catalogue constrains the file, and nothing that
/// happens here has produced a file yet. Every one of them is a RoadMaker-side
/// limitation, which is the same split `validate_network` uses.
Diagnostic advisory(std::string location, std::string message) {
  return Diagnostic{.severity = Severity::Warning,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = {},
                    .road = {},
                    .lane = {}};
}

std::string junction_location(const RoadNetwork& network, JunctionId junction) {
  const Junction* record = network.junction(junction);
  return record == nullptr ? std::string{"junction (stale id)"}
                           : fmt::format("junction id={}", record->odr_id);
}

} // namespace

std::string_view state_token(SignalState state) {
  // A switch with no `default`, so a fifth SignalState breaks the build here
  // rather than silently exporting as one of these four.
  switch (state) {
  case SignalState::Red:
    return "red";
  case SignalState::Yellow:
    return "yellow";
  case SignalState::Green:
    return "green";
  case SignalState::Off:
    return "off";
  }
  return "red";
}

JunctionSignalDecomposition decompose_junction_signals(const RoadNetwork& network,
                                                       JunctionId junction) {
  JunctionSignalDecomposition out;
  const std::string where = junction_location(network, junction);

  // The PLAN, never Junction::phases — the Red-fill happens here and nowhere
  // else (osc/decompose.hpp, ADR-0014 §8).
  const JunctionPhasePlan plan = junction_phases(network, junction);
  if (plan.phases.empty()) {
    out.findings.push_back(
        advisory(where,
                 "no signal cycle to export: the junction is unsignalized, span (virtual), "
                 "signalized by a static template, or the id is stale. No TrafficSignalController "
                 "is emitted."));
    return out;
  }

  if (!plan.dormant_controller_odr_ids.empty()) {
    std::string dormant;
    for (const std::string& id : plan.dormant_controller_odr_ids) {
      dormant += dormant.empty() ? "" : ", ";
      dormant += id;
    }
    out.findings.push_back(
        advisory(where,
                 fmt::format("{} authored phase state(s) name controller(s) that are not live "
                             "members of this junction's sync group ({}); they are not exported.",
                             plan.dormant_controller_odr_ids.size(),
                             dormant)));
  }

  // OpenDRIVE @id -> the signal ids that controller's <control> children own.
  // std::map, so the traversal below is by id and never by arena slot.
  std::map<std::string, std::set<std::string>> owned;
  network.for_each_controller([&owned](ControllerId, const Controller& controller) {
    std::set<std::string>& ids = owned[controller.odr_id];
    for (const Control& control : controller.controls) {
      ids.insert(control.signal_odr_id);
    }
  });

  // The junction's MEMBER controllers only. A controller belonging to another
  // junction has no row in this timeline, so exporting it here would emit a
  // controller whose every phase is empty — a signal group that is never
  // anything. Sorted, per this file's header.
  std::set<std::string> members(plan.controller_odr_ids.begin(), plan.controller_odr_ids.end());

  for (const std::string& controller_odr_id : members) {
    const auto controls = owned.find(controller_odr_id);
    if (controls == owned.end()) {
      out.findings.push_back(
          advisory(where,
                   fmt::format("member controller '{}' is named by the junction's sync "
                               "group but no live <controller> carries that id; it is "
                               "not exported.",
                               controller_odr_id)));
      continue;
    }

    TrafficSignalController exported;
    exported.name = controller_odr_id; // the OpenDRIVE @id, §10.10 — never Controller::name.

    bool carries_any_state = false;
    for (const JunctionPhaseInfo& info : plan.phases) {
      Phase phase;
      phase.name = info.name; // may be empty; the writer synthesizes and de-duplicates.
      phase.duration = info.duration;

      for (const PhaseSignalState& entry : info.signal_states) {
        const Signal* head = network.signal(entry.signal);
        if (head == nullptr) {
          out.findings.push_back(
              advisory(where,
                       fmt::format("phase '{}' of controller '{}' names a signal handle "
                                   "that no longer resolves; that state is omitted rather "
                                   "than written as an empty trafficSignalId.",
                                   info.name,
                                   controller_odr_id)));
          continue;
        }
        if (head->odr_id.empty()) {
          out.findings.push_back(
              advisory(where,
                       fmt::format("phase '{}' of controller '{}' names a signal with an "
                                   "empty @id; that state is omitted rather than written "
                                   "as an empty trafficSignalId.",
                                   info.name,
                                   controller_odr_id)));
          continue;
        }
        if (!controls->second.contains(head->odr_id)) {
          continue; // another member controller's head — its own row carries it.
        }

        TrafficSignalState state;
        state.traffic_signal_id = head->odr_id;
        state.state = std::string{state_token(entry.state)};
        phase.signal_states.push_back(std::move(state));
      }

      std::sort(phase.signal_states.begin(),
                phase.signal_states.end(),
                [](const TrafficSignalState& a, const TrafficSignalState& b) {
                  return a.traffic_signal_id < b.traffic_signal_id;
                });
      carries_any_state = carries_any_state || !phase.signal_states.empty();
      exported.phases.push_back(std::move(phase));
    }

    if (!carries_any_state) {
      out.findings.push_back(
          advisory(where,
                   fmt::format("controller '{}' controls no signal head that this "
                               "junction's cycle resolves, so its phases carry no states.",
                               controller_odr_id)));
    }
    out.controllers.push_back(std::move(exported));
  }

  return out;
}

} // namespace roadmaker::osc
