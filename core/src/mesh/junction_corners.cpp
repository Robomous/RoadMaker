#include "roadmaker/mesh/junction_corners.hpp"

#include "roadmaker/road/junction.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "junction_corner_detail.hpp"

namespace roadmaker {

namespace {

using junction_corner_detail::corner_faces;
using junction_corner_detail::CornerFace;
using junction_corner_detail::CornerSolution;
using junction_corner_detail::solve_corner;

} // namespace

std::array<double, 2> JunctionCornerInfo::apex() const {
  if (curve.empty()) {
    return corner;
  }
  CornerSolution solution;
  solution.corner = corner;
  solution.phi = phi;
  solution.tangent_a = tangent_a;
  solution.tangent_b = tangent_b;
  return junction_corner_detail::corner_curve_point(solution, 0.5);
}

std::vector<JunctionCornerInfo> junction_corners(const RoadNetwork& network,
                                                 JunctionId junction_id) {
  std::vector<JunctionCornerInfo> corners;
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return corners;
  }
  const std::vector<CornerFace> faces = corner_faces(network, *junction);
  if (faces.size() < 2) {
    return corners;
  }
  corners.reserve(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const CornerFace& a = faces[i];
    const CornerFace& b = faces[(i + 1) % faces.size()];
    const CornerSolution solution = solve_corner(network, *junction, a, b);
    if (!solution.valid) {
      continue; // parallel edges, a corner behind a face, or near-tangent arms
    }
    corners.push_back(JunctionCornerInfo{
        .arm_a = a.arm,
        .arm_b = b.arm,
        .corner = solution.corner,
        .dir_a = solution.dir_a,
        .dir_b = solution.dir_b,
        .bisector = solution.bisector,
        .face_a = a.right,
        .face_b = b.left,
        .phi = solution.phi,
        .extent_a = solution.extent_a,
        .extent_b = solution.extent_b,
        .tangent_a = solution.tangent_a,
        .tangent_b = solution.tangent_b,
        .radius = solution.radius,
        .max_radius = solution.max_radius,
        .max_extent_a = solution.max_extent_a,
        .max_extent_b = solution.max_extent_b,
        .radius_authored = solution.radius_authored,
        .extents_authored = solution.extents_authored,
        .curve = junction_corner_detail::corner_curve(solution),
    });
  }
  return corners;
}

} // namespace roadmaker
