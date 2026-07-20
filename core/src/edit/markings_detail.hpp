#pragma once

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <optional>
#include <vector>

/// Arm bookkeeping shared by the junction-marking generators. It lives here
/// rather than in one generator's anonymous namespace because the stop-line
/// entity (p4-s3) moved out of edit/markings.cpp into mesh/junction_stoplines.cpp
/// and both it and the surviving lane-arrow generator must agree, to the letter,
/// on what an arm of a junction is.
namespace roadmaker::edit::markings_detail {

/// The contact end of arm `road` that belongs to `junction` (Start or End), or
/// nullopt when neither end does — the road is not an arm of this junction.
inline std::optional<ContactPoint>
facing_end(const RoadNetwork& network, RoadId road, JunctionId junction) {
  for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
    if (junction_at_end(network, RoadEnd{.road = road, .contact = contact}) == junction) {
      return contact;
    }
  }
  return std::nullopt;
}

/// Distinct arm (incoming) roads of a junction, in connection order — one entry
/// per road even when it feeds several turns. Connection order is what makes
/// every consumer's output deterministic.
inline std::vector<RoadId> distinct_arms(const Junction& record) {
  std::vector<RoadId> arms;
  for (const JunctionConnection& connection : record.connections) {
    if (std::ranges::find(arms, connection.incoming_road) == arms.end()) {
      arms.push_back(connection.incoming_road);
    }
  }
  return arms;
}

} // namespace roadmaker::edit::markings_detail
