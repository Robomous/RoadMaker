#include "roadmaker/xodr/writer.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

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

/// Pre-write validation per the writer contract.
Expected<void> validate(const RoadNetwork& network) {
  Error error;
  bool failed = false;
  network.for_each_road([&](RoadId, const Road& road) {
    if (failed) {
      return;
    }
    const std::string context = fmt::format("road id={}", road.odr_id);
    if (road.plan_view.empty()) {
      error = Error{.code = ErrorCode::InvalidArgument,
                    .message = "road has no plan-view geometry",
                    .context = context};
      failed = true;
      return;
    }
    if (road.sections.empty()) {
      error = Error{.code = ErrorCode::InvalidArgument,
                    .message = "road has no lane sections",
                    .context = context};
      failed = true;
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
        error = Error{
            .code = ErrorCode::InvalidArgument,
            .message = fmt::format(
                "geometry discontinuity at record {} (gap {} m, {} rad)", i + 1, gap, heading_gap),
            .context = context};
        failed = true;
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
          error = Error{.code = ErrorCode::InvalidArgument,
                        .message = fmt::format("lane {} successor {} missing in next section",
                                               lane.odr_id,
                                               *lane.successor),
                        .context = context};
          failed = true;
          return;
        }
      }
    }
  });
  if (failed) {
    return tl::unexpected<Error>(std::move(error));
  }
  return {};
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
    set_num(mark_node, "width", mark.width);
  }
}

void write_road(pugi::xml_node root, const RoadNetwork& network, const Road& road) {
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
}

} // namespace

Expected<std::string> write_xodr(const RoadNetwork& network, std::string_view document_name) {
  if (auto valid = validate(network); !valid) {
    return tl::unexpected<Error>(valid.error());
  }

  pugi::xml_document doc;
  pugi::xml_node decl = doc.append_child(pugi::node_declaration);
  decl.append_attribute("version").set_value("1.0");
  decl.append_attribute("encoding").set_value("UTF-8");

  pugi::xml_node root = doc.append_child("OpenDRIVE");
  pugi::xml_node header = root.append_child("header");
  header.append_attribute("revMajor").set_value(1);
  header.append_attribute("revMinor").set_value(7);
  header.append_attribute("name").set_value(std::string(document_name).c_str());
  header.append_attribute("vendor").set_value("RoadMaker");

  network.for_each_road([&](RoadId, const Road& road) { write_road(root, network, road); });
  network.for_each_junction(
      [&](JunctionId, const Junction& junction) { write_junction(root, network, junction); });

  std::ostringstream out;
  doc.save(out, "  ", pugi::format_default, pugi::encoding_utf8);
  return std::move(out).str();
}

Expected<void> save_xodr(const RoadNetwork& network,
                         const std::filesystem::path& path,
                         std::string_view document_name) {
  auto text = write_xodr(network, document_name);
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
