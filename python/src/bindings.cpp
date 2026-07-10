// nanobind bindings for the RoadMaker kernel. The API mirrors the C++ data
// model pythonically: properties, natural containers, exceptions instead of
// Expected, __repr__ everywhere. No C++ exception ever crosses this
// boundary unhandled — rm::Error is translated to Python exceptions.

#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/version.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

std::vector<roadmaker::Waypoint> to_waypoints(const std::vector<std::pair<double, double>>& xy) {
  std::vector<roadmaker::Waypoint> points;
  points.reserve(xy.size());
  for (const auto& [x, y] : xy) {
    points.push_back(roadmaker::Waypoint{.x = x, .y = y});
  }
  return points;
}

/// Carrier for rm::Error through the binding boundary; translated below.
struct RmException {
  roadmaker::Error error;
};

template <class T>
T unwrap(roadmaker::Expected<T>&& expected) {
  if (!expected) {
    throw RmException{std::move(expected.error())};
  }
  return std::move(*expected);
}

void unwrap(roadmaker::Expected<void>&& expected) {
  if (!expected) {
    throw RmException{std::move(expected.error())};
  }
}

template <class Id>
void bind_id(nb::module_& m, const char* name) {
  nb::class_<Id>(m, name)
      .def(nb::init<>())
      .def_prop_ro("valid", [](Id id) { return id.is_valid(); })
      .def("__bool__", [](Id id) { return id.is_valid(); })
      .def("__eq__",
           [](Id a, nb::object b) { return nb::isinstance<Id>(b) && a == nb::cast<Id>(b); })
      .def("__hash__", [](Id id) { return std::hash<Id>{}(id); })
      .def("__repr__", [name](Id id) {
        return id.is_valid() ? std::string(name) + "(" + std::to_string(id.index) + ")"
                             : std::string(name) + "(invalid)";
      });
}

} // namespace

NB_MODULE(_roadmaker, m) {
  m.doc() = "RoadMaker kernel bindings";

  nb::register_exception_translator([](const std::exception_ptr& p, void*) {
    try {
      std::rethrow_exception(p);
    } catch (const RmException& e) {
      std::string message = e.error.message;
      if (!e.error.context.empty()) {
        message += " (" + e.error.context + ")";
      }
      PyObject* type = PyExc_ValueError;
      switch (e.error.code) {
      case roadmaker::ErrorCode::FileNotFound:
        type = PyExc_FileNotFoundError;
        break;
      case roadmaker::ErrorCode::IoFailure:
        type = PyExc_OSError;
        break;
      default:
        break;
      }
      PyErr_SetString(type, message.c_str());
    }
  });

  m.def("version", &roadmaker::version, "Kernel semantic version string.");

  // --- enums ---------------------------------------------------------------

  nb::enum_<roadmaker::LaneType>(m, "LaneType")
      .value("DRIVING", roadmaker::LaneType::Driving)
      .value("STOP", roadmaker::LaneType::Stop)
      .value("SHOULDER", roadmaker::LaneType::Shoulder)
      .value("BIKING", roadmaker::LaneType::Biking)
      .value("SIDEWALK", roadmaker::LaneType::Sidewalk)
      .value("BORDER", roadmaker::LaneType::Border)
      .value("RESTRICTED", roadmaker::LaneType::Restricted)
      .value("PARKING", roadmaker::LaneType::Parking)
      .value("MEDIAN", roadmaker::LaneType::Median)
      .value("CURB", roadmaker::LaneType::Curb)
      .value("NONE", roadmaker::LaneType::None)
      .value("OTHER", roadmaker::LaneType::Other);

  nb::enum_<roadmaker::RoadMarkType>(m, "RoadMarkType")
      .value("NONE", roadmaker::RoadMarkType::None)
      .value("SOLID", roadmaker::RoadMarkType::Solid)
      .value("BROKEN", roadmaker::RoadMarkType::Broken)
      .value("SOLID_SOLID", roadmaker::RoadMarkType::SolidSolid)
      .value("SOLID_BROKEN", roadmaker::RoadMarkType::SolidBroken)
      .value("BROKEN_SOLID", roadmaker::RoadMarkType::BrokenSolid)
      .value("OTHER", roadmaker::RoadMarkType::Other);

  nb::enum_<roadmaker::ContactPoint>(m, "ContactPoint")
      .value("START", roadmaker::ContactPoint::Start)
      .value("END", roadmaker::ContactPoint::End);

  nb::enum_<roadmaker::Severity>(m, "Severity")
      .value("INFO", roadmaker::Severity::Info)
      .value("WARNING", roadmaker::Severity::Warning)
      .value("ERROR", roadmaker::Severity::Error);

  // --- ids -------------------------------------------------------------------

  bind_id<roadmaker::RoadId>(m, "RoadId");
  bind_id<roadmaker::LaneSectionId>(m, "LaneSectionId");
  bind_id<roadmaker::LaneId>(m, "LaneId");
  bind_id<roadmaker::JunctionId>(m, "JunctionId");

  // --- value types -----------------------------------------------------------

  nb::class_<roadmaker::Poly3>(m, "Poly3")
      .def(nb::init<>())
      .def_rw("s", &roadmaker::Poly3::s)
      .def_rw("a", &roadmaker::Poly3::a)
      .def_rw("b", &roadmaker::Poly3::b)
      .def_rw("c", &roadmaker::Poly3::c)
      .def_rw("d", &roadmaker::Poly3::d)
      .def("eval", &roadmaker::Poly3::eval, "s_query"_a)
      .def("__repr__", [](const roadmaker::Poly3& p) {
        return "Poly3(s=" + std::to_string(p.s) + ", a=" + std::to_string(p.a) + ", ...)";
      });

  nb::class_<roadmaker::PathPoint>(m, "PathPoint")
      .def_ro("x", &roadmaker::PathPoint::x)
      .def_ro("y", &roadmaker::PathPoint::y)
      .def_ro("hdg", &roadmaker::PathPoint::hdg)
      .def_ro("curvature", &roadmaker::PathPoint::curvature)
      .def("__repr__", [](const roadmaker::PathPoint& p) {
        return "PathPoint(x=" + std::to_string(p.x) + ", y=" + std::to_string(p.y) +
               ", hdg=" + std::to_string(p.hdg) + ")";
      });

  nb::class_<roadmaker::ReferenceLine>(m, "ReferenceLine")
      .def_prop_ro("length", &roadmaker::ReferenceLine::length)
      .def_prop_ro("record_count",
                   [](const roadmaker::ReferenceLine& line) { return line.records().size(); })
      .def("evaluate",
           &roadmaker::ReferenceLine::evaluate,
           "s"_a,
           "Pose at station s [m], clamped to [0, length].");

  nb::class_<roadmaker::RoadMark>(m, "RoadMark")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::RoadMark* self,
             double s_offset,
             roadmaker::RoadMarkType type,
             double width) {
            new (self) roadmaker::RoadMark{.s_offset = s_offset, .type = type, .width = width};
          },
          "s_offset"_a = 0.0,
          "type"_a = roadmaker::RoadMarkType::None,
          "width"_a = 0.12)
      .def_rw("s_offset", &roadmaker::RoadMark::s_offset)
      .def_rw("type", &roadmaker::RoadMark::type)
      .def_rw("width", &roadmaker::RoadMark::width);

  nb::class_<roadmaker::Waypoint>(m, "Waypoint")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::Waypoint* self, double x, double y) {
            new (self) roadmaker::Waypoint{.x = x, .y = y};
          },
          "x"_a,
          "y"_a)
      .def_rw("x", &roadmaker::Waypoint::x)
      .def_rw("y", &roadmaker::Waypoint::y)
      .def("__repr__", [](const roadmaker::Waypoint& p) {
        return "Waypoint(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")";
      });

  nb::class_<roadmaker::RoadEnd>(m, "RoadEnd")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::RoadEnd* self, roadmaker::RoadId road, roadmaker::ContactPoint contact) {
            new (self) roadmaker::RoadEnd{.road = road, .contact = contact};
          },
          "road"_a,
          "contact"_a)
      .def_rw("road", &roadmaker::RoadEnd::road)
      .def_rw("contact", &roadmaker::RoadEnd::contact);

  nb::class_<roadmaker::Diagnostic>(m, "Diagnostic")
      .def_ro("severity", &roadmaker::Diagnostic::severity)
      .def_ro("location", &roadmaker::Diagnostic::location)
      .def_ro("message", &roadmaker::Diagnostic::message)
      .def_ro("rule_id",
              &roadmaker::Diagnostic::rule_id,
              "ASAM checker-rule UID; empty when no normative rule applies.")
      .def_ro("road",
              &roadmaker::Diagnostic::road,
              "RoadId the finding concerns; invalid when document-scoped.")
      .def_ro("lane",
              &roadmaker::Diagnostic::lane,
              "LaneId the finding concerns; invalid when not lane-scoped.")
      .def("__repr__", [](const roadmaker::Diagnostic& d) {
        const char* severity = d.severity == roadmaker::Severity::Error     ? "ERROR"
                               : d.severity == roadmaker::Severity::Warning ? "WARNING"
                                                                            : "INFO";
        std::string repr = "<" + std::string(severity) + " " + d.location + ": " + d.message;
        if (!d.rule_id.empty()) {
          repr += " [" + d.rule_id + "]";
        }
        return repr + ">";
      });

  // --- domain objects (arena-backed; references tied to the network) -------

  nb::class_<roadmaker::Lane>(m, "Lane")
      .def_ro("odr_id", &roadmaker::Lane::odr_id)
      .def_rw("type", &roadmaker::Lane::type)
      .def_ro("widths", &roadmaker::Lane::widths)
      .def_ro("road_marks", &roadmaker::Lane::road_marks)
      .def_ro("predecessor", &roadmaker::Lane::predecessor)
      .def_ro("successor", &roadmaker::Lane::successor)
      .def("__repr__", [](const roadmaker::Lane& lane) {
        return "Lane(odr_id=" + std::to_string(lane.odr_id) + ")";
      });

  nb::class_<roadmaker::LaneSection>(m, "LaneSection")
      .def_ro("s0", &roadmaker::LaneSection::s0)
      .def_ro("lanes",
              &roadmaker::LaneSection::lanes,
              "LaneIds sorted leftmost-first (descending OpenDRIVE lane id).")
      .def("__repr__", [](const roadmaker::LaneSection& section) {
        return "LaneSection(s0=" + std::to_string(section.s0) + ")";
      });

  nb::class_<roadmaker::Road>(m, "Road")
      .def_ro("name", &roadmaker::Road::name)
      .def_ro("odr_id", &roadmaker::Road::odr_id)
      .def_ro("length", &roadmaker::Road::length)
      .def_ro("junction", &roadmaker::Road::junction)
      .def_ro("sections", &roadmaker::Road::sections)
      .def_prop_ro(
          "plan_view",
          [](const roadmaker::Road& road) -> const roadmaker::ReferenceLine& {
            return road.plan_view;
          },
          nb::rv_policy::reference_internal)
      .def_ro("elevation", &roadmaker::Road::elevation)
      .def_ro("superelevation", &roadmaker::Road::superelevation)
      .def_ro("lane_offset", &roadmaker::Road::lane_offset)
      .def_ro("authoring_waypoints",
              &roadmaker::Road::authoring_waypoints,
              "Waypoints the reference line was fitted through; None for "
              "roads loaded without rm:waypoints userData.")
      .def("__repr__", [](const roadmaker::Road& road) {
        return "Road(odr_id='" + road.odr_id + "', name='" + road.name +
               "', length=" + std::to_string(road.length) + ")";
      });

  nb::class_<roadmaker::JunctionConnection>(m, "JunctionConnection")
      .def_ro("incoming_road", &roadmaker::JunctionConnection::incoming_road)
      .def_ro("connecting_road", &roadmaker::JunctionConnection::connecting_road)
      .def_ro("contact_point", &roadmaker::JunctionConnection::contact_point)
      .def_ro("lane_links", &roadmaker::JunctionConnection::lane_links);

  nb::class_<roadmaker::Junction>(m, "Junction")
      .def_ro("odr_id", &roadmaker::Junction::odr_id)
      .def_ro("name", &roadmaker::Junction::name)
      .def_ro("connections", &roadmaker::Junction::connections)
      .def("__repr__", [](const roadmaker::Junction& junction) {
        return "Junction(odr_id='" + junction.odr_id +
               "', connections=" + std::to_string(junction.connections.size()) + ")";
      });

  // --- the network -----------------------------------------------------------

  nb::class_<roadmaker::RoadNetwork>(m, "RoadNetwork")
      .def(nb::init<>())
      .def("create_road", &roadmaker::RoadNetwork::create_road, "name"_a, "odr_id"_a)
      .def("create_junction", &roadmaker::RoadNetwork::create_junction, "odr_id"_a, "name"_a)
      .def("add_lane_section", &roadmaker::RoadNetwork::add_lane_section, "road"_a, "s0"_a)
      .def("add_lane", &roadmaker::RoadNetwork::add_lane, "section"_a, "odr_lane_id"_a, "type"_a)
      .def("erase_road", &roadmaker::RoadNetwork::erase_road, "road"_a)
      .def("erase_junction", &roadmaker::RoadNetwork::erase_junction, "junction"_a)
      .def("road",
           nb::overload_cast<roadmaker::RoadId>(&roadmaker::RoadNetwork::road),
           "id"_a,
           nb::rv_policy::reference_internal,
           "Road for id, or None if the id is stale. The reference is valid "
           "only until the network is mutated.")
      .def("lane_section",
           nb::overload_cast<roadmaker::LaneSectionId>(&roadmaker::RoadNetwork::lane_section),
           "id"_a,
           nb::rv_policy::reference_internal)
      .def("lane",
           nb::overload_cast<roadmaker::LaneId>(&roadmaker::RoadNetwork::lane),
           "id"_a,
           nb::rv_policy::reference_internal)
      .def("junction",
           nb::overload_cast<roadmaker::JunctionId>(&roadmaker::RoadNetwork::junction),
           "id"_a,
           nb::rv_policy::reference_internal)
      .def("find_road", &roadmaker::RoadNetwork::find_road, "odr_id"_a)
      .def("find_junction", &roadmaker::RoadNetwork::find_junction, "odr_id"_a)
      .def_prop_ro("road_ids",
                   [](roadmaker::RoadNetwork& network) {
                     std::vector<roadmaker::RoadId> ids;
                     network.for_each_road(
                         [&](roadmaker::RoadId id, const roadmaker::Road&) { ids.push_back(id); });
                     return ids;
                   })
      .def_prop_ro(
          "junction_ids",
          [](const roadmaker::RoadNetwork& network) {
            std::vector<roadmaker::JunctionId> ids;
            network.for_each_junction(
                [&](roadmaker::JunctionId id, const roadmaker::Junction&) { ids.push_back(id); });
            return ids;
          })
      .def_prop_ro("road_count", &roadmaker::RoadNetwork::road_count)
      .def_prop_ro("lane_count", &roadmaker::RoadNetwork::lane_count)
      .def_prop_ro("junction_count", &roadmaker::RoadNetwork::junction_count)
      .def("__repr__", [](const roadmaker::RoadNetwork& network) {
        return "RoadNetwork(roads=" + std::to_string(network.road_count()) +
               ", junctions=" + std::to_string(network.junction_count()) + ")";
      });

  // --- xodr I/O ----------------------------------------------------------------

  m.def(
      "load_xodr",
      [](const std::filesystem::path& path) {
        auto result = unwrap(roadmaker::load_xodr(path));
        return std::make_pair(std::move(result.network), std::move(result.diagnostics));
      },
      "path"_a,
      "Parses a .xodr file. Returns (network, diagnostics). Raises "
      "FileNotFoundError / ValueError on structural failures.");

  m.def(
      "parse_xodr",
      [](std::string_view text) {
        auto result = unwrap(roadmaker::parse_xodr(text));
        return std::make_pair(std::move(result.network), std::move(result.diagnostics));
      },
      "text"_a,
      "Parses OpenDRIVE XML from a string. Returns (network, diagnostics).");

  m.def(
      "write_xodr",
      [](const roadmaker::RoadNetwork& network, std::string_view name) {
        return unwrap(roadmaker::write_xodr(network, name));
      },
      "network"_a,
      "name"_a = "roadmaker",
      "Serializes the network as OpenDRIVE 1.7 XML (validates first).");

  m.def(
      "save_xodr",
      [](const roadmaker::RoadNetwork& network,
         const std::filesystem::path& path,
         std::string_view name) { unwrap(roadmaker::save_xodr(network, path, name)); },
      "network"_a,
      "path"_a,
      "name"_a = "roadmaker");

  // --- authoring -----------------------------------------------------------------

  nb::class_<roadmaker::LaneSpec>(m, "LaneSpec")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::LaneSpec* self,
             roadmaker::LaneType type,
             double width,
             bool outer_marking) {
            new (self)
                roadmaker::LaneSpec{.type = type, .width = width, .outer_marking = outer_marking};
          },
          "type"_a = roadmaker::LaneType::Driving,
          "width"_a = 3.5,
          "outer_marking"_a = false)
      .def_rw("type", &roadmaker::LaneSpec::type)
      .def_rw("width", &roadmaker::LaneSpec::width)
      .def_rw("outer_marking", &roadmaker::LaneSpec::outer_marking);

  nb::class_<roadmaker::LaneProfile>(m, "LaneProfile")
      .def(nb::init<>())
      .def_rw("left", &roadmaker::LaneProfile::left)
      .def_rw("right", &roadmaker::LaneProfile::right)
      .def_rw("center_marking", &roadmaker::LaneProfile::center_marking)
      .def_static("two_lane_default", &roadmaker::LaneProfile::two_lane_default);

  m.def(
      "author_clothoid_road",
      [](roadmaker::RoadNetwork& network,
         const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         std::string odr_id) {
        std::vector<roadmaker::Waypoint> points;
        points.reserve(waypoints.size());
        for (const auto& [x, y] : waypoints) {
          points.push_back(roadmaker::Waypoint{.x = x, .y = y});
        }
        return unwrap(roadmaker::author_clothoid_road(
            network, points, profile, std::move(name), std::move(odr_id)));
      },
      "network"_a,
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "odr_id"_a = "",
      "Fits a G1 clothoid path through (x, y) waypoints and inserts a road. "
      "Returns its RoadId. Raises ValueError on invalid input.");

  // --- editing (undo/redo parity with the editor) ------------------------------

  nb::module_ edit =
      m.def_submodule("edit",
                      "Undoable edit commands (kernel command layer). Create a command with "
                      "a factory, then push it onto an EditStack — pushing applies it.");

  nb::class_<roadmaker::edit::DirtySet>(edit, "DirtySet")
      .def_ro("roads", &roadmaker::edit::DirtySet::roads)
      .def_ro("junctions", &roadmaker::edit::DirtySet::junctions)
      .def_ro("topology", &roadmaker::edit::DirtySet::topology);

  nb::class_<roadmaker::edit::Command>(edit, "Command")
      .def_prop_ro(
          "name",
          [](const roadmaker::edit::Command& command) { return std::string(command.name()); })
      .def("__repr__", [](const roadmaker::edit::Command& command) {
        return "Command('" + std::string(command.name()) + "')";
      });

  nb::class_<roadmaker::edit::EditStack>(edit, "EditStack")
      .def(nb::init<>())
      .def(
          "push",
          [](roadmaker::edit::EditStack& stack,
             roadmaker::RoadNetwork& network,
             std::unique_ptr<roadmaker::edit::Command> command) {
            unwrap(stack.push(network, std::move(command)));
          },
          "network"_a,
          "command"_a,
          "Applies the command and records it. Raises ValueError when the "
          "apply fails (the network is left unchanged).")
      .def(
          "undo",
          [](roadmaker::edit::EditStack& stack, roadmaker::RoadNetwork& network) {
            unwrap(stack.undo(network));
          },
          "network"_a)
      .def(
          "redo",
          [](roadmaker::edit::EditStack& stack, roadmaker::RoadNetwork& network) {
            unwrap(stack.redo(network));
          },
          "network"_a)
      .def_prop_ro("can_undo", &roadmaker::edit::EditStack::can_undo)
      .def_prop_ro("can_redo", &roadmaker::edit::EditStack::can_redo)
      .def_prop_ro("size", &roadmaker::edit::EditStack::size)
      .def("clear", &roadmaker::edit::EditStack::clear)
      .def_prop_rw("depth_limit",
                   &roadmaker::edit::EditStack::depth_limit,
                   &roadmaker::edit::EditStack::set_depth_limit);

  edit.def(
      "move_waypoint",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId road,
         std::size_t index,
         std::pair<double, double> to) {
        return roadmaker::edit::move_waypoint(
            network, road, index, roadmaker::Waypoint{.x = to.first, .y = to.second});
      },
      "network"_a,
      "road"_a,
      "index"_a,
      "to"_a);
  edit.def(
      "insert_waypoint",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId road,
         std::size_t index,
         std::pair<double, double> at) {
        return roadmaker::edit::insert_waypoint(
            network, road, index, roadmaker::Waypoint{.x = at.first, .y = at.second});
      },
      "network"_a,
      "road"_a,
      "index"_a,
      "at"_a);
  edit.def("delete_waypoint", &roadmaker::edit::delete_waypoint, "network"_a, "road"_a, "index"_a);
  edit.def(
      "create_road",
      [](const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name) {
        return roadmaker::edit::create_road(to_waypoints(waypoints), profile, std::move(name));
      },
      "waypoints"_a,
      "profile"_a,
      "name"_a = "");
  edit.def("split_road", &roadmaker::edit::split_road, "network"_a, "road"_a, "s"_a);
  edit.def("delete_road", &roadmaker::edit::delete_road, "network"_a, "road"_a);
  edit.def(
      "create_junction",
      [](const roadmaker::RoadNetwork& network, const std::vector<roadmaker::RoadEnd>& ends) {
        return roadmaker::edit::create_junction(network, ends);
      },
      "network"_a,
      "ends"_a);
  edit.def("delete_junction", &roadmaker::edit::delete_junction, "network"_a, "junction"_a);
  edit.def("add_lane",
           &roadmaker::edit::add_lane,
           "network"_a,
           "section"_a,
           "side"_a,
           "type"_a,
           "side: +1 = left of the reference line, -1 = right.");
  edit.def("remove_lane", &roadmaker::edit::remove_lane, "network"_a, "lane"_a);
  edit.def("set_lane_type", &roadmaker::edit::set_lane_type, "network"_a, "lane"_a, "type"_a);
  edit.def("set_lane_width", &roadmaker::edit::set_lane_width, "network"_a, "lane"_a, "width_m"_a);
  edit.def("set_road_mark", &roadmaker::edit::set_road_mark, "network"_a, "lane"_a, "mark"_a);
  edit.def("set_node_elevation",
           &roadmaker::edit::set_node_elevation,
           "network"_a,
           "road"_a,
           "waypoint_index"_a,
           "z"_a);
  edit.def(
      "rename_road",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, std::string name) {
        return roadmaker::edit::rename_road(network, road, std::move(name));
      },
      "network"_a,
      "road"_a,
      "name"_a);

  // --- meshing / export ------------------------------------------------------------

  nb::class_<roadmaker::MeshOptions>(m, "MeshOptions")
      .def(nb::init<>())
      .def_rw("markings", &roadmaker::MeshOptions::markings)
      .def_rw("junction_floors", &roadmaker::MeshOptions::junction_floors)
      .def_prop_rw(
          "chord_tolerance",
          [](const roadmaker::MeshOptions& o) { return o.sampling.chord_tolerance; },
          [](roadmaker::MeshOptions& o, double value) { o.sampling.chord_tolerance = value; });

  nb::class_<roadmaker::NetworkMesh>(m, "NetworkMesh")
      .def_prop_ro("road_count",
                   [](const roadmaker::NetworkMesh& mesh) { return mesh.roads.size(); })
      .def_prop_ro("junction_floor_count",
                   [](const roadmaker::NetworkMesh& mesh) { return mesh.junction_floors.size(); })
      .def_prop_ro("vertex_count",
                   [](const roadmaker::NetworkMesh& mesh) {
                     std::size_t count = 0;
                     for (const auto& road : mesh.roads) {
                       count += road.positions.size() / 3;
                       for (const auto& marking : road.markings) {
                         count += marking.positions.size() / 3;
                       }
                     }
                     for (const auto& floor : mesh.junction_floors) {
                       count += floor.mesh.positions.size() / 3;
                     }
                     return count;
                   })
      .def("__repr__", [](const roadmaker::NetworkMesh& mesh) {
        return "NetworkMesh(roads=" + std::to_string(mesh.roads.size()) +
               ", junction_floors=" + std::to_string(mesh.junction_floors.size()) + ")";
      });

  m.def(
      "build_network_mesh",
      [](const roadmaker::RoadNetwork& network, const roadmaker::MeshOptions& options) {
        return roadmaker::build_network_mesh(network, options);
      },
      "network"_a,
      "options"_a = roadmaker::MeshOptions{},
      "Tessellates the network (kernel frame: Z-up, meters).");

  m.def(
      "export_glb",
      [](const roadmaker::NetworkMesh& mesh, const std::filesystem::path& path) {
        unwrap(roadmaker::export_glb(mesh, path));
      },
      "mesh"_a,
      "path"_a,
      "Writes binary glTF 2.0 (Y-up, meters).");
}
