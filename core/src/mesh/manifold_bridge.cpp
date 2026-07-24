/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "manifold_bridge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace roadmaker::bridge {

namespace {

/// Unit-length face normal of triangle (a, b, c); {0,0,1} for a degenerate
/// triangle so a sliver never yields a NaN normal.
std::array<double, 3> face_normal(const std::array<double, 3>& a,
                                  const std::array<double, 3>& b,
                                  const std::array<double, 3>& c) {
  const std::array<double, 3> ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const std::array<double, 3> ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  std::array<double, 3> n{
      (ab[1] * ac[2]) - (ab[2] * ac[1]),
      (ab[2] * ac[0]) - (ab[0] * ac[2]),
      (ab[0] * ac[1]) - (ab[1] * ac[0]),
  };
  const double len = std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2]));
  if (len < 1e-12) {
    return {0.0, 0.0, 1.0};
  }
  return {n[0] / len, n[1] / len, n[2] / len};
}

} // namespace

manifold::Manifold
sweep_section(const std::vector<SectionPoint>& section, int n_div, const SweepFrameFn& frame) {
  if (section.size() < 3 || n_div < 1) {
    return manifold::Manifold();
  }
  // The cross-section polygon lives in the extruder's XY plane as (t, w); Extrude
  // raises it along +Z to z=1 with n_div intermediate station loops, so the raw
  // prism has vertices (t, w, u) with u in [0, 1]. Extrude guarantees a closed,
  // consistently wound solid — we never touch the winding ourselves.
  manifold::SimplePolygon contour;
  contour.reserve(section.size());
  for (const SectionPoint& p : section) {
    contour.push_back({p.t, p.w});
  }
  const manifold::Manifold prism = manifold::Manifold::Extrude({contour}, 1.0, n_div);

  // Warp bends the straight prism onto the reference line: each vertex's z IS
  // the longitudinal fraction u, its x/y ARE the section coords (t, w). Warp
  // moves vertices only, so topology — and watertightness — survive intact.
  return prism.Warp([&frame](manifold::vec3& p) {
    const std::array<double, 3> world = frame(p.x, p.y, p.z);
    p.x = world[0];
    p.y = world[1];
    p.z = world[2];
  });
}

manifold::Manifold box(std::array<double, 3> center, std::array<double, 3> size) {
  return manifold::Manifold::Cube({size[0], size[1], size[2]}, /*center=*/true)
      .Translate({center[0], center[1], center[2]});
}

SubMesh to_submesh(const manifold::Manifold& solid) {
  SubMesh out;
  const manifold::MeshGL mesh = solid.GetMeshGL();
  const std::size_t tri_count = mesh.NumTri();
  if (tri_count == 0) {
    return out;
  }
  const std::uint32_t stride = mesh.numProp;
  const auto vpos = [&](std::uint32_t v) -> std::array<double, 3> {
    const std::size_t base = static_cast<std::size_t>(v) * stride;
    return {static_cast<double>(mesh.vertProperties[base + 0]),
            static_cast<double>(mesh.vertProperties[base + 1]),
            static_cast<double>(mesh.vertProperties[base + 2])};
  };

  out.positions.reserve(tri_count * 9);
  out.normals.reserve(tri_count * 9);
  out.uvs.reserve(tri_count * 6);
  out.indices.reserve(tri_count * 3);
  for (std::size_t tri = 0; tri < tri_count; ++tri) {
    const std::array<double, 3> a = vpos(mesh.triVerts[(tri * 3) + 0]);
    const std::array<double, 3> b = vpos(mesh.triVerts[(tri * 3) + 1]);
    const std::array<double, 3> c = vpos(mesh.triVerts[(tri * 3) + 2]);
    const std::array<double, 3> n = face_normal(a, b, c);
    const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size() / 3);
    for (const std::array<double, 3>& v : {a, b, c}) {
      out.positions.insert(out.positions.end(), v.begin(), v.end());
      out.normals.insert(out.normals.end(), n.begin(), n.end());
      out.uvs.insert(out.uvs.end(), {v[0], v[1]}); // planar UV in meters
    }
    out.indices.insert(out.indices.end(), {base, base + 1, base + 2});
  }
  return out;
}

} // namespace roadmaker::bridge
