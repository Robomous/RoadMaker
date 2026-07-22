// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/lane.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace roadmaker {

/// Triangle mesh with its own buffers. Kernel frame: right-handed, Z-up,
/// meters; positions/normals are xyz triplets; indices are CCW when viewed
/// from +Z (up). double here — conversion to float happens at export/render
/// boundaries only.
struct SubMesh {
  std::vector<double> positions;
  std::vector<double> normals;
  /// Planar texture coordinates in meters (u=s along the road, v=t across),
  /// one uv pair per position. Empty = untextured (markings, junction floors
  /// until the render layer assigns them). Sized 2*(positions/3) when present.
  std::vector<double> uvs;
  std::vector<std::uint32_t> indices;

  /// Material class for surfaces; markings and junction floors carry their
  /// role in `name` and are materialized separately by exporters.
  LaneType material = LaneType::None;

  /// e_roadMarkColor of the lane marking this submesh paints (§11.9), so the
  /// render/export layer can pick the paint colour instead of assuming white.
  /// `Standard` on everything that is not a lane marking — including object
  /// markings (crosswalks, stop lines, arrows), whose colour is an object
  /// property, not a roadMark one.
  RoadMarkColor mark_color = RoadMarkColor::Standard;

  /// Assigned material code for painted object markings (§13.8), e.g.
  /// "material.paint_white" from a crosswalk asset's Default Material — lets the
  /// viewport/exporter tint the marking from the material library. Empty when
  /// the marking carries no material (the renderer then uses its paint colour).
  /// Parallel to RoadMesh::LanePatch::surface.
  std::string surface;

  std::string name;
};

/// One road's tessellation. Lane patches index into the SHARED grid buffers
/// (positions/normals) so adjacent lanes reuse identical boundary vertices —
/// the road surface is watertight by construction.
struct RoadMesh {
  RoadId road;
  std::string name;

  /// Shared vertex grid for all lane patches (xyz triplets).
  std::vector<double> positions;
  std::vector<double> normals;
  /// Planar texture coordinates (u=s, v=t in meters), one uv pair per grid
  /// vertex — parallel to positions/normals, so lane patches indexing the
  /// shared grid inherit continuous UVs across boundaries.
  std::vector<double> uvs;

  struct LanePatch {
    LaneId lane;
    int odr_lane_id = 0;
    LaneType material = LaneType::None;
    /// Assigned surface material code from the covering <material> record
    /// (§11.8.2), e.g. "rm:asphalt_worn". Empty when the lane carries no
    /// material there — the renderer then falls back to the `material` lane
    /// type palette. A lane whose material varies along s is split into one
    /// patch per record so each patch carries a single code.
    std::string surface;
    /// Triangles into RoadMesh::positions/normals.
    std::vector<std::uint32_t> indices;
  };

  std::vector<LanePatch> lanes;

  /// Lane markings as thin quad strips slightly above the surface —
  /// separate primitives, never baked into textures (M1 rule).
  std::vector<SubMesh> markings;
};

/// One junction's blended surface, keyed by its id so incremental re-meshing
/// can replace exactly the affected entry.
struct JunctionFloor {
  JunctionId junction;
  SubMesh mesh;

  /// Authored corner overlays (p4-s2, issue #226): sidewalk wedges and median
  /// noses, lifted just above the floor and materialled independently of it.
  /// Empty unless a JunctionCorner authors a material, so a junction nobody
  /// has painted meshes and exports exactly as it did before the feature —
  /// `mesh` itself is never cut, only overlaid.
  std::vector<SubMesh> details;
};

/// One enclosed-area ground surface (#215), keyed by its SurfaceId so
/// incremental re-meshing can replace exactly the affected entry and the
/// editor/framing can resolve a SurfaceId back to its geometry.
struct SurfaceMesh {
  SurfaceId surface;
  SubMesh mesh;
};

/// A placed prop (tree/vegetation) as an INSTANCE of a bundled prop model
/// (roadmaker::props). The renderer and the glTF/USD exporters draw
/// props::model(model_id) at this world transform — no per-prop geometry is
/// baked into the mesh, so many props of one model share a single mesh and one
/// draw path. World frame: right-handed, Z-up, meters; heading rotates about
/// +Z. `position` is the prop's base centre (sits on the road surface).
struct ObjectInstance {
  ObjectId object;                  ///< source OpenDRIVE <object>
  RoadId road;                      ///< owning road (DirtySet::objects channel)
  std::string model_id;             ///< prop_library id, e.g. "tree_pine"
  std::array<double, 3> position{}; ///< world origin (base centre), xyz
  double heading = 0.0;             ///< world heading [rad] about +Z
  /// Uniform scale applied to the model, derived from the object's declared
  /// OpenDRIVE @height (props::instance_scale). 1.0 = draw at model size.
  double scale = 1.0;
};

/// The editable text face of a static sign, as a MODEL-SPACE quad the renderer
/// and glTF exporter drape a text-to-texture bitmap (roadmaker::signs::
/// render_face) over. Present on a SignalInstance only when the signal is static,
/// declares non-empty @text, and its model carries a props::FacePlate. Geometry
/// is in the same model frame as props::model(model_id); consumers apply the
/// instance transform (position/heading/scale). The quad sits +0.005 m in front
/// of the plate's +x face; UVs are [0,1] with v=0 at the TOP row — matching the
/// row-0-top FaceBitmap and glTF's top-left texture origin, so one bitmap serves
/// both with no flips. The bitmap itself is NOT stored here: it is cheap to
/// re-raster and consumers cache it keyed on (model_id, text).
struct SignalFaceOverlay {
  std::string text;                   ///< the @text the draped bitmap renders
  std::vector<double> positions;      ///< model-space xyz, 4 verts
  std::vector<double> normals;        ///< model-space xyz (+x), 4 verts
  std::vector<double> uvs;            ///< uv per vert, 4 pairs, in [0,1]
  std::vector<std::uint32_t> indices; ///< 2 triangles (CCW seen from +x)
};

/// A placed <signal> as an INSTANCE of a bundled signal model
/// (roadmaker::props — "signal_light" for a dynamic signal, "sign_generic" for
/// a static one). Same instanced draw path as ObjectInstance: no per-signal
/// geometry is baked, so many signals share one mesh. World frame is Z-up,
/// meters; `position` is the pole base (sits on the road surface at z_offset);
/// `heading` faces the model's +x front along the road tangent + hOffset.
struct SignalInstance {
  SignalId signal;                  ///< source OpenDRIVE <signal>
  RoadId road;                      ///< owning road (DirtySet::objects channel)
  std::string model_id;             ///< prop_library id ("signal_light"/"sign_generic")
  std::array<double, 3> position{}; ///< world origin (pole base), xyz
  double heading = 0.0;             ///< world heading [rad] about +Z
  /// Editable text face, present only for a static sign with non-empty @text on
  /// a face-plate model (e.g. "sign_plate"). Absent on lights and blank signs.
  std::optional<SignalFaceOverlay> face;
};

/// Whole-network tessellation result.
struct NetworkMesh {
  std::vector<RoadMesh> roads;

  /// Blended 2.5D junction surfaces (Clipper2 union → CDT → harmonic elevation
  /// field, stitched watertight to the road meshes). Built by
  /// junction_surface.cpp — docs/design/m2/03_junction_blending.md.
  std::vector<JunctionFloor> junction_floors;

  /// Enclosed-area ground surfaces (#215): the ground that fills areas ringed
  /// by roads, built by surface_fill.cpp — same Clipper2 union → CDT → harmonic
  /// field → watertight-stitch backend as junction floors, but keeping the
  /// union's interior HOLE. Populated from whatever surfaces already exist in
  /// the arena (derive_surfaces owns that arena); build_network_mesh never
  /// derives them itself.
  std::vector<SurfaceMesh> surfaces;

  /// Placed props (trees/vegetation), instanced from the bundled prop library
  /// — regenerated per owning road via the DirtySet::objects channel
  /// (remesh_objects), so a prop edit never re-tessellates a road surface.
  std::vector<ObjectInstance> objects;

  /// Placed signals (traffic lights/signs), instanced from the bundled signal
  /// models. Regenerated on the same DirtySet::objects channel as objects — a
  /// signal edit never re-tessellates a road surface. (Named `signal_instances`,
  /// not `signals`, because Qt's moc `signals` macro would rewrite the member in
  /// any editor TU that includes this header.)
  std::vector<SignalInstance> signal_instances;
};

} // namespace roadmaker
