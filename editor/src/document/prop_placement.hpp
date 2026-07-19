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
#include <cstdint>
#include <optional>
#include <random>
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

/// A prop library item is placeable when it is a bundled tree/shrub model
/// (Kind::Tree), or a Kind::PropSet with at least one entry whose every model
/// resolves via props::model. Anything else is refused before the tool places
/// or bakes.
[[nodiscard]] bool is_prop_asset(const LibraryItem& item);

/// Resolves a prop asset to a concrete Tree item for one placement. A Tree is
/// returned unchanged; a PropSet draws one entry via a portion-weighted
/// std::discrete_distribution over `rng` and returns a SYNTHETIC Tree item
/// (kind = Tree, model = the drawn model, key/label carried from the set). The
/// draw semantics per tool: Point = one draw per click; Curve/Polygon = one
/// draw per instance; Span = ONE draw per span.
[[nodiscard]] LibraryItem resolve_prop_asset(const LibraryItem& item, std::mt19937& rng);

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
///
/// `seed` seeds a local RNG for the PropSet draw (one draw per placed instance,
/// so a mixed set scatters); a plain Tree item ignores it and behaves exactly as
/// before. The default keeps existing callers source-compatible.
[[nodiscard]] Expected<PropCurveDistribution>
distribute_props_along_curve(const RoadNetwork& network,
                             RoadId anchor,
                             std::span<const Waypoint> world_points,
                             const LibraryItem& item,
                             double spacing_m,
                             std::uint32_t seed = 0);

// ---- Prop Span (p6-s5, issue #239) -----------------------------------------

/// The `<repeat>` a span authors between stations `s_a` and `s_b` on one road:
/// origin s = min(s_a, s_b), length = |s_a - s_b|, `distance` between instances,
/// t and z held constant across the section (`t_start == t_end == t`, z = 0).
/// Shared by make_prop_span_object (the committed object) and the tool preview
/// (span_preview_points) so the ghost matches what the mesher expands.
[[nodiscard]] ObjectRepeat make_span_repeat(double s_a, double s_b, double t, double distance);

/// The single prop `Object` a span authors: a base prop at (min(s_a,s_b), t)
/// carrying exactly ONE `<repeat>` (make_span_repeat) so the kernel mesher places
/// a series of `item`'s model along the road. A span is ONE object with ONE
/// model — a PropSet must be pre-resolved (resolve_prop_asset) to a concrete Tree
/// by the caller, since a repeat cannot mix models.
///
/// Errors (InvalidArgument): a non-prop `item`, `distance <= 0`, or a section
/// shorter than tol::kLength (the two stations coincide).
[[nodiscard]] Expected<Object> make_prop_span_object(
    const LibraryItem& item, std::string odr_id, double s_a, double s_b, double t, double distance);

/// The world (x, y) of every instance `repeat` places on `road`, via
/// expand_repeat + station_to_world — the SAME projection the mesher uses, so a
/// span ghost lands exactly where the baked instances render. Instances whose
/// absolute s overshoots the road length are dropped (mesh_builder does the same).
/// Empty when the road is stale/empty.
[[nodiscard]] std::vector<std::array<double, 2>>
span_preview_points(const RoadNetwork& network, RoadId road, const ObjectRepeat& repeat);

// ---- Prop Polygon (p6-s5, issue #239) --------------------------------------

/// A polygon region's interior sits OFF the reference line (a park, a verge
/// several metres wide), so a scattered sample is anchored to the nearest road
/// within this generous lateral reach [m] rather than the 12 m object snap — the
/// props stay road-relative but the region need not hug the carriageway.
inline constexpr double kPolygonAnchorMaxT = 50.0;

/// Scatter density and seed for distribute_props_in_polygon. `density_per_100m2`
/// props are targeted per 100 m² of region area; `seed` makes the whole scatter
/// (positions AND per-sample PropSet draws) reproducible.
struct PropScatterParams {
  double density_per_100m2 = 4.0;
  std::uint32_t seed = 0;
};

/// Scatters props from `item` across the closed `polygon` (region vertices in
/// world order): target = max(1, round(area · density / 100)) via the shoelace
/// area, rejection-sampled uniformly over the polygon's bounding box (capped at
/// 100× target attempts) and tested with a crossing-number point-in-polygon. Each
/// accepted sample anchors to the nearest road within kPolygonAnchorMaxT (else it
/// is counted in `skipped`), a PropSet draws one model per sample (resolve_prop_asset
/// over the seeded RNG), and every placed prop mints a UNIQUE odr id from one
/// growing taken-set. Deterministic: identical inputs + seed → identical output.
///
/// Errors (InvalidArgument): fewer than three vertices, a degenerate (≈0) area,
/// a non-prop item, or zero samples landing on any road.
[[nodiscard]] Expected<PropCurveDistribution>
distribute_props_in_polygon(const RoadNetwork& network,
                            std::span<const Waypoint> polygon,
                            const LibraryItem& item,
                            const PropScatterParams& params);

} // namespace roadmaker::editor
