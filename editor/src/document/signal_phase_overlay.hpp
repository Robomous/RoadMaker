#pragma once

// The pane-independent viewport overlay for the signal phase editor (p4-s8,
// issue #229, GW-4 steps 5-7). A PURE builder: given the effective phase's
// resolved signal states and its moving connecting roads (both from
// `mesh::junction_phases()`), it produces the colored head discs, the moving
// road list, and the dotted gate links the ViewportWidget paints in its
// QPainter overlay pass.
//
// Pure data over the kernel + the shared `signal_world` projection — the only
// Qt dependency is QColor, so it is unit-testable headless (test_phase_panel.cpp
// exercises marker count/colors, the moving list, and the gate-link pair count).
// Keeping it here rather than on the panel is what lets the PhasePanel stay
// viewport-unaware: MainWindow pulls the panel's getters, calls this, and hands
// the result to ViewportWidget::set_signal_phase_preview.

#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"

#include <QColor>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// The traffic-signal display colors, LITERAL constants (not theme tokens): a
/// timeline and a viewport disc must read as red/yellow/green regardless of the
/// editor's light or dark theme. `Off` is a dim grey (a dark or flashing head).
[[nodiscard]] QColor signal_state_color(SignalState state);

/// One phase's viewport preview: colored discs at the live heads, the moving
/// connecting roads to brighten, and the dotted gate links from each green head
/// to the movements it currently permits.
struct SignalPhasePreview {
  /// A colored disc at a resolved head's world position (kernel frame, meters).
  struct StateMarker {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    QColor color;
  };

  std::vector<StateMarker> signal_states; ///< one per resolved head with a world pose
  std::vector<RoadId> moving_roads;       ///< brightened via the viewport highlight path
  std::vector<double> gate_links;         ///< xyz pairs (6 doubles/segment), drawn dashed

  [[nodiscard]] bool empty() const {
    return signal_states.empty() && moving_roads.empty() && gate_links.empty();
  }
};

/// Builds the overlay for the phase whose resolved `signal_states` and `moving`
/// roads are given (straight off `JunctionPhaseInfo`). Head discs come from
/// `signal_world`; gate links connect each GREEN head to the midpoint of every
/// movement it gates (`junction_signals().gated` × `junction_maneuvers()`), so
/// the dotted leaders track the active greens rather than the whole structure.
[[nodiscard]] SignalPhasePreview
build_signal_phase_preview(const RoadNetwork& network,
                           JunctionId junction,
                           const std::vector<PhaseSignalState>& signal_states,
                           const std::vector<RoadId>& moving);

} // namespace roadmaker::editor
