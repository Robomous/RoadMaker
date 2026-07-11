#include "roadmaker/xodr/reader.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <fast_float/fast_float.h>

#include <cmath>
#include <fstream>
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
    warn_unsupported_root_children(root);

    return std::move(result_);
  }

private:
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
    parse_road_user_data(road_node, road, location);

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
          name != "lanes" && name != "link" && name != "type" && name != "userData") {
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
      };
      if (mark.type == RoadMarkType::Other) {
        diag(Severity::Warning,
             location,
             fmt::format("road mark type '{}' rendered as generic marking",
                         mark_node.attribute("type").value()));
      }
      lane.road_marks.push_back(mark);
    }

    if (const pugi::xml_node link = lane_node.child("link")) {
      if (const pugi::xml_node pred = link.child("predecessor")) {
        lane.predecessor = pred.attribute("id").as_int();
      }
      if (const pugi::xml_node succ = link.child("successor")) {
        lane.successor = succ.attribute("id").as_int();
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
    return RoadMarkType::Other;
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

  /// The generator's arm list (roadmaker::edit) round-trips through
  /// <userData code="rm:arms"> ("roadOdrId:start|end;…"); roads parse before
  /// junctions, so arm road ids resolve here. Unknown codes are reported and
  /// ignored; a malformed value drops the arms (the junction still loads but
  /// cannot regenerate until recreated).
  void parse_junction_user_data(const pugi::xml_node& junction_node,
                                Junction& junction,
                                const std::string& location) {
    for (const pugi::xml_node node : junction_node.children("userData")) {
      const std::string code = node.attribute("code").value();
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
      if (name != "header" && name != "road" && name != "junction") {
        warn_unsupported(std::string(name), "OpenDRIVE");
      }
    }
  }

  RoadNetwork& network() { return result_.network; }

  std::string_view source_;
  XodrParseResult result_;
  std::vector<PendingRoadRefs> pending_refs_;
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
