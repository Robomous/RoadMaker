#pragma once

// Pure CPU picking math: mouse ray generation, ray/mesh intersection over
// kernel meshes, and inverse s/t lookup on a reference line. No Qt, no GL —
// unit-testable headless. Kernel frame: right-handed, Z-up, meters; doubles
// end-to-end (camera matrices arrive as float and are widened once).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"

#include <array>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "render/renderer.hpp"

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
  RoadId road;
  LaneId lane; // invalid when the hit is not a lane patch
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
/// on the same mesh. Markings and junction floors are not pickable in M1.
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

} // namespace roadmaker::editor
