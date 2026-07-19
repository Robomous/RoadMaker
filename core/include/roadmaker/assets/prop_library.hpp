#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/object.hpp" // ObjectType

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// Bundled low-poly prop meshes (trees, shrubs). The geometry is procedurally
/// authored original work — see scripts/gen_prop_meshes.py, which emits the
/// data table in src/assets/prop_meshes.gen.cpp. One canonical source for the
/// kernel mesh builder, the glTF/USD exporters, and the editor renderer, so a
/// tree looks identical wherever it is drawn or exported.
namespace roadmaker::props {

/// One flat-shaded part of a prop (e.g. trunk, crown). Model space is Z-up,
/// meters, origin at the base centre of the prop (z=0 sits on the surface).
/// positions/normals are xyz triplets; indices are CCW viewed from outside.
/// color is flat, linear RGB in [0,1].
struct PropPart {
  std::vector<double> positions;
  std::vector<double> normals;
  std::vector<std::uint32_t> indices;
  std::array<float, 3> color;
  std::string name;
};

/// A complete prop model, assembled from one or more flat-shaded parts.
struct PropModel {
  std::string id;
  std::vector<PropPart> parts;
  double height = 0.0; ///< bounding height, meters (maps to OpenDRIVE @height)
  double radius = 0.0; ///< crown radius, meters (maps to OpenDRIVE @radius)
  /// The OpenDRIVE object class a placed instance of this model carries — the
  /// single source of truth for prop classification (the placement/drop code
  /// reads it instead of hardcoding per-id). Signal models (traffic lights and
  /// sign plates) are placed as <signal>s, not <object>s, so they carry None.
  ObjectType type = ObjectType::Tree;
};

/// Stable ids of every bundled prop model (e.g. "tree_pine"), in catalogue
/// order.
RM_API const std::vector<std::string>& ids();

/// The model for `id`, or nullptr if unknown. The returned pointer is valid for
/// the program lifetime (models are static data).
RM_API const PropModel* model(std::string_view id);

} // namespace roadmaker::props
