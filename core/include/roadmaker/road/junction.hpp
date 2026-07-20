#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker {

/// One allowed route through a junction: an incoming road continues onto a
/// connecting road, with per-lane linkage.
struct JunctionConnection {
  RoadId incoming_road;

  /// The connecting road; its Road::junction points back at this junction.
  RoadId connecting_road;

  /// Which end of the connecting road touches the incoming road.
  ContactPoint contact_point = ContactPoint::Start;

  /// Pairs of {incoming lane odr_id, connecting lane odr_id}.
  std::vector<std::pair<int, int>> lane_links;
};

/// An authored override for the pavement fillet at one junction corner — the
/// re-entrant corner between two angularly adjacent arms (p4-s1, issue #225).
///
/// Corners are otherwise DERIVED: the mesher fillets every adjacent arm pair
/// with a radius read off the crossing connecting road. An entry here overrides
/// that derivation for the named pair; entries whose arms are no longer
/// adjacent (or whose roads are gone) lie dormant and are ignored.
///
/// ASAM OpenDRIVE 1.9.0 §12.10 gives `<boundary>` no corner-radius carrier, so
/// these persist as `<userData code="rm:corners">` on `<junction>` (the rm:arms
/// pattern); the exported `<boundary>`/`<elevationGrid>` stay fully derived.
struct JunctionCorner {
  /// The corner's identity: the ordered pair of CCW-adjacent arms it sits
  /// between (arm_a's right edge meeting arm_b's left edge, entering).
  RoadEnd arm_a;
  RoadEnd arm_b;

  /// Authored fillet radius [m]. Unset ⇒ the derived radius is used.
  std::optional<double> radius;

  /// Authored tangent-leg setbacks [m] from the edge-line intersection to each
  /// side's tangency point. Unset ⇒ symmetric legs from `radius`.
  std::optional<double> extent_a;
  std::optional<double> extent_b;
};

struct Junction {
  /// OpenDRIVE junction id (string, unique within a network).
  std::string odr_id;

  std::string name;

  std::vector<JunctionConnection> connections;

  /// The road ends this junction was generated from, in selection order —
  /// the deterministic input the connecting-road generator re-runs from on
  /// regeneration (docs/design/m2/02_editing_tools.md §6). Persisted in
  /// .xodr as `<userData code="rm:arms">` on the junction so edit sessions
  /// survive save/load; junctions from foreign files load with an empty arm
  /// list and cannot regenerate until recreated.
  std::vector<RoadEnd> arms;

  /// Authored corner-fillet overrides, keyed by adjacent arm pair. Sparse: a
  /// corner with no entry keeps the derived fillet. `regenerate_junction`
  /// leaves this untouched — an override outlives a turn-set change and simply
  /// goes dormant if its pair stops being adjacent.
  std::vector<JunctionCorner> corners;
};

} // namespace roadmaker
