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

#include "roadmaker/road/id.hpp"
#include "roadmaker/xodr/raw_xml.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace roadmaker {

/// e_objectType subset RoadMaker classifies (OpenDRIVE 1.9.0 §13.1, Table 85).
/// The authored (GS-1) set is Crosswalk/Tree/Vegetation/Pole; the rest are
/// common classes worth typing. Any other spec value parses as Other with the
/// original spelling kept in Object::type_str, so nothing is lost on write.
enum class ObjectType {
  None,
  Crosswalk,
  Tree,
  Vegetation,
  Pole,
  Barrier,
  Building,
  Obstacle,
  Other,
};

/// e_orientation (OpenDRIVE 1.9.0 §13.1): validity direction along the road.
/// Does not affect the heading.
enum class ObjectOrientation {
  Plus,  ///< valid in positive s-direction
  Minus, ///< valid in negative s-direction
  None,  ///< valid in both directions
};

/// One <marking> line of an object (§13.8, Table 99): a dashed or solid painted
/// line either attached to a side of the object's bounding volume (@side, no
/// <cornerReference>) or running between referenced outline points
/// (`corner_refs`). RoadMaker authors crosswalks as outline-referenced markings
/// (1.9.0, inside <outline>); the same struct models the 1.8.1 object-level form
/// (@side, directly under <object>) so both round-trip. A solid line is
/// `space_length == 0` with `line_length` the run length (>0, spec t_grZero).
struct ObjectMarking {
  std::string color;                 ///< e_roadMarkColor (required, §13.8)
  double line_length = 0.0;          ///< length of the visible part [m] (>0, required)
  double space_length = 0.0;         ///< gap between visible parts [m] (>=0); 0 = solid
  double start_offset = 0.0;         ///< lateral u-offset where the marking starts [m]
  double stop_offset = 0.0;          ///< lateral u-offset where the marking ends [m]
  std::optional<std::string> side;   ///< e_sideType (bounding-volume marking)
  std::optional<std::string> weight; ///< e_roadMarkWeight (optical weight)
  std::optional<double> width;       ///< marking width [m] (>0)
  std::optional<double> z_offset;    ///< thickness above the road [m] (>=0)
  /// <cornerReference @id> values, in outline order (§13.8.1.3). Empty for a
  /// bounding-volume (@side) marking.
  std::vector<int> corner_refs;

  /// Unknown @marking attributes preserved verbatim (never-drop contract).
  RawXml preserved;

  friend bool operator==(const ObjectMarking&, const ObjectMarking&) = default;
};

/// One <cornerRoad> or <cornerLocal> outline vertex (§13.2.1/§13.2.2).
/// ObjectOutline::road_coords selects which coordinate pair `a`/`b` holds —
/// the two corner kinds are mutually exclusive within an outline
/// (road.corner_road.corner_road_local_exclusivity).
struct OutlineCorner {
  double a = 0.0;       ///< s (cornerRoad) or u (cornerLocal) [m]
  double b = 0.0;       ///< t (cornerRoad) or v (cornerLocal) [m]
  double height = 0.0;  ///< object height at this corner, along z [m]
  double dz_or_z = 0.0; ///< dz relative to the reference line (road) or local z
  std::optional<int> id;

  friend bool operator==(const OutlineCorner&, const OutlineCorner&) = default;
};

/// <outline> (§13.2, Table 86): a series of corner points describing a
/// polygonal object — crosswalks and painted markings in the GS-1 set.
struct ObjectOutline {
  bool road_coords = true; ///< true = <cornerRoad> children, false = <cornerLocal>
  /// §13.2: absent @closed defaults per object type (Table 86), so absence
  /// must survive round-trip — nullopt means "not written in the file".
  std::optional<bool> closed;
  bool outer = true; ///< exactly one outer outline per object (§13.2 rules)
  std::optional<int> id;
  std::optional<std::string> fill_type; ///< e_outlineFillType, e.g. "paint"
  std::optional<std::string> lane_type; ///< e_laneType the object is treated as
  std::vector<OutlineCorner> corners;
  /// <markings> referencing this outline's points (§13.2.4/§13.8). Populated
  /// for 1.9.0 outline-nested markings (crosswalk stripes). Each marking's
  /// `corner_refs` index into `corners` by @id.
  std::vector<ObjectMarking> markings;

  /// Verbatim fallback: when the outline cannot be modeled faithfully
  /// (<curveLocal> children, mixed corner kinds), the whole <outline>
  /// element is preserved here and `corners` stays empty. The writer
  /// re-emits it unchanged inside <outlines>.
  std::string raw;

  friend bool operator==(const ObjectOutline&, const ObjectOutline&) = default;
};

/// <repeat> (§13.4, Table 95) — object series (tree lines, railings) or, with
/// distance == 0, one continuous extruded shape. Multiplicity is 0..*.
struct ObjectRepeat {
  double s = 0.0;        ///< start of the repetition section [m]
  double length = 0.0;   ///< section length along s [m]
  double distance = 0.0; ///< spacing between instances; 0 = continuous object
  double t_start = 0.0, t_end = 0.0;
  double z_offset_start = 0.0, z_offset_end = 0.0;
  std::optional<double> width_start, width_end;
  std::optional<double> height_start, height_end;
  std::optional<double> length_start, length_end;
  std::optional<double> radius_start, radius_end;
  /// Cubic t(ds) coefficients — 1.9.0; emitted only when targeting >=1.9.0.
  std::optional<double> b_t, c_t, d_t;
  /// 1.8.0: repeat in a straight line instead of following the reference line.
  bool detach_from_reference_line = false;

  friend bool operator==(const ObjectRepeat&, const ObjectRepeat&) = default;
};

/// RoadMaker's authoring truth for a parametric crosswalk asset instance,
/// carried in `<userData code="rm:crosswalk">` (the rm:surface precedent). The
/// OpenDRIVE outline + <markings> are the interop projection authored from
/// these params; on reload this is the source of truth the mesher renders,
/// while a foreign crosswalk without it falls back to the synthesized zebra.
/// Instances follow their asset's Default Material unless `material_override`
/// pins a per-instance choice (GW-5 steps 7/9).
struct CrosswalkData {
  std::string asset;              ///< Library asset key this instance follows
  double border_width = 0.0;      ///< edge-line width [m]; 0 = no border lines
  double dash_length = 0.5;       ///< stripe length along the crossing [m]; 0 = solid
  double dash_gap = 0.5;          ///< gap between stripes [m]
  std::string material;           ///< material code, e.g. "material.paint_white"
  bool material_override = false; ///< true = keep `material` on asset changes
  std::string category;           ///< segmentation category tag

  friend bool operator==(const CrosswalkData&, const CrosswalkData&) = default;
};

/// RoadMaker's authoring truth for a free-form marking-curve instance (p3-s4),
/// carried in `<userData code="rm:markingCurve">`. Mirrors CrosswalkData but the
/// geometry is an open polyline centreline rather than a rectangular band: the
/// drawn curve is stored as (s,t) sample pairs (~0.5 m spacing) in the owning
/// road's frame, which is the source of truth the mesher renders and the record
/// asset re-materialization re-runs authoring over. The OpenDRIVE outline +
/// <markings> are the interop projection authored from these params. A curve
/// consuming a crosswalk asset paints a striped band (`striped == true`), a
/// plain marking asset a solid/dashed line.
struct MarkingCurveData {
  std::string asset;              ///< Library asset key this instance follows
  double width = 0.12;            ///< band width across the curve [m] (>0)
  double dash_length = 0.0;       ///< visible run along the curve [m]; 0 = solid
  double dash_gap = 0.0;          ///< gap between runs [m]
  std::string material;           ///< material code, e.g. "material.paint_white"
  bool material_override = false; ///< true = keep `material` on asset changes
  std::string category;           ///< segmentation category tag
  bool striped = false;           ///< crosswalk-asset band vs. plain line marking
  /// Centreline as road-frame (s,t) sample pairs, in draw order (~0.5 m apart).
  /// At least two; the mesher walks this by arc length.
  std::vector<std::array<double, 2>> samples;

  friend bool operator==(const MarkingCurveData&, const MarkingCurveData&) = default;
};

/// RoadMaker's authoring truth for a point-stencil asset instance (p3-s4),
/// carried in `<userData code="rm:stencil">`. A stencil authors ONE closed
/// cornerLocal arrow outline; this record keys the instance to its Library asset
/// so p3-s5's per-instance material override can match instance↔asset exactly
/// (like `object.crosswalk->asset == key`). The glyph itself is the object's
/// subtype + outline; this carries only asset/material/category tags.
struct StencilData {
  std::string asset;              ///< Library asset key this instance follows
  std::string material;           ///< material code, e.g. "material.paint_white"
  bool material_override = false; ///< true = keep `material` on asset changes
  std::string category;           ///< segmentation category tag

  friend bool operator==(const StencilData&, const StencilData&) = default;
};

/// <object> (§13.1, Table 85). Placement is in road s/t/zOffset, resolved to
/// world through the owning road's reference line + elevation; objects never
/// move or re-orient (§13.1). Owned by RoadNetwork's object arena; `road` is
/// the back-reference mirroring Lane::section.
struct Object {
  RoadId road; ///< owning road

  std::string odr_id; ///< <object @id> — required, unique in file (string)
  std::string name;

  ObjectType type = ObjectType::None;
  /// @type exactly as spelled in the file; empty when the attribute was
  /// absent (the normative rule road.object.type_attr then warns). The
  /// writer emits this spelling when present, so ObjectType::Other never
  /// loses the original value; authored objects may leave it empty and let
  /// the writer derive it from `type`.
  std::string type_str;
  std::string subtype;

  double s = 0.0, t = 0.0; ///< origin in road coordinates (required, §13.1)
  double z_offset = 0.0;   ///< relative to reference-line elevation (required)
  double hdg = 0.0, pitch = 0.0, roll = 0.0;
  ObjectOrientation orientation = ObjectOrientation::None;
  /// 1.7.0: vertically perpendicular to the road surface; overrides
  /// pitch/roll when true.
  bool perp_to_road = false;

  /// Bounding volume: angular (length/width) XOR circular (radius), §13.1
  /// (road.object.circular_vs_angular). All optional in the schema; absent
  /// attributes stay nullopt so round-trip never invents dimensions.
  std::optional<double> length, width, radius;
  std::optional<double> height;
  std::optional<double> valid_length; ///< 0 for a point object

  std::optional<bool> dynamic;     ///< @dynamic yes/no; absent = static
  std::optional<bool> temporary;   ///< 1.9.0
  std::optional<bool> invalidated; ///< 1.9.0

  std::vector<ObjectOutline> outlines; ///< crosswalks / painted markings
  std::vector<ObjectRepeat> repeats;   ///< tree lines etc. (<repeat> is 0..*)

  /// Object-level <markings> (§13.8): markings attached to the bounding volume
  /// via @side, with no <outline> (the 1.8.1 crosswalk/parking-space form).
  /// The 1.9.0 outline-nested form lives on ObjectOutline::markings instead.
  std::vector<ObjectMarking> markings;

  /// RoadMaker parametric-crosswalk authoring data (§7.2 userData
  /// "rm:crosswalk"). Present on crosswalks authored by the editor; absent for
  /// foreign crosswalks, which mesh from the fallback zebra.
  std::optional<CrosswalkData> crosswalk;

  /// RoadMaker free-form marking-curve authoring data (§7.2 userData
  /// "rm:markingCurve"). Present on curves drawn with the Marking Curve tool;
  /// absent for every other object. Mutually exclusive with `crosswalk`.
  std::optional<MarkingCurveData> marking_curve;

  /// RoadMaker point-stencil authoring data (§7.2 userData "rm:stencil").
  /// Present on stencils placed by the Marking Point tool; absent otherwise.
  std::optional<StencilData> stencil;

  /// Unknown attributes and unmodeled children (<skeleton>, <material>,
  /// <parkingSpace>, <borders>, <userData>, ...) — preserved verbatim per the
  /// never-drop contract (docs/design/m3a/01 §5). <markings> is modeled above
  /// and no longer lands here.
  RawXml preserved;
};

} // namespace roadmaker
