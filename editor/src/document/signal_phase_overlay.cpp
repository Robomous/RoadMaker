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

#include "document/signal_phase_overlay.hpp"

#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <optional>
#include <unordered_map>

#include "document/signal_placement.hpp" // signal_world

namespace roadmaker::editor {

namespace {

/// Height [m] the overlay floats above the pavement, matched to the Signal
/// tool's overlay lift so the two read as one layer if both are ever up.
constexpr double kPhaseOverlayLift = 0.05;

/// The mid sample of a connecting road's maneuver path — where a gate link
/// points. Mirrors the Signal tool's leader anchor.
std::optional<std::array<double, 3>> maneuver_midpoint(const std::vector<JunctionManeuverInfo>& all,
                                                       RoadId road) {
  for (const JunctionManeuverInfo& info : all) {
    if (info.road == road && !info.path.empty()) {
      return info.path[info.path.size() / 2];
    }
  }
  return std::nullopt;
}

void append_segment(std::vector<double>& links,
                    const std::array<double, 3>& a,
                    const std::array<double, 3>& b) {
  links.insert(links.end(),
               {a[0], a[1], a[2] + kPhaseOverlayLift, b[0], b[1], b[2] + kPhaseOverlayLift});
}

} // namespace

QColor signal_state_color(SignalState state) {
  switch (state) {
  case SignalState::Green:
    return QColor(0x42, 0xB2, 0x58);
  case SignalState::Yellow:
    return QColor(0xEC, 0xB4, 0x34);
  case SignalState::Off:
    return QColor(0x8A, 0x8A, 0x8A);
  case SignalState::Red:
    break;
  }
  return QColor(0xD6, 0x45, 0x38);
}

SignalPhasePreview build_signal_phase_preview(const RoadNetwork& network,
                                              JunctionId junction,
                                              const std::vector<PhaseSignalState>& signal_states,
                                              const std::vector<RoadId>& moving) {
  SignalPhasePreview preview;
  preview.moving_roads = moving;

  // One colored disc per resolved head with a world pose.
  std::unordered_map<SignalId, SignalState> head_state;
  for (const PhaseSignalState& head : signal_states) {
    head_state.emplace(head.signal, head.state);
    if (const std::optional<std::array<double, 3>> world = signal_world(network, head.signal)) {
      preview.signal_states.push_back(
          SignalPhasePreview::StateMarker{.x = (*world)[0],
                                          .y = (*world)[1],
                                          .z = (*world)[2] + kPhaseOverlayLift,
                                          .color = signal_state_color(head.state)});
    }
  }

  // Dotted gate links: from each GREEN head to every movement it gates. Tracks
  // the active greens rather than the whole structure, so the leaders come and
  // go as the author scrubs through the cycle (GW-4 step 7).
  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction);
  for (const JunctionApproachInfo& approach : junction_signals(network, junction)) {
    for (const SignalId head : approach.signal_ids) {
      const auto it = head_state.find(head);
      if (it == head_state.end() || it->second != SignalState::Green) {
        continue;
      }
      const std::optional<std::array<double, 3>> from = signal_world(network, head);
      if (!from.has_value()) {
        continue;
      }
      for (const RoadId gated : approach.gated) {
        if (const std::optional<std::array<double, 3>> to = maneuver_midpoint(maneuvers, gated)) {
          append_segment(preview.gate_links, *from, *to);
        }
      }
    }
  }
  return preview;
}

} // namespace roadmaker::editor
