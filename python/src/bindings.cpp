// nanobind bindings for the RoadMaker kernel. The API mirrors the C++ data
// model pythonically: properties, natural containers, exceptions instead of
// Expected, __repr__ everywhere. No C++ exception ever crosses this
// boundary unhandled — rm::Error is translated to Python exceptions.

#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/edit/snap.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/geometry/profile_fit.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/io/usd_exporter.hpp"
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
#include <nanobind/stl/variant.h>
#include <nanobind/stl/vector.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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

  nb::enum_<roadmaker::RoadMarkColor>(m, "RoadMarkColor")
      .value("STANDARD", roadmaker::RoadMarkColor::Standard)
      .value("WHITE", roadmaker::RoadMarkColor::White)
      .value("YELLOW", roadmaker::RoadMarkColor::Yellow)
      .value("RED", roadmaker::RoadMarkColor::Red)
      .value("BLUE", roadmaker::RoadMarkColor::Blue)
      .value("GREEN", roadmaker::RoadMarkColor::Green)
      .value("ORANGE", roadmaker::RoadMarkColor::Orange)
      .value("OTHER", roadmaker::RoadMarkColor::Other);

  nb::enum_<roadmaker::ContactPoint>(m, "ContactPoint")
      .value("START", roadmaker::ContactPoint::Start)
      .value("END", roadmaker::ContactPoint::End);

  nb::enum_<roadmaker::ObjectType>(m, "ObjectType")
      .value("NONE", roadmaker::ObjectType::None)
      .value("CROSSWALK", roadmaker::ObjectType::Crosswalk)
      .value("TREE", roadmaker::ObjectType::Tree)
      .value("VEGETATION", roadmaker::ObjectType::Vegetation)
      .value("POLE", roadmaker::ObjectType::Pole)
      .value("BARRIER", roadmaker::ObjectType::Barrier)
      .value("BUILDING", roadmaker::ObjectType::Building)
      .value("OBSTACLE", roadmaker::ObjectType::Obstacle)
      .value("OTHER", roadmaker::ObjectType::Other);

  nb::enum_<roadmaker::ObjectOrientation>(m, "ObjectOrientation")
      .value("PLUS", roadmaker::ObjectOrientation::Plus)
      .value("MINUS", roadmaker::ObjectOrientation::Minus)
      .value("NONE", roadmaker::ObjectOrientation::None);

  nb::enum_<roadmaker::Severity>(m, "Severity")
      .value("INFO", roadmaker::Severity::Info)
      .value("WARNING", roadmaker::Severity::Warning)
      .value("ERROR", roadmaker::Severity::Error);

  // --- ids -------------------------------------------------------------------

  bind_id<roadmaker::RoadId>(m, "RoadId");
  bind_id<roadmaker::LaneSectionId>(m, "LaneSectionId");
  bind_id<roadmaker::LaneId>(m, "LaneId");
  bind_id<roadmaker::JunctionId>(m, "JunctionId");
  bind_id<roadmaker::ObjectId>(m, "ObjectId");
  bind_id<roadmaker::SignalId>(m, "SignalId");

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

  nb::class_<roadmaker::RoadMarkLine>(m, "RoadMarkLine")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::RoadMarkLine* self,
             double width,
             double length,
             double space,
             double t_offset,
             double s_offset) {
            new (self) roadmaker::RoadMarkLine{.width = width,
                                               .length = length,
                                               .space = space,
                                               .t_offset = t_offset,
                                               .s_offset = s_offset};
          },
          "width"_a = 0.12,
          "length"_a = 0.0,
          "space"_a = 0.0,
          "t_offset"_a = 0.0,
          "s_offset"_a = 0.0)
      .def_rw("width", &roadmaker::RoadMarkLine::width)
      .def_rw("length", &roadmaker::RoadMarkLine::length)
      .def_rw("space", &roadmaker::RoadMarkLine::space)
      .def_rw("t_offset", &roadmaker::RoadMarkLine::t_offset)
      .def_rw("s_offset", &roadmaker::RoadMarkLine::s_offset);

  nb::class_<roadmaker::RoadMark>(m, "RoadMark")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::RoadMark* self,
             double s_offset,
             roadmaker::RoadMarkType type,
             double width,
             roadmaker::RoadMarkColor color) {
            new (self) roadmaker::RoadMark{
                .s_offset = s_offset, .type = type, .width = width, .color = color};
          },
          "s_offset"_a = 0.0,
          "type"_a = roadmaker::RoadMarkType::None,
          "width"_a = 0.12,
          "color"_a = roadmaker::RoadMarkColor::Standard)
      .def_rw("s_offset", &roadmaker::RoadMark::s_offset)
      .def_rw("type", &roadmaker::RoadMark::type)
      .def_rw("width", &roadmaker::RoadMark::width)
      .def_rw("color", &roadmaker::RoadMark::color)
      .def_rw("lines",
              &roadmaker::RoadMark::lines,
              "Explicit multi-line stripes (<type>/<line>); empty for a simple "
              "single-stripe mark.");

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
      .def_rw("road_marks",
              &roadmaker::Lane::road_marks,
              "Marks on this lane's OUTER boundary, ascending s_offset "
              "(rm.edit.set_road_mark edits the first record only).")
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

  nb::class_<roadmaker::RoadLink>(m, "RoadLink")
      .def_ro(
          "target", &roadmaker::RoadLink::target, "The linked element: a RoadId or a JunctionId.")
      .def_ro("contact", &roadmaker::RoadLink::contact)
      .def("__repr__", [](const roadmaker::RoadLink& link) {
        return std::holds_alternative<roadmaker::RoadId>(link.target)
                   ? std::string("RoadLink(road)")
                   : std::string("RoadLink(junction)");
      });

  nb::class_<roadmaker::Road>(m, "Road")
      .def_ro("name", &roadmaker::Road::name)
      .def_ro("odr_id", &roadmaker::Road::odr_id)
      .def_ro("length", &roadmaker::Road::length)
      .def_ro("junction", &roadmaker::Road::junction)
      .def_ro("predecessor",
              &roadmaker::Road::predecessor,
              "Road-level predecessor link; None when detached.")
      .def_ro("successor",
              &roadmaker::Road::successor,
              "Road-level successor link; None when detached.")
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
      .def_ro("arms", &roadmaker::Junction::arms)
      .def("__repr__", [](const roadmaker::Junction& junction) {
        return "Junction(odr_id='" + junction.odr_id +
               "', connections=" + std::to_string(junction.connections.size()) + ")";
      });

  nb::class_<roadmaker::RawXml>(m, "RawXml")
      .def_ro("attributes",
              &roadmaker::RawXml::attributes,
              "Unknown attributes preserved verbatim, as (name, value) pairs.")
      .def_ro("children",
              &roadmaker::RawXml::children,
              "Unmodeled child elements preserved as XML fragments.")
      .def("__bool__", [](const roadmaker::RawXml& raw) { return !raw.empty(); });

  nb::class_<roadmaker::OutlineCorner>(m, "OutlineCorner")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::OutlineCorner* self, double a, double b, double height, double dz_or_z) {
            new (self)
                roadmaker::OutlineCorner{.a = a, .b = b, .height = height, .dz_or_z = dz_or_z};
          },
          "a"_a,
          "b"_a,
          "height"_a = 0.0,
          "dz_or_z"_a = 0.0)
      .def_rw("a", &roadmaker::OutlineCorner::a, "s (cornerRoad) or u (cornerLocal) [m].")
      .def_rw("b", &roadmaker::OutlineCorner::b, "t (cornerRoad) or v (cornerLocal) [m].")
      .def_rw("height", &roadmaker::OutlineCorner::height)
      .def_rw("dz_or_z", &roadmaker::OutlineCorner::dz_or_z)
      .def_rw("id", &roadmaker::OutlineCorner::id);

  nb::class_<roadmaker::ObjectOutline>(m, "ObjectOutline")
      .def(nb::init<>())
      .def_rw("road_coords",
              &roadmaker::ObjectOutline::road_coords,
              "True for <cornerRoad> corners, False for <cornerLocal>.")
      .def_rw("closed", &roadmaker::ObjectOutline::closed)
      .def_rw("outer", &roadmaker::ObjectOutline::outer)
      .def_rw("id", &roadmaker::ObjectOutline::id)
      .def_rw("fill_type", &roadmaker::ObjectOutline::fill_type)
      .def_rw("lane_type", &roadmaker::ObjectOutline::lane_type)
      .def_rw("corners", &roadmaker::ObjectOutline::corners)
      .def_ro("raw",
              &roadmaker::ObjectOutline::raw,
              "Verbatim <outline> XML when the outline is preserved, not modeled.");

  nb::class_<roadmaker::ObjectRepeat>(m, "ObjectRepeat")
      .def(nb::init<>())
      .def_rw("s", &roadmaker::ObjectRepeat::s)
      .def_rw("length", &roadmaker::ObjectRepeat::length)
      .def_rw("distance",
              &roadmaker::ObjectRepeat::distance,
              "Spacing between instances [m]; 0 extrudes one continuous object.")
      .def_rw("t_start", &roadmaker::ObjectRepeat::t_start)
      .def_rw("t_end", &roadmaker::ObjectRepeat::t_end)
      .def_rw("z_offset_start", &roadmaker::ObjectRepeat::z_offset_start)
      .def_rw("z_offset_end", &roadmaker::ObjectRepeat::z_offset_end)
      .def_rw("width_start", &roadmaker::ObjectRepeat::width_start)
      .def_rw("width_end", &roadmaker::ObjectRepeat::width_end)
      .def_rw("height_start", &roadmaker::ObjectRepeat::height_start)
      .def_rw("height_end", &roadmaker::ObjectRepeat::height_end)
      .def_rw("length_start", &roadmaker::ObjectRepeat::length_start)
      .def_rw("length_end", &roadmaker::ObjectRepeat::length_end)
      .def_rw("radius_start", &roadmaker::ObjectRepeat::radius_start)
      .def_rw("radius_end", &roadmaker::ObjectRepeat::radius_end)
      .def_rw("b_t", &roadmaker::ObjectRepeat::b_t)
      .def_rw("c_t", &roadmaker::ObjectRepeat::c_t)
      .def_rw("d_t", &roadmaker::ObjectRepeat::d_t)
      .def_rw("detach_from_reference_line", &roadmaker::ObjectRepeat::detach_from_reference_line);

  nb::class_<roadmaker::Object>(m, "Object")
      .def(nb::init<>())
      .def_ro("road", &roadmaker::Object::road, "Owning road (back-reference).")
      .def_rw("odr_id", &roadmaker::Object::odr_id)
      .def_rw("name", &roadmaker::Object::name)
      .def_rw("type", &roadmaker::Object::type)
      .def_rw("type_str",
              &roadmaker::Object::type_str,
              "@type exactly as spelled in the file; empty for authored objects "
              "(the writer then derives it from `type`).")
      .def_rw("subtype", &roadmaker::Object::subtype)
      .def_rw("s", &roadmaker::Object::s)
      .def_rw("t", &roadmaker::Object::t)
      .def_rw("z_offset", &roadmaker::Object::z_offset)
      .def_rw("hdg", &roadmaker::Object::hdg)
      .def_rw("pitch", &roadmaker::Object::pitch)
      .def_rw("roll", &roadmaker::Object::roll)
      .def_rw("orientation", &roadmaker::Object::orientation)
      .def_rw("perp_to_road", &roadmaker::Object::perp_to_road)
      .def_rw("length", &roadmaker::Object::length)
      .def_rw("width", &roadmaker::Object::width)
      .def_rw("radius", &roadmaker::Object::radius)
      .def_rw("height", &roadmaker::Object::height)
      .def_rw("valid_length", &roadmaker::Object::valid_length)
      .def_rw("dynamic", &roadmaker::Object::dynamic)
      .def_rw("temporary", &roadmaker::Object::temporary)
      .def_rw("invalidated", &roadmaker::Object::invalidated)
      .def_rw("outlines", &roadmaker::Object::outlines)
      .def_rw("repeats", &roadmaker::Object::repeats)
      .def_ro("preserved", &roadmaker::Object::preserved)
      .def("__repr__", [](const roadmaker::Object& object) {
        return "Object(odr_id='" + object.odr_id + "', s=" + std::to_string(object.s) +
               ", t=" + std::to_string(object.t) + ")";
      });

  nb::class_<roadmaker::Signal>(m, "Signal")
      .def(nb::init<>())
      .def_ro("road", &roadmaker::Signal::road, "Owning road (back-reference).")
      .def_rw("odr_id", &roadmaker::Signal::odr_id)
      .def_rw("name", &roadmaker::Signal::name)
      .def_rw("s", &roadmaker::Signal::s)
      .def_rw("t", &roadmaker::Signal::t)
      .def_rw("z_offset", &roadmaker::Signal::z_offset)
      .def_rw("dynamic",
              &roadmaker::Signal::dynamic,
              "@dynamic yes/no: dynamic (traffic light) vs. static (sign).")
      .def_rw("orientation", &roadmaker::Signal::orientation)
      .def_rw("h_offset", &roadmaker::Signal::h_offset)
      .def_rw("pitch", &roadmaker::Signal::pitch)
      .def_rw("roll", &roadmaker::Signal::roll)
      .def_rw("type", &roadmaker::Signal::type)
      .def_rw("subtype", &roadmaker::Signal::subtype)
      .def_rw(
          "country", &roadmaker::Signal::country, "e_countryCode; 'OpenDRIVE' for catalog signals.")
      .def_rw("country_revision", &roadmaker::Signal::country_revision)
      .def_rw("value", &roadmaker::Signal::value, "Signal value; @unit is required when set.")
      .def_rw("unit", &roadmaker::Signal::unit)
      .def_rw("text", &roadmaker::Signal::text)
      .def_rw("height", &roadmaker::Signal::height)
      .def_rw("width", &roadmaker::Signal::width)
      .def_rw("length", &roadmaker::Signal::length, "1.8.0; emitted only when target >=1.8.0.")
      .def_rw("temporary", &roadmaker::Signal::temporary)
      .def_rw("invalidated", &roadmaker::Signal::invalidated)
      .def_ro("preserved", &roadmaker::Signal::preserved)
      .def("__repr__", [](const roadmaker::Signal& signal) {
        return "Signal(odr_id='" + signal.odr_id + "', type='" + signal.type +
               "', s=" + std::to_string(signal.s) + ", t=" + std::to_string(signal.t) + ")";
      });

  // --- the network -----------------------------------------------------------

  nb::class_<roadmaker::RoadNetwork>(m, "RoadNetwork")
      .def(nb::init<>())
      .def("create_road", &roadmaker::RoadNetwork::create_road, "name"_a, "odr_id"_a)
      .def("create_junction", &roadmaker::RoadNetwork::create_junction, "odr_id"_a, "name"_a)
      .def("add_lane_section", &roadmaker::RoadNetwork::add_lane_section, "road"_a, "s0"_a)
      .def("add_lane", &roadmaker::RoadNetwork::add_lane, "section"_a, "odr_lane_id"_a, "type"_a)
      .def("add_object",
           &roadmaker::RoadNetwork::add_object,
           "road"_a,
           "value"_a,
           "Adds an OpenDRIVE <object> to `road`; returns an invalid id if "
           "`road` is stale.")
      .def("add_signal",
           &roadmaker::RoadNetwork::add_signal,
           "road"_a,
           "value"_a,
           "Adds an OpenDRIVE <signal> to `road`; returns an invalid id if "
           "`road` is stale.")
      .def("erase_road", &roadmaker::RoadNetwork::erase_road, "road"_a)
      .def("erase_junction", &roadmaker::RoadNetwork::erase_junction, "junction"_a)
      .def("erase_object", &roadmaker::RoadNetwork::erase_object, "object"_a)
      .def("erase_signal", &roadmaker::RoadNetwork::erase_signal, "signal"_a)
      .def("object",
           nb::overload_cast<roadmaker::ObjectId>(&roadmaker::RoadNetwork::object),
           "id"_a,
           nb::rv_policy::reference_internal,
           "Object for id, or None if the id is stale. The reference is valid "
           "only until the network is mutated.")
      .def(
          "objects_of",
          [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road) {
            return roadmaker::objects_of(network, road);
          },
          "road"_a,
          "ObjectIds the road owns, in creation order.")
      .def("signal",
           nb::overload_cast<roadmaker::SignalId>(&roadmaker::RoadNetwork::signal),
           "id"_a,
           nb::rv_policy::reference_internal,
           "Signal for id, or None if the id is stale. The reference is valid "
           "only until the network is mutated.")
      .def(
          "signals_of",
          [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road) {
            return roadmaker::signals_of(network, road);
          },
          "road"_a,
          "SignalIds the road owns, in creation order.")
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
      .def_prop_ro("object_ids",
                   [](const roadmaker::RoadNetwork& network) {
                     std::vector<roadmaker::ObjectId> ids;
                     network.for_each_object([&](roadmaker::ObjectId id, const roadmaker::Object&) {
                       ids.push_back(id);
                     });
                     return ids;
                   })
      .def_prop_ro("signal_ids",
                   [](const roadmaker::RoadNetwork& network) {
                     std::vector<roadmaker::SignalId> ids;
                     network.for_each_signal([&](roadmaker::SignalId id, const roadmaker::Signal&) {
                       ids.push_back(id);
                     });
                     return ids;
                   })
      .def_prop_ro("road_count", &roadmaker::RoadNetwork::road_count)
      .def_prop_ro("lane_count", &roadmaker::RoadNetwork::lane_count)
      .def_prop_ro("junction_count", &roadmaker::RoadNetwork::junction_count)
      .def_prop_ro("object_count", &roadmaker::RoadNetwork::object_count)
      .def_prop_ro("signal_count", &roadmaker::RoadNetwork::signal_count)
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

  nb::enum_<roadmaker::XodrVersion>(m, "XodrVersion")
      .value("V1_8_1", roadmaker::XodrVersion::v1_8_1)
      .value("V1_9_0", roadmaker::XodrVersion::v1_9_0);

  m.def(
      "write_xodr",
      [](const roadmaker::RoadNetwork& network,
         std::string_view name,
         roadmaker::XodrVersion target_version) {
        return unwrap(roadmaker::write_xodr(network, name, {.target_version = target_version}));
      },
      "network"_a,
      "name"_a = "roadmaker",
      "target_version"_a = roadmaker::XodrVersion::v1_8_1,
      "Serializes the network as OpenDRIVE XML targeting `target_version` "
      "(1.8.1 default; validates first).");

  m.def(
      "save_xodr",
      [](const roadmaker::RoadNetwork& network,
         const std::filesystem::path& path,
         std::string_view name,
         roadmaker::XodrVersion target_version) {
        unwrap(roadmaker::save_xodr(network, path, name, {.target_version = target_version}));
      },
      "network"_a,
      "path"_a,
      "name"_a = "roadmaker",
      "target_version"_a = roadmaker::XodrVersion::v1_8_1);

  m.def(
      "validate_network",
      [](const roadmaker::RoadNetwork& network, roadmaker::XodrVersion target_version) {
        return roadmaker::validate_network(network, {.target_version = target_version});
      },
      "network"_a,
      "target_version"_a = roadmaker::XodrVersion::v1_8_1,
      "Checker-rule validation against the target version's catalog. Returns "
      "a list of Diagnostic citing normative rule UIDs; rules present in only "
      "one version's catalog are cited only for that target. Findings never "
      "block write_xodr/save_xodr.");

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
      .def_static("two_lane_rural",
                  &roadmaker::LaneProfile::two_lane_rural,
                  "One driving lane each way, right-hand shoulder.")
      .def_static("urban_sidewalk",
                  &roadmaker::LaneProfile::urban_sidewalk,
                  "One driving lane each way, sidewalks both sides.")
      .def_static("highway",
                  &roadmaker::LaneProfile::highway,
                  "Two driving lanes each way, wide shoulders, no center mark.")
      .def_static("two_lane_default",
                  &roadmaker::LaneProfile::two_lane_default,
                  "Historical alias of two_lane_rural().");

  m.def(
      "author_clothoid_road",
      [](roadmaker::RoadNetwork& network,
         const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         std::string odr_id,
         std::optional<double> start_heading,
         std::optional<double> end_heading) {
        std::vector<roadmaker::Waypoint> points;
        points.reserve(waypoints.size());
        for (const auto& [x, y] : waypoints) {
          points.push_back(roadmaker::Waypoint{.x = x, .y = y});
        }
        return unwrap(
            roadmaker::author_clothoid_road(network,
                                            points,
                                            profile,
                                            std::move(name),
                                            std::move(odr_id),
                                            {.start = start_heading, .end = end_heading}));
      },
      "network"_a,
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "odr_id"_a = "",
      "start_heading"_a = nb::none(),
      "end_heading"_a = nb::none(),
      "Fits a G1 clothoid path through (x, y) waypoints and inserts a road. "
      "A start/end heading [rad] locks the fit there (tangent-snap "
      "chaining). Returns its RoadId. Raises ValueError on invalid input.");

  m.def(
      "fit_elevation_profile",
      [](const std::vector<double>& s, const std::vector<double>& z) {
        return roadmaker::fit_elevation_profile(s, z);
      },
      "s"_a,
      "z"_a,
      "Fits a smooth C1 cubic elevation profile z(s) through the (s, z) node "
      "pairs (cubic Hermite, finite-difference tangents; not overshoot-limited). "
      "Returns the list of Poly3 records, ascending in s, that "
      "edit.set_node_elevation writes. s must be strictly ascending and the same "
      "length as z; degenerate inputs return an empty list.");

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
      "junction_crosswalks",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::edit::junction_crosswalks(network, junction);
      },
      "network"_a,
      "junction"_a,
      "One zebra crosswalk Object per arm of the junction, spanning its driving "
      "lanes just inside it. Returns a list of (RoadId, Object); add each with "
      "edit.add_object (the editor groups them into one undo step).");
  edit.def(
      "junction_stop_lines",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::edit::junction_stop_lines(network, junction);
      },
      "network"_a,
      "junction"_a,
      "One solid stop line Object across each arm's approach lanes, just behind "
      "the crosswalk. Returns a list of (RoadId, Object); add each with "
      "edit.add_object.");
  edit.def(
      "junction_lane_arrows",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::edit::junction_lane_arrows(network, junction);
      },
      "network"_a,
      "junction"_a,
      "One straight lane-arrow Object on each approach lane of every arm, "
      "pointing into the junction. Returns a list of (RoadId, Object); add each "
      "with edit.add_object.");
  edit.def("effective_waypoints",
           &roadmaker::edit::effective_waypoints,
           "road"_a,
           "The road's editing nodes: recorded authoring waypoints, or the set "
           "derived from geometry-record endpoints for foreign roads.");
  edit.def(
      "waypoint_stations",
      [](const roadmaker::Road& road) { return unwrap(roadmaker::edit::waypoint_stations(road)); },
      "road"_a,
      "Stations [m] of effective_waypoints(road) along the reference line.");
  edit.def(
      "create_road",
      [](const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         std::optional<double> start_heading,
         std::optional<double> end_heading) {
        return roadmaker::edit::create_road(to_waypoints(waypoints),
                                            profile,
                                            std::move(name),
                                            {.start = start_heading, .end = end_heading});
      },
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "start_heading"_a = nb::none(),
      "end_heading"_a = nb::none(),
      "Authors a clothoid road (auto id; empty name auto-names \"Road <id>\"). "
      "A start/end heading [rad] locks the fit there for G1 chaining.");
  edit.def(
      "create_linked_road",
      [](const roadmaker::RoadNetwork& network,
         const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         const roadmaker::RoadEnd& link_start,
         std::optional<double> start_heading,
         std::optional<double> end_heading) {
        return roadmaker::edit::create_linked_road(network,
                                                   to_waypoints(waypoints),
                                                   profile,
                                                   std::move(name),
                                                   link_start,
                                                   {.start = start_heading, .end = end_heading});
      },
      "network"_a,
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "link_start"_a,
      "start_heading"_a = nb::none(),
      "end_heading"_a = nb::none(),
      "Authors a clothoid road AND welds its start to the free road end "
      "`link_start` in one undoable command (the weld is skipped when that end "
      "can't link, so creation never fails on it).");
  edit.def("split_road", &roadmaker::edit::split_road, "network"_a, "road"_a, "s"_a);
  edit.def(
      "check_mergeable",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId a, roadmaker::RoadId b) {
        unwrap(roadmaker::edit::check_mergeable(network, a, b));
      },
      "network"_a,
      "a"_a,
      "b"_a,
      "Raises ValueError with the reason if a's END can't merge into b's START, "
      "else returns None.");
  edit.def("merge_roads",
           &roadmaker::edit::merge_roads,
           "network"_a,
           "a"_a,
           "b"_a,
           "Merges a's END into b's START into one road keeping a's id (b is erased).");
  edit.def("delete_road", &roadmaker::edit::delete_road, "network"_a, "road"_a);
  edit.def("translate_road",
           &roadmaker::edit::translate_road,
           "network"_a,
           "road"_a,
           "dx"_a,
           "dy"_a,
           "Moves a whole road by (dx, dy) [m] in plan view; breaks links leaving "
           "the road and refuses junction roads.");
  edit.def(
      "translate_roads",
      [](const roadmaker::RoadNetwork& network,
         std::vector<roadmaker::RoadId> roads,
         double dx,
         double dy) { return roadmaker::edit::translate_roads(network, roads, dx, dy); },
      "network"_a,
      "roads"_a,
      "dx"_a,
      "dy"_a,
      "Moves N roads together by (dx, dy) [m] as ONE command; links between the "
      "moved roads survive, links leaving the set break on both sides.");
  edit.def("insert_node_at",
           &roadmaker::edit::insert_node_at,
           "network"_a,
           "road"_a,
           "s"_a,
           "Inserts a bend node at station s, pinning headings from the current "
           "curve so the shape is preserved and only the covering record splits.");
  nb::class_<roadmaker::edit::JunctionGenOptions>(edit, "JunctionGenOptions")
      .def(nb::init<>())
      .def_rw("max_end_distance_m", &roadmaker::edit::JunctionGenOptions::max_end_distance_m)
      .def_rw("max_loop_factor", &roadmaker::edit::JunctionGenOptions::max_loop_factor)
      .def_rw("min_turn_radius_m", &roadmaker::edit::JunctionGenOptions::min_turn_radius_m);

  nb::class_<roadmaker::edit::JunctionPreview>(edit, "JunctionPreview")
      .def_ro("connection_count", &roadmaker::edit::JunctionPreview::connection_count)
      .def_ro("dropped_turns", &roadmaker::edit::JunctionPreview::dropped_turns);

  edit.def(
      "preview_junction",
      [](const roadmaker::RoadNetwork& network,
         const std::vector<roadmaker::RoadEnd>& ends,
         const roadmaker::edit::JunctionGenOptions& options) {
        return unwrap(roadmaker::edit::preview_junction(network, ends, options));
      },
      "network"_a,
      "ends"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Non-mutating summary of what create_junction would generate: connection "
      "count and any dropped turns.");
  edit.def(
      "create_junction",
      [](const roadmaker::RoadNetwork& network,
         const std::vector<roadmaker::RoadEnd>& ends,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::create_junction(network, ends, options);
      },
      "network"_a,
      "ends"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Generates a common junction: links each arm and builds one connecting "
      "road per permitted (incoming lane, outgoing lane) turn.");

  // --- connection engine (gate-extension WS-2) ------------------------------
  nb::class_<roadmaker::edit::CloseGapOptions>(edit, "CloseGapOptions")
      .def(nb::init<>())
      .def_rw("max_gap_m", &roadmaker::edit::CloseGapOptions::max_gap_m)
      .def_rw("coincident_gap_m", &roadmaker::edit::CloseGapOptions::coincident_gap_m);
  edit.def(
      "junction_at_end",
      [](const roadmaker::RoadNetwork& network, const roadmaker::RoadEnd& end) {
        return roadmaker::edit::junction_at_end(network, end);
      },
      "network"_a,
      "end"_a,
      "The junction that already owns this road end as an arm, or None.");
  edit.def(
      "matching_junction",
      [](const roadmaker::RoadNetwork& network, const std::vector<roadmaker::RoadEnd>& ends) {
        return roadmaker::edit::matching_junction(network, ends);
      },
      "network"_a,
      "ends"_a,
      "The junction whose recorded arm set is exactly these ends (order-free), "
      "or None — so a repeat selection regenerates in place, never duplicates.");
  edit.def(
      "check_linkable",
      [](const roadmaker::RoadNetwork& network,
         const roadmaker::RoadEnd& a,
         const roadmaker::RoadEnd& b,
         const roadmaker::edit::CloseGapOptions& options) {
        unwrap(roadmaker::edit::check_linkable(network, a, b, options));
      },
      "network"_a,
      "a"_a,
      "b"_a,
      "options"_a = roadmaker::edit::CloseGapOptions{},
      "Raises ValueError with the reason if the two free ends can't be linked, "
      "else returns None.");
  edit.def(
      "close_gap",
      [](const roadmaker::RoadNetwork& network,
         const roadmaker::RoadEnd& a,
         const roadmaker::RoadEnd& b,
         const roadmaker::edit::CloseGapOptions& options) {
        return roadmaker::edit::close_gap(network, a, b, options);
      },
      "network"_a,
      "a"_a,
      "b"_a,
      "options"_a = roadmaker::edit::CloseGapOptions{},
      "Closes the gap between two free road ends: a pure link when they nearly "
      "coincide, else a single-lane connector road, in one undoable command.");

  nb::class_<roadmaker::edit::ElevationPoint>(edit, "ElevationPoint")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::edit::ElevationPoint* self,
             double s,
             double z,
             std::optional<double> grade) {
            new (self) roadmaker::edit::ElevationPoint{.s = s, .z = z, .grade = grade};
          },
          "s"_a,
          "z"_a,
          "grade"_a = nb::none())
      .def_rw("s", &roadmaker::edit::ElevationPoint::s)
      .def_rw("z", &roadmaker::edit::ElevationPoint::z)
      .def_rw("grade", &roadmaker::edit::ElevationPoint::grade);

  edit.def(
      "elevation_profile_points",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road) {
        const roadmaker::Road* value = network.road(road);
        if (value == nullptr) {
          throw RmException{roadmaker::Error{.code = roadmaker::ErrorCode::InvalidArgument,
                                             .message = "stale road id"}};
        }
        return roadmaker::edit::elevation_profile_points(*value);
      },
      "network"_a,
      "road"_a,
      "The road's vertical profile as editable nodes (s, z, grade).");

  edit.def(
      "set_elevation_profile",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId road,
         std::vector<roadmaker::edit::ElevationPoint> points) {
        return roadmaker::edit::set_elevation_profile(network, road, std::move(points));
      },
      "network"_a,
      "road"_a,
      "points"_a,
      "Replaces the road's elevation with a C1 cubic through the nodes "
      "(explicit grades honored; all-zero writes no profile).");

  nb::class_<roadmaker::edit::TAttachOptions>(edit, "TAttachOptions")
      .def(nb::init<>())
      .def_rw("gap_m", &roadmaker::edit::TAttachOptions::gap_m)
      .def_rw("generation", &roadmaker::edit::TAttachOptions::generation);

  edit.def(
      "t_attach_gap",
      [](const roadmaker::RoadNetwork& network,
         const roadmaker::RoadEnd& end,
         roadmaker::RoadId target,
         double s,
         const roadmaker::edit::TAttachOptions& options) {
        return roadmaker::edit::t_attach_gap(network, end, target, s, options);
      },
      "network"_a,
      "end"_a,
      "target"_a,
      "s"_a,
      "options"_a = roadmaker::edit::TAttachOptions{},
      "Half-length [m] of the junction area attach_t_junction would remove "
      "around s (the editor preview's [s-gap, s+gap] highlight).");
  edit.def(
      "attach_t_junction",
      [](const roadmaker::RoadNetwork& network,
         const roadmaker::RoadEnd& end,
         roadmaker::RoadId target,
         double s,
         const roadmaker::edit::TAttachOptions& options) {
        return roadmaker::edit::attach_t_junction(network, end, target, s, options);
      },
      "network"_a,
      "end"_a,
      "target"_a,
      "s"_a,
      "options"_a = roadmaker::edit::TAttachOptions{},
      "Attaches a road end to the SIDE of another road at station s — the "
      "T-junction workflow: splits the target around s, deletes the middle "
      "stub, and generates a junction from the three ends (all legal turns). "
      "One undoable command.");
  edit.def(
      "regenerate_junction",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::regenerate_junction(network, junction, options);
      },
      "network"_a,
      "junction"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Re-runs the generator from the junction's recorded arms, replacing "
      "connecting-road geometry in place (ids preserved).");
  edit.def("delete_junction", &roadmaker::edit::delete_junction, "network"_a, "junction"_a);

  // --- parametric intersection assemblies (rm.edit.assembly) ---
  auto assembly = edit.def_submodule(
      "assembly",
      "Parametric intersection generators: one undoable command that lays down "
      "the stub roads of a standalone junction and generates its connecting roads.");
  nb::class_<roadmaker::edit::assembly::Pose>(assembly, "Pose")
      .def(
          "__init__",
          [](roadmaker::edit::assembly::Pose* self, double x, double y, double heading) {
            new (self) roadmaker::edit::assembly::Pose{.x = x, .y = y, .heading = heading};
          },
          "x"_a = 0.0,
          "y"_a = 0.0,
          "heading"_a = 0.0)
      .def_rw("x", &roadmaker::edit::assembly::Pose::x)
      .def_rw("y", &roadmaker::edit::assembly::Pose::y)
      .def_rw("heading", &roadmaker::edit::assembly::Pose::heading);
  nb::class_<roadmaker::edit::assembly::IntersectionParams>(assembly, "IntersectionParams")
      .def(nb::init<>())
      .def_rw("arm_length_m", &roadmaker::edit::assembly::IntersectionParams::arm_length_m)
      .def_rw("gap_m", &roadmaker::edit::assembly::IntersectionParams::gap_m)
      .def_rw("profile", &roadmaker::edit::assembly::IntersectionParams::profile)
      .def_rw("generation", &roadmaker::edit::assembly::IntersectionParams::generation);
  assembly.def(
      "t_intersection",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::edit::assembly::Pose pose,
         roadmaker::edit::assembly::IntersectionParams params) {
        return roadmaker::edit::assembly::t_intersection(network, pose, params);
      },
      "network"_a,
      "pose"_a,
      "params"_a = roadmaker::edit::assembly::IntersectionParams{},
      "A 3-way T-intersection at pose (through road along pose.heading + a "
      "perpendicular stem). One undoable command.");
  assembly.def(
      "x_intersection",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::edit::assembly::Pose pose,
         roadmaker::edit::assembly::IntersectionParams params) {
        return roadmaker::edit::assembly::x_intersection(network, pose, params);
      },
      "network"_a,
      "pose"_a,
      "params"_a = roadmaker::edit::assembly::IntersectionParams{},
      "A 4-way X-intersection at pose. One undoable command.");
  assembly.def(
      "tee_onto_road",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId target,
         double s,
         roadmaker::edit::assembly::IntersectionParams params) {
        return roadmaker::edit::assembly::tee_onto_road(network, target, s, params);
      },
      "network"_a,
      "target"_a,
      "s"_a,
      "params"_a = roadmaker::edit::assembly::IntersectionParams{},
      "Tees a perpendicular stem INTO the side of `target` at station s (aligned "
      "to the road tangent, split + junction). One undoable command.");
  assembly.def(
      "cross_onto_road",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId target,
         double s,
         roadmaker::edit::assembly::IntersectionParams params) {
        return roadmaker::edit::assembly::cross_onto_road(network, target, s, params);
      },
      "network"_a,
      "target"_a,
      "s"_a,
      "params"_a = roadmaker::edit::assembly::IntersectionParams{},
      "Crosses a 4-way junction OVER `target` at station s (the two halves are "
      "the collinear through arms + two perpendicular stems). One undoable command.");

  edit.def("add_lane",
           &roadmaker::edit::add_lane,
           "network"_a,
           "section"_a,
           "side"_a,
           "type"_a,
           "side: +1 = left of the reference line, -1 = right.");
  edit.def("remove_lane",
           &roadmaker::edit::remove_lane,
           "network"_a,
           "lane"_a,
           "Outermost lane of its side only; adjacent-section links and junction "
           "lane_links referencing the lane are cleared (undo restores them).");
  edit.def("set_lane_type", &roadmaker::edit::set_lane_type, "network"_a, "lane"_a, "type"_a);
  edit.def("set_lane_width", &roadmaker::edit::set_lane_width, "network"_a, "lane"_a, "width_m"_a);
  edit.def("set_road_mark",
           &roadmaker::edit::set_road_mark,
           "network"_a,
           "lane"_a,
           "mark"_a,
           "Edits the FIRST road-mark record; later records survive untouched.");
  edit.def("set_node_elevation",
           &roadmaker::edit::set_node_elevation,
           "network"_a,
           "road"_a,
           "waypoint_index"_a,
           "z"_a);
  edit.def(
      "add_object",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, roadmaker::Object object) {
        return roadmaker::edit::add_object(network, road, std::move(object));
      },
      "network"_a,
      "road"_a,
      "object"_a,
      "Adds an OpenDRIVE <object> (e.g. a tree prop) to a road; object.road is "
      "set to `road`. Located by road-relative s/t. Undo/redo keep the id.");
  edit.def("delete_object", &roadmaker::edit::delete_object, "network"_a, "object"_a);
  edit.def("move_object",
           &roadmaker::edit::move_object,
           "network"_a,
           "object"_a,
           "s"_a,
           "t"_a,
           "hdg"_a = std::optional<double>{},
           "Re-locates an object to road-relative (s, t); hdg (rad) optional.");
  edit.def(
      "add_signal",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, roadmaker::Signal signal) {
        return roadmaker::edit::add_signal(network, road, std::move(signal));
      },
      "network"_a,
      "road"_a,
      "signal"_a,
      "Adds an OpenDRIVE <signal> (traffic light / sign) to a road; signal.road "
      "is set to `road`. Located by road-relative s/t. Undo/redo keep the id.");
  edit.def("delete_signal", &roadmaker::edit::delete_signal, "network"_a, "signal"_a);
  edit.def("move_signal",
           &roadmaker::edit::move_signal,
           "network"_a,
           "signal"_a,
           "s"_a,
           "t"_a,
           "h_offset"_a = std::optional<double>{},
           "Re-locates a signal to road-relative (s, t); h_offset (rad) optional.");
  edit.def(
      "rename_road",
      [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, std::string name) {
        return roadmaker::edit::rename_road(network, road, std::move(name));
      },
      "network"_a,
      "road"_a,
      "name"_a);

  // --- snapping (docs/m2/01_editing_framework.md §6) ---------------------------

  nb::enum_<roadmaker::edit::SnapKind>(edit, "SnapKind")
      .value("Grid", roadmaker::edit::SnapKind::Grid)
      .value("RoadEndpoint", roadmaker::edit::SnapKind::RoadEndpoint)
      .value("TangentContinuation", roadmaker::edit::SnapKind::TangentContinuation);

  nb::class_<roadmaker::edit::SnapOptions>(edit, "SnapOptions")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::edit::SnapOptions* self,
             double radius,
             std::optional<double> grid,
             bool endpoints,
             bool tangent,
             std::optional<roadmaker::RoadId> exclude_road) {
            new (self) roadmaker::edit::SnapOptions{.radius = radius,
                                                    .grid = grid,
                                                    .endpoints = endpoints,
                                                    .tangent = tangent,
                                                    .exclude_road = exclude_road};
          },
          "radius"_a = 2.0,
          "grid"_a = nb::none(),
          "endpoints"_a = true,
          "tangent"_a = true,
          "exclude_road"_a = nb::none())
      .def_rw("radius", &roadmaker::edit::SnapOptions::radius)
      .def_rw("grid", &roadmaker::edit::SnapOptions::grid)
      .def_rw("endpoints", &roadmaker::edit::SnapOptions::endpoints)
      .def_rw("tangent", &roadmaker::edit::SnapOptions::tangent)
      .def_rw("exclude_road", &roadmaker::edit::SnapOptions::exclude_road);

  nb::class_<roadmaker::edit::SnapResult>(edit, "SnapResult")
      .def_ro("position", &roadmaker::edit::SnapResult::position)
      .def_ro("heading", &roadmaker::edit::SnapResult::heading)
      .def_ro("kind", &roadmaker::edit::SnapResult::kind)
      .def_ro("road", &roadmaker::edit::SnapResult::road)
      .def("__repr__", [](const roadmaker::edit::SnapResult& r) {
        const char* kind = r.kind == roadmaker::edit::SnapKind::Grid ? "Grid"
                           : r.kind == roadmaker::edit::SnapKind::RoadEndpoint
                               ? "RoadEndpoint"
                               : "TangentContinuation";
        return "SnapResult(" + std::string(kind) + ", " + std::to_string(r.position.x) + ", " +
               std::to_string(r.position.y) + ")";
      });

  edit.def(
      "snap_point",
      [](const roadmaker::RoadNetwork& network,
         std::pair<double, double> cursor,
         const roadmaker::edit::SnapOptions& options) {
        return roadmaker::edit::snap_point(
            network, roadmaker::Waypoint{.x = cursor.first, .y = cursor.second}, options);
      },
      "network"_a,
      "cursor"_a,
      "options"_a = roadmaker::edit::SnapOptions{},
      "Best snap candidate for the cursor, or None. Priority: RoadEndpoint > "
      "TangentContinuation > Grid; closest wins within a kind.");

  nb::class_<roadmaker::edit::SideSnap>(edit, "SideSnap")
      .def_ro("road", &roadmaker::edit::SideSnap::road)
      .def_ro("s", &roadmaker::edit::SideSnap::s)
      .def_ro("position", &roadmaker::edit::SideSnap::position)
      .def_ro("distance", &roadmaker::edit::SideSnap::distance);

  edit.def(
      "snap_to_road_side",
      [](const roadmaker::RoadNetwork& network,
         std::pair<double, double> cursor,
         const roadmaker::edit::SnapOptions& options,
         double end_margin) {
        return roadmaker::edit::snap_to_road_side(
            network,
            roadmaker::Waypoint{.x = cursor.first, .y = cursor.second},
            options,
            end_margin);
      },
      "network"_a,
      "cursor"_a,
      "options"_a = roadmaker::edit::SnapOptions{},
      "end_margin"_a = 8.0,
      "Nearest road-body projection within options.radius (the T-attach "
      "anchor): road, station s, position, distance. Skips connecting roads "
      "and stations within end_margin of either road end.");

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
      .def_prop_ro("object_count",
                   [](const roadmaker::NetworkMesh& mesh) { return mesh.objects.size(); })
      .def_prop_ro("signal_count",
                   [](const roadmaker::NetworkMesh& mesh) { return mesh.signal_instances.size(); })
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
               ", junction_floors=" + std::to_string(mesh.junction_floors.size()) +
               ", objects=" + std::to_string(mesh.objects.size()) +
               ", signals=" + std::to_string(mesh.signal_instances.size()) + ")";
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

#ifdef RM_HAVE_USD
  // Only present when the kernel was built with RM_BUILD_USD=ON (wheels ship
  // USD-off in M2). Callers should feature-detect with hasattr(rm,
  // "export_usda").
  m.def(
      "export_usda",
      [](const roadmaker::NetworkMesh& mesh, const std::filesystem::path& path) {
        unwrap(roadmaker::export_usda(mesh, path));
      },
      "mesh"_a,
      "path"_a,
      "Writes OpenUSD ASCII .usda (Y-up, meters). USDA only — .usdc/.usdz crate "
      "output is unsupported in M2. Requires a kernel built with RM_BUILD_USD=ON.");
#endif
}
