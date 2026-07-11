#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

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
};

} // namespace roadmaker
