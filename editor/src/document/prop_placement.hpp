#pragma once

// Prop placement helpers (p6-s4, issue #238), shared by the Prop Point tool, the
// Prop Curve tool, and the Library tree-drop path so all three funnel through
// identical snapping and identical id-minting. Pure geometry over the kernel's
// object model (edit::add_object / props::model) — no widgets, no kernel changes
// — so it is unit-testable headless. Resolves a world cursor to a road-relative
// station, materializes ONE prop Object from a Tree library asset, and (for the
// curve tool) distributes props at a fixed spacing along a fitted clothoid,
// projecting each onto the anchor road and minting a UNIQUE odr id per instance.

#include "roadmaker/error.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// A road-relative station on a specific road: where a snapped drop lands.
struct RoadStation {
  RoadId road;
  double s = 0.0;
  double t = 0.0;
};

/// The nearest road to (x, y) whose reference line passes within `max_t` [m], with
/// the drop's road-relative (s, t). nullopt when no road is close enough. Shared
/// by the Library drop and the prop tools so all agree on where a road is in reach.
[[nodiscard]] std::optional<RoadStation>
nearest_road_station(const RoadNetwork& network, double x, double y, double max_t);

/// Lowest positive integer odr id not already used by an object (id-unique in
/// class). For a single placement; a batch bake mints across a growing taken-set
/// instead (see distribute_props_along_curve) so N props never collide.
[[nodiscard]] std::string next_object_odr_id(const RoadNetwork& network);

/// A prop library item is a bundled tree/shrub model (Kind::Tree); anything else
/// is refused before the tool places or bakes.
[[nodiscard]] bool is_prop_asset(const LibraryItem& item);

/// The prop `Object` a Tree library `item` authors at road-relative (s, t):
/// name = the bundled model id, type Tree (or Vegetation for a shrub), and the
/// bounding radius/height from props::model. Mirrors the Library tree-drop
/// construction so a click, a drop, and a bake all land the same object.
[[nodiscard]] Object
make_prop_object(const LibraryItem& item, std::string odr_id, double s, double t);

/// The result of distributing props along a curve: the (owning road, object)
/// pairs to add, the world positions for the tool's ghost handles (one per placed
/// prop, ghost==commit), and how many samples were skipped for leaving the anchor.
struct PropCurveDistribution {
  std::vector<std::pair<RoadId, Object>> props;
  std::vector<std::array<double, 2>> preview_points;
  std::size_t skipped = 0;
};

/// Distributes props from `item` every `spacing_m` metres along a clothoid fitted
/// through `world_points`, anchored to `anchor`: samples s = 0, spacing, 2·spacing
/// … over the fit, projects each sample onto the anchor road's reference line, and
/// mints a UNIQUE odr id per placed prop (one taken-set built once — a per-call
/// next_object_odr_id would mint N identical ids). A sample that leaves the anchor
/// is skipped (counted in `skipped`), not relocated.
///
/// Errors (InvalidArgument): fewer than two points, spacing ≤ 0, a failed clothoid
/// fit, a stale/empty anchor road, or zero props surviving on the anchor.
[[nodiscard]] Expected<PropCurveDistribution>
distribute_props_along_curve(const RoadNetwork& network,
                             RoadId anchor,
                             std::span<const Waypoint> world_points,
                             const LibraryItem& item,
                             double spacing_m);

} // namespace roadmaker::editor
