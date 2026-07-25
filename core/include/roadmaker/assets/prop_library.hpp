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
#include "roadmaker/road/object.hpp" // ObjectType

#include <array>
#include <cstdint>
#include <optional>
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

/// A normalised sub-rectangle of a sign face: {left, top, width, height} in
/// [0,1], origin at the face's TOP-left (matching the rasterised bitmap's
/// row-0-top convention). The whole face is {0, 0, 1, 1}.
using FaceBox = std::array<double, 4>;

/// The flat rectangular front of a sign plate a placed <signal> shows its face
/// on. Model space is Z-up, meters; the plate face looks down +x, so the
/// rectangle spans y (width) and z (height). x is the model-space x of the
/// plate's front surface; z is the centre height.
///
/// The rasteriser (roadmaker::signs::render_face) paints three layers, in
/// order: the flat `background` fill; the sign's artwork, if `symbol` names a
/// bundled SVG (the US pack's designation faces — assets/signs/us/*.svg,
/// rasterised by nanosvg at whatever size the face needs); and up to two text
/// layers. `legend` is the sign's FIXED wording, baked into the model
/// ("SPEED LIMIT", "DO NOT ENTER"); the placed signal's own @text is the
/// EDITABLE one. Each draws inside its own box, so a speed-limit face can carry
/// its wordmark above and its posted number below.
///
/// A model carries this only when it is meant to show a face; plain props omit
/// it. NOTE: scripts/gen_prop_meshes.py emits this by positional aggregate
/// initialisation — reorder or insert a field here and that emitter must change
/// in the same commit or the generated table silently mis-assigns.
struct FacePlate {
  double x = 0.0;                         ///< model-space x of the plate's front face (m)
  double z = 0.0;                         ///< model-space centre height (m)
  double half_w = 0.0;                    ///< half width along y (m)
  double half_h = 0.0;                    ///< half height along z (m)
  std::array<float, 3> background{};      ///< plate fill, linear RGB [0,1]
  std::array<float, 3> ink{};             ///< editable-text ink, linear RGB [0,1]
  std::string symbol;                     ///< bundled artwork key (a designation), "" for none
  std::string legend;                     ///< the sign's fixed wording, "" for none
  std::array<float, 3> legend_ink{};      ///< fixed-legend ink, linear RGB [0,1]
  FaceBox legend_box{0.0, 0.0, 1.0, 1.0}; ///< where `legend` draws
  FaceBox text_box{0.0, 0.0, 1.0, 1.0};   ///< where the signal's @text draws
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
  /// Present on every sign model, absent on every plain prop. The mesh builder
  /// emits a textured quad in front of this plate for a placed static signal —
  /// the sign's artwork and fixed legend, plus whatever the signal itself
  /// carries (@text, or a speed limit's @value).
  std::optional<FacePlate> face_plate;
};

/// Stable ids of every bundled prop model (e.g. "tree_pine"), in catalogue
/// order.
RM_API const std::vector<std::string>& ids();

/// The model for `id`, or nullptr if unknown. The returned pointer is valid for
/// the program lifetime (models are static data).
RM_API const PropModel* model(std::string_view id);

/// Uniform render scale for a placed prop: the object's declared OpenDRIVE
/// @height divided by the model's authored height. This is what makes a
/// third-party .xodr that declares a 10 m tree draw as a 10 m tree instead of at
/// model unit size. Returns 1.0 when the model is unknown, when either height is
/// absent or non-positive, and — by IEEE x/x == 1.0 — when the object declares
/// exactly the model height, so scenes authored before per-instance sizing keep
/// rendering identically. Only @height drives the scale: props scale uniformly,
/// so @radius/@width/@length ride along rather than skewing the model.
[[nodiscard]] inline double instance_scale(const Object& object, const PropModel* model) {
  if (model == nullptr || !(model->height > 0.0)) {
    return 1.0;
  }
  if (!object.height.has_value() || !(*object.height > 0.0)) {
    return 1.0;
  }
  return *object.height / model->height;
}

} // namespace roadmaker::props
