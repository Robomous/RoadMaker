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

#include "roadmaker/xodr/reader.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/terrain_sidecar.hpp"

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

/// Strict decimal integer parsing for the rm:floor sort index (p4-s5, issue
/// #320): no whitespace, no sign but a leading '-', no leading zeros, and the
/// same magnitude bound the command layer clamps authors to. Anything else is a
/// corrupt file, not a value to salvage.
std::optional<int> to_sort_index(std::string_view text) {
  bool negative = false;
  if (text.starts_with('-')) {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.empty() || text.size() > 4 || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = (value * 10) + (c - '0');
  }
  if (value > kMaxSurfaceSpanSortIndex) {
    return std::nullopt;
  }
  return negative ? -value : value;
}

/// Strict `nonNegativeInteger` parsing for @sequence on `<controller>` (§14.6
/// Table 128) and on `<junction><controller>` (§12.14 Table 84): decimal
/// digits only, no sign, no whitespace, no leading zeros, and bounded so a
/// corrupt file cannot overflow. Anything else is reported by the caller and
/// the attribute is preserved verbatim rather than invented.
std::optional<unsigned> to_sequence(std::string_view text) {
  if (text.empty() || text.size() > 9 || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = (value * 10U) + static_cast<unsigned>(c - '0');
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
    // Top-level <controller> (§14.6). Element overview Table 12 places it
    // between the roads and the junctions, and that is where it is read and
    // written. It references signals by string id, so it needs no resolution
    // pass; parsing it before the junctions only keeps document order.
    std::size_t controller_index = 0;
    for (const pugi::xml_node controller_node : root.children("controller")) {
      parse_controller(controller_node, controller_index++);
    }
    for (const pugi::xml_node junction_node : root.children("junction")) {
      parse_junction(junction_node);
    }
    resolve_references();
    resolve_stoplines();
    parse_surfaces(root);
    parse_terrain_reference(root);
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

    /// `@junction` — present only on a SPAN junction's face (p4-s4, issue
    /// #319). Empty selects the arm resolution path.
    std::string junction;
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
    data.junction = node.attribute("junction").value();

    // An attribute we do not model is reported but does not cost the record —
    // a newer RoadMaker's extra field must not silently delete the stop line.
    static constexpr std::array<std::string_view, 6> kKnown{
        "code", "contact", "distance", "flipped", "crosswalk", "junction"};
    for (const pugi::xml_attribute attr : node.attributes()) {
      if (std::ranges::find(kKnown, std::string_view(attr.name())) == kKnown.end()) {
        diag(Severity::Warning,
             location,
             fmt::format("unknown rm:stopline attribute '{}' ignored", attr.name()));
      }
    }
    return data;
  }

  /// Resolves the junction that owns a span (virtual) junction's stop-line face
  /// (p4-s4, issue #319) — the SECOND resolution path, and a separate one on
  /// purpose: a span junction has no arms, no connections and no road links, so
  /// nothing the arm path keys on (junction_at_end) can ever match it.
  ///
  /// The key is the span itself: `@junction` names the owning junction and the
  /// enclosing `<road>` names the span road, and the record is accepted only
  /// when that junction really carries a span on that road. Geometry is
  /// deliberately NOT the key — a face station is clamped to the road, so two
  /// spans that both begin inside the first half-thickness of a road would
  /// solve to the same station and the mapping would stop being injective.
  std::optional<JunctionId> span_junction_for(const PendingStopLine& pending) {
    const JunctionId junction_id = network().find_junction(pending.data.junction);
    if (!junction_id.is_valid()) {
      return std::nullopt;
    }
    const Junction& junction = *network().junction(junction_id);
    const bool spans_the_road = std::ranges::any_of(
        junction.spans, [&](const SpanArm& span) { return span.road == pending.road; });
    return spans_the_road ? std::optional<JunctionId>{junction_id} : std::nullopt;
  }

  /// Pass 2: fold every absorbed stop line into its junction's record. Two
  /// resolution paths, selected by whether `@junction` is present:
  ///
  ///  - absent: an ARM line. The arm is the road end named by `contact`;
  ///    junctions parse before this, so junction_at_end resolves.
  ///  - present: a SPAN junction's FACE, resolved by span_junction_for(). Here
  ///    `contact` names which face of the span it guards, not a road end.
  ///
  /// A record that carries nothing beyond its identity is a pure derived
  /// default and is absorbed silently — re-deriving it reproduces the same line.
  /// A record that resolves to no junction at all cannot be owned, so the object
  /// is restored live with its userData verbatim.
  void resolve_stoplines() {
    for (PendingStopLine& pending : pending_stoplines_) {
      const RoadEnd arm{.road = pending.road, .contact = pending.data.contact};
      const bool is_span_face = !pending.data.junction.empty();
      const std::optional<JunctionId> junction_id =
          is_span_face ? span_junction_for(pending) : edit::junction_at_end(network(), arm);
      if (!junction_id.has_value()) {
        diag(Severity::Warning,
             pending.location,
             is_span_face
                 ? fmt::format("rm:stopline names junction '{}', which is not a span junction of "
                               "this road — kept as a plain object",
                               pending.data.junction)
                 : std::string(
                       "rm:stopline names a road end with no junction — kept as a plain object"));
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

  // --- signal controllers (OpenDRIVE §14.6) ---------------------------------

  /// Top-level `<controller>` (Table 128) with its `<control>` children (Table
  /// 129). Before p4-s7 the element was warned about as unsupported and then
  /// DROPPED, losing a third-party file's whole signal grouping; it is now a
  /// first-class arena entity. Unknown attributes and unmodeled children are
  /// preserved verbatim, exactly as parse_signal does.
  void parse_controller(const pugi::xml_node& node, std::size_t index) {
    Controller controller;
    controller.odr_id = node.attribute("id").value();
    const std::string location = controller.odr_id.empty()
                                     ? fmt::format("controller[{}]", index)
                                     : fmt::format("controller id={}", controller.odr_id);
    if (controller.odr_id.empty()) {
      diag(Severity::Warning, location, "controller without 'id' attribute");
    }
    controller.name = node.attribute("name").value();
    if (const pugi::xml_attribute sequence = node.attribute("sequence")) {
      controller.sequence = to_sequence(sequence.value());
      if (!controller.sequence) {
        diag(Severity::Warning,
             location,
             fmt::format("controller 'sequence' is not a nonNegativeInteger ('{}'); it is "
                         "preserved verbatim",
                         sequence.value()));
      }
    }

    static constexpr std::string_view kModeledAttrs[] = {"id", "name", "sequence"};
    for (const pugi::xml_attribute attr : node.attributes()) {
      const std::string_view name = attr.name();
      if (name == "sequence" && controller.sequence.has_value()) {
        continue; // modeled and representable
      }
      if (name == "sequence" ||
          std::find(std::begin(kModeledAttrs), std::end(kModeledAttrs), name) ==
              std::end(kModeledAttrs)) {
        controller.preserved.attributes.emplace_back(std::string(name), attr.value());
      }
    }
    for (const pugi::xml_node child : node.children()) {
      if (std::string_view(child.name()) != "control") {
        controller.preserved.children.push_back(node_to_string(child));
        continue;
      }
      Control control;
      control.signal_odr_id = child.attribute("signalId").value();
      control.type = child.attribute("type").value();
      if (control.signal_odr_id.empty()) {
        diag(Severity::Warning, location, "control without 'signalId' attribute");
      }
      controller.controls.push_back(std::move(control));
    }
    if (controller.controls.empty()) {
      // "Controllers shall be valid for one or more signals." (§14.6.) The
      // element still loads — the parser never drops input — and the writer
      // re-emits it, so validate_network is where the finding lives.
      diag(Severity::Warning,
           location,
           "controller has no <control> children",
           rules::kControllerValidForSignals);
    }
    network().add_controller(std::move(controller));
  }

  /// `<junction><controller>` (§12.14 Table 84): a REFERENCE into a signal
  /// synchronization group. Before p4-s7 this child was dropped without even a
  /// warning. Unknown attributes and children are preserved verbatim.
  void parse_junction_controllers(const pugi::xml_node& junction_node,
                                  Junction& junction,
                                  const std::string& location) {
    for (const pugi::xml_node node : junction_node.children("controller")) {
      JunctionController entry;
      entry.controller_odr_id = node.attribute("id").value();
      if (entry.controller_odr_id.empty()) {
        diag(Severity::Warning, location, "junction controller without 'id' attribute");
      }
      entry.type = node.attribute("type").value();
      if (const pugi::xml_attribute sequence = node.attribute("sequence")) {
        entry.sequence = to_sequence(sequence.value());
        if (!entry.sequence) {
          diag(Severity::Warning,
               location,
               fmt::format("junction controller 'sequence' is not a nonNegativeInteger ('{}'); "
                           "it is preserved verbatim",
                           sequence.value()));
        }
      }
      static constexpr std::string_view kModeledAttrs[] = {"id", "sequence", "type"};
      for (const pugi::xml_attribute attr : node.attributes()) {
        const std::string_view name = attr.name();
        if (name == "sequence" && entry.sequence.has_value()) {
          continue;
        }
        if (name == "sequence" ||
            std::find(std::begin(kModeledAttrs), std::end(kModeledAttrs), name) ==
                std::end(kModeledAttrs)) {
          entry.preserved.attributes.emplace_back(std::string(name), attr.value());
        }
      }
      for (const pugi::xml_node child : node.children()) {
        entry.preserved.children.push_back(node_to_string(child));
      }
      junction.junction_controllers.push_back(std::move(entry));
    }
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
    parse_junction_controllers(junction_node, junction, location);
    parse_virtual_junction(junction_node, junction, location);
    parse_junction_user_data(junction_node, junction, location);

    // arms-xor-spans (p4-s4, issue #319): a span junction never cuts its main
    // road, so it owns no arms and no connecting roads, and there is no
    // automatic derivation for it to skip — it is locked by definition. A
    // hand-written file that mixes the two is degraded here rather than loaded
    // into a state the writer could not reproduce.
    if (!junction.spans.empty()) {
      if (!junction.arms.empty() || !junction.connections.empty()) {
        diag(Severity::Warning,
             location,
             "virtual junction also declares connections or rm:arms; they were dropped",
             rules::kJunctionVirtualAttributes);
        junction.arms.clear();
        junction.connections.clear();
      }
      junction.locked = true;
    }
  }

  /// Layer 0 of a span junction: `<junction type="virtual">` with the
  /// mandatory @mainRoad/@orientation/@sStart/@sEnd (ASAM OpenDRIVE 1.9.0
  /// §12.7 Table 69; 1.8.1 §12.7 Table 69 is identical). It carries exactly one
  /// span, which a well-formed `<userData code="rm:spans">` then replaces with
  /// the full list. Roads parse before junctions, so @mainRoad resolves here.
  /// An unresolvable or malformed virtual junction is warned about and loads as
  /// a plain junction — the parser never drops input silently.
  void parse_virtual_junction(const pugi::xml_node& junction_node,
                              Junction& junction,
                              const std::string& location) {
    if (std::string_view(junction_node.attribute("type").value()) != "virtual") {
      return;
    }
    const std::string main_road = junction_node.attribute("mainRoad").value();
    const RoadId road_id = network().find_road(main_road);
    if (!road_id.is_valid()) {
      diag(Severity::Warning,
           location,
           fmt::format("virtual junction names unknown mainRoad '{}'; the span was skipped",
                       main_road),
           rules::kOnlyRefDefinedIds);
      return;
    }
    const std::optional<double> s_start = to_double(junction_node.attribute("sStart").value());
    const std::optional<double> s_end = to_double(junction_node.attribute("sEnd").value());
    // t_grEqZero on both, and an interval that runs forwards.
    if (!s_start || !s_end || *s_start < 0.0 || *s_end < *s_start) {
      diag(Severity::Warning,
           location,
           "virtual junction needs sStart and sEnd with 0 <= sStart <= sEnd; the span was skipped",
           rules::kJunctionVirtualAttributes);
      return;
    }
    junction.spans.push_back(SpanArm{.road = road_id, .s_start = *s_start, .s_end = *s_end});
  }

  /// `<userData code="rm:spans">` ("roadOdrId:s_start:s_end;…", p4-s4 issue
  /// #319): the FULL span list of a virtual junction, spans[0] included, since
  /// Layer 0 can only carry the one main-road span. All-or-nothing like its
  /// rm:arms/rm:corners siblings — an unknown road id or a malformed interval
  /// rejects the whole value, and the caller then keeps the Layer-0 span.
  std::optional<std::vector<SpanArm>> parse_spans_value(std::string_view value) {
    std::vector<SpanArm> spans;
    for (std::size_t begin = 0; begin <= value.size();) {
      std::size_t end = value.find(';', begin);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      const std::string_view entry = value.substr(begin, end - begin);
      begin = end + 1;
      const std::size_t first = entry.find(':');
      if (first == std::string_view::npos) {
        return std::nullopt;
      }
      const std::size_t second = entry.find(':', first + 1);
      if (second == std::string_view::npos) {
        return std::nullopt;
      }
      const RoadId road_id = network().find_road(std::string(entry.substr(0, first)));
      const std::optional<double> s_start = to_double(entry.substr(first + 1, second - first - 1));
      const std::optional<double> s_end = to_double(entry.substr(second + 1));
      if (!road_id.is_valid() || !s_start || !s_end || *s_start < 0.0 || *s_end < *s_start) {
        return std::nullopt;
      }
      spans.push_back(SpanArm{.road = road_id, .s_start = *s_start, .s_end = *s_end});
    }
    if (spans.empty()) {
      return std::nullopt;
    }
    return spans;
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

  /// The separator-free alphabet every rm:* record value shares: it excludes
  /// the grammars' ':' ';' ',' '=' separators and whitespace, so no value ever
  /// needs escaping and a token can always be read back exactly as written.
  static bool is_record_token(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '_' || c == '.' || c == '-';
    });
  }

  /// The material-name alphabet the command layer enforces at author time
  /// (p4-s2, issue #226) — the shared record alphabet above.
  static bool is_material_token(std::string_view text) { return is_record_token(text); }

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

  /// Authored floor-contribution overrides (p4-s5, issue #320) round-trip
  /// through <userData code="rm:floor">, entries ";"-joined and fields
  /// ":"-joined: "roadOdrId[:inc=0][:sort=<int>]". Each optional field may
  /// appear at most once and at least one must be present (the writer never
  /// emits an entry that authors nothing).
  ///
  /// All-or-nothing on the entry grammar like rm:corners — an unresolvable or
  /// duplicated road, a malformed or repeated known field, or an empty override
  /// drops the whole value. An UNKNOWN field key is warned about and skipped
  /// instead, so a file written by a newer RoadMaker still loads the fields
  /// this build understands (the rm:junction forward-compat rule).
  std::optional<std::vector<SurfaceSpan>> parse_floor_value(std::string_view value,
                                                            const std::string& location) {
    std::vector<SurfaceSpan> spans;
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
      if (fields.size() < 2 || fields.size() > 3) {
        return std::nullopt;
      }
      const RoadId road_id = network().find_road(std::string(fields[0]));
      if (!road_id.is_valid()) {
        return std::nullopt;
      }
      if (std::ranges::any_of(spans,
                              [&](const SurfaceSpan& seen) { return seen.road == road_id; })) {
        return std::nullopt; // the same road twice
      }
      SurfaceSpan span{.road = road_id};
      bool included_seen = false;
      bool sort_seen = false;
      for (std::size_t i = 1; i < fields.size(); ++i) {
        const std::string_view field = fields[i];
        if (field.starts_with("inc=")) {
          // The writer emits `inc=0` and nothing else — any other value is a
          // corrupt file.
          if (included_seen || field.substr(4) != "0") {
            return std::nullopt;
          }
          included_seen = true;
          span.included = false;
          continue;
        }
        if (field.starts_with("sort=")) {
          const std::optional<int> parsed = to_sort_index(field.substr(5));
          if (sort_seen || !parsed || *parsed == 0) {
            return std::nullopt;
          }
          sort_seen = true;
          span.sort_index = *parsed;
          continue;
        }
        diag(Severity::Warning,
             location,
             fmt::format("rm:floor field '{}' is not understood and was ignored", field));
      }
      if (!included_seen && !sort_seen) {
        return std::nullopt; // the writer never emits an empty override
      }
      spans.push_back(span);
    }
    if (spans.empty()) {
      return std::nullopt;
    }
    return spans;
  }

  /// Authored maneuver overrides (p4-s6, issue #227) round-trip through
  /// <userData code="rm:maneuver">, entries ";"-joined and fields ":"-joined:
  /// "roadOdrId[:lock=1][:turn=left|straight|right|uturn][:so=<num>][:eo=<num>]
  ///  [:pts=x,y|x,y|…]". Points use ',' within a point and '|' between points,
  /// neither of which collides with the ';'/':' joins. Each optional field may
  /// appear at most once and at least one must be present (the writer never
  /// emits an entry that authors nothing).
  ///
  /// All-or-nothing on the entry grammar like rm:floor — an unresolvable or
  /// duplicated road, a malformed or repeated KNOWN field, a point list longer
  /// than kMaxManeuverControlPoints, or an empty override drops the whole
  /// value. An UNKNOWN field key is warned about and skipped instead, so a file
  /// written by a newer RoadMaker still loads the fields this build
  /// understands (the rm:junction forward-compat rule).
  ///
  /// No refit happens here: the planView already read from the file is Layer 0
  /// truth and wins. These records only say how the path was authored, so a
  /// load never regenerates geometry from them.
  std::optional<std::vector<Maneuver>> parse_maneuver_value(std::string_view value,
                                                            const std::string& location) {
    std::vector<Maneuver> maneuvers;
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
      if (fields.size() < 2 || fields.size() > 6) {
        return std::nullopt;
      }
      const RoadId road_id = network().find_road(std::string(fields[0]));
      if (!road_id.is_valid()) {
        return std::nullopt;
      }
      if (std::ranges::any_of(maneuvers,
                              [&](const Maneuver& seen) { return seen.road == road_id; })) {
        return std::nullopt; // the same road twice
      }
      Maneuver maneuver{.road = road_id};
      bool lock_seen = false;
      bool turn_seen = false;
      bool points_seen = false;
      for (std::size_t i = 1; i < fields.size(); ++i) {
        const std::string_view field = fields[i];
        if (field.starts_with("lock=")) {
          // The writer emits `lock=1` and nothing else — any other value is a
          // corrupt file.
          if (lock_seen || field.substr(5) != "1") {
            return std::nullopt;
          }
          lock_seen = true;
          maneuver.locked = true;
          continue;
        }
        if (field.starts_with("turn=")) {
          const std::string_view text = field.substr(5);
          if (turn_seen) {
            return std::nullopt;
          }
          if (text == "left") {
            maneuver.turn_type = TurnType::Left;
          } else if (text == "straight") {
            maneuver.turn_type = TurnType::Straight;
          } else if (text == "right") {
            maneuver.turn_type = TurnType::Right;
          } else if (text == "uturn") {
            maneuver.turn_type = TurnType::UTurn;
          } else {
            return std::nullopt;
          }
          turn_seen = true;
          continue;
        }
        if (field.starts_with("so=") || field.starts_with("eo=")) {
          std::optional<double>& slot =
              field.starts_with("so=") ? maneuver.start_offset : maneuver.end_offset;
          const std::optional<double> parsed = to_double(field.substr(3));
          if (slot.has_value() || !parsed) {
            return std::nullopt;
          }
          slot = *parsed;
          continue;
        }
        if (field.starts_with("pts=")) {
          if (points_seen) {
            return std::nullopt;
          }
          points_seen = true;
          const std::string_view list = field.substr(4);
          if (list.empty()) {
            return std::nullopt; // the writer never emits an empty list
          }
          for (std::size_t p = 0; p <= list.size();) {
            std::size_t stop = list.find('|', p);
            if (stop == std::string_view::npos) {
              stop = list.size();
            }
            const std::string_view point = list.substr(p, stop - p);
            p = stop + 1;
            const std::size_t comma = point.find(',');
            if (comma == std::string_view::npos) {
              return std::nullopt;
            }
            const std::optional<double> x = to_double(point.substr(0, comma));
            const std::optional<double> y = to_double(point.substr(comma + 1));
            if (!x || !y) {
              return std::nullopt;
            }
            if (maneuver.control_points.size() >= kMaxManeuverControlPoints) {
              return std::nullopt; // longer than any writer would emit
            }
            maneuver.control_points.push_back(Waypoint{.x = *x, .y = *y});
          }
          continue;
        }
        diag(Severity::Warning,
             location,
             fmt::format("rm:maneuver field '{}' is not understood and was ignored", field));
      }
      if (!maneuver.locked && !maneuver.turn_type && !maneuver.start_offset &&
          !maneuver.end_offset && maneuver.control_points.empty()) {
        return std::nullopt; // the writer never emits an empty override
      }
      maneuvers.push_back(std::move(maneuver));
    }
    if (maneuvers.empty()) {
      return std::nullopt;
    }
    return maneuvers;
  }

  /// The applied signalization template (p4-s7, issue #228) round-trips through
  /// <userData code="rm:signal">, fields ":"-joined:
  /// "template=protected_left|two_phase|all_way_stop|two_way_stop[:mount=<modelId>]".
  /// One entry per junction — it says HOW the junction was signalized, never
  /// WHAT was placed (the <signal>/<controller> elements are Layer 0 truth and
  /// are never re-derived from this).
  ///
  /// All-or-nothing on the KNOWN grammar like rm:maneuver: a missing, repeated
  /// or unrecognized template, a repeated or malformed mount, drops the whole
  /// value. An UNKNOWN field key is warned about and skipped (forward compat).
  std::optional<Signalization> parse_signalization_value(std::string_view value,
                                                         const std::string& location) {
    Signalization record;
    bool template_seen = false;
    bool mount_seen = false;
    for (std::size_t begin = 0; begin <= value.size();) {
      std::size_t end = value.find(':', begin);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      const std::string_view field = value.substr(begin, end - begin);
      begin = end + 1;
      if (field.starts_with("template=")) {
        const std::string_view text = field.substr(9);
        if (template_seen ||
            std::ranges::find(kSignalizationTemplates, text) == std::end(kSignalizationTemplates)) {
          return std::nullopt;
        }
        template_seen = true;
        record.tmpl = std::string(text);
        continue;
      }
      if (field.starts_with("mount=")) {
        const std::string_view text = field.substr(6);
        if (mount_seen || !is_record_token(text)) {
          return std::nullopt;
        }
        mount_seen = true;
        record.mount_model = std::string(text);
        continue;
      }
      diag(Severity::Warning,
           location,
           fmt::format("rm:signal field '{}' is not understood and was ignored", field));
    }
    if (!template_seen) {
      return std::nullopt; // the writer never emits a record without a template
    }
    return record;
  }

  /// The signal→prop mounts (p4-s7, issue #228) round-trip through
  /// <userData code="rm:signalmount">, entries ";"-joined and each entry
  /// "signalOdrId=objOdrId[,objOdrId…]". The object list is a LIST from day one
  /// so #323 assemblies need no schema change.
  ///
  /// All-or-nothing throughout — the value is a map, so there is no field key to
  /// be forward-compatible about: a missing '=', a non-token id, an empty or
  /// over-long object list, or the same signal twice drops the whole value.
  std::optional<std::vector<SignalMount>> parse_signal_mounts_value(std::string_view value) {
    std::vector<SignalMount> mounts;
    for (std::size_t begin = 0; begin <= value.size();) {
      std::size_t end = value.find(';', begin);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      const std::string_view entry = value.substr(begin, end - begin);
      begin = end + 1;
      const std::size_t equals = entry.find('=');
      if (equals == std::string_view::npos) {
        return std::nullopt;
      }
      SignalMount mount;
      mount.signal_odr_id = std::string(entry.substr(0, equals));
      if (!is_record_token(mount.signal_odr_id)) {
        return std::nullopt;
      }
      if (std::ranges::any_of(mounts, [&](const SignalMount& seen) {
            return seen.signal_odr_id == mount.signal_odr_id;
          })) {
        return std::nullopt; // the same signal twice
      }
      const std::string_view list = entry.substr(equals + 1);
      if (list.empty()) {
        return std::nullopt; // the writer never emits an entry without a part
      }
      for (std::size_t p = 0; p <= list.size();) {
        std::size_t stop = list.find(',', p);
        if (stop == std::string_view::npos) {
          stop = list.size();
        }
        const std::string_view part = list.substr(p, stop - p);
        p = stop + 1;
        if (!is_record_token(part) || mount.object_odr_ids.size() >= kMaxSignalMountParts) {
          return std::nullopt; // longer than any writer would emit
        }
        mount.object_odr_ids.emplace_back(part);
      }
      mounts.push_back(std::move(mount));
    }
    if (mounts.empty()) {
      return std::nullopt;
    }
    return mounts;
  }

  /// The junction signal cycle (p4-s8, issue #229) round-trips through
  /// <userData code="rm:phases">, entries ";"-joined in cycle (storage) order
  /// and each entry ":"-joined "name=<tok>", "dur=<num>", "st=<ctrl>,<state>…"
  /// with the state list '|'-joined. State chars are g=Green, y=Yellow, r=Red,
  /// o=Off. Layer 1 alone (ADR-0008): OpenDRIVE §14.6 excludes signal timing.
  ///
  /// Controller ids are NOT resolved here — a dormant reference (a controller
  /// outside the sync group or since deleted) LOADS, so a foreign file survives
  /// and the validator, never the reader, speaks about it.
  ///
  /// All-or-nothing on the KNOWN grammar (the rm:maneuver rule): a malformed or
  /// duplicated name/dur/st, a missing dur, a duration that is non-finite, <= 0
  /// or > kMaxSignalPhaseDuration, a bad pair (no comma), a bad state char, a
  /// non-token ctrl, a ctrl twice in one entry, or a list longer than
  /// kMaxSignalPhases / kMaxSignalPhaseStates drops the WHOLE value. An UNKNOWN
  /// "key=" field is warned about and skipped (forward compat, the rm:junction
  /// rule); a field WITHOUT '=' is malformed and drops the value.
  std::optional<std::vector<SignalPhase>> parse_signal_phases_value(std::string_view value,
                                                                    const std::string& location) {
    std::vector<SignalPhase> phases;
    for (std::size_t begin = 0; begin <= value.size();) {
      std::size_t end = value.find(';', begin);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      const std::string_view entry = value.substr(begin, end - begin);
      begin = end + 1;

      if (phases.size() >= kMaxSignalPhases) {
        return std::nullopt; // more entries than any writer would emit
      }

      SignalPhase phase;
      bool name_seen = false;
      bool dur_seen = false;
      bool st_seen = false;
      for (std::size_t fbegin = 0; fbegin <= entry.size();) {
        std::size_t fend = entry.find(':', fbegin);
        if (fend == std::string_view::npos) {
          fend = entry.size();
        }
        const std::string_view field = entry.substr(fbegin, fend - fbegin);
        fbegin = fend + 1;

        if (field.starts_with("name=")) {
          const std::string_view text = field.substr(5);
          if (name_seen || !is_record_token(text)) {
            return std::nullopt;
          }
          name_seen = true;
          phase.name = std::string(text);
        } else if (field.starts_with("dur=")) {
          const std::optional<double> parsed = to_double(field.substr(4));
          if (dur_seen || !parsed || *parsed <= 0.0 || *parsed > kMaxSignalPhaseDuration) {
            return std::nullopt;
          }
          dur_seen = true;
          phase.duration = *parsed;
        } else if (field.starts_with("st=")) {
          if (st_seen) {
            return std::nullopt;
          }
          st_seen = true;
          const std::string_view list = field.substr(3);
          if (list.empty()) {
            return std::nullopt; // the writer never emits an empty st field
          }
          for (std::size_t pbegin = 0; pbegin <= list.size();) {
            std::size_t pend = list.find('|', pbegin);
            if (pend == std::string_view::npos) {
              pend = list.size();
            }
            const std::string_view pair = list.substr(pbegin, pend - pbegin);
            pbegin = pend + 1;
            const std::size_t comma = pair.find(',');
            if (comma == std::string_view::npos) {
              return std::nullopt; // a pair is "ctrl,state"
            }
            const std::string_view ctrl = pair.substr(0, comma);
            const std::string_view state_text = pair.substr(comma + 1);
            if (!is_record_token(ctrl) || state_text.size() != 1) {
              return std::nullopt;
            }
            SignalState state{};
            switch (state_text[0]) {
            case 'g':
              state = SignalState::Green;
              break;
            case 'y':
              state = SignalState::Yellow;
              break;
            case 'r':
              state = SignalState::Red;
              break;
            case 'o':
              state = SignalState::Off;
              break;
            default:
              return std::nullopt;
            }
            if (phase.states.size() >= kMaxSignalPhaseStates) {
              return std::nullopt;
            }
            if (std::ranges::any_of(phase.states, [&](const PhaseState& seen) {
                  return seen.controller_odr_id == ctrl;
                })) {
              return std::nullopt; // the same controller twice in one entry
            }
            phase.states.push_back(
                PhaseState{.controller_odr_id = std::string(ctrl), .state = state});
          }
        } else if (field.find('=') != std::string_view::npos) {
          diag(Severity::Warning,
               location,
               fmt::format("rm:phases field '{}' is not understood and was ignored", field));
        } else {
          return std::nullopt; // a field without '=' is malformed
        }
      }
      if (!dur_seen) {
        return std::nullopt; // every phase carries a duration
      }
      phases.push_back(std::move(phase));
    }
    if (phases.empty()) {
      return std::nullopt; // the writer never emits an empty cycle
    }
    return phases;
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
      if (code == "rm:spans") {
        // A well-formed value REPLACES the single Layer-0 span with the full
        // list; a malformed one warns and leaves the Layer-0 span in place, so
        // the junction still loads as the virtual junction the file declared.
        std::optional<std::vector<SpanArm>> spans =
            parse_spans_value(node.attribute("value").value());
        if (!spans) {
          diag(Severity::Warning, location, "malformed rm:spans userData ignored");
          continue;
        }
        junction.spans = std::move(*spans);
        continue;
      }
      if (code == "rm:floor") {
        // Authored floor-contribution overrides (p4-s5, issue #320):
        // ";"-joined "roadOdrId[:inc=0][:sort=<int>]". All-or-nothing on the
        // ENTRY grammar (an unresolvable road, a duplicate road, a malformed
        // or repeated inc/sort, or an entry authoring nothing drops the whole
        // value — the rm:corners rule); an UNKNOWN field key inside an entry
        // is warned about and skipped, so a file written by a newer RoadMaker
        // still loads the fields this build understands (the rm:junction
        // forward-compat rule).
        std::optional<std::vector<SurfaceSpan>> spans =
            parse_floor_value(node.attribute("value").value(), location);
        if (!spans) {
          diag(Severity::Warning, location, "malformed rm:floor userData ignored");
          continue;
        }
        junction.surface_spans = std::move(*spans);
        continue;
      }
      if (code == "rm:maneuver") {
        // Authored maneuver overrides (p4-s6, issue #227): ";"-joined
        // "roadOdrId[:lock=1][:turn=…][:so=<num>][:eo=<num>][:pts=x,y|…]".
        // All-or-nothing on the ENTRY grammar (the rm:floor rule); an UNKNOWN
        // field key inside an entry is warned about and skipped. Nothing is
        // refitted here — the planView is Layer 0 truth and wins.
        std::optional<std::vector<Maneuver>> maneuvers =
            parse_maneuver_value(node.attribute("value").value(), location);
        if (!maneuvers) {
          diag(Severity::Warning, location, "malformed rm:maneuver userData ignored");
          continue;
        }
        junction.maneuvers = std::move(*maneuvers);
        continue;
      }
      if (code == "rm:signal") {
        // The applied signalization template (p4-s7, issue #228):
        // ":"-joined "template=<tok>[:mount=<tok>]". All-or-nothing on the
        // KNOWN grammar (the rm:maneuver rule); an UNKNOWN field key is warned
        // about and skipped. Nothing is re-derived from it — the <signal> and
        // <controller> elements already in the file are Layer 0 truth.
        std::optional<Signalization> record =
            parse_signalization_value(node.attribute("value").value(), location);
        if (!record) {
          diag(Severity::Warning, location, "malformed rm:signal userData ignored");
          continue;
        }
        junction.signalization = std::move(*record);
        continue;
      }
      if (code == "rm:signalmount") {
        // The signal→prop mounts (p4-s7, issue #228): ";"-joined
        // "signalOdrId=objOdrId[,objOdrId…]". All-or-nothing throughout — the
        // value is a map, so it has no field key to be forward-compatible
        // about.
        std::optional<std::vector<SignalMount>> mounts =
            parse_signal_mounts_value(node.attribute("value").value());
        if (!mounts) {
          diag(Severity::Warning, location, "malformed rm:signalmount userData ignored");
          continue;
        }
        junction.signal_mounts = std::move(*mounts);
        continue;
      }
      if (code == "rm:phases") {
        // The junction signal cycle (p4-s8, issue #229): ";"-joined phase
        // entries, each ":"-joined "name=<tok>:dur=<num>:st=<ctrl>,<state>…".
        // Layer 1 alone — OpenDRIVE §14.6 puts the cycle outside the standard
        // (ADR-0008). All-or-nothing on the KNOWN grammar (the rm:maneuver
        // rule); an UNKNOWN field key is warned about and skipped. Controller
        // ids are NOT resolved here — a dormant reference loads and the
        // validator, never the reader, speaks. A well-formed value REPLACES
        // junction.phases.
        std::optional<std::vector<SignalPhase>> phases =
            parse_signal_phases_value(node.attribute("value").value(), location);
        if (!phases) {
          diag(Severity::Warning, location, "malformed rm:phases userData ignored");
          continue;
        }
        junction.phases = std::move(*phases);
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
        bool locked = false;
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
          } else if (field.starts_with("locked=")) {
            // p4-s4 (issue #319): the writer emits `locked=1` and nothing else,
            // so any other value is a corrupt file — all-or-nothing, like the
            // KNOWN keys above.
            malformed = field.substr(7) != "1" || locked;
            locked = true;
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
        junction.locked = locked;
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

  /// One authored boundary node record: "x,y,tix,tiy,tox,toy". Returns nullopt
  /// on any field that is not a finite number, or on the wrong field count —
  /// the caller then warns and drops the whole surface rather than inventing a
  /// boundary the user never drew.
  static std::optional<SurfaceNode> parse_surface_node(std::string_view record) {
    std::array<double, 6> fields{};
    std::size_t count = 0;
    for (std::size_t begin = 0; begin <= record.size(); ++count) {
      if (count >= fields.size()) {
        return std::nullopt; // too many fields
      }
      std::size_t end = record.find(',', begin);
      if (end == std::string_view::npos) {
        end = record.size();
      }
      const std::optional<double> parsed = to_double(record.substr(begin, end - begin));
      if (!parsed.has_value()) { // to_double already rejects non-finite values
        return std::nullopt;
      }
      fields[count] = *parsed;
      begin = end + 1;
    }
    if (count != fields.size()) {
      return std::nullopt; // too few fields
    }
    return SurfaceNode{.x = fields[0],
                       .y = fields[1],
                       .tangent_in_x = fields[2],
                       .tangent_in_y = fields[3],
                       .tangent_out_x = fields[4],
                       .tangent_out_y = fields[5]};
  }

  /// Root-level <userData code="rm:surface"> ("roadOdrId;…"): a ground surface,
  /// reconstructed from its bounding-road ids. Roads parse before this pass so
  /// the ids resolve. A DERIVED surface stores no geometry — it is re-derived
  /// from those roads — and a malformed value or fewer than 3 valid roads drops
  /// it (it cannot enclose an area anyway), mirroring the writer's guard.
  ///
  /// An AUTHORED surface (p5-s1) additionally carries a `nodes` attribute
  /// ("x,y,tix,tiy,tox,toy;…"): that node graph IS its boundary, so `value`
  /// degrades to provenance and the road ids may legitimately be absent or
  /// stale. A `nodes` attribute that is present but malformed, or that carries
  /// fewer than 3 nodes, drops the surface with a warning — never silently, and
  /// never downgraded to a derived surface whose shape would be someone else's.
  void parse_surfaces(const pugi::xml_node& root) {
    for (const pugi::xml_node node : root.children("userData")) {
      const std::string code = node.attribute("code").value();
      if (code != "rm:surface") {
        continue;
      }
      const pugi::xml_attribute nodes_attr = node.attribute("nodes");
      const bool authored = !nodes_attr.empty();

      std::vector<SurfaceNode> boundary;
      if (authored) {
        const std::string encoded = nodes_attr.value();
        bool malformed = encoded.empty();
        for (std::size_t begin = 0; !malformed && begin <= encoded.size();) {
          std::size_t end = encoded.find(';', begin);
          if (end == std::string::npos) {
            end = encoded.size();
          }
          const std::optional<SurfaceNode> parsed =
              parse_surface_node(std::string_view(encoded).substr(begin, end - begin));
          if (!parsed.has_value()) {
            malformed = true;
            break;
          }
          boundary.push_back(*parsed);
          begin = end + 1;
        }
        if (malformed || boundary.size() < 3) {
          diag(Severity::Warning,
               "OpenDRIVE",
               "malformed rm:surface nodes userData ignored (authored surface dropped)");
          continue;
        }
      }

      std::vector<RoadId> roads;
      bool malformed = false;
      const std::string value = node.attribute("value").value();
      // An authored surface may have lost every provenance road; an empty value
      // is then legitimate, whereas for a derived surface it is the < 3 drop.
      if (!(authored && value.empty())) {
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
      }
      if (authored) {
        if (malformed) {
          // Provenance only: a road that no longer exists costs the elevation
          // samples, not the boundary. Warn and keep what resolved.
          diag(Severity::Warning,
               "OpenDRIVE",
               "rm:surface names an unknown road; authored boundary kept without it");
          roads.clear();
        }
      } else if (malformed || roads.size() < 3) {
        diag(Severity::Warning, "OpenDRIVE", "malformed rm:surface userData ignored");
        continue;
      }
      network().create_surface(
          Surface{.source = authored ? BoundarySource::Authored : BoundarySource::Derived,
                  .bounding_roads = std::move(roads),
                  .nodes = std::move(boundary),
                  .material = node.attribute("material").value()});
    }
  }

  /// `<userData code="rm:terrain" value="<sidecar>">` (p5-s2, #232): the scene
  /// height field's Layer-1 REFERENCE. Only the reference is stored here — the
  /// grid itself lives in the sidecar, which an in-memory parse has no
  /// directory to resolve against, so load_xodr does that second step. Keeping
  /// the reference either way is what makes the round-trip byte-identical for
  /// callers that never touch the filesystem.
  void parse_terrain_reference(const pugi::xml_node& root) {
    bool seen = false;
    for (const pugi::xml_node node : root.children("userData")) {
      if (std::string(node.attribute("code").value()) != "rm:terrain") {
        continue;
      }
      const std::string value = node.attribute("value").value();
      if (seen) {
        diag(Severity::Warning,
             "OpenDRIVE",
             "more than one rm:terrain userData; only the first is kept");
        continue;
      }
      seen = true;
      if (!is_safe_sidecar_reference(value)) {
        // Kept out of the network deliberately: an absolute or escaping path is
        // not something we will ever open, and storing it would make the next
        // save re-emit it.
        diag(Severity::Warning,
             "OpenDRIVE",
             "rm:terrain names an unusable sidecar path; the reference is dropped");
        continue;
      }
      HeightField field = network().terrain();
      field.sidecar = value;
      network().set_terrain(std::move(field));
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
      // Root <userData> carries RoadMaker extensions (rm:surface, rm:terrain);
      // they are parsed by parse_surfaces/parse_terrain_reference, not
      // unsupported.
      if (name != "header" && name != "road" && name != "junction" && name != "controller" &&
          name != "userData") {
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
  auto result = parse_xodr(text, path.string());
  if (!result) {
    return result;
  }

  // Second step of the height-field load (p5-s2, #232): parse_xodr kept the
  // sidecar REFERENCE, and only here — where a document directory exists — can
  // the grid behind it be resolved. A sidecar that is missing or malformed is a
  // warning and an empty grid, never a failed load: the network is perfectly
  // usable without its terrain, and failing would make one unreadable file cost
  // the user the whole scene. The reference survives either way, so re-saving
  // does not silently drop it.
  const std::string reference = result->network.terrain().sidecar;
  if (!reference.empty()) {
    auto sidecar = load_terrain_asc(path.parent_path() / reference);
    if (!sidecar) {
      result->diagnostics.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = path.string(),
          .message = fmt::format("rm:terrain sidecar '{}' could not be read ({}); the scene "
                                 "loads without its height field",
                                 reference,
                                 sidecar.error().message),
      });
    } else {
      HeightField field = std::move(sidecar->field);
      field.sidecar = reference;
      result->network.set_terrain(std::move(field));
      result->diagnostics.insert(
          result->diagnostics.end(), sidecar->diagnostics.begin(), sidecar->diagnostics.end());
    }
  }
  return result;
}

} // namespace roadmaker
