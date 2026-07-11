#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/xodr/raw_xml.hpp"

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

  /// Unknown attributes and unmodeled children (<skeleton>, <markings>,
  /// <material>, <parkingSpace>, <borders>, <userData>, ...) — preserved
  /// verbatim per the never-drop contract (docs/design/m3a/01 §5).
  RawXml preserved;
};

} // namespace roadmaker
