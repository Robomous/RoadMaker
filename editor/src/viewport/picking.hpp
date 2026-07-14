#pragma once

// Pure CPU picking math: mouse ray generation, ray/mesh intersection over
// kernel meshes, and inverse s/t lookup on a reference line. No Qt, no GL —
// unit-testable headless. Kernel frame: right-handed, Z-up, meters; doubles
// end-to-end (camera matrices arrive as float and are widened once).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "render/renderer.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

struct Ray {
  std::array<double, 3> origin{};
  std::array<double, 3> direction{}; // normalized
};

/// World-space ray through pixel (px, py) — Qt convention: origin top-left,
/// y down — in a viewport of width x height DEVICE pixels. Unprojects the
/// near/far NDC points through inverse(projection * view).
[[nodiscard]] Ray
make_pick_ray(const CameraMatrices& camera, double px, double py, double width, double height);

/// Intersection of the pixel (px, py) ray with the ground plane z=0, in the
/// same viewport convention as make_pick_ray. nullopt when the ray is parallel
/// to the plane, points away from it (t < 0), or the hit lies farther than
/// `max_t` along the ray — callers cap that to reject near-horizon rays at low
/// pitch. Shared by the hover readout, tool events, and the ground-anchored
/// MMB pan (which pins the grabbed point under the cursor).
[[nodiscard]] std::optional<std::array<double, 3>>
ground_point(const CameraMatrices& camera,
             double px,
             double py,
             double width,
             double height,
             double max_t = std::numeric_limits<double>::infinity());

struct PickHit {
  RoadId road;         // for an object hit: its owning road
  LaneId lane;         // invalid when the hit is not a lane patch
  ObjectId object;     // valid when the hit is a placed prop (nearer than any road)
  JunctionId junction; // valid when a junction floor was hit (road/lane invalid)
  std::array<double, 3> position{};
  double distance = 0.0; // along the ray [m]
};

/// Per-road AABB prefilter; INDEX-PARALLEL to NetworkMesh::roads. Rebuild
/// whenever the mesh changes (or refresh single slots after an incremental
/// re-mesh — replaced-in-place roads keep their index).
struct RoadAabb {
  std::array<double, 3> lo{};
  std::array<double, 3> hi{};
};

[[nodiscard]] RoadAabb compute_road_aabb(const RoadMesh& road);

[[nodiscard]] std::vector<RoadAabb> compute_road_aabbs(const NetworkMesh& mesh);

/// Nearest lane-patch hit via Möller–Trumbore over roads whose AABB the ray
/// enters. Backfaces are NOT culled: roads are thin sheets and downhill-facing
/// patches must stay pickable. `road_aabbs` must come from compute_road_aabbs
/// on the same mesh. Junction floors ARE pickable (nearest triangle wins,
/// sharing the road/prop depth test) and report their JunctionId with road,
/// lane and object left invalid — every rendered surface maps to a selectable
/// entity. Lane markings remain non-pickable (they ride on the road they
/// annotate).
[[nodiscard]] std::optional<PickHit>
pick(const NetworkMesh& mesh, std::span<const RoadAabb> road_aabbs, const Ray& ray);

/// Road-relative coordinates per ASAM OpenDRIVE §8.3 (v1.9.0): s along the
/// reference line measured in the x/y plane, t perpendicular and POSITIVE TO
/// THE LEFT of the direction of travel.
struct StationCoord {
  double s = 0.0;
  double t = 0.0;
};

/// Nearest station to plan-view point (x, y): coarse scan over evaluate(s)
/// (step clamped to [0.25, 5] m), then golden-section refinement of the
/// bracketing interval. The kernel has no analytic inverse projection; the
/// coarse pass keeps multimodal cases (hairpins) on the right branch.
[[nodiscard]] StationCoord find_station(const ReferenceLine& line, double x, double y);

/// A hovered authoring waypoint of one of `roads`: which road, its index, and
/// position. The shared node hit-test behind SelectTool, EditNodesTool, and the
/// context menu — nearest effective waypoint within `radius` [m], ties keep the
/// first road in `roads` order.
struct WaypointHit {
  RoadId road;
  std::size_t index = 0;
  Waypoint position;
};

[[nodiscard]] std::optional<WaypointHit> pick_waypoint(
    const RoadNetwork& network, std::span<const RoadId> roads, double x, double y, double radius);

} // namespace roadmaker::editor
