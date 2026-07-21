#include "roadmaker/xodr/reader.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace roadmaker {

namespace {

/// Locale-independent double parsing; rejects trailing garbage (whitespace
/// is tolerated). std::stod is locale-dependent — never use it for xodr IO.
std::optional<double> to_double(std::string_view text) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  double value{};
  const auto result = fast_float::from_chars(first, last, value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  for (const char* p = result.ptr; p != last; ++p) {
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      return std::nullopt;
    }
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

/// Serializes a node as a self-contained XML fragment (no indentation), for
/// the verbatim-preservation tier (roadmaker/xodr/raw_xml.hpp).
std::string node_to_string(const pugi::xml_node& node) {
  std::ostringstream out;
  node.print(out, "", pugi::format_raw);
  return out.str();
}

class Parser {
public:
  explicit Parser(std::string_view source_name) : source_(source_name) {}

  Expected<XodrParseResult> run(const pugi::xml_document& doc) {
    const pugi::xml_node root = doc.child("OpenDRIVE");
    if (!root) {
      return make_error(
          ErrorCode::InvalidDocument, "missing <OpenDRIVE> root element", std::string(source_));
    }
    parse_header(root.child("header"));

    // Pass 1: roads (and everything inside them). Cross-references stay
    // string-typed until every road exists.
    std::size_t road_index = 0;
    for (const pugi::xml_node road_node : root.children("road")) {
      parse_road(road_node, road_index++);
    }
    for (const pugi::xml_node junction_node : root.children("junction")) {
      parse_junction(junction_node);
    }
    resolve_references();
    resolve_stoplines();
    parse_surfaces(root);
    warn_unsupported_root_children(root);

    return std::move(result_);
  }

private:
  /// The fields of one `<userData code="rm:stopline">`, already validated.
  struct ParsedStopLine {
    ContactPoint contact = ContactPoint::Start;
    std::optional<double> distance;
    bool flipped = false;
    std::string crosswalk;
  };

  /// A stop line absorbed from an object, waiting for junctions to parse so its
  /// owning arm can be resolved. `fallback` is the object it becomes again if no
  /// junction claims that road end.
  struct PendingStopLine {
    RoadId road;
    ParsedStopLine data;
    std::string location;
    std::string fragment; ///< the userData element verbatim, for the fallback
    Object fallback;
  };

  // --- diagnostics ---------------------------------------------------------

  /// `rule` is the ASAM checker-rule UID (roadmaker/xodr/rules.hpp) when a
  /// normative rule applies; the current road/lane context is stamped on
  /// every entry so consumers can navigate without parsing `location`.
  void
  diag(Severity severity, std::string location, std::string message, std::string_view rule = {}) {
    result_.diagnostics.push_back(Diagnostic{.severity = severity,
                                             .location = std::move(location),
                                             .message = std::move(message),
                                             .rule_id = std::string(rule),
                                             .road = current_road_,
                                             .lane = current_lane_});
  }

  /// Unsupported-element warnings are emitted once per element name.
  void warn_unsupported(const std::string& element, const std::string& location) {
    if (warned_elements_.insert(element).second) {
      diag(Severity::Warning,
           location,
           fmt::format("element <{}> is not supported yet and was ignored", element));
    }
  }

  /// Attribute as double; absent or malformed values fall back to
  /// `fallback` with a diagnostic (Warning when a default is sensible).
  double attr_double(const pugi::xml_node& node,
                     const char* name,
                     const std::string& location,
                     double fallback = 0.0,
                     bool required = true) {
    const pugi::xml_attribute attr = node.attribute(name);
    if (!attr) {
      if (required) {
        diag(Severity::Warning,
             location,
             fmt::format("missing attribute '{}', using {}", name, fallback));
      }
      return fallback;
    }
    if (const auto value = to_double(attr.value())) {
      return *value;
    }
    diag(
        Severity::Warning,
        location,
        fmt::format(
            "attribute '{}' is not a valid number ('{}'), using {}", name, attr.value(), fallback));
    return fallback;
  }

  /// Attribute as optional double: nullopt when absent (no diagnostic —
  /// optional per schema) or malformed (with a diagnostic).
  std::optional<double>
  attr_optional_double(const pugi::xml_node& node, const char* name, const std::string& location) {
    const pugi::xml_attribute attr = node.attribute(name);
    if (!attr) {
      return std::nullopt;
    }
    if (const auto value = to_double(attr.value())) {
      return value;
    }
    diag(Severity::Warning,
         location,
         fmt::format("attribute '{}' is not a valid number ('{}'), ignored", name, attr.value()));
    return std::nullopt;
  }

  // --- header --------------------------------------------------------------

  void parse_header(const pugi::xml_node& header) {
    if (!header) {
      diag(Severity::Warning, "header", "missing <header>");
      return;
    }
    const double major = attr_double(header, "revMajor", "header", 1.0, false);
    const double minor = attr_double(header, "revMinor", "header", 0.0, false);
    result_.revision = major + (minor / 10.0);
    if (major != 1.0 || minor < 4.0) {
      diag(Severity::Warning,
           "header",
           fmt::format(
               "OpenDRIVE revision {}.{} is outside the tested 1.4-1.9 range", major, minor));
    }
  }

  // --- roads ---------------------------------------------------------------

  void parse_road(const pugi::xml_node& road_node, std::size_t index) {
    // Auxiliary boundary roads are derived closure geometry the writer emits as
    // <road @junction> tagged rm:aux_boundary (junction_export.hpp). They are
    // not part of the model, so drop them on read — this keeps write→parse→write
    // a byte-identical fixed point. The marker is RoadMaker-namespaced and only
    // ever appears on a junction road, so no user road is silently eaten.
    for (const pugi::xml_node node : road_node.children("userData")) {
      if (std::string_view(node.attribute("code").value()) == "rm:aux_boundary") {
        return;
      }
    }

    const std::string location = fmt::format("road[{}]", index);
    std::string odr_id = road_node.attribute("id").value();
    if (odr_id.empty()) {
      diag(Severity::Error, location, "road without 'id' attribute skipped");
      return;
    }
    if (network().find_road(odr_id).is_valid()) {
      diag(Severity::Error,
           location,
           fmt::format("duplicate road id '{}' skipped", odr_id),
           rules::kIdUniqueInClass);
      return;
    }
    const RoadId road_id =
        network().create_road(road_node.attribute("name").value(), std::move(odr_id));
    Road& road = *network().road(road_id);
    current_road_ = road_id;

    const double declared_length = attr_double(road_node, "length", location);

    parse_plan_view(road_node.child("planView"), road, location);
    if (road.plan_view.empty()) {
      diag(
          Severity::Error, location, "road has no usable planView geometry", rules::kReflineExists);
    }
    road.length = road.plan_view.length();
    if (std::abs(declared_length - road.length) > tol::kRoundTripPosition) {
      diag(Severity::Warning,
           location,
           fmt::format("declared length {} differs from geometry length {}; using geometry",
                       declared_length,
                       road.length),
           rules::kRoadLengthSumGeometries);
    }

    parse_elevation(road_node.child("elevationProfile"), road, location);
    parse_lateral_profile(road_node.child("lateralProfile"), road, location);
    parse_lanes(road_node.child("lanes"), road_id, location);
    parse_objects(road_node.child("objects"), road_id, location);
    parse_signals(road_node.child("signals"), road_id, location);
    parse_road_user_data(road_node, *network().road(road_id), location);

    // Stash string-typed references for pass 2.
    PendingRoadRefs pending{.road = road_id};
    pending.junction = road_node.attribute("junction").value();
    if (const pugi::xml_node link = road_node.child("link")) {
      pending.predecessor = read_link(link.child("predecessor"));
      pending.successor = read_link(link.child("successor"));
    }
    pending_refs_.push_back(std::move(pending));

    for (const pugi::xml_node child : road_node.children()) {
      const std::string name = child.name();
      if (name != "planView" && name != "elevationProfile" && name != "lateralProfile" &&
          name != "lanes" && name != "link" && name != "type" && name != "userData" &&
          name != "objects" && name != "signals") {
        warn_unsupported(name, location);
      }
    }
    current_road_ = {};
  }

  /// RoadMaker's own <userData> extensions (OpenDRIVE 1.9.0 §7.2). Unknown
  /// codes are reported, never silently dropped; a malformed rm:waypoints
  /// value is diagnosed and ignored (the road still loads, Edit Nodes then
  /// derives waypoints from geometry as for any foreign road).
  void
  parse_road_user_data(const pugi::xml_node& road_node, Road& road, const std::string& location) {
    for (const pugi::xml_node node : road_node.children("userData")) {
      const std::string code = node.attribute("code").value();
      if (code != "rm:waypoints") {
        diag(Severity::Warning,
             location,
             fmt::format("userData code '{}' is not understood and was ignored", code));
        continue;
      }
      std::vector<Waypoint> waypoints;
      bool malformed = false;
      const std::string value = node.attribute("value").value();
      for (std::size_t begin = 0; begin <= value.size();) {
        std::size_t end = value.find(';', begin);
        if (end == std::string::npos) {
          end = value.size();
        }
        const std::string_view pair = std::string_view(value).substr(begin, end - begin);
        const std::size_t comma = pair.find(',');
        std::optional<double> x;
        std::optional<double> y;
        if (comma != std::string_view::npos) {
          x = to_double(pair.substr(0, comma));
          y = to_double(pair.substr(comma + 1));
        }
        if (!x.has_value() || !y.has_value()) {
          malformed = true;
          break;
        }
        waypoints.push_back(Waypoint{.x = *x, .y = *y});
        begin = end + 1;
      }
      if (malformed || waypoints.size() < 2) {
        diag(Severity::Warning, location, "malformed rm:waypoints userData ignored");
        continue;
      }
      road.authoring_waypoints = std::move(waypoints);
    }
  }

  void parse_plan_view(const pugi::xml_node& plan_view, Road& road, const std::string& location) {
    std::size_t index = 0;
    for (const pugi::xml_node geometry : plan_view.children("geometry")) {
      const std::string geo_location = fmt::format("{}/planView/geometry[{}]", location, index);
      GeometryRecord record{
          .x = attr_double(geometry, "x", geo_location),
          .y = attr_double(geometry, "y", geo_location),
          .hdg = attr_double(geometry, "hdg", geo_location),
          .length = attr_double(geometry, "length", geo_location),
      };
      const double declared_s = attr_double(geometry, "s", geo_location);

      bool supported = true;
      if (geometry.child("line")) {
        record.shape = LineGeom{};
      } else if (const pugi::xml_node arc = geometry.child("arc")) {
        record.shape = ArcGeom{.curvature = attr_double(arc, "curvature", geo_location)};
      } else if (const pugi::xml_node spiral = geometry.child("spiral")) {
        record.shape = SpiralGeom{
            .curv_start = attr_double(spiral, "curvStart", geo_location),
            .curv_end = attr_double(spiral, "curvEnd", geo_location),
        };
      } else if (const pugi::xml_node poly = geometry.child("paramPoly3")) {
        record.shape = ParamPoly3Geom{
            .au = attr_double(poly, "aU", geo_location, 0.0, false),
            .bu = attr_double(poly, "bU", geo_location, 0.0, false),
            .cu = attr_double(poly, "cU", geo_location, 0.0, false),
            .du = attr_double(poly, "dU", geo_location, 0.0, false),
            .av = attr_double(poly, "aV", geo_location, 0.0, false),
            .bv = attr_double(poly, "bV", geo_location, 0.0, false),
            .cv = attr_double(poly, "cV", geo_location, 0.0, false),
            .dv = attr_double(poly, "dV", geo_location, 0.0, false),
            .normalized = std::string_view(poly.attribute("pRange").value()) == "normalized",
        };
      } else {
        supported = false;
        const pugi::xml_node shape_node = geometry.first_child();
        warn_unsupported(shape_node ? shape_node.name() : "<empty geometry>", geo_location);
      }

      if (supported) {
        if (record.length <= 0.0) {
          diag(Severity::Warning, geo_location, "geometry record with length <= 0 skipped");
        } else {
          const double derived_s = road.plan_view.length();
          if (std::abs(declared_s - derived_s) > tol::kRoundTripPosition) {
            // Closest normative rule: an s that disagrees with the accumulated
            // arc length implies a gap or overlap in the reference line.
            diag(Severity::Warning,
                 geo_location,
                 fmt::format("declared s {} differs from accumulated s {}; using accumulated",
                             declared_s,
                             derived_s),
                 rules::kReflineNoGaps);
          }
          road.plan_view.append(record);
        }
      }
      ++index;
    }
  }

  void parse_elevation(const pugi::xml_node& profile, Road& road, const std::string& location) {
    std::size_t index = 0;
    for (const pugi::xml_node elevation : profile.children("elevation")) {
      road.elevation.push_back(
          read_poly3(elevation, fmt::format("{}/elevationProfile/elevation[{}]", location, index)));
      ++index;
    }
  }

  void
  parse_lateral_profile(const pugi::xml_node& profile, Road& road, const std::string& location) {
    std::size_t index = 0;
    for (const pugi::xml_node superelevation : profile.children("superelevation")) {
      road.superelevation.push_back(read_poly3(
          superelevation, fmt::format("{}/lateralProfile/superelevation[{}]", location, index)));
      ++index;
    }
    for (const pugi::xml_node child : profile.children()) {
      if (std::string_view(child.name()) != "superelevation") {
        warn_unsupported(child.name(), location + "/lateralProfile");
      }
    }
  }

  Poly3
  read_poly3(const pugi::xml_node& node, const std::string& location, const char* s_name = "s") {
    return Poly3{
        .s = attr_double(node, s_name, location),
        .a = attr_double(node, "a", location),
        .b = attr_double(node, "b", location, 0.0, false),
        .c = attr_double(node, "c", location, 0.0, false),
        .d = attr_double(node, "d", location, 0.0, false),
    };
  }

  // --- lanes ---------------------------------------------------------------

  void parse_lanes(const pugi::xml_node& lanes_node, RoadId road_id, const std::string& location) {
    if (!lanes_node) {
      diag(Severity::Warning, location, "road has no <lanes> element", rules::kLaneSectionRequired);
      return;
    }
    Road& road = *network().road(road_id);
    std::size_t offset_index = 0;
    for (const pugi::xml_node offset : lanes_node.children("laneOffset")) {
      road.lane_offset.push_back(
          read_poly3(offset, fmt::format("{}/lanes/laneOffset[{}]", location, offset_index)));
      ++offset_index;
    }

    std::size_t section_index = 0;
    for (const pugi::xml_node section_node : lanes_node.children("laneSection")) {
      const std::string section_location =
          fmt::format("{}/laneSection[{}]", location, section_index);
      const double s0 = attr_double(section_node, "s", section_location);
      const LaneSectionId section_id = network().add_lane_section(road_id, s0);
      if (!section_id.is_valid()) {
        diag(Severity::Error,
             section_location,
             fmt::format("lane section at duplicate s={} skipped", s0),
             rules::kLaneSectionValidLength);
        ++section_index;
        continue;
      }
      for (const char* side : {"left", "center", "right"}) {
        for (const pugi::xml_node lane_node : section_node.child(side).children("lane")) {
          parse_lane(lane_node, section_id, section_location);
        }
      }
      ++section_index;
    }
    if (network().road(road_id)->sections.empty()) {
      diag(Severity::Warning, location, "road has no lane sections", rules::kLaneSectionRequired);
    }
  }

  void parse_lane(const pugi::xml_node& lane_node,
                  LaneSectionId section_id,
                  const std::string& section_location) {
    const int odr_lane_id = lane_node.attribute("id").as_int();
    const std::string location = fmt::format("{}/lane[{}]", section_location, odr_lane_id);

    const std::string type_name = lane_node.attribute("type").value();
    const LaneType type = lane_type_from_string(type_name);
    if (type == LaneType::Other && !type_name.empty()) {
      diag(Severity::Warning,
           location,
           fmt::format("unknown lane type '{}' mapped to 'other'", type_name));
    }

    const LaneId lane_id = network().add_lane(section_id, odr_lane_id, type);
    if (!lane_id.is_valid()) {
      diag(Severity::Error,
           location,
           "duplicate lane id within section skipped",
           rules::kIdUniqueInLaneSection);
      return;
    }
    Lane& lane = *network().lane(lane_id);
    current_lane_ = lane_id;

    // @direction (e_lane_direction, §11). Absent -> Standard; unknown spelling
    // -> Standard + a Warning, mirroring the unknown-lane-type pattern above.
    const std::string direction_name = lane_node.attribute("direction").value();
    if (const auto direction = lane_direction_from_string(direction_name)) {
      lane.direction = *direction;
    } else {
      diag(Severity::Warning,
           location,
           fmt::format("unknown lane direction '{}' mapped to 'standard'", direction_name));
    }

    std::size_t width_index = 0;
    for (const pugi::xml_node width : lane_node.children("width")) {
      lane.widths.push_back(
          read_poly3(width, fmt::format("{}/width[{}]", location, width_index), "sOffset"));
      ++width_index;
    }
    if (lane_node.child("border")) {
      warn_unsupported("border", location);
    }
    if (odr_lane_id != 0 && lane.widths.empty()) {
      diag(Severity::Warning,
           location,
           "non-center lane without <width> records",
           rules::kWidthDefinedWholeSection);
    }

    for (const pugi::xml_node mark_node : lane_node.children("roadMark")) {
      RoadMark mark{
          .s_offset = attr_double(mark_node, "sOffset", location, 0.0, false),
          .type = road_mark_type_from_string(mark_node.attribute("type").value()),
          .width = attr_double(mark_node, "width", location, 0.12, false),
          .color = road_mark_color_from_string(mark_node.attribute("color").value()),
      };
      // @material (§11.9, Table 47) — kept verbatim whenever present (incl. an
      // explicit "standard") so a foreign file round-trips; RoadMaker writes
      // "rm:<id>" here for GW-2 step 15. Absent stays absent (byte-stable).
      if (const pugi::xml_attribute material_attr = mark_node.attribute("material")) {
        mark.material = material_attr.value();
      }
      if (mark.type == RoadMarkType::Other) {
        diag(Severity::Warning,
             location,
             fmt::format("road mark type '{}' rendered as generic marking",
                         mark_node.attribute("type").value()));
      }
      if (mark.color == RoadMarkColor::Other) {
        diag(Severity::Warning,
             location,
             fmt::format("road mark color '{}' rendered as standard",
                         mark_node.attribute("color").value()));
      }
      // Explicit multi-line geometry: <type> with <line> children (§11.9.1).
      // Each <line> becomes a RoadMarkLine; the simple single-stripe mark
      // leaves `lines` empty and keeps the scalar @width (M2 behaviour).
      if (const pugi::xml_node type_node = mark_node.child("type")) {
        for (const pugi::xml_node line_node : type_node.children("line")) {
          mark.lines.push_back(RoadMarkLine{
              .width = attr_double(line_node, "width", location, mark.width, false),
              .length = attr_double(line_node, "length", location, 0.0, false),
              .space = attr_double(line_node, "space", location, 0.0, false),
              .t_offset = attr_double(line_node, "tOffset", location, 0.0, false),
              .s_offset = attr_double(line_node, "sOffset", location, 0.0, false),
          });
        }
      }
      lane.road_marks.push_back(std::move(mark));
    }

    if (const pugi::xml_node link = lane_node.child("link")) {
      if (const pugi::xml_node pred = link.child("predecessor")) {
        lane.predecessor = pred.attribute("id").as_int();
      }
      if (const pugi::xml_node succ = link.child("successor")) {
        lane.successor = succ.attribute("id").as_int();
      }
    }

    // <material> records (§11.8.2, Table 44). Promoted out of the Preserved
    // tier: modeled attrs are typed, unknown attrs survive on the record's
    // preserved tier (risk-3). Diagnose-but-keep for both rules (mirrors the
    // width treatment), so a foreign file round-trips.
    static constexpr std::string_view kMaterialModeledAttrs[] = {
        "sOffset", "friction", "roughness", "surface"};
    double last_material_s = -std::numeric_limits<double>::infinity();
    std::size_t material_index = 0;
    for (const pugi::xml_node material_node : lane_node.children("material")) {
      const std::string mat_location = fmt::format("{}/material[{}]", location, material_index++);
      LaneMaterial material;
      material.s_offset = attr_double(material_node, "sOffset", mat_location, 0.0, false);
      material.friction = attr_optional_double(material_node, "friction", mat_location);
      material.roughness = attr_optional_double(material_node, "roughness", mat_location);
      if (const pugi::xml_attribute surface = material_node.attribute("surface")) {
        material.surface = surface.value();
      }
      for (const pugi::xml_attribute attr : material_node.attributes()) {
        const std::string_view name = attr.name();
        if (std::find(std::begin(kMaterialModeledAttrs), std::end(kMaterialModeledAttrs), name) ==
            std::end(kMaterialModeledAttrs)) {
          material.preserved.attributes.emplace_back(std::string(name), attr.value());
        }
      }
      if (!material.friction.has_value()) {
        diag(Severity::Warning,
             mat_location,
             "lane <material> without required @friction (Table 44)");
      }
      if (odr_lane_id == 0) {
        diag(Severity::Error,
             mat_location,
             "center lane (id 0) shall have no <material> elements",
             rules::kMaterialCenterLaneNone);
      }
      if (material.s_offset < last_material_s) {
        diag(Severity::Error,
             mat_location,
             "<material> elements are not in ascending sOffset order",
             rules::kMaterialElemAscOrder);
      }
      last_material_s = material.s_offset;
      lane.materials.push_back(std::move(material));
    }

    // Preserved tier: unmodeled lane children (<speed>/<access>/<height>/
    // <rule>/<userData>/…) survive verbatim in document order — the parser
    // never silently drops input (the Object precedent, docs/design/m3a/01 §5).
    // <border> keeps its own warn-and-skip above; modeled children are excluded.
    for (const pugi::xml_node child : lane_node.children()) {
      const std::string_view name = child.name();
      if (name != "link" && name != "width" && name != "border" && name != "roadMark" &&
          name != "material") {
        lane.preserved.children.push_back(node_to_string(child));
      }
    }
    current_lane_ = {};
  }

  static LaneType lane_type_from_string(std::string_view name) {
    if (name == "driving")
      return LaneType::Driving;
    if (name == "stop")
      return LaneType::Stop;
    if (name == "shoulder")
      return LaneType::Shoulder;
    if (name == "biking")
      return LaneType::Biking;
    if (name == "sidewalk" || name == "walking")
      return LaneType::Sidewalk;
    if (name == "border")
      return LaneType::Border;
    if (name == "restricted")
      return LaneType::Restricted;
    if (name == "parking")
      return LaneType::Parking;
    if (name == "median")
      return LaneType::Median;
    if (name == "curb")
      return LaneType::Curb;
    if (name == "none" || name.empty())
      return LaneType::None;
    return LaneType::Other;
  }

  static RoadMarkType road_mark_type_from_string(std::string_view name) {
    if (name == "none" || name.empty())
      return RoadMarkType::None;
    if (name == "solid")
      return RoadMarkType::Solid;
    if (name == "broken")
      return RoadMarkType::Broken;
    if (name == "solid solid")
      return RoadMarkType::SolidSolid;
    if (name == "solid broken")
      return RoadMarkType::SolidBroken;
    if (name == "broken solid")
      return RoadMarkType::BrokenSolid;
    if (name == "broken broken")
      return RoadMarkType::BrokenBroken;
    return RoadMarkType::Other;
  }

  /// e_roadMarkColor (§11.9, Table 48). Empty/absent -> Standard; unknown ->
  /// Other with a diagnostic at the call site (never dropped).
  static RoadMarkColor road_mark_color_from_string(std::string_view name) {
    if (name == "standard" || name.empty())
      return RoadMarkColor::Standard;
    if (name == "white")
      return RoadMarkColor::White;
    if (name == "yellow")
      return RoadMarkColor::Yellow;
    if (name == "red")
      return RoadMarkColor::Red;
    if (name == "blue")
      return RoadMarkColor::Blue;
    if (name == "green")
      return RoadMarkColor::Green;
    if (name == "orange")
      return RoadMarkColor::Orange;
    return RoadMarkColor::Other;
  }

  /// e_lane_direction (1.8.1 Annex A.3.10 Table 173 / 1.9.0 Annex A.3.11
  /// Table 180). Empty/absent -> Standard; an unknown spelling -> nullopt so
  /// the caller can default to Standard AND warn (never dropped).
  static std::optional<LaneDirection> lane_direction_from_string(std::string_view name) {
    if (name == "standard" || name.empty())
      return LaneDirection::Standard;
    if (name == "reversed")
      return LaneDirection::Reversed;
    if (name == "both")
      return LaneDirection::Both;
    return std::nullopt;
  }

  // --- objects (OpenDRIVE §13) ----------------------------------------------

  static ObjectType object_type_from_string(std::string_view name) {
    if (name == "crosswalk")
      return ObjectType::Crosswalk;
    if (name == "tree")
      return ObjectType::Tree;
    if (name == "vegetation")
      return ObjectType::Vegetation;
    if (name == "pole")
      return ObjectType::Pole;
    if (name == "barrier")
      return ObjectType::Barrier;
    if (name == "building")
      return ObjectType::Building;
    if (name == "obstacle")
      return ObjectType::Obstacle;
    if (name == "none" || name.empty())
      return ObjectType::None;
    return ObjectType::Other; // spelling survives in Object::type_str
  }

  void
  parse_objects(const pugi::xml_node& objects_node, RoadId road_id, const std::string& location) {
    if (!objects_node) {
      return;
    }
    std::size_t index = 0;
    for (const pugi::xml_node child : objects_node.children()) {
      if (std::string_view(child.name()) == "object") {
        parse_object(child, road_id, fmt::format("{}/objects/object[{}]", location, index++));
      } else {
        // <objectReference>/<tunnel>/<bridge> (§13.10–§13.12) are not modeled
        // in M3a — preserved verbatim so round-trip loses nothing.
        network().road(road_id)->object_extras.push_back(node_to_string(child));
      }
    }
  }

  void parse_object(const pugi::xml_node& node, RoadId road_id, const std::string& location) {
    Object object;
    object.odr_id = node.attribute("id").value();
    if (object.odr_id.empty()) {
      diag(Severity::Warning, location, "object without 'id' attribute");
    }
    object.name = node.attribute("name").value();

    object.type_str = node.attribute("type").value();
    object.type = object_type_from_string(object.type_str);
    if (!node.attribute("type")) {
      diag(Severity::Warning, location, "object without 'type' attribute", rules::kObjectTypeAttr);
    }
    object.subtype = node.attribute("subtype").value();

    if (!node.attribute("s") || !node.attribute("t")) {
      diag(Severity::Warning,
           location,
           "object origin requires 's' and 't' coordinates, using 0",
           rules::kObjectStTCoords);
    }
    object.s = attr_double(node, "s", location, 0.0, false);
    object.t = attr_double(node, "t", location, 0.0, false);
    object.z_offset = attr_double(node, "zOffset", location);
    object.hdg = attr_double(node, "hdg", location, 0.0, false);
    object.pitch = attr_double(node, "pitch", location, 0.0, false);
    object.roll = attr_double(node, "roll", location, 0.0, false);

    const pugi::xml_attribute orientation = node.attribute("orientation");
    if (!orientation) {
      diag(Severity::Warning,
           location,
           "object without 'orientation' attribute, assuming 'none'",
           rules::kObjectOrientation);
    }
    const std::string_view orientation_value = orientation.value();
    if (orientation_value == "+") {
      object.orientation = ObjectOrientation::Plus;
    } else if (orientation_value == "-") {
      object.orientation = ObjectOrientation::Minus;
    } else {
      if (!orientation_value.empty() && orientation_value != "none") {
        diag(Severity::Warning,
             location,
             fmt::format("unknown orientation '{}' mapped to 'none'", orientation_value));
      }
      object.orientation = ObjectOrientation::None;
    }

    // Set by the rm:stopline branch of the child loop below: when present this
    // object is absorbed into a junction record instead of the arena.
    std::optional<ParsedStopLine> stopline;
    std::string stopline_fragment;

    object.perp_to_road = node.attribute("perpToRoad").as_bool(false);
    object.length = attr_optional_double(node, "length", location);
    object.width = attr_optional_double(node, "width", location);
    object.radius = attr_optional_double(node, "radius", location);
    object.height = attr_optional_double(node, "height", location);
    object.valid_length = attr_optional_double(node, "validLength", location);
    if (const pugi::xml_attribute dynamic = node.attribute("dynamic")) {
      object.dynamic = std::string_view(dynamic.value()) == "yes";
    }
    if (const pugi::xml_attribute temporary = node.attribute("temporary")) {
      object.temporary = temporary.as_bool(false);
    }
    if (const pugi::xml_attribute invalidated = node.attribute("invalidated")) {
      object.invalidated = invalidated.as_bool(false);
    }

    // ASAM OpenDRIVE 1.4 outline definitions (direct <outline> child, no
    // <outlines> wrapper) shall still be supported (1.9.0 §13.2).
    std::size_t outline_index = 0;
    for (const pugi::xml_node outline : node.children("outline")) {
      parse_outline(outline, object, fmt::format("{}/outline[{}]", location, outline_index++));
    }
    for (const pugi::xml_node outline : node.child("outlines").children("outline")) {
      parse_outline(
          outline, object, fmt::format("{}/outlines/outline[{}]", location, outline_index++));
    }

    std::size_t repeat_index = 0;
    for (const pugi::xml_node repeat : node.children("repeat")) {
      parse_repeat(repeat, object, fmt::format("{}/repeat[{}]", location, repeat_index++));
    }

    // Object-level <markings> (§13.8): the 1.8.1 bounding-volume form, direct
    // under <object>. Outline-nested markings (1.9.0) are parsed into the
    // outline above; @side is mandatory here (no outline to reference).
    if (const pugi::xml_node markings_node = node.child("markings")) {
      object.markings = parse_markings(markings_node,
                                       /*has_outline=*/!object.outlines.empty(),
                                       fmt::format("{}/markings", location));
    }

    // Preserved tier: unknown attributes and unmodeled children survive
    // verbatim (never dropped) — docs/design/m3a/01 §5.
    static constexpr std::string_view kModeledAttrs[] = {
        "id",    "name",   "type",   "subtype",     "s",          "t",          "zOffset",
        "hdg",   "pitch",  "roll",   "orientation", "perpToRoad", "dynamic",    "length",
        "width", "radius", "height", "validLength", "temporary",  "invalidated"};
    for (const pugi::xml_attribute attr : node.attributes()) {
      const std::string_view name = attr.name();
      if (std::find(std::begin(kModeledAttrs), std::end(kModeledAttrs), name) ==
          std::end(kModeledAttrs)) {
        object.preserved.attributes.emplace_back(std::string(name), attr.value());
      }
    }
    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      // <userData code="rm:crosswalk"> is RoadMaker's own extension: parsed
      // into CrosswalkData, not preserved. Other userData codes and unmodeled
      // children round-trip verbatim.
      if (name == "userData" &&
          std::string_view(child.attribute("code").value()) == "rm:crosswalk") {
        if (auto data = parse_crosswalk_data(child, fmt::format("{}/userData", location))) {
          object.crosswalk = std::move(*data);
        }
        continue;
      }
      if (name == "userData" &&
          std::string_view(child.attribute("code").value()) == "rm:markingCurve") {
        if (auto data = parse_marking_curve_data(child, fmt::format("{}/userData", location))) {
          object.marking_curve = std::move(*data);
        }
        continue;
      }
      if (name == "userData" && std::string_view(child.attribute("code").value()) == "rm:stencil") {
        StencilData stencil;
        stencil.asset = child.attribute("asset").value();
        stencil.material = child.attribute("material").value();
        stencil.category = child.attribute("category").value();
        stencil.material_override = child.attribute("materialOverride").as_bool(false);
        object.stencil = std::move(stencil);
        continue;
      }
      // <userData code="rm:stopline">: this object is a stop line RoadMaker
      // materialized on write. It is absorbed back into the owning junction's
      // StopLine record rather than added to the arena — see resolve_stoplines.
      // The element is kept verbatim so an object that fails to resolve can be
      // restored with its record intact.
      if (name == "userData" &&
          std::string_view(child.attribute("code").value()) == "rm:stopline") {
        if (auto data = parse_stopline_data(child, fmt::format("{}/userData", location))) {
          stopline = std::move(*data);
          stopline_fragment = node_to_string(child);
          continue;
        }
        // Malformed: drop the record, keep the object live and its userData
        // verbatim (ADR-0008 — degrade to Layer 0 rather than lose the line).
        object.preserved.children.push_back(node_to_string(child));
        continue;
      }
      if (name != "outline" && name != "outlines" && name != "repeat" && name != "markings") {
        object.preserved.children.push_back(node_to_string(child));
      }
    }

    if (stopline.has_value()) {
      // Junctions have not been parsed yet, so the owning arm cannot be
      // resolved until pass 2 — hold the record and the object it would fall
      // back to.
      pending_stoplines_.push_back(PendingStopLine{.road = road_id,
                                                   .data = *std::move(stopline),
                                                   .location = location,
                                                   .fragment = std::move(stopline_fragment),
                                                   .fallback = std::move(object)});
      return;
    }

    network().add_object(road_id, std::move(object));
  }

  /// <userData code="rm:stopline"> on an <object> (§7.2): the parametric record
  /// behind a materialized stop line. `contact` is mandatory and names the
  /// junction-facing end of the enclosing road; everything else is optional and
  /// absent at its default. A malformed value drops the whole record (the caller
  /// keeps the object live) rather than guessing.
  std::optional<ParsedStopLine> parse_stopline_data(const pugi::xml_node& node,
                                                    const std::string& location) {
    ParsedStopLine data;
    const std::string_view contact = node.attribute("contact").value();
    if (contact == "end") {
      data.contact = ContactPoint::End;
    } else if (contact == "start") {
      data.contact = ContactPoint::Start;
    } else {
      diag(Severity::Warning,
           location,
           fmt::format("rm:stopline userData with missing or unknown contact '{}' ignored",
                       contact));
      return std::nullopt;
    }

    if (const pugi::xml_attribute attr = node.attribute("distance")) {
      const auto value = to_double(attr.value());
      if (!value.has_value() || !std::isfinite(*value) || *value < 0.0) {
        diag(
            Severity::Warning,
            location,
            fmt::format("rm:stopline userData with malformed distance '{}' ignored", attr.value()));
        return std::nullopt;
      }
      data.distance = *value;
    }

    if (const pugi::xml_attribute attr = node.attribute("flipped")) {
      const std::string_view value = attr.value();
      if (value != "true" && value != "false") {
        diag(Severity::Warning,
             location,
             fmt::format("rm:stopline userData with malformed flipped '{}' ignored", value));
        return std::nullopt;
      }
      data.flipped = value == "true";
    }

    data.crosswalk = node.attribute("crosswalk").value();

    // An attribute we do not model is reported but does not cost the record —
    // a newer RoadMaker's extra field must not silently delete the stop line.
    static constexpr std::array<std::string_view, 5> kKnown{
        "code", "contact", "distance", "flipped", "crosswalk"};
    for (const pugi::xml_attribute attr : node.attributes()) {
      if (std::ranges::find(kKnown, std::string_view(attr.name())) == kKnown.end()) {
        diag(Severity::Warning,
             location,
             fmt::format("unknown rm:stopline attribute '{}' ignored", attr.name()));
      }
    }
    return data;
  }

  /// Pass 2: fold every absorbed stop line into its junction's record. The arm
  /// is the road end named by `contact`; junctions parse before this, so
  /// junction_at_end resolves. A record that carries nothing beyond its identity
  /// is a pure derived default and is absorbed silently — re-deriving it
  /// reproduces the same line. A road end with no junction cannot own a record
  /// at all, so the object is restored live with its userData verbatim.
  void resolve_stoplines() {
    for (PendingStopLine& pending : pending_stoplines_) {
      const RoadEnd arm{.road = pending.road, .contact = pending.data.contact};
      const std::optional<JunctionId> junction_id = edit::junction_at_end(network(), arm);
      if (!junction_id.has_value()) {
        diag(Severity::Warning,
             pending.location,
             "rm:stopline names a road end with no junction — kept as a plain object");
        pending.fallback.preserved.children.push_back(std::move(pending.fragment));
        network().add_object(pending.road, std::move(pending.fallback));
        continue;
      }
      const bool derived = !pending.data.distance.has_value() && !pending.data.flipped &&
                           pending.data.crosswalk.empty();
      if (derived) {
        continue; // a pure default — re-derived on demand, nothing to store
      }
      network()
          .junction(*junction_id)
          ->stoplines.push_back(StopLine{.arm = arm,
                                         .distance = pending.data.distance,
                                         .flipped = pending.data.flipped,
                                         .crosswalk_odr_id = std::move(pending.data.crosswalk)});
    }
    pending_stoplines_.clear();
  }

  /// <markings> (§13.8): an object's painted lines, either attached to the
  /// bounding volume (@side, no outline) or referencing outline points
  /// (<cornerReference>). Returns the parsed markings; unknown @marking
  /// attributes and children survive verbatim. `has_outline` selects the @side
  /// diagnostic (mandatory only when no outline is used).
  std::vector<ObjectMarking> parse_markings(const pugi::xml_node& markings_node,
                                            bool has_outline,
                                            const std::string& location) {
    std::vector<ObjectMarking> markings;
    std::size_t index = 0;
    for (const pugi::xml_node marking_node : markings_node.children("marking")) {
      const std::string ml = fmt::format("{}/marking[{}]", location, index++);
      ObjectMarking marking;
      marking.color = marking_node.attribute("color").value();
      if (!marking_node.attribute("color")) {
        diag(Severity::Warning,
             ml,
             "marking without 'color' attribute",
             rules::kObjectMarkingColour);
      }
      marking.line_length = attr_double(marking_node, "lineLength", ml);
      marking.space_length = attr_double(marking_node, "spaceLength", ml);
      marking.start_offset = attr_double(marking_node, "startOffset", ml);
      marking.stop_offset = attr_double(marking_node, "stopOffset", ml);
      if (const pugi::xml_attribute side = marking_node.attribute("side")) {
        marking.side = side.value();
      }
      if (const pugi::xml_attribute weight = marking_node.attribute("weight")) {
        marking.weight = weight.value();
      }
      marking.width = attr_optional_double(marking_node, "width", ml);
      marking.z_offset = attr_optional_double(marking_node, "zOffset", ml);
      for (const pugi::xml_node ref : marking_node.children("cornerReference")) {
        marking.corner_refs.push_back(ref.attribute("id").as_int());
      }
      if (!has_outline && !marking.side.has_value()) {
        diag(Severity::Warning,
             ml,
             "object marking without an outline requires a 'side' attribute",
             rules::kObjectMarkingNoOutlineSide);
      }
      static constexpr std::string_view kModeledMarkingAttrs[] = {"color",
                                                                  "lineLength",
                                                                  "spaceLength",
                                                                  "startOffset",
                                                                  "stopOffset",
                                                                  "side",
                                                                  "weight",
                                                                  "width",
                                                                  "zOffset"};
      for (const pugi::xml_attribute attr : marking_node.attributes()) {
        const std::string_view name = attr.name();
        if (std::find(std::begin(kModeledMarkingAttrs), std::end(kModeledMarkingAttrs), name) ==
            std::end(kModeledMarkingAttrs)) {
          marking.preserved.attributes.emplace_back(std::string(name), attr.value());
        }
      }
      for (const pugi::xml_node child : marking_node.children()) {
        if (std::string_view(child.name()) != "cornerReference") {
          marking.preserved.children.push_back(node_to_string(child));
        }
      }
      markings.push_back(std::move(marking));
    }
    return markings;
  }

  /// <userData code="rm:crosswalk"> on an <object> (§7.2): RoadMaker's
  /// parametric-crosswalk authoring record. A non-numeric numeric attribute is
  /// diagnosed and the whole record ignored (rm:waypoints precedent), leaving
  /// the crosswalk to mesh from its outline/markings fallback.
  std::optional<CrosswalkData> parse_crosswalk_data(const pugi::xml_node& node,
                                                    const std::string& location) {
    CrosswalkData data;
    data.asset = node.attribute("asset").value();
    data.material = node.attribute("material").value();
    data.category = node.attribute("category").value();
    data.material_override = node.attribute("materialOverride").as_bool(false);
    bool ok = true;
    const auto num = [&](const char* name, double fallback) {
      const pugi::xml_attribute attr = node.attribute(name);
      if (!attr) {
        return fallback;
      }
      if (const auto v = to_double(attr.value())) {
        return *v;
      }
      ok = false;
      return fallback;
    };
    data.border_width = num("borderWidth", 0.0);
    data.dash_length = num("dashLength", 0.5);
    data.dash_gap = num("dashGap", 0.5);
    if (!ok) {
      diag(Severity::Warning, location, "malformed rm:crosswalk userData ignored");
      return std::nullopt;
    }
    return data;
  }

  /// <userData code="rm:markingCurve"> on an <object> (§7.2): RoadMaker's
  /// free-form marking-curve authoring record. `samples` is a "s,t;s,t;..."
  /// centreline; a malformed sample list or a non-numeric numeric attribute is
  /// diagnosed and the whole record ignored, leaving the object to mesh from its
  /// outline/markings fallback.
  std::optional<MarkingCurveData> parse_marking_curve_data(const pugi::xml_node& node,
                                                           const std::string& location) {
    MarkingCurveData data;
    data.asset = node.attribute("asset").value();
    data.material = node.attribute("material").value();
    data.category = node.attribute("category").value();
    data.material_override = node.attribute("materialOverride").as_bool(false);
    data.striped = node.attribute("striped").as_bool(false);
    bool ok = true;
    const auto num = [&](const char* name, double fallback) {
      const pugi::xml_attribute attr = node.attribute(name);
      if (!attr) {
        return fallback;
      }
      if (const auto v = to_double(attr.value())) {
        return *v;
      }
      ok = false;
      return fallback;
    };
    data.width = num("width", 0.12);
    data.dash_length = num("dashLength", 0.0);
    data.dash_gap = num("dashGap", 0.0);

    // samples="s,t;s,t;..." — at least two well-formed pairs.
    const std::string_view samples = node.attribute("samples").value();
    std::size_t pos = 0;
    while (pos < samples.size() && ok) {
      std::size_t semi = samples.find(';', pos);
      const std::string_view pair =
          samples.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
      const std::size_t comma = pair.find(',');
      if (comma == std::string_view::npos) {
        ok = false;
        break;
      }
      const auto s = to_double(pair.substr(0, comma));
      const auto t = to_double(pair.substr(comma + 1));
      if (!s || !t) {
        ok = false;
        break;
      }
      data.samples.push_back({*s, *t});
      if (semi == std::string_view::npos) {
        break;
      }
      pos = semi + 1;
    }
    if (!ok || data.samples.size() < 2) {
      diag(Severity::Warning, location, "malformed rm:markingCurve userData ignored");
      return std::nullopt;
    }
    return data;
  }

  void parse_outline(const pugi::xml_node& node, Object& object, const std::string& location) {
    ObjectOutline outline;
    outline.outer = node.attribute("outer").as_bool(true);
    if (const pugi::xml_attribute closed = node.attribute("closed")) {
      outline.closed = closed.as_bool(false);
    }
    if (const pugi::xml_attribute id = node.attribute("id")) {
      outline.id = id.as_int();
    }
    if (const pugi::xml_attribute fill = node.attribute("fillType")) {
      outline.fill_type = fill.value();
    }
    if (const pugi::xml_attribute lane_type = node.attribute("laneType")) {
      outline.lane_type = lane_type.value();
    }

    const bool has_road = static_cast<bool>(node.child("cornerRoad"));
    const bool has_local = static_cast<bool>(node.child("cornerLocal"));
    const bool has_curve = static_cast<bool>(node.child("curveLocal"));
    if (has_curve || (has_road && has_local)) {
      if (has_road && has_local) {
        diag(Severity::Warning,
             location,
             "outline mixes cornerRoad and cornerLocal elements; preserved verbatim",
             rules::kCornerRoadLocalExcl);
      }
      // <curveLocal> outlines are the Preserved tier in M3a: kept verbatim,
      // not interpreted (docs/design/m3a/01 §1).
      outline.raw = node_to_string(node);
      object.outlines.push_back(std::move(outline));
      return;
    }
    if (!has_road && !has_local) {
      diag(Severity::Warning,
           location,
           "outline without corner elements",
           rules::kOutlineFollowedByCorner);
    }
    outline.road_coords = has_road || !has_local;
    const char* corner_name = outline.road_coords ? "cornerRoad" : "cornerLocal";
    for (const pugi::xml_node corner_node : node.children(corner_name)) {
      OutlineCorner corner;
      if (outline.road_coords) {
        corner.a = attr_double(corner_node, "s", location);
        corner.b = attr_double(corner_node, "t", location);
        corner.dz_or_z = attr_double(corner_node, "dz", location);
      } else {
        corner.a = attr_double(corner_node, "u", location);
        corner.b = attr_double(corner_node, "v", location);
        corner.dz_or_z = attr_double(corner_node, "z", location);
      }
      corner.height = attr_double(corner_node, "height", location);
      if (const pugi::xml_attribute id = corner_node.attribute("id")) {
        corner.id = id.as_int();
      }
      outline.corners.push_back(corner);
    }
    if ((has_road || has_local) && outline.corners.size() < 2) {
      diag(Severity::Warning,
           location,
           fmt::format("outline has {} corner element(s), expected at least 2",
                       outline.corners.size()),
           outline.road_coords ? rules::kCornerRoadMinAmount : rules::kCornerLocalMinAmount);
    }
    // §13.2.4/§13.8: markings referencing this outline's corner points. When
    // present, every corner @id becomes mandatory (mandatory_id_with_markings).
    if (const pugi::xml_node markings_node = node.child("markings")) {
      outline.markings = parse_markings(markings_node, /*has_outline=*/true, location);
      for (const OutlineCorner& corner : outline.corners) {
        if (!corner.id.has_value()) {
          diag(Severity::Warning,
               location,
               "outline with <markings> requires @id on every corner",
               outline.road_coords ? rules::kCornerRoadIdWithMarkings
                                   : rules::kCornerLocalIdWithMarkings);
          break;
        }
      }
    }
    object.outlines.push_back(std::move(outline));
  }

  void parse_repeat(const pugi::xml_node& node, Object& object, const std::string& location) {
    ObjectRepeat repeat{
        .s = attr_double(node, "s", location),
        .length = attr_double(node, "length", location),
        .distance = attr_double(node, "distance", location),
        .t_start = attr_double(node, "tStart", location),
        .t_end = attr_double(node, "tEnd", location),
        .z_offset_start = attr_double(node, "zOffsetStart", location),
        .z_offset_end = attr_double(node, "zOffsetEnd", location),
    };
    repeat.width_start = attr_optional_double(node, "widthStart", location);
    repeat.width_end = attr_optional_double(node, "widthEnd", location);
    repeat.height_start = attr_optional_double(node, "heightStart", location);
    repeat.height_end = attr_optional_double(node, "heightEnd", location);
    repeat.length_start = attr_optional_double(node, "lengthStart", location);
    repeat.length_end = attr_optional_double(node, "lengthEnd", location);
    repeat.radius_start = attr_optional_double(node, "radiusStart", location);
    repeat.radius_end = attr_optional_double(node, "radiusEnd", location);
    repeat.b_t = attr_optional_double(node, "bT", location);
    repeat.c_t = attr_optional_double(node, "cT", location);
    repeat.d_t = attr_optional_double(node, "dT", location);
    repeat.detach_from_reference_line = node.attribute("detachFromReferenceLine").as_bool(false);
    object.repeats.push_back(repeat);
  }

  // --- signals (OpenDRIVE §14) ----------------------------------------------

  void
  parse_signals(const pugi::xml_node& signals_node, RoadId road_id, const std::string& location) {
    if (!signals_node) {
      return;
    }
    std::size_t index = 0;
    for (const pugi::xml_node child : signals_node.children()) {
      if (std::string_view(child.name()) == "signal") {
        parse_signal(child, road_id, fmt::format("{}/signals/signal[{}]", location, index++));
      } else {
        // <signalReference> (§14.5, 0..*) is not modeled in M3a — preserved
        // verbatim so round-trip loses nothing.
        network().road(road_id)->signal_extras.push_back(node_to_string(child));
      }
    }
  }

  void parse_signal(const pugi::xml_node& node, RoadId road_id, const std::string& location) {
    Signal signal;
    signal.odr_id = node.attribute("id").value();
    if (signal.odr_id.empty()) {
      diag(Severity::Warning, location, "signal without 'id' attribute");
    }
    signal.name = node.attribute("name").value();

    if (!node.attribute("s") || !node.attribute("t")) {
      diag(Severity::Warning,
           location,
           "signal origin requires 's' and 't' coordinates, using 0",
           rules::kObjectStTCoords);
    }
    signal.s = attr_double(node, "s", location, 0.0, false);
    signal.t = attr_double(node, "t", location, 0.0, false);
    signal.z_offset = attr_double(node, "zOffset", location);
    signal.h_offset = attr_double(node, "hOffset", location, 0.0, false);
    signal.pitch = attr_double(node, "pitch", location, 0.0, false);
    signal.roll = attr_double(node, "roll", location, 0.0, false);

    if (const pugi::xml_attribute dynamic = node.attribute("dynamic")) {
      signal.dynamic = std::string_view(dynamic.value()) == "yes";
    }

    const pugi::xml_attribute orientation = node.attribute("orientation");
    if (!orientation) {
      diag(Severity::Warning,
           location,
           "signal without 'orientation' attribute, assuming 'none'",
           rules::kObjectOrientation);
    }
    const std::string_view orientation_value = orientation.value();
    if (orientation_value == "+") {
      signal.orientation = ObjectOrientation::Plus;
    } else if (orientation_value == "-") {
      signal.orientation = ObjectOrientation::Minus;
    } else {
      if (!orientation_value.empty() && orientation_value != "none") {
        diag(Severity::Warning,
             location,
             fmt::format("unknown orientation '{}' mapped to 'none'", orientation_value));
      }
      signal.orientation = ObjectOrientation::None;
    }

    signal.type = node.attribute("type").value();
    signal.subtype = node.attribute("subtype").value();
    if (signal.type.empty() || signal.subtype.empty()) {
      diag(Severity::Warning,
           location,
           "signal requires 'type' and 'subtype' attributes",
           rules::kSignalType);
    }
    signal.country = node.attribute("country").value();
    signal.country_revision = node.attribute("countryRevision").value();
    if (signal.country.empty()) {
      diag(Severity::Warning,
           location,
           "signal without 'country' attribute",
           rules::kSignalUseCountryCode);
    }

    signal.value = attr_optional_double(node, "value", location);
    signal.unit = node.attribute("unit").value();
    signal.text = node.attribute("text").value();
    signal.height = attr_optional_double(node, "height", location);
    signal.width = attr_optional_double(node, "width", location);
    signal.length = attr_optional_double(node, "length", location);
    if (const pugi::xml_attribute temporary = node.attribute("temporary")) {
      signal.temporary = temporary.as_bool(false);
    }
    if (const pugi::xml_attribute invalidated = node.attribute("invalidated")) {
      signal.invalidated = invalidated.as_bool(false);
    }

    // Preserved tier: unknown attributes and unmodeled children (<validity>,
    // <dependency>, <reference>, <userData>, boards, ...) survive verbatim —
    // never dropped (docs/design/m3a/01 §5).
    static constexpr std::string_view kModeledAttrs[] = {
        "id",         "name",    "s",           "t",      "zOffset", "hOffset", "pitch",
        "roll",       "dynamic", "orientation", "type",   "subtype", "country", "countryRevision",
        "value",      "unit",    "text",        "height", "width",   "length",  "temporary",
        "invalidated"};
    for (const pugi::xml_attribute attr : node.attributes()) {
      const std::string_view name = attr.name();
      if (std::find(std::begin(kModeledAttrs), std::end(kModeledAttrs), name) ==
          std::end(kModeledAttrs)) {
        signal.preserved.attributes.emplace_back(std::string(name), attr.value());
      }
    }
    for (const pugi::xml_node child : node.children()) {
      signal.preserved.children.push_back(node_to_string(child));
    }

    network().add_signal(road_id, std::move(signal));
  }

  // --- junctions -----------------------------------------------------------

  void parse_junction(const pugi::xml_node& junction_node) {
    std::string odr_id = junction_node.attribute("id").value();
    const std::string location = fmt::format("junction id={}", odr_id);
    if (odr_id.empty()) {
      diag(Severity::Error, "junction", "junction without 'id' attribute skipped");
      return;
    }
    const JunctionId junction_id =
        network().create_junction(std::move(odr_id), junction_node.attribute("name").value());
    Junction& junction = *network().junction(junction_id);

    for (const pugi::xml_node connection : junction_node.children("connection")) {
      const std::string incoming = connection.attribute("incomingRoad").value();
      const std::string connecting = connection.attribute("connectingRoad").value();
      const RoadId incoming_id = network().find_road(incoming);
      const RoadId connecting_id = network().find_road(connecting);
      if (!incoming_id.is_valid() || !connecting_id.is_valid()) {
        diag(Severity::Warning,
             location,
             fmt::format("connection references unknown road '{}'; skipped",
                         incoming_id.is_valid() ? connecting : incoming),
             rules::kOnlyRefDefinedIds);
        continue;
      }
      JunctionConnection result{
          .incoming_road = incoming_id,
          .connecting_road = connecting_id,
          .contact_point = std::string_view(connection.attribute("contactPoint").value()) == "end"
                               ? ContactPoint::End
                               : ContactPoint::Start,
      };
      for (const pugi::xml_node lane_link : connection.children("laneLink")) {
        result.lane_links.emplace_back(lane_link.attribute("from").as_int(),
                                       lane_link.attribute("to").as_int());
      }
      junction.connections.push_back(std::move(result));
    }
    parse_junction_user_data(junction_node, junction, location);
  }

  /// One arm of an rm:corners entry: "roadOdrId:start|end" already split out.
  std::optional<RoadEnd> parse_road_end(std::string_view road, std::string_view contact) {
    const RoadId road_id = network().find_road(std::string(road));
    if (!road_id.is_valid() || (contact != "start" && contact != "end")) {
      return std::nullopt;
    }
    return RoadEnd{.road = road_id,
                   .contact = contact == "end" ? ContactPoint::End : ContactPoint::Start};
  }

  /// The material-name alphabet the command layer enforces at author time
  /// (p4-s2, issue #226) — it excludes the grammar's ':' and ';' separators
  /// and whitespace, so the userData values need no escaping.
  static bool is_material_token(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '_' || c == '.' || c == '-';
    });
  }

  /// Authored corner-fillet overrides (p4-s1, issue #225) round-trip through
  /// <userData code="rm:corners">, entries ";"-joined and fields ":"-joined:
  /// "roadA:start|end:roadB:start|end
  ///  [:r=<num>][:ea=<num>][:eb=<num>][:sw=<name>][:md=<name>]".
  /// The five optionals may appear in any order but each at most once, and at
  /// least one must be present (the writer never emits an empty override).
  /// All-or-nothing like rm:arms: any malformed entry drops the whole value.
  std::optional<std::vector<JunctionCorner>> parse_corners_value(std::string_view value) {
    std::vector<JunctionCorner> corners;
    for (std::size_t begin = 0; begin <= value.size();) {
      std::size_t end = value.find(';', begin);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      const std::string_view entry = value.substr(begin, end - begin);
      begin = end + 1;

      std::vector<std::string_view> fields;
      for (std::size_t f = 0; f <= entry.size();) {
        std::size_t stop = entry.find(':', f);
        if (stop == std::string_view::npos) {
          stop = entry.size();
        }
        fields.push_back(entry.substr(f, stop - f));
        f = stop + 1;
      }
      if (fields.size() < 5 || fields.size() > 9) {
        return std::nullopt;
      }
      const std::optional<RoadEnd> arm_a = parse_road_end(fields[0], fields[1]);
      const std::optional<RoadEnd> arm_b = parse_road_end(fields[2], fields[3]);
      if (!arm_a || !arm_b) {
        return std::nullopt;
      }
      JunctionCorner corner{.arm_a = *arm_a, .arm_b = *arm_b};
      for (std::size_t i = 4; i < fields.size(); ++i) {
        const std::string_view field = fields[i];
        std::optional<std::string>* text_slot = nullptr;
        std::optional<double>* slot = nullptr;
        if (field.starts_with("r=")) {
          slot = &corner.radius;
        } else if (field.starts_with("ea=")) {
          slot = &corner.extent_a;
        } else if (field.starts_with("eb=")) {
          slot = &corner.extent_b;
        } else if (field.starts_with("sw=")) {
          text_slot = &corner.sidewalk_material;
        } else if (field.starts_with("md=")) {
          text_slot = &corner.median_material;
        }
        const std::string_view text = field.substr(field.find('=') + 1);
        if (text_slot != nullptr) {
          // Material names (p4-s2): the writer only ever emits tokens the
          // command layer validated, so anything else is a corrupt file.
          if (text_slot->has_value() || !is_material_token(text)) {
            return std::nullopt;
          }
          *text_slot = std::string(text);
          continue;
        }
        if (slot == nullptr || slot->has_value()) {
          return std::nullopt; // unknown key, or the same key twice
        }
        const std::optional<double> parsed = to_double(text);
        if (!parsed) {
          return std::nullopt;
        }
        *slot = *parsed;
      }
      if (!corner.radius && !corner.extent_a && !corner.extent_b && !corner.sidewalk_material &&
          !corner.median_material) {
        return std::nullopt; // the writer never emits an empty override
      }
      corners.push_back(corner);
    }
    if (corners.empty()) {
      return std::nullopt;
    }
    return corners;
  }

  /// The generator's arm list (roadmaker::edit) round-trips through
  /// <userData code="rm:arms"> ("roadOdrId:start|end;…"); roads parse before
  /// junctions, so arm road ids resolve here. Corner overrides ride the
  /// sibling <userData code="rm:corners">. Unknown codes are reported and
  /// ignored; a malformed value drops the arms (the junction still loads but
  /// cannot regenerate until recreated).
  void parse_junction_user_data(const pugi::xml_node& junction_node,
                                Junction& junction,
                                const std::string& location) {
    for (const pugi::xml_node node : junction_node.children("userData")) {
      const std::string code = node.attribute("code").value();
      if (code == "rm:corners") {
        std::optional<std::vector<JunctionCorner>> corners =
            parse_corners_value(node.attribute("value").value());
        if (!corners) {
          diag(Severity::Warning, location, "malformed rm:corners userData ignored");
          continue;
        }
        junction.corners = std::move(*corners);
        continue;
      }
      if (code == "rm:junction") {
        // Junction-scope authored values (p4-s2, issue #226): ";"-joined
        // "key=value". A malformed or repeated KNOWN key drops the whole value
        // (all-or-nothing, like the siblings); an UNKNOWN key is warned about
        // and skipped, so a file written by a newer RoadMaker still loads its
        // radius and material here.
        const std::string value = node.attribute("value").value();
        std::optional<double> radius;
        std::string material;
        bool malformed = false;
        for (std::size_t begin = 0; begin <= value.size() && !malformed;) {
          std::size_t end = value.find(';', begin);
          if (end == std::string::npos) {
            end = value.size();
          }
          const std::string_view field = std::string_view(value).substr(begin, end - begin);
          begin = end + 1;
          if (field.starts_with("r=")) {
            const std::optional<double> parsed = to_double(field.substr(2));
            malformed = !parsed || radius.has_value();
            radius = parsed;
          } else if (field.starts_with("mat=")) {
            const std::string_view name = field.substr(4);
            malformed = !is_material_token(name) || !material.empty();
            material = std::string(name);
          } else {
            diag(Severity::Warning,
                 location,
                 fmt::format("rm:junction field '{}' is not understood and was ignored", field));
          }
        }
        if (malformed) {
          diag(Severity::Warning, location, "malformed rm:junction userData ignored");
          continue;
        }
        junction.default_corner_radius = radius;
        junction.material = std::move(material);
        continue;
      }
      if (code != "rm:arms") {
        diag(Severity::Warning,
             location,
             fmt::format("userData code '{}' is not understood and was ignored", code));
        continue;
      }
      std::vector<RoadEnd> arms;
      bool malformed = false;
      const std::string value = node.attribute("value").value();
      for (std::size_t begin = 0; begin <= value.size();) {
        std::size_t end = value.find(';', begin);
        if (end == std::string::npos) {
          end = value.size();
        }
        const std::string_view entry = std::string_view(value).substr(begin, end - begin);
        const std::size_t colon = entry.find(':');
        const RoadId road_id = colon != std::string_view::npos
                                   ? network().find_road(std::string(entry.substr(0, colon)))
                                   : RoadId{};
        const std::string_view contact =
            colon != std::string_view::npos ? entry.substr(colon + 1) : std::string_view{};
        if (!road_id.is_valid() || (contact != "start" && contact != "end")) {
          malformed = true;
          break;
        }
        arms.push_back(
            RoadEnd{.road = road_id,
                    .contact = contact == "end" ? ContactPoint::End : ContactPoint::Start});
        begin = end + 1;
      }
      if (malformed || arms.size() < 2) {
        diag(Severity::Warning, location, "malformed rm:arms userData ignored");
        continue;
      }
      junction.arms = std::move(arms);
    }
  }

  /// Root-level <userData code="rm:surface"> ("roadOdrId;…"): a derived ground
  /// surface, reconstructed from its bounding-road ids. Roads parse before this
  /// pass so the ids resolve. Geometry is re-derived from the roads, not stored.
  /// A malformed value or fewer than 3 valid roads drops the surface (it cannot
  /// enclose an area anyway), mirroring the writer's guard.
  void parse_surfaces(const pugi::xml_node& root) {
    for (const pugi::xml_node node : root.children("userData")) {
      const std::string code = node.attribute("code").value();
      if (code != "rm:surface") {
        continue;
      }
      std::vector<RoadId> roads;
      bool malformed = false;
      const std::string value = node.attribute("value").value();
      for (std::size_t begin = 0; begin <= value.size();) {
        std::size_t end = value.find(';', begin);
        if (end == std::string::npos) {
          end = value.size();
        }
        const std::string_view entry = std::string_view(value).substr(begin, end - begin);
        const RoadId road_id = network().find_road(std::string(entry));
        if (!road_id.is_valid()) {
          malformed = true;
          break;
        }
        roads.push_back(road_id);
        begin = end + 1;
      }
      if (malformed || roads.size() < 3) {
        diag(Severity::Warning, "OpenDRIVE", "malformed rm:surface userData ignored");
        continue;
      }
      network().create_surface(Surface{.source = BoundarySource::Derived,
                                       .bounding_roads = std::move(roads),
                                       .material = node.attribute("material").value()});
    }
  }

  // --- pass 2: reference resolution ---------------------------------------

  struct PendingLink {
    std::string element_type; // "road" | "junction"
    std::string element_id;
    std::string contact_point; // "start" | "end" (roads only)
    bool present = false;
  };

  struct PendingRoadRefs {
    RoadId road;
    std::string junction;
    PendingLink predecessor;
    PendingLink successor;
  };

  static PendingLink read_link(const pugi::xml_node& node) {
    if (!node) {
      return {};
    }
    return PendingLink{
        .element_type = node.attribute("elementType").value(),
        .element_id = node.attribute("elementId").value(),
        .contact_point = node.attribute("contactPoint").value(),
        .present = true,
    };
  }

  void resolve_references() {
    for (const PendingRoadRefs& pending : pending_refs_) {
      Road& road = *network().road(pending.road);
      const std::string location = fmt::format("road id={}", road.odr_id);
      current_road_ = pending.road;

      if (!pending.junction.empty() && pending.junction != "-1") {
        road.junction = network().find_junction(pending.junction);
        if (!road.junction.is_valid()) {
          diag(Severity::Warning,
               location,
               fmt::format("unknown junction '{}' referenced", pending.junction),
               rules::kOnlyRefDefinedIds);
        }
      }
      road.predecessor = resolve_link(pending.predecessor, location, "predecessor");
      road.successor = resolve_link(pending.successor, location, "successor");
    }
    current_road_ = {};
  }

  std::optional<RoadLink>
  resolve_link(const PendingLink& pending, const std::string& location, const char* kind) {
    if (!pending.present) {
      return std::nullopt;
    }
    RoadLink link;
    if (pending.element_type == "road") {
      const RoadId target = network().find_road(pending.element_id);
      if (!target.is_valid()) {
        diag(Severity::Warning,
             location,
             fmt::format("{} references unknown road '{}'", kind, pending.element_id),
             rules::kOnlyRefDefinedIds);
        return std::nullopt;
      }
      link.target = target;
      link.contact = pending.contact_point == "end" ? ContactPoint::End : ContactPoint::Start;
      if (pending.contact_point.empty()) {
        diag(Severity::Warning,
             location,
             fmt::format("{} road link without contactPoint; assuming 'start'", kind),
             rules::kRoadLinkAttributeUsage);
      }
    } else if (pending.element_type == "junction") {
      const JunctionId target = network().find_junction(pending.element_id);
      if (!target.is_valid()) {
        diag(Severity::Warning,
             location,
             fmt::format("{} references unknown junction '{}'", kind, pending.element_id),
             rules::kOnlyRefDefinedIds);
        return std::nullopt;
      }
      link.target = target;
    } else {
      diag(Severity::Warning,
           location,
           fmt::format("{} with unknown elementType '{}'", kind, pending.element_type),
           rules::kRoadLinkAttributeUsage);
      return std::nullopt;
    }
    return link;
  }

  void warn_unsupported_root_children(const pugi::xml_node& root) {
    for (const pugi::xml_node child : root.children()) {
      const std::string_view name = child.name();
      // Root <userData> carries RoadMaker extensions (rm:surface); it is parsed
      // by parse_surfaces, not unsupported.
      if (name != "header" && name != "road" && name != "junction" && name != "userData") {
        warn_unsupported(std::string(name), "OpenDRIVE");
      }
    }
  }

  RoadNetwork& network() { return result_.network; }

  std::string_view source_;
  XodrParseResult result_;
  std::vector<PendingRoadRefs> pending_refs_;
  std::vector<PendingStopLine> pending_stoplines_;
  std::set<std::string> warned_elements_;
  /// Entity context stamped onto diagnostics by diag(). Set once the entity
  /// exists in the arena, reset when its scope ends; single-pass parsing
  /// keeps plain assignment sufficient (no RAII guard needed).
  RoadId current_road_;
  LaneId current_lane_;
};

} // namespace

Expected<XodrParseResult> parse_xodr(std::string_view xml_text, std::string_view source_name) {
  pugi::xml_document doc;
  const pugi::xml_parse_result parsed = doc.load_buffer(xml_text.data(), xml_text.size());
  if (!parsed) {
    return make_error(ErrorCode::MalformedXml,
                      fmt::format("XML parse error: {}", parsed.description()),
                      std::string(source_name));
  }
  return Parser(source_name).run(doc);
}

Expected<XodrParseResult> load_xodr(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return make_error(ErrorCode::FileNotFound, "file not found", path.string());
  }
  // Binary mode: the parser owns newline handling (CRLF must survive).
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open file", path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = std::move(buffer).str();
  return parse_xodr(text, path.string());
}

} // namespace roadmaker
