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

#include "roadmaker/xodr/writer.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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

/// The separator-free alphabet every rm:* record value shares (the reader's
/// is_record_token). A token outside it could not be read back, so the writer
/// drops the record rather than emitting a value its own reader would refuse.
bool is_record_token(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
  });
}

/// The one-character state code the rm:phases grammar uses (p4-s8, issue #229):
/// g=Green, y=Yellow, r=Red, o=Off. Red is stored sparsely and never written,
/// so 'r' is only ever produced when normalizing a foreign explicit Red away.
char state_char(SignalState state) {
  switch (state) {
  case SignalState::Green:
    return 'g';
  case SignalState::Yellow:
    return 'y';
  case SignalState::Red:
    return 'r';
  case SignalState::Off:
    return 'o';
  }
  return 'r';
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

const char* lane_direction_name(LaneDirection direction) {
  switch (direction) {
  case LaneDirection::Standard:
    return "standard";
  case LaneDirection::Reversed:
    return "reversed";
  case LaneDirection::Both:
    return "both";
  }
  return "standard";
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
  case RoadMarkType::BrokenBroken:
    return "broken broken";
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
    // The mirror direction. A lane continuing across sections is linked both
    // ways (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections), so
    // a dangling predecessor is exactly as broken as a dangling successor —
    // but until P2 only the successor side was checked, and a section split
    // that mis-set predecessors would pass validation and write a subtly
    // wrong file.
    for (const LaneId lane_id : next.lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.predecessor && !lane_exists(here, *lane.predecessor)) {
        findings.push_back(
            Diagnostic{.severity = Severity::Error,
                       .location = location,
                       .message = fmt::format("lane {} predecessor {} missing in previous section",
                                              lane.odr_id,
                                              *lane.predecessor),
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

/// The `@elementId` a road link points at, or nullptr when the target no
/// longer resolves.
///
/// A stale target is reachable: `erase_road`/`erase_junction` drop the
/// junction connections that name the erased object but deliberately leave
/// surviving roads' `predecessor`/`successor` alone (road/network.hpp — the
/// edit layer's `delete_road` closure is what normally repairs them), and a
/// hand-edited or foreign file can carry the same shape. The writer resolves
/// first and never dereferences, mirroring the rm:arms rule that stale
/// references are not serialized; `validate_network` reports the omission so
/// the drop is never silent.
const std::string* link_target_odr_id(const RoadNetwork& network, const RoadLink& road_link) {
  if (const RoadId* road_target = std::get_if<RoadId>(&road_link.target)) {
    const Road* road = network.road(*road_target);
    return road != nullptr ? &road->odr_id : nullptr;
  }
  const Junction* junction = network.junction(std::get<JunctionId>(road_link.target));
  return junction != nullptr ? &junction->odr_id : nullptr;
}

/// Writes one <predecessor>/<successor>. `element_id` is the caller's already
/// resolved link_target_odr_id — a link that did not resolve is not written
/// at all, so this is never reached with a stale target.
void write_link_element(pugi::xml_node link,
                        const char* kind,
                        const RoadLink& road_link,
                        const std::string& element_id) {
  pugi::xml_node node = link.append_child(kind);
  if (std::holds_alternative<RoadId>(road_link.target)) {
    node.append_attribute("elementType").set_value("road");
    node.append_attribute("elementId").set_value(element_id.c_str());
    node.append_attribute("contactPoint")
        .set_value(road_link.contact == ContactPoint::End ? "end" : "start");
  } else {
    node.append_attribute("elementType").set_value("junction");
    node.append_attribute("elementId").set_value(element_id.c_str());
  }
}

void write_lane(pugi::xml_node side, const Lane& lane) {
  pugi::xml_node lane_node = side.append_child("lane");
  lane_node.append_attribute("id").set_value(lane.odr_id);
  lane_node.append_attribute("type").set_value(lane_type_name(lane.type));
  lane_node.append_attribute("level").set_value("false");
  // @direction (e_lane_direction, §11) is written explicitly only when not
  // Standard: the default keeps every existing fixture byte-identical (§11
  // says an absent @direction means the reference-line-derived standard).
  if (lane.direction != LaneDirection::Standard) {
    lane_node.append_attribute("direction").set_value(lane_direction_name(lane.direction));
  }
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
    // @material (§11.9, Table 47) — emitted only when assigned; the byte-stable
    // default is no attribute (spec default "standard"), so unset writes nothing.
    if (mark.material.has_value()) {
      mark_node.append_attribute("material").set_value(mark.material->c_str());
    }
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
  // <material> records (§11.8.2) — XSD sequence puts them after <roadMark>
  // and before the g_additionalData (speed/access/…/userData) group. Canonical
  // attr order sOffset, friction?, roughness?, surface?, then preserved attrs;
  // optionals omit when unset so a foreign file that lacked @friction stays
  // byte-identical. (set_optional_num/append_fragment are defined below this
  // function, so the optional writes are inlined here.)
  for (const LaneMaterial& material : lane.materials) {
    pugi::xml_node material_node = lane_node.append_child("material");
    set_num(material_node, "sOffset", material.s_offset);
    if (material.friction.has_value()) {
      set_num(material_node, "friction", *material.friction);
    }
    if (material.roughness.has_value()) {
      set_num(material_node, "roughness", *material.roughness);
    }
    if (material.surface.has_value()) {
      material_node.append_attribute("surface").set_value(material.surface->c_str());
    }
    for (const auto& [name, value] : material.preserved.attributes) {
      material_node.append_attribute(name.c_str()).set_value(value.c_str());
    }
  }
  // Preserved tier: unmodeled lane children re-emitted verbatim after the
  // modeled content (the Object precedent). A foreign file whose <speed>/etc.
  // preceded <material> re-canonicalizes to this order — the accepted
  // limitation shared by every modeled element.
  for (const std::string& fragment : lane.preserved.children) {
    lane_node.append_buffer(fragment.data(), fragment.size());
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

/// One <marking> line (§13.8, Table 99). Canonical attribute order is fixed so
/// the writer output is idempotent; @side/@weight/@width/@zOffset omit when
/// unset. cornerReferences (§13.8.1.3) follow, then any preserved unknowns.
void write_marking(pugi::xml_node markings_node, const ObjectMarking& marking) {
  pugi::xml_node node = markings_node.append_child("marking");
  node.append_attribute("color").set_value(marking.color.c_str());
  if (marking.side.has_value()) {
    node.append_attribute("side").set_value(marking.side->c_str());
  }
  if (marking.weight.has_value()) {
    node.append_attribute("weight").set_value(marking.weight->c_str());
  }
  set_optional_num(node, "width", marking.width);
  set_optional_num(node, "zOffset", marking.z_offset);
  set_num(node, "spaceLength", marking.space_length);
  set_num(node, "lineLength", marking.line_length);
  set_num(node, "startOffset", marking.start_offset);
  set_num(node, "stopOffset", marking.stop_offset);
  for (const auto& [name, value] : marking.preserved.attributes) {
    node.append_attribute(name.c_str()).set_value(value.c_str());
  }
  for (const int ref : marking.corner_refs) {
    node.append_child("cornerReference").append_attribute("id").set_value(ref);
  }
  for (const std::string& fragment : marking.preserved.children) {
    append_fragment(node, fragment);
  }
}

/// <userData code="rm:crosswalk"> (§7.2): RoadMaker's parametric-crosswalk
/// authoring record. Attributes carry the asset key + params; spec-neutral
/// defaults (borderWidth 0, dashLength/dashGap 0.5, no override, empty strings)
/// are omitted so a canonical instance stays compact and round-trips.
void write_crosswalk_data(pugi::xml_node object_node, const CrosswalkData& crosswalk) {
  pugi::xml_node node = object_node.append_child("userData");
  node.append_attribute("code").set_value("rm:crosswalk");
  if (!crosswalk.asset.empty()) {
    node.append_attribute("asset").set_value(crosswalk.asset.c_str());
  }
  if (crosswalk.border_width != 0.0) {
    set_num(node, "borderWidth", crosswalk.border_width);
  }
  if (crosswalk.dash_length != 0.5) {
    set_num(node, "dashLength", crosswalk.dash_length);
  }
  if (crosswalk.dash_gap != 0.5) {
    set_num(node, "dashGap", crosswalk.dash_gap);
  }
  if (!crosswalk.material.empty()) {
    node.append_attribute("material").set_value(crosswalk.material.c_str());
  }
  if (crosswalk.material_override) {
    node.append_attribute("materialOverride").set_value("true");
  }
  if (!crosswalk.category.empty()) {
    node.append_attribute("category").set_value(crosswalk.category.c_str());
  }
}

/// <userData code="rm:markingCurve"> (§7.2): RoadMaker's free-form marking-curve
/// authoring record. `samples` is the centreline as a "s,t;s,t;..." list (the
/// mesher's source of truth); spec-neutral defaults (dash 0, no override, empty
/// strings, striped false) are omitted so a canonical instance stays compact.
void write_marking_curve_data(pugi::xml_node object_node, const MarkingCurveData& curve) {
  pugi::xml_node node = object_node.append_child("userData");
  node.append_attribute("code").set_value("rm:markingCurve");
  if (!curve.asset.empty()) {
    node.append_attribute("asset").set_value(curve.asset.c_str());
  }
  set_num(node, "width", curve.width);
  if (curve.dash_length != 0.0) {
    set_num(node, "dashLength", curve.dash_length);
  }
  if (curve.dash_gap != 0.0) {
    set_num(node, "dashGap", curve.dash_gap);
  }
  if (!curve.material.empty()) {
    node.append_attribute("material").set_value(curve.material.c_str());
  }
  if (curve.material_override) {
    node.append_attribute("materialOverride").set_value("true");
  }
  if (!curve.category.empty()) {
    node.append_attribute("category").set_value(curve.category.c_str());
  }
  if (curve.striped) {
    node.append_attribute("striped").set_value("true");
  }
  std::string samples;
  for (const std::array<double, 2>& p : curve.samples) {
    if (!samples.empty()) {
      samples += ';';
    }
    samples += num(p[0]);
    samples += ',';
    samples += num(p[1]);
  }
  node.append_attribute("samples").set_value(samples.c_str());
}

/// <userData code="rm:stencil"> (§7.2): keys a placed arrow stencil to its
/// Library asset (and its paint material) so per-instance overrides can match
/// instance↔asset exactly. Empty tags are omitted so a canonical instance stays
/// compact.
void write_stencil_data(pugi::xml_node object_node, const StencilData& stencil) {
  pugi::xml_node node = object_node.append_child("userData");
  node.append_attribute("code").set_value("rm:stencil");
  if (!stencil.asset.empty()) {
    node.append_attribute("asset").set_value(stencil.asset.c_str());
  }
  if (!stencil.material.empty()) {
    node.append_attribute("material").set_value(stencil.material.c_str());
  }
  if (stencil.material_override) {
    node.append_attribute("materialOverride").set_value("true");
  }
  if (!stencil.category.empty()) {
    node.append_attribute("category").set_value(stencil.category.c_str());
  }
}

void write_object(pugi::xml_node objects_node, const Object& object, const WriterOptions& options) {
  // 1.8.1 §13.8 places <markings> only under <object>; 1.9.0 §13.2.4 also
  // allows them inside <outline>. Demote outline markings to object level when
  // targeting 1.8.1 (see the outline-writing loop below).
  const bool demote_markings = options.target_version == XodrVersion::v1_8_1;
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
      // §13.2.4/§13.8: outline-nested <markings> (1.9.0). Targeting 1.8.1
      // demotes them to object level below — 1.8.1 §13.8 only places <markings>
      // directly under <object>, referencing outline points via
      // <cornerReference> (which round-trips unchanged).
      if (!demote_markings && !outline.markings.empty()) {
        pugi::xml_node outline_markings = outline_node.append_child("markings");
        for (const ObjectMarking& marking : outline.markings) {
          write_marking(outline_markings, marking);
        }
      }
    }
  }
  // Object-level <markings> (§13.8): the 1.8.1 bounding-volume form always, plus
  // — when demoting for a 1.8.1 target — every outline's markings hoisted here.
  std::vector<const ObjectMarking*> object_level_markings;
  for (const ObjectMarking& marking : object.markings) {
    object_level_markings.push_back(&marking);
  }
  if (demote_markings) {
    for (const ObjectOutline& outline : object.outlines) {
      for (const ObjectMarking& marking : outline.markings) {
        object_level_markings.push_back(&marking);
      }
    }
  }
  if (!object_level_markings.empty()) {
    pugi::xml_node markings_node = node.append_child("markings");
    for (const ObjectMarking* marking : object_level_markings) {
      write_marking(markings_node, *marking);
    }
  }
  // RoadMaker parametric-crosswalk authoring data (§7.2 userData
  // "rm:crosswalk"): the interop-projection outline/markings above are derived
  // from these params; on reload this is the source of truth.
  if (object.crosswalk.has_value()) {
    write_crosswalk_data(node, *object.crosswalk);
  }
  // RoadMaker free-form marking-curve authoring data (§7.2 userData
  // "rm:markingCurve"): the interop outline/markings above are derived from
  // these params; on reload this is the source of truth the mesher walks.
  if (object.marking_curve.has_value()) {
    write_marking_curve_data(node, *object.marking_curve);
  }
  // RoadMaker point-stencil authoring data (§7.2 userData "rm:stencil"): keys the
  // placed arrow to its Library asset for per-instance overrides.
  if (object.stencil.has_value()) {
    write_stencil_data(node, *object.stencil);
  }
  for (const std::string& fragment : object.preserved.children) {
    append_fragment(node, fragment);
  }
}

/// How many of a junction's arms will actually be serialized into `rm:arms` —
/// stale entries (whose road is gone) are not written.
///
/// Shared by the rm:arms writer and the stop-line exporter so the two cannot
/// drift: anything whose ownership is recovered on load through
/// `Junction::arms` is only writable when that list itself round-trips.
std::size_t writable_arm_count(const RoadNetwork& network, const Junction& junction) {
  return static_cast<std::size_t>(std::ranges::count_if(
      junction.arms, [&](const RoadEnd& arm) { return network.road(arm.road) != nullptr; }));
}

/// True when `junction`'s arm list survives a round trip. The reader rejects an
/// rm:arms with fewer than 2 arms, so the writer does not emit one either
/// (issue #90) — and a junction that comes back arm-less cannot re-resolve
/// anything keyed on a RoadEnd.
bool arms_round_trip(const RoadNetwork& network, const Junction& junction) {
  return writable_arm_count(network, junction) >= 2;
}

/// True when `junction` is a span (virtual) junction that can be written —
/// i.e. it has spans and EVERY span road still resolves (p4-s4, issue #319).
///
/// All-or-nothing on purpose: the reader treats a malformed rm:spans as
/// all-or-nothing too, and @mainRoad/@sStart/@sEnd are mandatory for a
/// `<junction type="virtual">` (ASAM 1.9.0 §12.7 Table 69), so a junction whose
/// main road went stale cannot be written as virtual at all. It then degrades
/// to a plain arm-less junction; validate_network says so, so the omission is
/// never silent (issue #311 — never dereference a stale road ref).
bool spans_round_trip(const RoadNetwork& network, const Junction& junction) {
  return !junction.spans.empty() && std::ranges::all_of(junction.spans, [&](const SpanArm& span) {
    return network.road(span.road) != nullptr;
  });
}

/// One materialized stop line: the solved geometry plus the deterministic
/// `@id` its object will carry.
struct StopLineExport {
  JunctionStopLineInfo info;
  std::string odr_id;

  /// The owning junction's odr id, set ONLY for a span (virtual) junction's
  /// faces. An arm line needs no such back-reference — the reader recovers its
  /// junction from the road end — but a span face's RoadEnd is a pseudo key
  /// that resolves to no junction at all, so the reference is what makes the
  /// record readable back (p4-s4, issue #319).
  std::string span_junction_odr_id;
};

/// Materialized stop lines keyed by the owning road's odr id. Built once per
/// write (see build_stopline_exports) because the solve is per JUNCTION while
/// the emission is per ROAD.
using StopLineExports = std::map<std::string, std::vector<StopLineExport>>;

/// `<userData code="rm:stopline">` (§7.2): the parametric record behind a
/// materialized stop line. `contact` is mandatory — it names the junction-facing
/// end of the enclosing road, which is the record's identity and the only thing
/// that lets the reader absorb the object back into a StopLine. Everything else
/// is omitted at its default, so a fully derived line exports as a bare tag and
/// stays byte-stable.
///
/// `junction` is written for a SPAN junction's faces only (p4-s4, issue #319).
/// There `contact` names which FACE of the span the line guards rather than a
/// junction-facing road end, and no road end of the span road belongs to the
/// junction at all — so without the junction's id the reader has nothing to key
/// the record back on. Its absence is exactly what selects the arm path on
/// read, which is why an arm line's bytes are untouched.
void write_stopline_data(pugi::xml_node object_node, const StopLineExport& line) {
  const JunctionStopLineInfo& info = line.info;
  pugi::xml_node node = object_node.append_child("userData");
  node.append_attribute("code").set_value("rm:stopline");
  node.append_attribute("contact").set_value(info.arm.contact == ContactPoint::End ? "end"
                                                                                   : "start");
  if (!line.span_junction_odr_id.empty()) {
    node.append_attribute("junction").set_value(line.span_junction_odr_id.c_str());
  }
  if (info.distance_authored) {
    set_num(node, "distance", info.distance);
  }
  if (info.flipped) {
    node.append_attribute("flipped").set_value("true");
  }
  if (!info.crosswalk_odr_id.empty()) {
    node.append_attribute("crosswalk").set_value(info.crosswalk_odr_id.c_str());
  }
}

/// Emits one stop line as a self-contained `<object type="roadMark"
/// subtype="signalLines">` (§13.7 Table 117 — a bounding-volume road-marking
/// object; `<outline>` is optional for this subtype and none is written, so the
/// element is identical under both 1.8.1 and 1.9.0). A foreign consumer reads a
/// valid stop line with no RoadMaker knowledge (ADR-0008 Layer 0); the
/// rm:stopline userData beside it is what RoadMaker absorbs on reload (Layer 1).
///
/// Axis convention, shared with the pre-p4-s3 generator: `@length` is the thin
/// ALONG-road extent and `@width` spans ACROSS the lanes — inverted with respect
/// to the crosswalk object.
void write_stopline_object(pugi::xml_node objects_node, const StopLineExport& line) {
  // Attribute order and defaults follow write_object exactly (orientation is
  // always written, hdg is omitted at 0), so a materialized line is
  // indistinguishable from an authored object to a foreign reader — and does
  // not trip the reader's own "missing @orientation" advisory on reload.
  pugi::xml_node node = objects_node.append_child("object");
  node.append_attribute("type").set_value("roadMark");
  node.append_attribute("subtype").set_value("signalLines");
  node.append_attribute("id").set_value(line.odr_id.c_str());
  set_num(node, "s", line.info.s_center);
  set_num(node, "t", line.info.t_center);
  set_num(node, "zOffset", 0.0);
  node.append_attribute("orientation").set_value(orientation_name(ObjectOrientation::None));
  set_num(node, "length", line.info.thickness);
  set_num(node, "width", line.info.span);
  write_stopline_data(node, line);
}

/// Solves every junction's stop lines and buckets them by owning road, minting
/// each one's `@id`.
///
/// Both the ids and the per-road emission order are keyed on INTRINSIC data —
/// the owning junction's and road's odr ids and the contact end — never on
/// arena iteration order. A road can be an arm of two junctions (one at each
/// end), and junction arena order is NOT stable across a round trip: erasing a
/// junction leaves a slot that a later creation reuses, while a freshly parsed
/// network has no holes. Bucketing in arena order therefore flipped the two
/// objects' positions on the second write and broke write→parse→write
/// idempotence. Sorting before minting also keeps the collision suffixes
/// themselves order-independent.
StopLineExports build_stopline_exports(const RoadNetwork& network) {
  struct Pending {
    std::string junction_odr_id;
    std::string road_odr_id;
    bool span = false;
    JunctionStopLineInfo info;
  };

  std::vector<Pending> pending;
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    // A stop line is owned by a RoadEnd, and the reader recovers that ownership
    // through the junction's arm list. When that list will not be written, the
    // object would come back as an orphan — a live, untagged-looking object that
    // then SUPPRESSES the derived line and lands in arena order, breaking
    // write→parse→write. Emit nothing rather than something unreadable; the
    // junction is degenerate (below 2 arms) and already persists as arm-less.
    //
    // A span junction's faces are keyed on the junction id instead, so their
    // gate is the one that decides whether that junction is written as virtual
    // at all — all-or-nothing, exactly like rm:spans (#319, #311). Never write
    // what you cannot read back.
    const bool span = !junction.spans.empty();
    if (span ? !spans_round_trip(network, junction) : !arms_round_trip(network, junction)) {
      return;
    }
    for (const JunctionStopLineInfo& info : junction_stoplines(network, junction_id)) {
      const Road* road = network.road(info.arm.road);
      if (road == nullptr) {
        continue;
      }
      pending.push_back(Pending{.junction_odr_id = junction.odr_id,
                                .road_odr_id = road->odr_id,
                                .span = span,
                                .info = info});
    }
  });
  std::ranges::sort(pending, [](const Pending& a, const Pending& b) {
    return std::tie(a.road_odr_id, a.junction_odr_id, a.info.arm.contact) <
           std::tie(b.road_odr_id, b.junction_odr_id, b.info.arm.contact);
  });

  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });

  StopLineExports out;
  for (Pending& entry : pending) {
    const std::string base = "sl_" + entry.junction_odr_id + "_" + entry.road_odr_id + "_" +
                             (entry.info.arm.contact == ContactPoint::End ? "e" : "s");
    std::string id = base;
    for (int suffix = 1; taken.contains(id); ++suffix) {
      id = base + "_" + std::to_string(suffix);
    }
    taken.insert(id);
    out[entry.road_odr_id].push_back(
        StopLineExport{.info = std::move(entry.info),
                       .odr_id = std::move(id),
                       .span_junction_odr_id = entry.span ? entry.junction_odr_id : std::string{}});
  }
  return out;
}

void write_objects(pugi::xml_node road_node,
                   const RoadNetwork& network,
                   RoadId road_id,
                   const Road& road,
                   const WriterOptions& options,
                   const StopLineExports& stoplines) {
  const std::vector<ObjectId> owned = objects_of(network, road_id);
  const auto materialized = stoplines.find(road.odr_id);
  const bool has_stoplines = materialized != stoplines.end();
  if (owned.empty() && road.object_extras.empty() && !has_stoplines) {
    return;
  }
  pugi::xml_node objects_node = road_node.append_child("objects");
  for (const ObjectId object_id : owned) {
    write_object(objects_node, *network.object(object_id), options);
  }
  // Materialized stop lines come after the road's real objects: they are
  // derived output, not part of the arena, and keeping them last leaves the
  // authored objects' serialization untouched.
  if (has_stoplines) {
    for (const StopLineExport& line : materialized->second) {
      write_stopline_object(objects_node, line);
    }
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

/// Top-level `<controller>` (§14.6, Table 128) with its `<control>` children
/// (Table 129). Attribute order follows the table; optional attributes are
/// omitted when absent so a file that never had them keeps its exact bytes.
void write_controller(pugi::xml_node root, const Controller& controller) {
  pugi::xml_node node = root.append_child("controller");
  node.append_attribute("id").set_value(controller.odr_id.c_str());
  if (!controller.name.empty()) {
    node.append_attribute("name").set_value(controller.name.c_str());
  }
  if (controller.sequence.has_value()) {
    node.append_attribute("sequence").set_value(*controller.sequence);
  }
  for (const auto& [name, value] : controller.preserved.attributes) {
    node.append_attribute(name.c_str()).set_value(value.c_str());
  }
  for (const Control& control : controller.controls) {
    pugi::xml_node child = node.append_child("control");
    child.append_attribute("signalId").set_value(control.signal_odr_id.c_str());
    if (!control.type.empty()) {
      child.append_attribute("type").set_value(control.type.c_str());
    }
  }
  for (const std::string& fragment : controller.preserved.children) {
    append_fragment(node, fragment);
  }
}

void write_road(pugi::xml_node root,
                const RoadNetwork& network,
                RoadId road_id,
                const Road& road,
                const WriterOptions& options,
                const StopLineExports& stoplines) {
  pugi::xml_node road_node = root.append_child("road");
  if (!road.name.empty()) {
    road_node.append_attribute("name").set_value(road.name.c_str());
  }
  set_num(road_node, "length", road.plan_view.length());
  road_node.append_attribute("id").set_value(road.odr_id.c_str());
  const Junction* junction = road.junction.is_valid() ? network.junction(road.junction) : nullptr;
  road_node.append_attribute("junction")
      .set_value(junction != nullptr ? junction->odr_id.c_str() : "-1");

  // Resolve both ends before opening <link>: a link whose target was erased is
  // dropped (link_target_odr_id), and a <link> with no surviving child would be
  // an empty element rather than an absent one.
  const std::string* predecessor_id =
      road.predecessor ? link_target_odr_id(network, *road.predecessor) : nullptr;
  const std::string* successor_id =
      road.successor ? link_target_odr_id(network, *road.successor) : nullptr;
  if (predecessor_id != nullptr || successor_id != nullptr) {
    pugi::xml_node link = road_node.append_child("link");
    if (predecessor_id != nullptr) {
      write_link_element(link, "predecessor", *road.predecessor, *predecessor_id);
    }
    if (successor_id != nullptr) {
      write_link_element(link, "successor", *road.successor, *successor_id);
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
  write_objects(road_node, network, road_id, road, options, stoplines);

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
/// so these are always written for junctions that carry a surface. The
/// <boundary> is written separately by write_junction_boundary.
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

/// Emits a synthesized auxiliary boundary road (§12.10,
/// junctions.boundary.close_gap_with_new_roads, spec Fig. 99): a minimal
/// <road @junction> — one straight reference line and a single center lane —
/// whose outer edge closes a junction boundary gap. Tagged rm:aux_boundary so
/// the reader drops it (round-trip stays a fixed point); consumers like esmini
/// keep it and close the boundary. Emitted among the real <road>s.
void write_aux_boundary_road(pugi::xml_node root, const AuxBoundaryRoad& aux) {
  pugi::xml_node road_node = root.append_child("road");
  set_num(road_node, "length", aux.length);
  road_node.append_attribute("id").set_value(aux.odr_id.c_str());
  road_node.append_attribute("junction").set_value(aux.junction_odr_id.c_str());

  pugi::xml_node link = road_node.append_child("link");
  const auto write_end = [&](const char* kind, const std::string& road_id, ContactPoint contact) {
    pugi::xml_node node = link.append_child(kind);
    node.append_attribute("elementType").set_value("road");
    node.append_attribute("elementId").set_value(road_id.c_str());
    node.append_attribute("contactPoint").set_value(contact == ContactPoint::End ? "end" : "start");
  };
  write_end("predecessor", aux.pred_road, aux.pred_contact);
  write_end("successor", aux.succ_road, aux.succ_contact);

  pugi::xml_node plan_view = road_node.append_child("planView");
  pugi::xml_node geometry = plan_view.append_child("geometry");
  set_num(geometry, "s", 0.0);
  set_num(geometry, "x", aux.x);
  set_num(geometry, "y", aux.y);
  set_num(geometry, "hdg", aux.hdg);
  set_num(geometry, "length", aux.length);
  geometry.append_child("line");

  // A single center lane (id 0, type none) so boundaryLane=0 is meaningful and
  // the road is not lane-less (which would trip the reader's laneSection check).
  pugi::xml_node lanes = road_node.append_child("lanes");
  pugi::xml_node section = lanes.append_child("laneSection");
  set_num(section, "s", 0.0);
  pugi::xml_node lane = section.append_child("center").append_child("lane");
  lane.append_attribute("id").set_value(0);
  lane.append_attribute("type").set_value("none");
  lane.append_attribute("level").set_value("false");

  pugi::xml_node user_data = road_node.append_child("userData");
  user_data.append_attribute("code").set_value("rm:aux_boundary");
}

void write_junction(pugi::xml_node root,
                    const RoadNetwork& network,
                    const Junction& junction,
                    const JunctionBoundaryExport& boundary) {
  pugi::xml_node junction_node = root.append_child("junction");
  junction_node.append_attribute("id").set_value(junction.odr_id.c_str());
  if (!junction.name.empty()) {
    junction_node.append_attribute("name").set_value(junction.name.c_str());
  }

  // Span (virtual) junctions, p4-s4 issue #319. Layer 0 is a spec-valid
  // `<junction type="virtual">`: ASAM OpenDRIVE 1.9.0 §12.7 Table 69 marks
  // @mainRoad, @orientation, @sStart and @sEnd all `required` (1.8.1 §12.7
  // Table 69 is identical, and its rules list states the same as prose: "The
  // @mainRoad, @sStart, @sEnd, @orientation attributes shall only be valid for
  // junctions of type virtual") — and rule
  // asam.net:xodr:1.5.0:junctions.common.virtual_junction_attributes forbids
  // all four on any other junction type, which is why an arm-based junction
  // still writes neither them nor @type and keeps its pre-#319 bytes exactly.
  // @orientation="none" = "valid in both driving directions" (§12.7 Table 69).
  // Only spans[0] fits Layer 0; the whole list rides rm:spans below.
  const bool span = spans_round_trip(network, junction);
  if (span) {
    const SpanArm& main = junction.spans.front();
    junction_node.append_attribute("type").set_value("virtual");
    junction_node.append_attribute("mainRoad").set_value(network.road(main.road)->odr_id.c_str());
    junction_node.append_attribute("orientation").set_value("none");
    set_num(junction_node, "sStart", main.s_start);
    set_num(junction_node, "sEnd", main.s_end);
  }

  int connection_id = 0;
  for (const JunctionConnection& connection : junction.connections) {
    if (span) {
      break; // arms-xor-spans: a span junction generates no connecting roads
    }
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

  // The signal synchronization group (§12.14, Table 84): `<controller>`
  // children that REFERENCE top-level controllers by id. Document order comes
  // from the element overview (1.9.0 §6.5, Table 12): inside `<junction>` the
  // sequence is <connection> … <priority> … <controller> … <surface> …
  // <planView> … <boundary> … <elevationGrid>, so these are emitted right after
  // the connections and before any derived geometry — and before <userData>,
  // like every other normative child.
  //
  // A virtual junction "shall not have controllers"
  // (asam.net:xodr:1.9.0:junctions.virtual.no_controllers) and RoadMaker never
  // authors one there, but a foreign file that does is written back verbatim
  // rather than silently stripped; validate_network reports the violation.
  for (const JunctionController& entry : junction.junction_controllers) {
    pugi::xml_node node = junction_node.append_child("controller");
    node.append_attribute("id").set_value(entry.controller_odr_id.c_str());
    if (entry.sequence.has_value()) {
      node.append_attribute("sequence").set_value(*entry.sequence);
    }
    if (!entry.type.empty()) {
      node.append_attribute("type").set_value(entry.type.c_str());
    }
    for (const auto& [name, value] : entry.preserved.attributes) {
      node.append_attribute(name.c_str()).set_value(value.c_str());
    }
    for (const std::string& fragment : entry.preserved.children) {
      append_fragment(node, fragment);
    }
  }

  // Junction <boundary> (§12.10) then the blended 2.5D surface (§12.11:
  // reference line + elevation grid), both derived from the network so no model
  // state is stored, and emitted before <userData> so the normative children
  // keep their order. The boundary defines the area the grid applies to, so it
  // precedes the grid. The boundary is precomputed by the caller so its
  // synthesized auxiliary roads (if any) are emitted among the real <road>s.
  // A span junction gets neither: it encloses no pavement area (the main road
  // is never cut), and "junction boundaries are currently only valid for common
  // junctions" (asam.net:xodr:1.8.0:junctions.boundary.only_for_common_junctions,
  // §12.10).
  if (!span) {
    write_junction_boundary(junction_node, boundary);
    write_junction_surface(junction_node, build_junction_export(network, junction));
  }

  // The generator's arm list round-trips through <userData> (OpenDRIVE 1.9.0
  // §7.2) so regeneration survives save/load; junctions from foreign files
  // carry no arms and emit nothing. Format: "roadOdrId:start|end;…".
  // Reader/writer symmetry (issue #90, found by the soak round-trip
  // invariant): the reader rejects rm:arms with fewer than 2 arms, so
  // writing a degenerate list (a junction that lost arms to delete_road's
  // closure) would not round-trip byte-identically. A junction below 2
  // arms cannot regenerate anyway — it persists as arm-less (foreign
  // semantics: recreate to edit). `arms_round_trip` is the shared predicate;
  // the stop-line exporter gates on it too, for the same reason.
  if (!span && arms_round_trip(network, junction)) {
    std::string value;
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
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:arms");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // Authored corner-fillet overrides (p4-s1, issue #225) ride the same
  // <userData> pattern — ASAM OpenDRIVE 1.9.0 §12.10 gives <boundary> no
  // corner-radius carrier, and the exported boundary/grid stay derived.
  // Format: ";"-joined entries, each
  //   "roadAOdrId:start|end:roadBOdrId:start|end
  //    [:r=<num>][:ea=<num>][:eb=<num>][:sw=<name>][:md=<name>]".
  // Stale entries (a road that no longer resolves) and entries that authored
  // nothing are dropped, mirroring the stale-arm rule; storage order is kept
  // so save→load→save is byte-identical.
  if (!span && !junction.corners.empty()) {
    std::string value;
    for (const JunctionCorner& corner : junction.corners) {
      const Road* road_a = network.road(corner.arm_a.road);
      const Road* road_b = network.road(corner.arm_b.road);
      if (road_a == nullptr || road_b == nullptr) {
        continue; // stale corner references are not written
      }
      if (!corner.radius && !corner.extent_a && !corner.extent_b && !corner.sidewalk_material &&
          !corner.median_material) {
        continue; // nothing authored — the derived fillet already says it
      }
      if (!value.empty()) {
        value += ';';
      }
      value += road_a->odr_id;
      value += ':';
      value += corner.arm_a.contact == ContactPoint::End ? "end" : "start";
      value += ':';
      value += road_b->odr_id;
      value += ':';
      value += corner.arm_b.contact == ContactPoint::End ? "end" : "start";
      if (corner.radius) {
        value += ":r=";
        value += num(*corner.radius);
      }
      if (corner.extent_a) {
        value += ":ea=";
        value += num(*corner.extent_a);
      }
      if (corner.extent_b) {
        value += ":eb=";
        value += num(*corner.extent_b);
      }
      // Overlay materials (p4-s2, issue #226). The command layer restricts
      // these tokens to [A-Za-z0-9_.-]+, so they never carry a separator and
      // the grammar needs no escaping.
      if (corner.sidewalk_material) {
        value += ":sw=";
        value += *corner.sidewalk_material;
      }
      if (corner.median_material) {
        value += ":md=";
        value += *corner.median_material;
      }
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:corners");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // Authored floor-contribution overrides (p4-s5, issue #320). ASAM OpenDRIVE
  // 1.9.0 §12.10 gives <junction> no carrier for how its pavement is
  // triangulated — <boundary>/<elevationGrid> are pure output geometry — so
  // these ride their own sibling code (ADR-0008 Layer 1). Format: ";"-joined
  // entries, each "roadOdrId[:inc=0][:sort=<int>]"; `inc` appears only when
  // the samples are excluded and `sort` only when non-zero, so a record that
  // authors nothing is dropped and a junction that predates the feature keeps
  // its exact bytes. Stale road references are dropped like stale arms;
  // storage order is kept so save→load→save is byte-identical.
  //
  // A span (virtual) junction has no floor, so it never carries these.
  if (!span && !junction.surface_spans.empty()) {
    std::string value;
    for (const SurfaceSpan& entry : junction.surface_spans) {
      const Road* road = network.road(entry.road);
      if (road == nullptr) {
        continue; // stale span references are not written
      }
      if (entry.included && entry.sort_index == 0) {
        continue; // nothing authored — the derivation already says it
      }
      if (!value.empty()) {
        value += ';';
      }
      value += road->odr_id;
      if (!entry.included) {
        value += ":inc=0";
      }
      if (entry.sort_index != 0) {
        value += ":sort=";
        value += std::to_string(entry.sort_index);
      }
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:floor");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // Authored maneuver overrides (p4-s6, issue #227). ASAM OpenDRIVE 1.9.0
  // §12.2 Table 56 gives <connection> exactly @connectingRoad, @contactPoint,
  // @id and @incomingRoad, and §12.4/§12.4.2 describe a connecting road purely
  // by its geometry and lane linkage — there is no turn-type, no endpoint
  // slide and no control-point carrier anywhere in the standard. The records
  // are therefore Layer 1 alone (ADR-0008), riding their own sibling code
  // after rm:floor. Format: ";"-joined entries, each
  //   "roadOdrId[:lock=1][:turn=left|straight|right|uturn]
  //    [:so=<num>][:eo=<num>][:pts=x,y|x,y|…]"
  // — points use ',' within a point and '|' between points so no separator
  // collides with the ';'/':' the value is already joined by. Every field is
  // omitted at its default and an entry that authors nothing is dropped (the
  // AUTHORS-NOTHING ⇒ ERASE rule of Maneuver), so a junction that predates the
  // feature re-exports byte-identically. Stale road references are dropped
  // like stale arms; storage order is kept so save→load→save is byte-identical.
  //
  // A span (virtual) junction has no connections, so it never carries these.
  if (!span && !junction.maneuvers.empty()) {
    std::string value;
    for (const Maneuver& entry : junction.maneuvers) {
      const Road* road = network.road(entry.road);
      if (road == nullptr) {
        continue; // stale maneuver references are not written
      }
      if (!entry.locked && !entry.turn_type && !entry.start_offset && !entry.end_offset &&
          entry.control_points.empty()) {
        continue; // nothing authored — the derivation already says it
      }
      if (!value.empty()) {
        value += ';';
      }
      value += road->odr_id;
      if (entry.locked) {
        value += ":lock=1";
      }
      if (entry.turn_type) {
        value += ":turn=";
        switch (*entry.turn_type) {
        case TurnType::Left:
          value += "left";
          break;
        case TurnType::Straight:
          value += "straight";
          break;
        case TurnType::Right:
          value += "right";
          break;
        case TurnType::UTurn:
          value += "uturn";
          break;
        }
      }
      if (entry.start_offset) {
        value += ":so=";
        value += num(*entry.start_offset);
      }
      if (entry.end_offset) {
        value += ":eo=";
        value += num(*entry.end_offset);
      }
      if (!entry.control_points.empty()) {
        // Never write what the reader would refuse: the command layer already
        // holds authors to kMaxManeuverControlPoints, so a longer list can only
        // come from a hand-built network and is truncated rather than emitted
        // as a value the very next load would drop whole.
        const std::size_t count = std::min(entry.control_points.size(), kMaxManeuverControlPoints);
        value += ":pts=";
        for (std::size_t i = 0; i < count; ++i) {
          if (i != 0) {
            value += '|';
          }
          value += num(entry.control_points[i].x);
          value += ',';
          value += num(entry.control_points[i].y);
        }
      }
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:maneuver");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // The applied signalization template (p4-s7, issue #228). Layer 1 alone: the
  // `<signal>` and `<controller>` elements ARE the export (§14.1/§14.6), and
  // ASAM has nowhere to record WHICH template an authoring tool used, so a
  // foreign reader loses nothing. Format: ":"-joined
  //   "template=protected_left|two_phase|all_way_stop|two_way_stop[:mount=<modelId>]".
  // Written only when a template was applied, and `mount` only when non-empty,
  // so an unsignalized junction (and every junction that predates the feature)
  // emits nothing at all and re-exports byte-identically. A model id outside
  // the record alphabet cannot be read back, so it is dropped rather than
  // written (validate_network says so).
  if (!junction.signalization.tmpl.empty() &&
      std::ranges::find(kSignalizationTemplates, junction.signalization.tmpl) !=
          std::end(kSignalizationTemplates)) {
    std::string value = "template=";
    value += junction.signalization.tmpl;
    if (is_record_token(junction.signalization.mount_model)) {
      value += ":mount=";
      value += junction.signalization.mount_model;
    }
    pugi::xml_node user_data = junction_node.append_child("userData");
    user_data.append_attribute("code").set_value("rm:signal");
    user_data.append_attribute("value").set_value(value.c_str());
  }

  // The signal→prop mounts (p4-s7, issue #228), the #323 extension point:
  // ASAM ties a `<signal>` to no `<object>` at all, so the pairing is Layer 1
  // alone. Format: ";"-joined entries, each "signalOdrId=objOdrId[,objOdrId…]".
  // Stale entries are dropped exactly like stale arms — a mount whose signal or
  // whose every object no longer exists authors nothing — and the part list is
  // truncated to kMaxSignalMountParts so the writer never emits a value its own
  // reader would drop whole. Storage order is kept so save→load→save is
  // byte-identical, and an empty result emits no element.
  if (!junction.signal_mounts.empty()) {
    std::set<std::string> live_signals;
    network.for_each_signal(
        [&](SignalId, const Signal& signal) { live_signals.insert(signal.odr_id); });
    std::set<std::string> live_objects;
    network.for_each_object(
        [&](ObjectId, const Object& object) { live_objects.insert(object.odr_id); });

    std::string value;
    for (const SignalMount& mount : junction.signal_mounts) {
      if (!is_record_token(mount.signal_odr_id) || !live_signals.contains(mount.signal_odr_id)) {
        continue; // stale or unencodable — not written
      }
      std::string parts;
      std::size_t count = 0;
      for (const std::string& object_odr_id : mount.object_odr_ids) {
        if (count >= kMaxSignalMountParts) {
          break;
        }
        if (!is_record_token(object_odr_id) || !live_objects.contains(object_odr_id)) {
          continue;
        }
        if (!parts.empty()) {
          parts += ',';
        }
        parts += object_odr_id;
        ++count;
      }
      if (parts.empty()) {
        continue; // nothing left to mount
      }
      if (!value.empty()) {
        value += ';';
      }
      value += mount.signal_odr_id;
      value += '=';
      value += parts;
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:signalmount");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // The junction signal cycle (p4-s8, issue #229). Layer 1 alone: OpenDRIVE
  // §14.6 puts signal timing outside the standard ("dynamic content like the
  // signal cycle itself is specified outside of this standard"), so a foreign
  // reader loses nothing. Format: ";"-joined phase entries in cycle order, each
  // ":"-joined "name=<tok>:dur=<num>:st=<ctrl>,<state>…" (states '|'-joined,
  // g/y/r/o). NEVER on a span (virtual) junction — it has no controllers — and
  // omitted entirely when empty, so an unsignalized junction (and every one
  // that predates the feature) re-exports byte-identically.
  //
  // Pruned to exactly what the reader would accept AND to LIVE state, so the
  // writer never emits a value its own reader would drop: an entry with a
  // reader-rejectable duration is skipped, and an st pair is written only for a
  // controller that is a record token, is in this junction's synchronization
  // group, AND resolves to a live top-level controller (the rm:signalmount
  // precedent — a dormant reference is dropped). Red is stored sparsely, so a
  // Red pair (only a foreign file carries one) is never written; it normalizes
  // away here on the second save.
  if (!span && !junction.phases.empty()) {
    std::set<std::string> group_ids;
    for (const JunctionController& reference : junction.junction_controllers) {
      group_ids.insert(reference.controller_odr_id);
    }
    std::set<std::string> live_controller_ids;
    network.for_each_controller([&](ControllerId, const Controller& controller) {
      live_controller_ids.insert(controller.odr_id);
    });

    std::string value;
    const std::size_t phase_count = std::min(junction.phases.size(), kMaxSignalPhases);
    for (std::size_t i = 0; i < phase_count; ++i) {
      const SignalPhase& phase = junction.phases[i];
      if (!std::isfinite(phase.duration) || phase.duration <= 0.0 ||
          phase.duration > kMaxSignalPhaseDuration) {
        continue; // the reader would drop the whole value on this entry
      }
      std::string entry;
      if (is_record_token(phase.name)) {
        entry += "name=";
        entry += phase.name;
      }
      if (!entry.empty()) {
        entry += ':';
      }
      entry += "dur=";
      entry += num(phase.duration);

      std::string pairs;
      std::size_t pair_count = 0;
      for (const PhaseState& state : phase.states) {
        if (pair_count >= kMaxSignalPhaseStates) {
          break;
        }
        if (state.state == SignalState::Red) {
          continue; // stored sparsely; a foreign explicit Red normalizes away
        }
        if (!is_record_token(state.controller_odr_id) ||
            !group_ids.contains(state.controller_odr_id) ||
            !live_controller_ids.contains(state.controller_odr_id)) {
          continue; // dormant or unencodable — not written
        }
        if (!pairs.empty()) {
          pairs += '|';
        }
        pairs += state.controller_odr_id;
        pairs += ',';
        pairs += state_char(state.state);
        ++pair_count;
      }
      if (!pairs.empty()) {
        entry += ":st=";
        entry += pairs;
      }
      if (!value.empty()) {
        value += ';';
      }
      value += entry;
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:phases");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // Junction-scope authored values (p4-s2, issue #226) get their own code so
  // an older reader drops only these and still understands rm:arms/rm:corners.
  // Format: ";"-joined "key=value", each key at most once, element omitted
  // entirely when nothing is authored:  "r=<num>;mat=<name>;locked=1".
  //
  // `locked` (p4-s4, issue #319) is emitted ONLY when true, so every junction
  // that predates the flag keeps its exact bytes. A span junction is locked by
  // definition (arms-xor-spans: there is no automatic derivation to skip), so
  // it is written as locked whatever the in-memory flag says — otherwise the
  // reader, which forces the invariant, would produce different bytes on the
  // second write.
  {
    std::string value;
    if (junction.default_corner_radius) {
      value += "r=";
      value += num(*junction.default_corner_radius);
    }
    if (!junction.material.empty()) {
      if (!value.empty()) {
        value += ';';
      }
      value += "mat=";
      value += junction.material;
    }
    if (junction.locked || span) {
      if (!value.empty()) {
        value += ';';
      }
      value += "locked=1";
    }
    if (!value.empty()) {
      pugi::xml_node user_data = junction_node.append_child("userData");
      user_data.append_attribute("code").set_value("rm:junction");
      user_data.append_attribute("value").set_value(value.c_str());
    }
  }

  // The full span list (p4-s4, issue #319) rides its own sibling code, because
  // Layer 0 can only carry ONE span (@mainRoad/@sStart/@sEnd). Every span is
  // written, spans[0] included, so that the reader — which REPLACES the
  // Layer-0 span with a well-formed rm:spans — reproduces the list exactly and
  // save->load->save stays byte-identical. Format: ";"-joined entries, each
  // "roadOdrId:s_start:s_end". Stale roads cannot appear: spans_round_trip
  // already required every one of them to resolve.
  if (span) {
    std::string value;
    for (const SpanArm& entry : junction.spans) {
      if (!value.empty()) {
        value += ';';
      }
      value += network.road(entry.road)->odr_id;
      value += ':';
      value += num(entry.s_start);
      value += ':';
      value += num(entry.s_end);
    }
    pugi::xml_node user_data = junction_node.append_child("userData");
    user_data.append_attribute("code").set_value("rm:spans");
    user_data.append_attribute("value").set_value(value.c_str());
  }
}

} // namespace

std::vector<Diagnostic> validate_network(const RoadNetwork& network, const WriterOptions& options) {
  std::vector<Diagnostic> findings;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    check_road_structure(network, road_id, road, findings);
    // "Only defined IDs may be referenced." A predecessor/successor whose
    // target was erased (erase_road leaves surviving roads' links alone) or
    // that a foreign file names without defining is not written — say so, so
    // the omission from the output is never silent.
    const auto check_link = [&](const char* kind, const std::optional<RoadLink>& link) {
      if (link.has_value() && link_target_odr_id(network, *link) == nullptr) {
        findings.push_back(Diagnostic{
            .severity = Severity::Warning,
            .location = fmt::format("road id={}", road.odr_id),
            .message =
                fmt::format("{} references a {} that no longer exists — the link is "
                            "omitted from the output",
                            kind,
                            std::holds_alternative<RoadId>(link->target) ? "road" : "junction"),
            .rule_id = std::string(rules::kOnlyRefDefinedIds),
            .road = road_id});
      }
    };
    check_link("predecessor", road.predecessor);
    check_link("successor", road.successor);
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
        // RoadMaker advisory (no normative ASAM rule id — rule_id stays empty):
        // the center lane (id 0) has no travel of its own, so a @direction on
        // it (e_lane_direction, §11) is meaningless. Warn rather than drop.
        if (lane.odr_id == 0 && lane.direction != LaneDirection::Standard) {
          findings.push_back(Diagnostic{.severity = Severity::Warning,
                                        .location = fmt::format("road id={}", road.odr_id),
                                        .message = "advisory: the center lane (id 0) carries a "
                                                   "non-standard travel direction",
                                        .road = road_id,
                                        .lane = lane_id});
        }
        // RoadMaker advisory (NOT a normative ASAM rule — rule_id stays empty
        // so it is never spoofed as one): a solid_solid-family mark authored
        // with explicit <line> geometry should carry exactly two stripes
        // (docs/design/m3a/02 §2). Bare marks (lines empty) keep the M2 path.
        for (const RoadMark& mark : lane.road_marks) {
          const bool multi =
              mark.type == RoadMarkType::SolidSolid || mark.type == RoadMarkType::SolidBroken ||
              mark.type == RoadMarkType::BrokenSolid || mark.type == RoadMarkType::BrokenBroken;
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
    // Object markings (§13.8): color/geometry integrity plus corner-reference
    // resolution. Applies to both the 1.9.0 outline-nested form and the 1.8.1
    // object-level form; `corner_ids` is the set of referenceable outline
    // point ids (nullptr for object-level markings, which use @side instead).
    const auto check_marking = [&](const ObjectMarking& marking, const std::set<int>* corner_ids) {
      if (marking.color.empty()) {
        finding("object marking has no color", rules::kObjectMarkingColour);
      }
      if (marking.line_length <= 0.0) {
        finding("object marking lineLength must be greater than 0", {});
      }
      if (marking.space_length < 0.0) {
        finding("object marking spaceLength must not be negative", {});
      }
      if (corner_ids != nullptr) {
        for (const int ref : marking.corner_refs) {
          if (!corner_ids->contains(ref)) {
            finding(
                fmt::format("marking cornerReference id {} has no matching outline corner", ref),
                {});
          }
        }
      }
    };
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
      std::set<int> corner_ids;
      for (const OutlineCorner& corner : outline.corners) {
        if (corner.id.has_value()) {
          corner_ids.insert(*corner.id);
        }
      }
      for (const ObjectMarking& marking : outline.markings) {
        check_marking(marking, &corner_ids);
      }
    }
    for (const ObjectMarking& marking : object.markings) {
      check_marking(marking, nullptr);
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

  // Signal controllers (§14.6): id uniqueness across the database, the
  // one-or-more-signals rule, and dangling <control> references. Erasing a
  // signal deliberately does NOT cascade into controllers (that keeps
  // delete_signal a leaf op), so a dangling reference is an expected state the
  // author is told about rather than a corruption.
  std::set<std::string> live_signal_ids;
  network.for_each_signal(
      [&](SignalId, const Signal& signal) { live_signal_ids.insert(signal.odr_id); });
  std::set<std::string> seen_controller_ids;
  network.for_each_controller([&](ControllerId, const Controller& controller) {
    const std::string location = fmt::format("controller id={}", controller.odr_id);
    if (!controller.odr_id.empty() && !seen_controller_ids.insert(controller.odr_id).second) {
      findings.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = location,
                     .message = fmt::format("duplicate controller id '{}'", controller.odr_id),
                     .rule_id = std::string(rules::kIdUniqueInClass)});
    }
    // "Controllers shall be valid for one or more signals." (§14.6; <control>
    // has multiplicity 1..*.)
    if (controller.controls.empty()) {
      findings.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = location,
                                    .message = "controller controls no signals",
                                    .rule_id = std::string(rules::kControllerValidForSignals)});
    }
    for (const Control& control : controller.controls) {
      if (control.signal_odr_id.empty() || live_signal_ids.contains(control.signal_odr_id)) {
        continue;
      }
      // RoadMaker advisory: ASAM has no rule id for a dangling @signalId (the
      // generic ids.only_ref_defined_ids covers linkage attributes, not
      // <control>), so rule_id stays EMPTY rather than citing an invented one.
      findings.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = location,
          .message = fmt::format("advisory: control references signal '{}', which does not exist",
                                 control.signal_odr_id)});
    }
  });

  // "Virtual junctions shall not have controllers and therefore no traffic
  // lights." (1.9.0 §12.7 / Annex F.4.13.5.) RoadMaker never authors one, so
  // this can only come from a foreign file — which is written back verbatim,
  // never silently stripped.
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    if (junction.spans.empty() || junction.junction_controllers.empty()) {
      return;
    }
    findings.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = fmt::format("junction id={}", junction.odr_id),
                   .message = "virtual junction declares a signal synchronization group",
                   .rule_id = std::string(rules::kVirtualNoControllers)});
  });

  // Signal phases (p4-s8, issue #229) are Layer 1: OpenDRIVE §14.6 excludes the
  // cycle, so these are RoadMaker advisories with an EMPTY rule_id (the
  // dangling-<control> precedent), except the span-junction case which reuses
  // the §12.7 virtual-junction rule. Each mirrors a way the writer prunes: what
  // it drops on write, the author is told about here.
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    if (junction.phases.empty()) {
      return;
    }
    const std::string location = fmt::format("junction id={}", junction.odr_id);

    // (1) A span (virtual) junction "shall not have controllers and therefore
    // no traffic lights" (§12.7), so a phase cycle on one drives nothing that
    // can exist. It has no synchronization group, so findings 2/3 do not apply.
    if (!junction.spans.empty()) {
      findings.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = location,
                                    .message = "virtual junction carries a signal phase cycle",
                                    .rule_id = std::string(rules::kVirtualNoControllers)});
      return;
    }

    // (5) The list is truncated to kMaxSignalPhases on write.
    if (junction.phases.size() > kMaxSignalPhases) {
      findings.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = location,
          .message =
              fmt::format("advisory: junction has {} signal phases; only the first {} are written",
                          junction.phases.size(),
                          kMaxSignalPhases)});
    }

    // (4) An out-of-band duration is not written (the reader would drop it).
    if (std::any_of(junction.phases.begin(), junction.phases.end(), [](const SignalPhase& phase) {
          return !std::isfinite(phase.duration) || phase.duration <= 0.0 ||
                 phase.duration > kMaxSignalPhaseDuration;
        })) {
      findings.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = location,
          .message = "advisory: a signal phase has an out-of-range duration and is not written"});
    }

    // (2)/(3) The one-query idiom: the effective cycle resolves member
    // controllers and reports every authored state that names a controller
    // outside the live sync group.
    const JunctionPhasePlan plan = junction_phases(network, junction_id);

    // (2) Phases authored, but no live member controller — the cycle drives
    // nothing.
    if (plan.controller_odr_ids.empty()) {
      findings.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = location,
                                    .message = "advisory: junction has signal phases but no live "
                                               "controller; the cycle drives nothing"});
    }

    // (3) States naming a controller outside the sync group or since deleted are
    // dormant — pruned on write.
    if (!plan.dormant_controller_odr_ids.empty()) {
      std::string names;
      for (const std::string& id : plan.dormant_controller_odr_ids) {
        if (!names.empty()) {
          names += ", ";
        }
        names += id;
      }
      findings.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = location,
          .message = fmt::format("advisory: signal phase names controller(s) not in this "
                                 "junction's group; they are not written: {}",
                                 names)});
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

  // A span (virtual) junction whose road references went stale cannot be
  // written as `<junction type="virtual">` at all — @mainRoad/@sStart/@sEnd are
  // mandatory (§12.7 Table 69) — so it degrades to a plain arm-less junction.
  // Say so rather than dropping the spans silently (issue #319, #311).
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    if (junction.spans.empty() || spans_round_trip(network, junction)) {
      return;
    }
    findings.push_back(Diagnostic{.severity = Severity::Warning,
                                  .location = fmt::format("junction id={}", junction.odr_id),
                                  .message =
                                      "span junction references a road that no longer exists — the "
                                      "virtual junction attributes and rm:spans were not written",
                                  .rule_id = std::string(rules::kJunctionVirtualAttributes)});
  });

  // junctions.common.not_only_two exists only in the 1.9.0 catalog
  // (Annex F.4.5.3); 1.8.1's Annex E (checker rules, normative) has no
  // equivalent, so the finding is version-gated on the writer target.
  if (options.target_version == XodrVersion::v1_9_0) {
    network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
      if (!junction.spans.empty()) {
        // A virtual junction legitimately joins one uninterrupted road
        // (§12.7); the "only two roads meet" advice is about common junctions.
        return;
      }
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

  // robomous.ai:rm:1.0.0:junctions.arm_single_owner (RoadMaker-authored, NOT
  // ASAM): a road end may be an arm of at most one junction. Two junctions
  // claiming the same (road, contact) end produce superimposed / ambiguous
  // topology (gate finding 5). Map each recorded arm to its junction; a second
  // claimant is an Error.
  {
    std::vector<std::pair<RoadEnd, JunctionId>> claimed;
    network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
      for (const RoadEnd& arm : junction.arms) {
        const auto prior = std::find_if(
            claimed.begin(), claimed.end(), [&](const auto& entry) { return entry.first == arm; });
        if (prior == claimed.end()) {
          claimed.emplace_back(arm, junction_id);
          continue;
        }
        const Road* road = network.road(arm.road);
        const Junction* owner = network.junction(prior->second);
        findings.push_back(Diagnostic{
            .severity = Severity::Error,
            .location = fmt::format("junction id={}", junction.odr_id),
            .message = fmt::format(
                "road '{}' {} end is an arm of two junctions (already junction {}) — regenerate "
                "that junction instead of overlaying a second",
                road != nullptr ? road->odr_id : "?",
                arm.contact == ContactPoint::Start ? "start" : "end",
                owner != nullptr ? owner->odr_id : "?"),
            .rule_id = std::string(rules::kJunctionArmSingleOwner)});
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

  // Stop lines are materialized, not stored: solve every junction's once, up
  // front, so the per-road emission below can stay a lookup (and so the ids
  // stay stable no matter which road is written first).
  const StopLineExports stoplines = build_stopline_exports(network);
  network.for_each_road([&](RoadId road_id, const Road& road) {
    write_road(root, network, road_id, road, options, stoplines);
  });

  // Derive each junction's <boundary> once (pure). Any synthesized auxiliary
  // boundary roads are emitted among the real <road>s (before the <junction>s,
  // matching document order) so a consumer resolves the boundary's closing lane
  // segments; the same cached boundary feeds write_junction so the two stay
  // consistent.
  std::vector<JunctionBoundaryExport> boundaries;
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    boundaries.push_back(build_junction_boundary(network, junction));
  });
  for (const JunctionBoundaryExport& boundary : boundaries) {
    for (const AuxBoundaryRoad& aux : boundary.aux_roads) {
      write_aux_boundary_road(root, aux);
    }
  }
  // Top-level `<controller>` (§14.6). VERIFIED document position: the element
  // overview (1.9.0 §6.5, Table 12 — see the reference at
  // 06_general_architecture.md:302 `<road>`, :513 `<controller>`, :515 the
  // first `<junction>`) lists the children of `<OpenDRIVE>` in the order
  // <vmsGroup>, <header>, <road>, <controller>, <junction>, <junctionGroup>,
  // <station>. Controllers therefore go after every road — including the
  // synthesized auxiliary boundary roads above — and before the first
  // junction, which is exactly where they are emitted. Empty arena ⇒ nothing.
  network.for_each_controller(
      [&](ControllerId, const Controller& controller) { write_controller(root, controller); });

  std::size_t junction_index = 0;
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    write_junction(root, network, junction, boundaries[junction_index++]);
  });

  // Ground surfaces round-trip through <userData code="rm:surface">
  // (OpenDRIVE 1.9.0 §7.2): value is the ";"-joined bounding-road odr ids in
  // the surface's derived ring order. A DERIVED surface's mesh is re-derived
  // from those roads and never serialized; an AUTHORED one (p5-s1) additionally
  // carries its node graph in a `nodes` attribute, and its `value` degrades to
  // provenance. Surfaces are emitted value-sorted so arena slot churn
  // (create/erase during reconciliation) cannot perturb the byte-identical
  // round-trip. A derived bounded face needs >= 3 roads; anything less (or a
  // stale road reference) is not written, mirroring the rm:arms < 2 guard — but
  // an authored surface is written even ring-less, because its boundary is its
  // own data and dropping it would lose the user's work.
  struct SurfaceRecord {
    std::string value;    ///< ";"-joined bounding-road odr ids (the sort key)
    std::string material; ///< empty = default; written as a `material` attribute
    std::string nodes;    ///< empty = derived; written as a `nodes` attribute
  };

  std::vector<SurfaceRecord> surface_records;
  network.for_each_surface([&](SurfaceId, const Surface& surface) {
    const bool authored = surface.source == BoundarySource::Authored && surface.nodes.size() >= 3;
    if (!authored && surface.bounding_roads.size() < 3) {
      return;
    }
    std::string value;
    for (const RoadId road_id : surface.bounding_roads) {
      const Road* road = network.road(road_id);
      if (road == nullptr) {
        if (authored) {
          continue; // provenance only — a vanished road just drops out of it
        }
        return; // stale reference — drop the whole derived surface
      }
      if (!value.empty()) {
        value += ';';
      }
      value += road->odr_id;
    }
    // "x,y,tix,tiy,tox,toy" per node, ";"-joined — the same separator grammar
    // as `value`, one record per node.
    std::string nodes;
    if (authored) {
      for (const SurfaceNode& node : surface.nodes) {
        if (!nodes.empty()) {
          nodes += ';';
        }
        nodes += fmt::format("{},{},{},{},{},{}",
                             num(node.x),
                             num(node.y),
                             num(node.tangent_in_x),
                             num(node.tangent_in_y),
                             num(node.tangent_out_x),
                             num(node.tangent_out_y));
      }
    }
    surface_records.push_back(SurfaceRecord{std::move(value), surface.material, std::move(nodes)});
  });
  // Sort on `value` then `nodes`: a bounding-road ring is unique per DERIVED
  // surface, but authored surfaces can share a provenance ring (or all have an
  // empty one), so the node graph is the tie-break that keeps the order total.
  std::ranges::sort(surface_records, [](const SurfaceRecord& a, const SurfaceRecord& b) {
    return std::tie(a.value, a.nodes) < std::tie(b.value, b.nodes);
  });
  for (const SurfaceRecord& record : surface_records) {
    pugi::xml_node user_data = root.append_child("userData");
    user_data.append_attribute("code").set_value("rm:surface");
    user_data.append_attribute("value").set_value(record.value.c_str());
    // material and nodes are written ONLY when set, so every existing file
    // (neither present) round-trips byte-identically.
    if (!record.material.empty()) {
      user_data.append_attribute("material").set_value(record.material.c_str());
    }
    if (!record.nodes.empty()) {
      user_data.append_attribute("nodes").set_value(record.nodes.c_str());
    }
  }

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
