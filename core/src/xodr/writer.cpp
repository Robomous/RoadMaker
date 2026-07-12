#include "roadmaker/xodr/writer.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "junction_export.hpp"

namespace roadmaker {

namespace {

/// Shortest-precision round-trippable formatting; locale-independent.
std::string num(double value) {
  std::string text = fmt::format("{}", value);
  return text == "-0" ? "0" : text;
}

void set_num(pugi::xml_node node, const char* name, double value) {
  node.append_attribute(name).set_value(num(value).c_str());
}

const char* lane_type_name(LaneType type) {
  switch (type) {
  case LaneType::Driving:
    return "driving";
  case LaneType::Stop:
    return "stop";
  case LaneType::Shoulder:
    return "shoulder";
  case LaneType::Biking:
    return "biking";
  case LaneType::Sidewalk:
    return "sidewalk";
  case LaneType::Border:
    return "border";
  case LaneType::Restricted:
    return "restricted";
  case LaneType::Parking:
    return "parking";
  case LaneType::Median:
    return "median";
  case LaneType::Curb:
    return "curb";
  case LaneType::None:
    return "none";
  case LaneType::Other:
    return "none"; // parsed-as-other exotic types have no faithful name
  }
  return "none";
}

const char* object_type_name(ObjectType type) {
  switch (type) {
  case ObjectType::Crosswalk:
    return "crosswalk";
  case ObjectType::Tree:
    return "tree";
  case ObjectType::Vegetation:
    return "vegetation";
  case ObjectType::Pole:
    return "pole";
  case ObjectType::Barrier:
    return "barrier";
  case ObjectType::Building:
    return "building";
  case ObjectType::Obstacle:
    return "obstacle";
  case ObjectType::None:
  case ObjectType::Other: // Other always carries its spelling in type_str
    return "none";
  }
  return "none";
}

const char* orientation_name(ObjectOrientation orientation) {
  switch (orientation) {
  case ObjectOrientation::Plus:
    return "+";
  case ObjectOrientation::Minus:
    return "-";
  case ObjectOrientation::None:
    return "none";
  }
  return "none";
}

const char* road_mark_name(RoadMarkType type) {
  switch (type) {
  case RoadMarkType::None:
    return "none";
  case RoadMarkType::Solid:
    return "solid";
  case RoadMarkType::Broken:
    return "broken";
  case RoadMarkType::SolidSolid:
    return "solid solid";
  case RoadMarkType::SolidBroken:
    return "solid broken";
  case RoadMarkType::BrokenSolid:
    return "broken solid";
  case RoadMarkType::Other:
    return "solid";
  }
  return "none";
}

const char* road_mark_color_name(RoadMarkColor color) {
  switch (color) {
  case RoadMarkColor::Standard:
    return "standard";
  case RoadMarkColor::White:
    return "white";
  case RoadMarkColor::Yellow:
    return "yellow";
  case RoadMarkColor::Red:
    return "red";
  case RoadMarkColor::Blue:
    return "blue";
  case RoadMarkColor::Green:
    return "green";
  case RoadMarkColor::Orange:
    return "orange";
  case RoadMarkColor::Other:
    return "standard"; // parsed-as-other exotic colors have no faithful name
  }
  return "standard";
}

/// Structural defects the writer refuses to serialize. Findings are
/// appended in check order; each cites its normative rule UID.
void check_road_structure(const RoadNetwork& network,
                          RoadId road_id,
                          const Road& road,
                          std::vector<Diagnostic>& findings) {
  const std::string location = fmt::format("road id={}", road.odr_id);
  if (road.plan_view.empty()) {
    findings.push_back(Diagnostic{.severity = Severity::Error,
                                  .location = location,
                                  .message = "road has no plan-view geometry",
                                  .rule_id = std::string(rules::kReflineExists),
                                  .road = road_id});
    return;
  }
  if (road.sections.empty()) {
    findings.push_back(Diagnostic{.severity = Severity::Error,
                                  .location = location,
                                  .message = "road has no lane sections",
                                  .rule_id = std::string(rules::kLaneSectionRequired),
                                  .road = road_id});
    return;
  }
  // Geometry continuity: end pose of record i vs start of record i+1.
  const auto& records = road.plan_view.records();
  for (std::size_t i = 0; i + 1 < records.size(); ++i) {
    const PathPoint end = road.plan_view.evaluate(records[i + 1].s - 1e-12);
    const double gap = std::hypot(end.x - records[i + 1].x, end.y - records[i + 1].y);
    const double heading_gap =
        std::abs(std::remainder(end.hdg - records[i + 1].hdg, 2.0 * std::numbers::pi));
    if (gap > tol::kRoundTripPosition || heading_gap > 1e-6) {
      findings.push_back(Diagnostic{
          .severity = Severity::Error,
          .location = location,
          .message = fmt::format(
              "geometry discontinuity at record {} (gap {} m, {} rad)", i + 1, gap, heading_gap),
          .rule_id = std::string(rules::kReflineNoGaps),
          .road = road_id});
      return;
    }
  }
  // Lane-link consistency between consecutive sections.
  for (std::size_t si = 0; si + 1 < road.sections.size(); ++si) {
    const LaneSection& here = *network.lane_section(road.sections[si]);
    const LaneSection& next = *network.lane_section(road.sections[si + 1]);
    auto lane_exists = [&](const LaneSection& section, int odr_id) {
      for (const LaneId id : section.lanes) {
        if (network.lane(id)->odr_id == odr_id) {
          return true;
        }
      }
      return false;
    };
    for (const LaneId lane_id : here.lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.successor && !lane_exists(next, *lane.successor)) {
        findings.push_back(Diagnostic{
            .severity = Severity::Error,
            .location = location,
            .message = fmt::format(
                "lane {} successor {} missing in next section", lane.odr_id, *lane.successor),
            .rule_id = std::string(rules::kOnlyRefDefinedIds),
            .road = road_id,
            .lane = lane_id});
        return;
      }
    }
  }
}

/// Pre-write validation per the writer contract: first structural defect
/// refuses the write, keeping the historical Error message/context shape.
Expected<void> validate(const RoadNetwork& network) {
  std::vector<Diagnostic> findings;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    if (findings.empty()) {
      check_road_structure(network, road_id, road, findings);
    }
  });
  if (!findings.empty()) {
    return tl::unexpected<Error>(Error{.code = ErrorCode::InvalidArgument,
                                       .message = findings.front().message,
                                       .context = findings.front().location});
  }
  return {};
}

bool links_to_junction(const std::optional<RoadLink>& link, JunctionId junction_id) {
  if (!link.has_value()) {
    return false;
  }
  const JunctionId* target = std::get_if<JunctionId>(&link->target);
  return target != nullptr && *target == junction_id;
}

void write_poly3_list(pugi::xml_node parent,
                      const char* element,
                      const char* s_name,
                      const std::vector<Poly3>& profile) {
  for (const Poly3& poly : profile) {
    pugi::xml_node node = parent.append_child(element);
    set_num(node, s_name, poly.s);
    set_num(node, "a", poly.a);
    set_num(node, "b", poly.b);
    set_num(node, "c", poly.c);
    set_num(node, "d", poly.d);
  }
}

void write_link_element(pugi::xml_node link,
                        const char* kind,
                        const RoadLink& road_link,
                        const RoadNetwork& network) {
  pugi::xml_node node = link.append_child(kind);
  if (const RoadId* road_target = std::get_if<RoadId>(&road_link.target)) {
    node.append_attribute("elementType").set_value("road");
    node.append_attribute("elementId").set_value(network.road(*road_target)->odr_id.c_str());
    node.append_attribute("contactPoint")
        .set_value(road_link.contact == ContactPoint::End ? "end" : "start");
  } else {
    const JunctionId junction = std::get<JunctionId>(road_link.target);
    node.append_attribute("elementType").set_value("junction");
    node.append_attribute("elementId").set_value(network.junction(junction)->odr_id.c_str());
  }
}

void write_lane(pugi::xml_node side, const Lane& lane) {
  pugi::xml_node lane_node = side.append_child("lane");
  lane_node.append_attribute("id").set_value(lane.odr_id);
  lane_node.append_attribute("type").set_value(lane_type_name(lane.type));
  lane_node.append_attribute("level").set_value("false");
  if (lane.predecessor || lane.successor) {
    pugi::xml_node link = lane_node.append_child("link");
    if (lane.predecessor) {
      link.append_child("predecessor").append_attribute("id").set_value(*lane.predecessor);
    }
    if (lane.successor) {
      link.append_child("successor").append_attribute("id").set_value(*lane.successor);
    }
  }
  for (const Poly3& width : lane.widths) {
    pugi::xml_node width_node = lane_node.append_child("width");
    set_num(width_node, "sOffset", width.s);
    set_num(width_node, "a", width.a);
    set_num(width_node, "b", width.b);
    set_num(width_node, "c", width.c);
    set_num(width_node, "d", width.d);
  }
  for (const RoadMark& mark : lane.road_marks) {
    pugi::xml_node mark_node = lane_node.append_child("roadMark");
    set_num(mark_node, "sOffset", mark.s_offset);
    mark_node.append_attribute("type").set_value(road_mark_name(mark.type));
    // @color is written explicitly only when not Standard (§11.9): the M2
    // single-line form (Standard, no <line>) stays byte-identical.
    if (mark.color != RoadMarkColor::Standard) {
      mark_node.append_attribute("color").set_value(road_mark_color_name(mark.color));
    }
    set_num(mark_node, "width", mark.width);
    // Explicit multi-line geometry (§11.9.1): a <type>/<line> block when the
    // mark carries stripes; @name is the mark type spelling, @width the mark
    // width (Table 49 requires both, and @width supersedes @roadMark/width).
    if (!mark.lines.empty()) {
      pugi::xml_node type_node = mark_node.append_child("type");
      type_node.append_attribute("name").set_value(road_mark_name(mark.type));
      set_num(type_node, "width", mark.width);
      for (const RoadMarkLine& line : mark.lines) {
        pugi::xml_node line_node = type_node.append_child("line");
        set_num(line_node, "length", line.length);
        set_num(line_node, "space", line.space);
        set_num(line_node, "tOffset", line.t_offset);
        set_num(line_node, "sOffset", line.s_offset);
        set_num(line_node, "width", line.width);
      }
    }
  }
}

void set_optional_num(pugi::xml_node node, const char* name, std::optional<double> value) {
  if (value.has_value()) {
    set_num(node, name, *value);
  }
}

void write_repeat(pugi::xml_node object_node,
                  const ObjectRepeat& repeat,
                  const WriterOptions& options) {
  pugi::xml_node node = object_node.append_child("repeat");
  set_num(node, "s", repeat.s);
  set_num(node, "length", repeat.length);
  set_num(node, "distance", repeat.distance);
  set_num(node, "tStart", repeat.t_start);
  set_num(node, "tEnd", repeat.t_end);
  set_num(node, "zOffsetStart", repeat.z_offset_start);
  set_num(node, "zOffsetEnd", repeat.z_offset_end);
  set_optional_num(node, "widthStart", repeat.width_start);
  set_optional_num(node, "widthEnd", repeat.width_end);
  set_optional_num(node, "heightStart", repeat.height_start);
  set_optional_num(node, "heightEnd", repeat.height_end);
  set_optional_num(node, "lengthStart", repeat.length_start);
  set_optional_num(node, "lengthEnd", repeat.length_end);
  set_optional_num(node, "radiusStart", repeat.radius_start);
  set_optional_num(node, "radiusEnd", repeat.radius_end);
  // @detachFromReferenceLine is 1.8.0 (1.9.0 §13.4, Table 95) — both writer
  // targets are >=1.8, so it needs no gate; @bT/@cT/@dT are 1.9.0 only
  // (absent from 1.8.1 §13.4), so a 1.8.1 target keeps the linear t
  // interpolation and drops the coefficients.
  if (repeat.detach_from_reference_line) {
    node.append_attribute("detachFromReferenceLine").set_value("true");
  }
  if (options.target_version == XodrVersion::v1_9_0) {
    set_optional_num(node, "bT", repeat.b_t);
    set_optional_num(node, "cT", repeat.c_t);
    set_optional_num(node, "dT", repeat.d_t);
  }
}

void append_fragment(pugi::xml_node parent, const std::string& fragment) {
  parent.append_buffer(fragment.data(), fragment.size());
}

void write_object(pugi::xml_node objects_node, const Object& object, const WriterOptions& options) {
  pugi::xml_node node = objects_node.append_child("object");
  // type_str keeps the file's exact spelling (incl. values outside the
  // ObjectType enum); authored objects without it derive from the enum.
  if (!object.type_str.empty()) {
    node.append_attribute("type").set_value(object.type_str.c_str());
  } else if (object.type != ObjectType::None) {
    node.append_attribute("type").set_value(object_type_name(object.type));
  }
  if (!object.subtype.empty()) {
    node.append_attribute("subtype").set_value(object.subtype.c_str());
  }
  if (!object.name.empty()) {
    node.append_attribute("name").set_value(object.name.c_str());
  }
  node.append_attribute("id").set_value(object.odr_id.c_str());
  set_num(node, "s", object.s);
  set_num(node, "t", object.t);
  set_num(node, "zOffset", object.z_offset);
  node.append_attribute("orientation").set_value(orientation_name(object.orientation));
  if (object.hdg != 0.0) {
    set_num(node, "hdg", object.hdg);
  }
  if (object.pitch != 0.0) {
    set_num(node, "pitch", object.pitch);
  }
  if (object.roll != 0.0) {
    set_num(node, "roll", object.roll);
  }
  if (object.perp_to_road) {
    node.append_attribute("perpToRoad").set_value("true");
  }
  if (object.dynamic.has_value()) {
    node.append_attribute("dynamic").set_value(*object.dynamic ? "yes" : "no");
  }
  set_optional_num(node, "length", object.length);
  set_optional_num(node, "width", object.width);
  set_optional_num(node, "radius", object.radius);
  set_optional_num(node, "height", object.height);
  set_optional_num(node, "validLength", object.valid_length);
  // @temporary/@invalidated exist since 1.9.0 (Table 85); 1.8.1 §13.1 has no
  // such attributes, so a 1.8.1 target omits them (the 1.9.0 defaults —
  // permanent, not invalidated — are the only expressible states there).
  if (options.target_version == XodrVersion::v1_9_0) {
    if (object.temporary.has_value()) {
      node.append_attribute("temporary").set_value(*object.temporary ? "true" : "false");
    }
    if (object.invalidated.has_value()) {
      node.append_attribute("invalidated").set_value(*object.invalidated ? "true" : "false");
    }
  }
  for (const auto& [name, value] : object.preserved.attributes) {
    node.append_attribute(name.c_str()).set_value(value.c_str());
  }

  for (const ObjectRepeat& repeat : object.repeats) {
    write_repeat(node, repeat, options);
  }
  if (!object.outlines.empty()) {
    pugi::xml_node outlines_node = node.append_child("outlines");
    for (const ObjectOutline& outline : object.outlines) {
      if (!outline.raw.empty()) {
        append_fragment(outlines_node, outline.raw); // preserved verbatim
        continue;
      }
      pugi::xml_node outline_node = outlines_node.append_child("outline");
      if (outline.id.has_value()) {
        outline_node.append_attribute("id").set_value(*outline.id);
      }
      if (outline.fill_type.has_value()) {
        outline_node.append_attribute("fillType").set_value(outline.fill_type->c_str());
      }
      outline_node.append_attribute("outer").set_value(outline.outer ? "true" : "false");
      if (outline.closed.has_value()) {
        outline_node.append_attribute("closed").set_value(*outline.closed ? "true" : "false");
      }
      if (outline.lane_type.has_value()) {
        outline_node.append_attribute("laneType").set_value(outline.lane_type->c_str());
      }
      for (const OutlineCorner& corner : outline.corners) {
        pugi::xml_node corner_node =
            outline_node.append_child(outline.road_coords ? "cornerRoad" : "cornerLocal");
        if (outline.road_coords) {
          set_num(corner_node, "s", corner.a);
          set_num(corner_node, "t", corner.b);
          set_num(corner_node, "dz", corner.dz_or_z);
        } else {
          set_num(corner_node, "u", corner.a);
          set_num(corner_node, "v", corner.b);
          set_num(corner_node, "z", corner.dz_or_z);
        }
        set_num(corner_node, "height", corner.height);
        if (corner.id.has_value()) {
          corner_node.append_attribute("id").set_value(*corner.id);
        }
      }
    }
  }
  for (const std::string& fragment : object.preserved.children) {
    append_fragment(node, fragment);
  }
}

void write_objects(pugi::xml_node road_node,
                   const RoadNetwork& network,
                   RoadId road_id,
                   const Road& road,
                   const WriterOptions& options) {
  const std::vector<ObjectId> owned = objects_of(network, road_id);
  if (owned.empty() && road.object_extras.empty()) {
    return;
  }
  pugi::xml_node objects_node = road_node.append_child("objects");
  for (const ObjectId object_id : owned) {
    write_object(objects_node, *network.object(object_id), options);
  }
  for (const std::string& fragment : road.object_extras) {
    append_fragment(objects_node, fragment);
  }
}

void write_signal(pugi::xml_node signals_node, const Signal& signal, const WriterOptions& options) {
  pugi::xml_node node = signals_node.append_child("signal");
  set_num(node, "s", signal.s);
  set_num(node, "t", signal.t);
  node.append_attribute("id").set_value(signal.odr_id.c_str());
  if (!signal.name.empty()) {
    node.append_attribute("name").set_value(signal.name.c_str());
  }
  if (signal.dynamic.has_value()) {
    node.append_attribute("dynamic").set_value(*signal.dynamic ? "yes" : "no");
  }
  node.append_attribute("orientation").set_value(orientation_name(signal.orientation));
  set_num(node, "zOffset", signal.z_offset);
  if (!signal.country.empty()) {
    node.append_attribute("country").set_value(signal.country.c_str());
  }
  if (!signal.country_revision.empty()) {
    node.append_attribute("countryRevision").set_value(signal.country_revision.c_str());
  }
  node.append_attribute("type").set_value(signal.type.c_str());
  node.append_attribute("subtype").set_value(signal.subtype.c_str());
  set_optional_num(node, "value", signal.value);
  if (!signal.unit.empty()) {
    node.append_attribute("unit").set_value(signal.unit.c_str());
  }
  if (!signal.text.empty()) {
    node.append_attribute("text").set_value(signal.text.c_str());
  }
  if (signal.h_offset != 0.0) {
    set_num(node, "hOffset", signal.h_offset);
  }
  if (signal.pitch != 0.0) {
    set_num(node, "pitch", signal.pitch);
  }
  if (signal.roll != 0.0) {
    set_num(node, "roll", signal.roll);
  }
  set_optional_num(node, "height", signal.height);
  set_optional_num(node, "width", signal.width);
  // @length is 1.8.0 (Table 122); both writer targets are >=1.8, so it needs
  // no gate. @temporary/@invalidated are 1.9.0 only (1.8.1 §14.1 has no such
  // attributes), so a 1.8.1 target omits them (the 1.9.0 default is false).
  set_optional_num(node, "length", signal.length);
  if (options.target_version == XodrVersion::v1_9_0) {
    if (signal.temporary.has_value()) {
      node.append_attribute("temporary").set_value(*signal.temporary ? "true" : "false");
    }
    if (signal.invalidated.has_value()) {
      node.append_attribute("invalidated").set_value(*signal.invalidated ? "true" : "false");
    }
  }
  for (const auto& [name, value] : signal.preserved.attributes) {
    node.append_attribute(name.c_str()).set_value(value.c_str());
  }
  for (const std::string& fragment : signal.preserved.children) {
    append_fragment(node, fragment);
  }
}

void write_signals(pugi::xml_node road_node,
                   const RoadNetwork& network,
                   RoadId road_id,
                   const Road& road,
                   const WriterOptions& options) {
  const std::vector<SignalId> owned = signals_of(network, road_id);
  if (owned.empty() && road.signal_extras.empty()) {
    return;
  }
  pugi::xml_node signals_node = road_node.append_child("signals");
  for (const SignalId signal_id : owned) {
    write_signal(signals_node, *network.signal(signal_id), options);
  }
  for (const std::string& fragment : road.signal_extras) {
    append_fragment(signals_node, fragment);
  }
}

void write_road(pugi::xml_node root,
                const RoadNetwork& network,
                RoadId road_id,
                const Road& road,
                const WriterOptions& options) {
  pugi::xml_node road_node = root.append_child("road");
  if (!road.name.empty()) {
    road_node.append_attribute("name").set_value(road.name.c_str());
  }
  set_num(road_node, "length", road.plan_view.length());
  road_node.append_attribute("id").set_value(road.odr_id.c_str());
  const Junction* junction = road.junction.is_valid() ? network.junction(road.junction) : nullptr;
  road_node.append_attribute("junction")
      .set_value(junction != nullptr ? junction->odr_id.c_str() : "-1");

  if (road.predecessor || road.successor) {
    pugi::xml_node link = road_node.append_child("link");
    if (road.predecessor) {
      write_link_element(link, "predecessor", *road.predecessor, network);
    }
    if (road.successor) {
      write_link_element(link, "successor", *road.successor, network);
    }
  }

  pugi::xml_node plan_view = road_node.append_child("planView");
  for (const GeometryRecord& record : road.plan_view.records()) {
    pugi::xml_node geometry = plan_view.append_child("geometry");
    set_num(geometry, "s", record.s);
    set_num(geometry, "x", record.x);
    set_num(geometry, "y", record.y);
    set_num(geometry, "hdg", record.hdg);
    set_num(geometry, "length", record.length);
    std::visit(
        [&](const auto& shape) {
          using Shape = std::decay_t<decltype(shape)>;
          if constexpr (std::is_same_v<Shape, LineGeom>) {
            geometry.append_child("line");
          } else if constexpr (std::is_same_v<Shape, ArcGeom>) {
            set_num(geometry.append_child("arc"), "curvature", shape.curvature);
          } else if constexpr (std::is_same_v<Shape, SpiralGeom>) {
            pugi::xml_node spiral = geometry.append_child("spiral");
            set_num(spiral, "curvStart", shape.curv_start);
            set_num(spiral, "curvEnd", shape.curv_end);
          } else {
            pugi::xml_node poly = geometry.append_child("paramPoly3");
            set_num(poly, "aU", shape.au);
            set_num(poly, "bU", shape.bu);
            set_num(poly, "cU", shape.cu);
            set_num(poly, "dU", shape.du);
            set_num(poly, "aV", shape.av);
            set_num(poly, "bV", shape.bv);
            set_num(poly, "cV", shape.cv);
            set_num(poly, "dV", shape.dv);
            poly.append_attribute("pRange").set_value(shape.normalized ? "normalized"
                                                                       : "arcLength");
          }
        },
        record.shape);
  }

  if (!road.elevation.empty()) {
    write_poly3_list(road_node.append_child("elevationProfile"), "elevation", "s", road.elevation);
  }
  if (!road.superelevation.empty()) {
    write_poly3_list(
        road_node.append_child("lateralProfile"), "superelevation", "s", road.superelevation);
  }

  pugi::xml_node lanes = road_node.append_child("lanes");
  write_poly3_list(lanes, "laneOffset", "s", road.lane_offset);
  for (const LaneSectionId section_id : road.sections) {
    const LaneSection& section = *network.lane_section(section_id);
    pugi::xml_node section_node = lanes.append_child("laneSection");
    set_num(section_node, "s", section.s0);

    pugi::xml_node left;
    pugi::xml_node center;
    pugi::xml_node right;
    for (const LaneId lane_id : section.lanes) { // leftmost first
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id > 0) {
        if (!left) {
          left = section_node.append_child("left");
        }
        write_lane(left, lane);
      } else if (lane.odr_id == 0) {
        if (!center) {
          center = section_node.append_child("center");
        }
        write_lane(center, lane);
      } else {
        if (!right) {
          right = section_node.append_child("right");
        }
        write_lane(right, lane);
      }
    }
  }

  // <objects> follows <lanes> in the road element sequence (1.9.0 §10.1).
  write_objects(road_node, network, road_id, road, options);

  // <signals> follows <objects> in the road element sequence (1.9.0 §10.1).
  write_signals(road_node, network, road_id, road, options);

  // Authoring waypoints round-trip through the spec-sanctioned <userData>
  // extension (OpenDRIVE 1.9.0 §7.2: code required, value optional free
  // text). Emitted last so the normative children keep their order.
  if (road.authoring_waypoints.has_value()) {
    std::string value;
    for (const Waypoint& waypoint : *road.authoring_waypoints) {
      if (!value.empty()) {
        value += ';';
      }
      value += num(waypoint.x);
      value += ',';
      value += num(waypoint.y);
    }
    pugi::xml_node user_data = road_node.append_child("userData");
    user_data.append_attribute("code").set_value("rm:waypoints");
    user_data.append_attribute("value").set_value(value.c_str());
  }
}

/// Emits the OpenDRIVE ≥1.8 junction surface elements: the <planView>
/// reference line and the <elevationGrid> sampled from the blended 2.5D surface
/// (docs/design/m2/03_junction_blending.md §3). Both writer targets are ≥1.8,
/// so these are always written for junctions that carry a surface. No
/// <boundary> is written in M2 — see junction_export.hpp.
void write_junction_surface(pugi::xml_node junction_node, const JunctionSurfaceExport& surface) {
  if (!surface.has_surface) {
    return;
  }
  pugi::xml_node plan_view = junction_node.append_child("planView");
  pugi::xml_node geometry = plan_view.append_child("geometry");
  set_num(geometry, "s", 0.0);
  set_num(geometry, "x", surface.ref_line.x);
  set_num(geometry, "y", surface.ref_line.y);
  set_num(geometry, "hdg", surface.ref_line.hdg);
  set_num(geometry, "length", surface.ref_line.length);
  geometry.append_child("line");

  pugi::xml_node grid = junction_node.append_child("elevationGrid");
  set_num(grid, "sStart", surface.grid.s_start);
  set_num(grid, "gridSpacing", surface.grid.grid_spacing);
  const auto join = [](const std::vector<double>& values) {
    std::string text;
    for (const double value : values) {
      if (!text.empty()) {
        text += ' ';
      }
      text += num(value);
    }
    return text;
  };
  for (const JunctionGridColumn& column : surface.grid.columns) {
    pugi::xml_node elevation = grid.append_child("elevation");
    // left/center/right list z from inside (nearest the reference line) to
    // outside; center sits on the line and is always present.
    if (!column.left.empty()) {
      elevation.append_attribute("left").set_value(join(column.left).c_str());
    }
    elevation.append_attribute("center").set_value(num(column.center).c_str());
    if (!column.right.empty()) {
      elevation.append_attribute("right").set_value(join(column.right).c_str());
    }
  }
}

/// Emits the OpenDRIVE ≥1.8 junction <boundary> (§12.10): a counter-clockwise,
/// closed loop of lane/joint <segment>s. Written only when the boundary can be
/// closed from the existing connecting roads (build_junction_boundary); a gap
/// that would need auxiliary boundary roads leaves it unwritten.
void write_junction_boundary(pugi::xml_node junction_node, const JunctionBoundaryExport& boundary) {
  if (!boundary.has_boundary) {
    return;
  }
  pugi::xml_node node = junction_node.append_child("boundary");
  for (const JunctionBoundarySegment& segment : boundary.segments) {
    pugi::xml_node seg = node.append_child("segment");
    seg.append_attribute("type").set_value(segment.is_lane ? "lane" : "joint");
    seg.append_attribute("roadId").set_value(segment.road_id.c_str());
    if (segment.is_lane) {
      seg.append_attribute("boundaryLane").set_value(segment.boundary_lane);
      seg.append_attribute("sStart").set_value(segment.s_begin_to_end ? "begin" : "end");
      seg.append_attribute("sEnd").set_value(segment.s_begin_to_end ? "end" : "begin");
    } else {
      seg.append_attribute("contactPoint")
          .set_value(segment.contact == ContactPoint::End ? "end" : "start");
    }
  }
}

void write_junction(pugi::xml_node root, const RoadNetwork& network, const Junction& junction) {
  pugi::xml_node junction_node = root.append_child("junction");
  junction_node.append_attribute("id").set_value(junction.odr_id.c_str());
  if (!junction.name.empty()) {
    junction_node.append_attribute("name").set_value(junction.name.c_str());
  }
  int connection_id = 0;
  for (const JunctionConnection& connection : junction.connections) {
    const Road* incoming = network.road(connection.incoming_road);
    const Road* connecting = network.road(connection.connecting_road);
    if (incoming == nullptr || connecting == nullptr) {
      continue; // stale references are not written
    }
    pugi::xml_node node = junction_node.append_child("connection");
    node.append_attribute("id").set_value(connection_id++);
    node.append_attribute("incomingRoad").set_value(incoming->odr_id.c_str());
    node.append_attribute("connectingRoad").set_value(connecting->odr_id.c_str());
    node.append_attribute("contactPoint")
        .set_value(connection.contact_point == ContactPoint::End ? "end" : "start");
    for (const auto& [from, to] : connection.lane_links) {
      pugi::xml_node link = node.append_child("laneLink");
      link.append_attribute("from").set_value(from);
      link.append_attribute("to").set_value(to);
    }
  }

  // Junction <boundary> (§12.10) then the blended 2.5D surface (§12.11:
  // reference line + elevation grid), both derived from the network so no model
  // state is stored, and emitted before <userData> so the normative children
  // keep their order. The boundary defines the area the grid applies to, so it
  // precedes the grid.
  write_junction_boundary(junction_node, build_junction_boundary(network, junction));
  write_junction_surface(junction_node, build_junction_export(network, junction));

  // The generator's arm list round-trips through <userData> (OpenDRIVE 1.9.0
  // §7.2) so regeneration survives save/load; junctions from foreign files
  // carry no arms and emit nothing. Format: "roadOdrId:start|end;…".
  if (!junction.arms.empty()) {
    std::string value;
    std::size_t written_arms = 0;
    for (const RoadEnd& arm : junction.arms) {
      const Road* road = network.road(arm.road);
      if (road == nullptr) {
        continue; // stale arm references are not written
      }
      if (!value.empty()) {
        value += ';';
      }
      value += road->odr_id;
      value += ':';
      value += arm.contact == ContactPoint::End ? "end" : "start";
      ++written_arms;
    }
    // Reader/writer symmetry (issue #90, found by the soak round-trip
    // invariant): the reader rejects rm:arms with fewer than 2 arms, so
    // writing a degenerate list (a junction that lost arms to delete_road's
    // closure) would not round-trip byte-identically. A junction below 2
    // arms cannot regenerate anyway — it persists as arm-less (foreign
    // semantics: recreate to edit).
    if (written_arms < 2) {
      value.clear();
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:arms");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }
}

} // namespace

std::vector<Diagnostic> validate_network(const RoadNetwork& network, const WriterOptions& options) {
  std::vector<Diagnostic> findings;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    check_road_structure(network, road_id, road, findings);
    // RoadMaker advisory (no ASAM rule id — rule_id stays empty): a grade
    // above options.max_grade_warning is drivable in a file but rarely in a
    // vehicle. A cubic's derivative is quadratic, so per record the extreme
    // sits at an endpoint or the derivative's vertex (hardening WS-C).
    if (options.max_grade_warning > 0.0 && !road.elevation.empty()) {
      double worst = 0.0;
      double worst_s = 0.0;
      for (std::size_t i = 0; i < road.elevation.size(); ++i) {
        const Poly3& record = road.elevation[i];
        const double end_s = i + 1 < road.elevation.size() ? road.elevation[i + 1].s : road.length;
        const auto consider = [&](double s) {
          const double grade = std::abs(record.eval_derivative(s));
          if (grade > worst) {
            worst = grade;
            worst_s = s;
          }
        };
        consider(record.s);
        consider(end_s);
        if (std::abs(record.d) > 1e-15) {
          const double vertex = record.s - record.c / (3.0 * record.d);
          if (vertex > record.s && vertex < end_s) {
            consider(vertex);
          }
        }
      }
      if (worst > options.max_grade_warning) {
        findings.push_back(Diagnostic{
            .severity = Severity::Warning,
            .location = fmt::format("road id={}", road.odr_id),
            .message = fmt::format("elevation grade {:.1f} % at s={:.1f} m exceeds {:.0f} %",
                                   worst * 100.0,
                                   worst_s,
                                   options.max_grade_warning * 100.0),
            .road = road_id});
      }
    }
    // "The width of the lane shall be defined for the full length of the
    // lane section" — a non-center lane needs a <width> at sOffset 0.
    for (const LaneSectionId section_id : road.sections) {
      const LaneSection& section = *network.lane_section(section_id);
      for (const LaneId lane_id : section.lanes) {
        const Lane& lane = *network.lane(lane_id);
        if (lane.odr_id != 0 && (lane.widths.empty() || lane.widths.front().s > 1e-9)) {
          findings.push_back(
              Diagnostic{.severity = Severity::Error,
                         .location = fmt::format("road id={}", road.odr_id),
                         .message = fmt::format("lane {} has no width at sOffset 0", lane.odr_id),
                         .rule_id = std::string(rules::kWidthDefinedWholeSection),
                         .road = road_id,
                         .lane = lane_id});
        }
        // RoadMaker advisory (NOT a normative ASAM rule — rule_id stays empty
        // so it is never spoofed as one): a solid_solid-family mark authored
        // with explicit <line> geometry should carry exactly two stripes
        // (docs/design/m3a/02 §2). Bare marks (lines empty) keep the M2 path.
        for (const RoadMark& mark : lane.road_marks) {
          const bool multi = mark.type == RoadMarkType::SolidSolid ||
                             mark.type == RoadMarkType::SolidBroken ||
                             mark.type == RoadMarkType::BrokenSolid;
          if (multi && !mark.lines.empty() && mark.lines.size() != 2) {
            findings.push_back(Diagnostic{
                .severity = Severity::Warning,
                .location = fmt::format("road id={}", road.odr_id),
                .message = fmt::format(
                    "advisory: lane {} multi-line road mark has {} <line> element(s), expected 2",
                    lane.odr_id,
                    mark.lines.size()),
                .road = road_id,
                .lane = lane_id});
          }
        }
      }
    }
  });
  // Objects (§13): model-level checker rules. Parse-time defects (missing
  // required attributes) are diagnosed by the reader; this pass checks what
  // only the assembled model can know.
  std::set<std::string> seen_object_ids;
  network.for_each_object([&](ObjectId, const Object& object) {
    const Road* owner = network.road(object.road);
    const std::string location = fmt::format(
        "road id={}/object id={}", owner != nullptr ? owner->odr_id : "?", object.odr_id);
    const auto finding = [&](std::string message, std::string_view rule) {
      findings.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = location,
                                    .message = std::move(message),
                                    .rule_id = std::string(rule),
                                    .road = object.road});
    };
    if (!object.odr_id.empty() && !seen_object_ids.insert(object.odr_id).second) {
      finding(fmt::format("duplicate object id '{}'", object.odr_id), rules::kIdUniqueInClass);
    }
    if (object.type == ObjectType::None && object.type_str.empty()) {
      finding("object has no type", rules::kObjectTypeAttr);
    }
    if (object.radius.has_value() && (object.length.has_value() || object.width.has_value())) {
      finding("object mixes circular (radius) and angular (length/width) bounding volumes",
              rules::kObjectCircularVsAngular);
    }
    if (!object.outlines.empty()) {
      const auto outer_count =
          std::count_if(object.outlines.begin(),
                        object.outlines.end(),
                        [](const ObjectOutline& outline) { return outline.outer; });
      if (outer_count != 1) {
        finding(fmt::format("object has {} outer outline(s), expected exactly 1", outer_count),
                rules::kOutlineExactlyOneOuter);
      }
    }
    for (const ObjectOutline& outline : object.outlines) {
      if (!outline.raw.empty()) {
        continue; // preserved verbatim — not interpreted, not re-validated
      }
      if (outline.corners.empty()) {
        finding("outline has no corner elements", rules::kOutlineFollowedByCorner);
      } else if (outline.corners.size() < 2) {
        finding("outline has fewer than 2 corner elements",
                outline.road_coords ? rules::kCornerRoadMinAmount : rules::kCornerLocalMinAmount);
      }
    }
  });

  // Signals (§14): model-level checker rules. Parse-time defects (missing
  // required attributes) are diagnosed by the reader; this pass checks what
  // only the assembled model can know (id uniqueness across the file) plus
  // the type/country rules whose absence is a Warning, never a drop.
  std::set<std::string> seen_signal_ids;
  network.for_each_signal([&](SignalId, const Signal& signal) {
    const Road* owner = network.road(signal.road);
    const std::string location = fmt::format(
        "road id={}/signal id={}", owner != nullptr ? owner->odr_id : "?", signal.odr_id);
    const auto finding = [&](std::string message, std::string_view rule) {
      findings.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = location,
                                    .message = std::move(message),
                                    .rule_id = std::string(rule),
                                    .road = signal.road});
    };
    if (!signal.odr_id.empty() && !seen_signal_ids.insert(signal.odr_id).second) {
      finding(fmt::format("duplicate signal id '{}'", signal.odr_id), rules::kIdUniqueInClass);
    }
    // "Signals shall have a specific type and subtype." (§14.1.)
    if (signal.type.empty() || signal.subtype.empty()) {
      finding("signal has no type/subtype", rules::kSignalType);
    }
    // "A country code shall be added to refer to country-specific rules." (§14.1.)
    if (signal.country.empty()) {
      finding("signal has no country code", rules::kSignalUseCountryCode);
    }
  });

  // The writer emits a junction <boundary> (§12.10) whenever it can be closed
  // from the existing connecting roads (build_junction_boundary). Only when a
  // gap remains — an adjacent arm pair with no bridging connecting road, or a
  // foreign junction with no arm metadata — does closing it need auxiliary
  // boundary roads (junctions.boundary.close_gap_with_new_roads, M3a #62
  // follow-up); that residual case keeps the structured warning so the
  // omission is never silent.
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    const bool has_connecting_road =
        std::any_of(junction.connections.begin(),
                    junction.connections.end(),
                    [&](const JunctionConnection& connection) {
                      return network.road(connection.connecting_road) != nullptr;
                    });
    if (has_connecting_road && !build_junction_boundary(network, junction).has_boundary) {
      findings.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = fmt::format("junction id={}", junction.odr_id),
                     .message = "junction boundary not closed — a gap between arms needs auxiliary "
                                "boundary roads; the elevation grid stays valid without it",
                     .rule_id = std::string(rules::kJunctionBoundaryCloseGap)});
    }
  });

  // junctions.common.not_only_two exists only in the 1.9.0 catalog
  // (Annex F.4.5.3); 1.8.1's Annex E (checker rules, normative) has no
  // equivalent, so the finding is version-gated on the writer target.
  if (options.target_version == XodrVersion::v1_9_0) {
    network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
      std::vector<RoadId> meeting;
      auto note = [&](RoadId id) {
        if (network.road(id) != nullptr &&
            std::find(meeting.begin(), meeting.end(), id) == meeting.end()) {
          meeting.push_back(id);
        }
      };
      for (const JunctionConnection& connection : junction.connections) {
        note(connection.incoming_road);
      }
      network.for_each_road([&](RoadId road_id, const Road& road) {
        if (road.junction != junction_id && (links_to_junction(road.predecessor, junction_id) ||
                                             links_to_junction(road.successor, junction_id))) {
          note(road_id);
        }
      });
      if (meeting.size() <= 2) {
        findings.push_back(Diagnostic{
            .severity = Severity::Warning,
            .location = fmt::format("junction id={}", junction.odr_id),
            .message = fmt::format("junction joins only {} road(s) — junctions should not be "
                                   "used when only two roads meet",
                                   meeting.size()),
            .rule_id = std::string(rules::kJunctionNotOnlyTwo)});
      }
    });
  }
  return findings;
}

Expected<std::string> write_xodr(const RoadNetwork& network,
                                 std::string_view document_name,
                                 const WriterOptions& options) {
  if (auto valid = validate(network); !valid) {
    return tl::unexpected<Error>(valid.error());
  }

  pugi::xml_document doc;
  pugi::xml_node decl = doc.append_child(pugi::node_declaration);
  decl.append_attribute("version").set_value("1.0");
  decl.append_attribute("encoding").set_value("UTF-8");

  pugi::xml_node root = doc.append_child("OpenDRIVE");
  pugi::xml_node header = root.append_child("header");
  // Both spec patch levels serialize as revMajor 1 + revMinor 8/9
  // (header carries no patch digit — 1.8.1 §6.4.1, 1.9.0 §6.4.1).
  header.append_attribute("revMajor").set_value(1);
  header.append_attribute("revMinor")
      .set_value(options.target_version == XodrVersion::v1_9_0 ? 9 : 8);
  header.append_attribute("name").set_value(std::string(document_name).c_str());
  header.append_attribute("vendor").set_value("RoadMaker");

  network.for_each_road(
      [&](RoadId road_id, const Road& road) { write_road(root, network, road_id, road, options); });
  network.for_each_junction(
      [&](JunctionId, const Junction& junction) { write_junction(root, network, junction); });

  std::ostringstream out;
  doc.save(out, "  ", pugi::format_default, pugi::encoding_utf8);
  return std::move(out).str();
}

Expected<void> save_xodr(const RoadNetwork& network,
                         const std::filesystem::path& path,
                         std::string_view document_name,
                         const WriterOptions& options) {
  auto text = write_xodr(network, document_name, options);
  if (!text) {
    return tl::unexpected<Error>(text.error());
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open file for writing", path.string());
  }
  stream << *text;
  if (!stream.good()) {
    return make_error(ErrorCode::IoFailure, "write failed", path.string());
  }
  return {};
}

} // namespace roadmaker
