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

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

#include <span>

namespace roadmaker {

/// Deterministic bridge-solid generator parameters (p5-s3, #233, design §4).
/// These live on the mesh options, NOT in the `.xodr` — the `<bridge>` record is
/// the span, and regenerating with different parameters is a mesh concern, not a
/// document edit. Defaults are the seed's approved values.
struct BridgeParams {
  /// Emit the bridge-solid channel at all. Off ⇒ mesh.bridges stays empty.
  bool enabled = true;
  double deck_depth = 0.8;       ///< visible deck thickness [m]
  double deck_overhang = 0.5;    ///< deck extends this far past each outer lane edge [m]
  double pier_free_span = 30.0;  ///< spans up to this need no pier [m]
  double pier_spacing = 25.0;    ///< above pier_free_span, a pier at least this often [m]
  double pier_size = 1.2;        ///< square pier section [m]
  double guardrail_height = 1.0; ///< guardrail height above the deck [m]
  double guardrail_width = 0.2;  ///< guardrail thickness [m]
};

struct MeshOptions {
  SamplingOptions sampling;

  /// Bridge-solid generation (p5-s3, #233).
  BridgeParams bridges;

  /// Emit lane-marking strips.
  bool markings = true;

  /// Emit plan-view junction floors.
  bool junction_floors = true;

  /// Emit the scene terrain channel (p5-s2, #232). With a height field present
  /// this triangulates the ground around the network; off, or with no field,
  /// the terrain channel stays empty and the scene looks exactly as it did
  /// before terrain existed.
  bool terrain = true;

  /// Width [m] of the skirt band along road edges over which the road-edge z
  /// wins over the raw field sample (p5-s2, #232): the ground blends up (or
  /// down) to meet the kerb across this distance, so there is no cliff at the
  /// seam. Beyond it the terrain is the bare field.
  double terrain_skirt = 8.0;
};

/// Tessellates every road (and junction floor) of the network.
/// Degenerate inputs (roads without geometry or lanes) are skipped — the
/// xodr parser already diagnosed them.
[[nodiscard]] RM_API NetworkMesh build_network_mesh(const RoadNetwork& network,
                                                    const MeshOptions& options = {});

/// Re-tessellates ONLY the listed roads, updating `mesh` in place: existing
/// entries are replaced, new roads appended, erased or degenerate roads
/// removed. Untouched roads keep their vertex buffers untouched (the editor
/// re-uploads only what changed). Meshing stays a pure function of the
/// network — this is the incremental entry point for the same result
/// build_network_mesh produces from scratch (docs/m2/01 §5).
RM_API void remesh_roads(const RoadNetwork& network,
                         NetworkMesh& mesh,
                         std::span<const RoadId> roads,
                         const MeshOptions& options = {});

/// Re-tessellates ONLY the marking layer (lane road marks + object markings:
/// crosswalks, stop lines, lane arrows) of the listed roads, in place — the
/// road surface grid and lane patches are left untouched, so the editor
/// re-uploads only what an object edit changed. This is the mesh consumer of
/// edit::DirtySet::objects (docs/design/m3a/01 §2.4). A road not yet present
/// in `mesh` is built in full; markings off clears them.
RM_API void remesh_objects(const RoadNetwork& network,
                           NetworkMesh& mesh,
                           std::span<const RoadId> roads,
                           const MeshOptions& options = {});

/// Same contract for junction floors, keyed by JunctionId. With
/// options.junction_floors off, listed junctions simply lose their floors.
RM_API void remesh_junctions(const RoadNetwork& network,
                             NetworkMesh& mesh,
                             std::span<const JunctionId> junctions,
                             const MeshOptions& options = {});

/// Same contract for enclosed-area ground surfaces (#215), keyed by SurfaceId:
/// existing entries are replaced, new surfaces appended, and surfaces that no
/// longer exist or enclose no area removed. Surfaces are const-meshed from the
/// arena derive_surfaces owns — this never re-derives the ring topology.
RM_API void remesh_surfaces(const RoadNetwork& network,
                            NetworkMesh& mesh,
                            std::span<const SurfaceId> surfaces,
                            const MeshOptions& options = {});

/// Rebuilds the scene terrain channel wholesale (p5-s2, #232). There is one
/// height field per network, so unlike the keyed re-mesh entry points this
/// takes no id span: it clears `mesh.terrain` and, when the network carries a
/// field and options.terrain is on, refills it. Const-meshed from the network,
/// like every other channel — this never mutates the field.
RM_API void
remesh_terrain(const RoadNetwork& network, NetworkMesh& mesh, const MeshOptions& options = {});

/// Rebuilds the bridge-solid channel wholesale (p5-s3, #233). Clears
/// `mesh.bridges` and, when `options.bridges.enabled`, regenerates one solid per
/// `<bridge>` span on every road. Const-meshed from the network: the solids are
/// derived from the span record plus the current road geometry (elevation,
/// superelevation, width) and the height field (for pier footings), so they
/// follow an elevation edit without a command. A span too short to build a
/// sensible solid is skipped, not an error.
RM_API void
remesh_bridges(const RoadNetwork& network, NetworkMesh& mesh, const MeshOptions& options = {});

} // namespace roadmaker
