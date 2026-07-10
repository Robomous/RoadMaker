#pragma once

#include "roadmaker/edit/command.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// Edit-operation factories (docs/m2/01_editing_framework.md §2.3). Each
/// returns a Command capturing value snapshots of exactly the objects it
/// touches; validation happens in the factory where possible, and a factory
/// given invalid input returns a command whose apply() reports the error
/// without touching the network. Create a command and push it immediately —
/// snapshots are taken against the network state at factory time.
namespace roadmaker::edit {

// --- geometry (re-fit clothoid through authoring waypoints, §2.5) ----------
// Roads loaded without rm:waypoints get them derived from geometry-record
// endpoints on their first node edit; the re-fit reproduces line/arc/spiral
// chains within rm::tol and is geometry-altering for paramPoly3 roads.

[[nodiscard]] RM_API std::unique_ptr<Command>
move_waypoint(const RoadNetwork& network, RoadId road, std::size_t index, Waypoint to);

/// Inserts before `index` (index == waypoint count appends).
[[nodiscard]] RM_API std::unique_ptr<Command>
insert_waypoint(const RoadNetwork& network, RoadId road, std::size_t index, Waypoint at);

[[nodiscard]] RM_API std::unique_ptr<Command>
delete_waypoint(const RoadNetwork& network, RoadId road, std::size_t index);

// --- topology ---------------------------------------------------------------

/// Authors a new clothoid road (auto-assigned OpenDRIVE id). Undo frees the
/// id; redo resurrects the identical road under the same ids.
[[nodiscard]] RM_API std::unique_ptr<Command>
create_road(std::vector<Waypoint> waypoints, LaneProfile profile, std::string name);

/// Splits at station s: the original keeps [0, s), a new road (auto id)
/// gets [s, length). Sections, profiles, links and lane links are carried
/// over. M2 restriction: roads participating in a junction cannot be split,
/// and the split station must not fall inside a paramPoly3 record.
[[nodiscard]] RM_API std::unique_ptr<Command>
split_road(const RoadNetwork& network, RoadId road, double s);

/// Deletes the road with its sections and lanes; junction connections and
/// road links referencing it are detached. Undo restores every object under
/// its original id, so references held elsewhere become valid again.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_road(const RoadNetwork& network, RoadId road);

/// Creates a junction record (auto id) and links each road end to it. M2
/// scope: topology only — connecting-road generation is the Create Junction
/// tool's kernel work (issue #17). Errors: fewer than 2 ends, duplicate
/// ends, or an end whose link slot is already occupied.
[[nodiscard]] RM_API std::unique_ptr<Command> create_junction(const RoadNetwork& network,
                                                              std::span<const RoadEnd> ends);

/// Deletes the junction; roads pointing at it (back-references and links)
/// are detached. Connecting roads survive, as with RoadNetwork semantics.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_junction(const RoadNetwork& network,
                                                              JunctionId junction);

// --- lanes ------------------------------------------------------------------

/// Adds a new outermost lane on `side` (+1 left, -1 right), copying the
/// current outermost lane's width profile (3.5 m constant when the side is
/// empty).
[[nodiscard]] RM_API std::unique_ptr<Command>
add_lane(const RoadNetwork& network, LaneSectionId section, int side, LaneType type);

/// Removes a lane. M2 restriction: only the outermost lane of its side can
/// be removed (keeps OpenDRIVE lane numbering contiguous); the center lane
/// never can. Dangling links in adjacent sections are cleared.
[[nodiscard]] RM_API std::unique_ptr<Command> remove_lane(const RoadNetwork& network, LaneId lane);

[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_type(const RoadNetwork& network, LaneId lane, LaneType type);

/// Constant width in M2 (replaces the width profile with one record).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_width(const RoadNetwork& network, LaneId lane, double width_m);

/// Replaces the lane's outer-boundary marking (single record in M2).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_road_mark(const RoadNetwork& network, LaneId lane, RoadMark mark);

// --- profiles ---------------------------------------------------------------

/// Sets the elevation at one authoring waypoint; the road's elevation
/// profile becomes piecewise-linear through all waypoint elevations (an
/// all-zero profile is written as no profile).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_node_elevation(const RoadNetwork& network, RoadId road, std::size_t waypoint_index, double z);

// --- document ---------------------------------------------------------------

[[nodiscard]] RM_API std::unique_ptr<Command>
rename_road(const RoadNetwork& network, RoadId road, std::string name);

} // namespace roadmaker::edit
