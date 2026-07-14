#pragma once

// Marking authoring: derive road-marking <object>s (crosswalks, later stop
// lines / arrows) from network topology. Pure geometry — returns objects ready
// for edit::add_object; the editor groups the adds into one undo step. Grounded
// in the crosswalk mesh path (docs/design/m3a/02 road marks).

#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp"

#include <utility>
#include <vector>

namespace roadmaker {
class RoadNetwork;
}

namespace roadmaker::edit {

/// Parameters for junction-arm crosswalk authoring.
struct CrosswalkParams {
  double depth_m = 3.0;   ///< crosswalk extent ALONG the road (walking depth)
  double setback_m = 1.0; ///< gap from the junction edge to the near stripe
};

/// A zebra crosswalk `Object` for each distinct arm road of `junction`, spanning
/// the arm's driving lanes just inside the junction (the "Add crosswalks to all
/// arms" action). Returns {owning road, object} pairs ready for
/// `edit::add_object`; the caller adds them (the editor as one undo macro). Each
/// object carries a unique `odr_id`. Empty when the junction is stale, foreign
/// (no resolvable arm ends), or has no driving lanes. Pure — no mutation, so it
/// is headless-testable and Python-bindable.
[[nodiscard]] RM_API std::vector<std::pair<RoadId, Object>> junction_crosswalks(
    const RoadNetwork& network, JunctionId junction, const CrosswalkParams& params = {});

} // namespace roadmaker::edit
