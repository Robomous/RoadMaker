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
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/repeat_expansion.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/version.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/variant.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <filesystem>
#include <functional>
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

/// Plan-view point as an (x, y) tuple on the Python side.
std::pair<double, double> to_xy(const std::array<double, 2>& point) {
  return {point[0], point[1]};
}

std::vector<std::pair<double, double>> to_xy(const std::vector<std::array<double, 2>>& points) {
  std::vector<std::pair<double, double>> out;
  out.reserve(points.size());
  for (const auto& point : points) {
    out.push_back(to_xy(point));
  }
  return out;
}

/// Compact RoadEnd text for the corner __repr__s.
std::string road_end_text(const roadmaker::RoadEnd& end) {
  return "RoadEnd(" + std::to_string(end.road.index) + ", " +
         (end.contact == roadmaker::ContactPoint::Start ? "START" : "END") + ")";
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
      .value("BROKEN_BROKEN", roadmaker::RoadMarkType::BrokenBroken)
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

  nb::enum_<roadmaker::LaneDirection>(m, "LaneDirection")
      .value("STANDARD", roadmaker::LaneDirection::Standard)
      .value("REVERSED", roadmaker::LaneDirection::Reversed)
      .value("BOTH", roadmaker::LaneDirection::Both);

  nb::enum_<roadmaker::ContactPoint>(m, "ContactPoint")
      .value("START", roadmaker::ContactPoint::Start)
      .value("END", roadmaker::ContactPoint::End);

  // Layer 1, RoadMaker-only: ASAM OpenDRIVE has no turn-type element (§12.2
  // Table 56 gives <connection> no such attribute), so the type is DERIVED from
  // the arm-face headings and only stored when the author overrides it.
  nb::enum_<roadmaker::TurnType>(m, "TurnType")
      .value("LEFT", roadmaker::TurnType::Left)
      .value("STRAIGHT", roadmaker::TurnType::Straight)
      .value("RIGHT", roadmaker::TurnType::Right)
      .value("UTURN", roadmaker::TurnType::UTurn);

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

  nb::enum_<roadmaker::BoundarySource>(m, "BoundarySource")
      .value("DERIVED", roadmaker::BoundarySource::Derived)
      .value("AUTHORED", roadmaker::BoundarySource::Authored);

  // The state a signal GROUP shows during one phase (p4-s8, §14.6 / ADR-0008
  // Layer 1). Yellow is an EXPLICIT value, never an automatic transition — the
  // derived cycle emits its own yellow clearance phases — and Off is the
  // flashing/dark state a maintenance cycle uses.
  nb::enum_<roadmaker::SignalState>(m, "SignalState")
      .value("RED", roadmaker::SignalState::Red)
      .value("YELLOW", roadmaker::SignalState::Yellow)
      .value("GREEN", roadmaker::SignalState::Green)
      .value("OFF", roadmaker::SignalState::Off);

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
  bind_id<roadmaker::ControllerId>(m, "ControllerId");
  bind_id<roadmaker::SurfaceId>(m, "SurfaceId");

  // --- value types -----------------------------------------------------------

  nb::class_<roadmaker::Poly3>(m, "Poly3")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::Poly3* self, double s, double a, double b, double c, double d) {
            new (self) roadmaker::Poly3{.s = s, .a = a, .b = b, .c = c, .d = d};
          },
          "s"_a = 0.0,
          "a"_a = 0.0,
          "b"_a = 0.0,
          "c"_a = 0.0,
          "d"_a = 0.0,
          "value(ds) = a + b*ds + c*ds^2 + d*ds^3, ds = query - s. Whether `s` "
          "is global or section-local depends on the owning profile.")
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
      .def_rw("material",
              &roadmaker::RoadMark::material,
              "@material code (§11.9); None writes nothing (spec default "
              "'standard'). RoadMaker authors 'rm:<id>'.")
      .def_rw("lines",
              &roadmaker::RoadMark::lines,
              "Explicit multi-line stripes (<type>/<line>); empty for a simple "
              "single-stripe mark.");

  nb::class_<roadmaker::LaneMaterial>(m, "LaneMaterial")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::LaneMaterial* self,
             double s_offset,
             std::optional<double> friction,
             std::optional<double> roughness,
             std::optional<std::string> surface) {
            new (self) roadmaker::LaneMaterial{.s_offset = s_offset,
                                               .friction = friction,
                                               .roughness = roughness,
                                               .surface = std::move(surface)};
          },
          "s_offset"_a = 0.0,
          "friction"_a = nb::none(),
          "roughness"_a = nb::none(),
          "surface"_a = nb::none())
      .def_rw("s_offset", &roadmaker::LaneMaterial::s_offset)
      .def_rw("friction", &roadmaker::LaneMaterial::friction)
      .def_rw("roughness", &roadmaker::LaneMaterial::roughness)
      .def_rw("surface",
              &roadmaker::LaneMaterial::surface,
              "Surface material code (§11.8.2); RoadMaker writes 'rm:<id>'.");

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
      .def_rw("contact", &roadmaker::RoadEnd::contact)
      // A RoadEnd is the IDENTITY of a junction arm — and so of its corner
      // (p4-s1) and its stop line (p4-s3). Without these, matching a solved
      // JunctionStopLineInfo back to the arm you asked about falls through to
      // identity comparison and silently never matches.
      .def("__eq__",
           [](const roadmaker::RoadEnd& self, const roadmaker::RoadEnd& other) {
             return self == other;
           })
      .def("__hash__",
           [](const roadmaker::RoadEnd& self) {
             return nb::hash(nb::make_tuple(self.road, self.contact));
           })
      .def("__repr__", [](const roadmaker::RoadEnd& self) { return road_end_text(self); });

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
      .def_rw("direction",
              &roadmaker::Lane::direction,
              "Travel direction (e_lane_direction, §11). Standard writes nothing; "
              "edit via rm.edit.set_lane_direction.")
      .def_ro("widths", &roadmaker::Lane::widths)
      .def_rw("road_marks",
              &roadmaker::Lane::road_marks,
              "Marks on this lane's OUTER boundary, ascending s_offset "
              "(rm.edit.set_road_mark edits the first record only).")
      .def_ro("materials",
              &roadmaker::Lane::materials,
              "<material> records (§11.8.2), ascending s_offset; edit via "
              "rm.edit.set_lane_material. Empty for the center lane.")
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

  nb::class_<roadmaker::JunctionCorner>(m, "JunctionCorner")
      .def_ro("arm_a",
              &roadmaker::JunctionCorner::arm_a,
              "First arm of the CCW-adjacent pair this override is keyed by.")
      .def_ro("arm_b", &roadmaker::JunctionCorner::arm_b, "Second arm of the pair.")
      .def_ro("radius",
              &roadmaker::JunctionCorner::radius,
              "Authored fillet radius [m], or None when the derived radius applies.")
      .def_ro("extent_a",
              &roadmaker::JunctionCorner::extent_a,
              "Authored tangent-leg setback [m] on arm_a's edge, or None.")
      .def_ro("extent_b",
              &roadmaker::JunctionCorner::extent_b,
              "Authored tangent-leg setback [m] on arm_b's edge, or None.")
      .def_ro("sidewalk_material",
              &roadmaker::JunctionCorner::sidewalk_material,
              "Bare catalog name of the corner's sidewalk overlay material, or "
              "None when the corner authors no sidewalk. Edit it with "
              "edit.set_corner_sidewalk_material.")
      .def_ro("median_material",
              &roadmaker::JunctionCorner::median_material,
              "Bare catalog name of the corner's median-nose overlay material, "
              "or None. Edit it with edit.set_corner_median_material.")
      .def("__repr__", [](const roadmaker::JunctionCorner& corner) {
        std::string text =
            "JunctionCorner(" + road_end_text(corner.arm_a) + ", " + road_end_text(corner.arm_b);
        if (corner.radius) {
          text += ", radius=" + std::to_string(*corner.radius);
        }
        if (corner.extent_a && corner.extent_b) {
          text += ", extents=(" + std::to_string(*corner.extent_a) + ", " +
                  std::to_string(*corner.extent_b) + ")";
        }
        return text + ")";
      });

  nb::class_<roadmaker::SurfaceSpan>(m, "SurfaceSpan")
      .def_ro("road",
              &roadmaker::SurfaceSpan::road,
              "The connecting road whose floor contribution this record "
              "overrides. NOT a SpanArm — that is a VIRTUAL junction's "
              "s-interval, and a span junction has no floor at all.")
      .def_ro("included",
              &roadmaker::SurfaceSpan::included,
              "False means this road's SAMPLES leave the floor's fill inputs. "
              "Its footprint stays in the union either way, so the pavement's "
              "coverage and the exported <boundary> never change. Edit it with "
              "edit.set_surface_span_included.")
      .def_ro("sort_index",
              &roadmaker::SurfaceSpan::sort_index,
              "Precedence where span footprints OVERLAP: higher wins. Edit it "
              "with edit.set_surface_span_sort_index.")
      .def("__repr__", [](const roadmaker::SurfaceSpan& span) {
        return "SurfaceSpan(included=" + std::string(span.included ? "True" : "False") +
               ", sort_index=" + std::to_string(span.sort_index) + ")";
      });

  nb::class_<roadmaker::Maneuver>(m, "Maneuver")
      .def_ro("road",
              &roadmaker::Maneuver::road,
              "The connecting road whose path through the junction this record "
              "overrides — the record key, stable across regeneration.")
      .def_ro("locked",
              &roadmaker::Maneuver::locked,
              "True when regeneration keeps this road's plan view, length, "
              "elevation and lane width instead of replanning them, and keeps "
              "the road itself even when the plan no longer contains its turn. "
              "Toggle it with edit.set_maneuver_locked; edit.set_maneuver_path "
              "sets it implicitly in the same undo step.")
      .def_ro("turn_type",
              &roadmaker::Maneuver::turn_type,
              "Authored TurnType, or None when the type computed from the "
              "arm-face headings applies. Purely semantic — it never moves "
              "geometry, which is why edit.rebuild_maneuvers keeps it while "
              "clearing everything else here.")
      .def_ro("start_offset",
              &roadmaker::Maneuver::start_offset,
              "Endpoint slide [m] along the INCOMING arm's face, measured from "
              "the anchor lane's inner boundary along the arm's +t axis. None "
              "means the derived anchor (0).")
      .def_ro("end_offset",
              &roadmaker::Maneuver::end_offset,
              "Endpoint slide [m] along the OUTGOING arm's face; same convention.")
      .def_ro("control_points",
              &roadmaker::Maneuver::control_points,
              "The authored INTERIOR waypoints, in driving direction. The "
              "endpoints are never stored — they are derived from the arm faces "
              "plus the offsets, so a maneuver keeps meeting its arms when they "
              "move. Edit them with edit.set_maneuver_path.")
      .def("__repr__", [](const roadmaker::Maneuver& maneuver) {
        return "Maneuver(locked=" + std::string(maneuver.locked ? "True" : "False") +
               ", turn_type=" + (maneuver.turn_type ? "set" : "None") +
               ", control_points=" + std::to_string(maneuver.control_points.size()) + ")";
      });

  nb::class_<roadmaker::SpanArm>(m, "SpanArm")
      .def(nb::init<>())
      // edit.create_span_junction takes a list of these, so they have to be
      // constructible from Python (WP4, issue #319).
      .def(
          "__init__",
          [](roadmaker::SpanArm* self, roadmaker::RoadId road, double s_start, double s_end) {
            new (self) roadmaker::SpanArm{.road = road, .s_start = s_start, .s_end = s_end};
          },
          "road"_a,
          "s_start"_a,
          "s_end"_a)
      .def_ro("road",
              &roadmaker::SpanArm::road,
              "The road the span lies on. spans[0].road is exported as the "
              "OpenDRIVE @mainRoad of the virtual junction.")
      .def_ro("s_start",
              &roadmaker::SpanArm::s_start,
              "Start of the covered interval in the road's reference-line s [m].")
      .def_ro("s_end",
              &roadmaker::SpanArm::s_end,
              "End of the covered interval in the road's reference-line s [m].")
      .def("__repr__", [](const roadmaker::SpanArm& span) {
        return "SpanArm(s_start=" + std::to_string(span.s_start) +
               ", s_end=" + std::to_string(span.s_end) + ")";
      });

  nb::class_<roadmaker::JunctionController>(m, "JunctionController")
      .def_ro("controller_odr_id",
              &roadmaker::JunctionController::controller_odr_id,
              "@id of the top-level <controller> this junction synchronizes. A "
              "REFERENCE, never a definition (OpenDRIVE 1.9.0 §12.14, Table 84) "
              "— look the controller itself up in RoadNetwork.controller_ids.")
      .def_ro("sequence", &roadmaker::JunctionController::sequence, "@sequence, or None.")
      .def_ro("type", &roadmaker::JunctionController::type, "@type — optional free text.")
      .def_ro("preserved", &roadmaker::JunctionController::preserved)
      .def("__repr__", [](const roadmaker::JunctionController& reference) {
        return "JunctionController(controller_odr_id='" + reference.controller_odr_id + "')";
      });

  nb::class_<roadmaker::SignalMount>(m, "SignalMount")
      .def_ro("signal_odr_id",
              &roadmaker::SignalMount::signal_odr_id,
              "odr id of the logical <signal> these props represent.")
      .def_ro("object_odr_ids",
              &roadmaker::SignalMount::object_odr_ids,
              "odr ids of the <object>s that physically represent it, in "
              "placement order. A LIST because an assembly's parts drop "
              "straight in; bounded by 16.")
      .def("__repr__", [](const roadmaker::SignalMount& mount) {
        return "SignalMount(signal_odr_id='" + mount.signal_odr_id +
               "', parts=" + std::to_string(mount.object_odr_ids.size()) + ")";
      });

  nb::class_<roadmaker::Signalization>(m, "Signalization")
      .def_ro("tmpl",
              &roadmaker::Signalization::tmpl,
              "The applied template token ('protected_left', 'two_phase', "
              "'all_way_stop', 'two_way_stop'); empty means the junction was "
              "not signalized from a template. Authoring provenance only — the "
              "<signal>/<controller> elements are the export.")
      .def_ro("mount_model",
              &roadmaker::Signalization::mount_model,
              "Prop model id mounted with each placed signal; empty = none.")
      .def("__repr__", [](const roadmaker::Signalization& signalization) {
        return "Signalization(tmpl='" + signalization.tmpl + "', mount_model='" +
               signalization.mount_model + "')";
      });

  // The RAW authored cycle stored on a junction (p4-s8, §14.6 / ADR-0008 Layer
  // 1). SPARSE: a PhaseState lists only a non-Red controller, so an unstated
  // member shows Red and an all-red clearance phase has an empty state list.
  // The RESOLVED, Red-filled cycle a consumer reads is mesh.junction_phases.
  nb::class_<roadmaker::PhaseState>(m, "PhaseState")
      .def_ro("controller_odr_id",
              &roadmaker::PhaseState::controller_odr_id,
              "String id of the top-level <controller> this state is for — what "
              "OpenDRIVE stores, dangling-tolerant, and surviving an erase / "
              "restore of the controller.")
      .def_ro("state", &roadmaker::PhaseState::state, "The color that group shows this phase.")
      .def("__repr__", [](const roadmaker::PhaseState& state) {
        return "PhaseState(controller_odr_id='" + state.controller_odr_id + "')";
      });

  nb::class_<roadmaker::SignalPhase>(m, "SignalPhase")
      .def_ro("name",
              &roadmaker::SignalPhase::name,
              "Optional label (token alphabet or empty); the derived cycle names "
              "phases 'axis0', 'axis0_clear', ...")
      .def_ro("duration",
              &roadmaker::SignalPhase::duration,
              "Duration [s]; an authored phase is > 0 and <= MAX_SIGNAL_PHASE_DURATION.")
      .def_ro("states",
              &roadmaker::SignalPhase::states,
              "The non-Red controller states only (sparse, <=1 per controller); "
              "everything unlisted is Red by omission.")
      .def("__repr__", [](const roadmaker::SignalPhase& phase) {
        return "SignalPhase(name='" + phase.name + "', duration=" + std::to_string(phase.duration) +
               ", states=" + std::to_string(phase.states.size()) + ")";
      });

  nb::class_<roadmaker::Junction>(m, "Junction")
      .def_ro("odr_id", &roadmaker::Junction::odr_id)
      .def_ro("name", &roadmaker::Junction::name)
      .def_ro("connections", &roadmaker::Junction::connections)
      .def_ro("arms", &roadmaker::Junction::arms)
      .def_ro("corners",
              &roadmaker::Junction::corners,
              "Authored corner-fillet overrides (sparse; read-only). Edit them "
              "with edit.set_corner_radius / edit.set_corner_extents.")
      .def_ro("default_corner_radius",
              &roadmaker::Junction::default_corner_radius,
              "Junction-wide fillet radius [m] used by every corner without its "
              "own radius, or None. Resolution order is per-corner override > "
              "this default > derived. Edit it with "
              "edit.set_junction_default_corner_radius.")
      .def_ro("material",
              &roadmaker::Junction::material,
              "Bare catalog material name for the junction carriageway; empty "
              "means the derived asphalt look. Edit it with "
              "edit.set_junction_material.")
      .def_ro("locked",
              &roadmaker::Junction::locked,
              "True when the automatic regeneration loops skip this junction, "
              "so hand-tuned connections, corners and stop lines survive edits "
              "to its arms. edit.regenerate_junction still re-derives it on "
              "demand. Toggle it with edit.set_junction_locked; a span junction "
              "is always locked.")
      .def_ro("surface_spans",
              &roadmaker::Junction::surface_spans,
              "Authored floor-contribution overrides, one per connecting road "
              "(sparse; read-only). Edit them with "
              "edit.set_surface_span_included / edit.set_surface_span_sort_index, "
              "and read the SOLVED spans with mesh.junction_surface_spans.")
      .def_ro("maneuvers",
              &roadmaker::Junction::maneuvers,
              "Authored maneuver overrides, one per connecting road (sparse; "
              "read-only). Edit them with edit.set_maneuver_locked / "
              "edit.set_maneuver_turn_type / edit.set_maneuver_path / "
              "edit.reset_maneuver / edit.rebuild_maneuvers, and read the "
              "SOLVED maneuvers with junction_maneuvers.")
      .def_ro("junction_controllers",
              &roadmaker::Junction::junction_controllers,
              "The junction's signal SYNCHRONIZATION group (OpenDRIVE §12.14): "
              "the top-level <controller>s whose groups belong together here, "
              "by string id. Sparse — empty on every unsignalized junction, and "
              "always empty on a span/virtual junction, which shall have no "
              "controllers. Authored by edit.signalize_junction.")
      .def_ro("signalization",
              &roadmaker::Junction::signalization,
              "Which auto-signalization template produced this junction's "
              "signals, if any (read-only; edit.signalize_junction / "
              "edit.clear_signalization author it).")
      .def_ro("signal_mounts",
              &roadmaker::Junction::signal_mounts,
              "The physical props representing each placed signal, keyed by "
              "signal odr id (sparse; read-only). A signal placed without a "
              "mount prop has no entry.")
      .def_ro("phases",
              &roadmaker::Junction::phases,
              "The AUTHORED signal cycle (ADR-0008 Layer 1, <userData "
              "code='rm:phases'>; read-only, sparse). EMPTY means unauthored — "
              "the effective cycle is DERIVED — so read the effective cycle with "
              "mesh.junction_phases and edit it with edit.set_phase_duration / "
              "set_phase_state / add_signal_phase / duplicate_signal_phase / "
              "remove_signal_phase / clear_signal_phases.")
      .def_ro("spans",
              &roadmaker::Junction::spans,
              "Membership spans of a VIRTUAL (span) junction — a stretch of a "
              "road that belongs to the junction without cutting it (OpenDRIVE "
              "1.8.1/1.9.0 §12.7). Non-empty means this is a span junction, "
              "which has no arms and no connections and is always locked.")
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

  nb::class_<roadmaker::ObjectMarking>(m, "ObjectMarking")
      .def(nb::init<>())
      .def_rw("color", &roadmaker::ObjectMarking::color, "e_roadMarkColor (required).")
      .def_rw("line_length",
              &roadmaker::ObjectMarking::line_length,
              "Length of the visible part [m] (>0); solid = full run, space_length 0.")
      .def_rw("space_length",
              &roadmaker::ObjectMarking::space_length,
              "Gap between visible parts [m] (>=0); 0 = solid line.")
      .def_rw("start_offset", &roadmaker::ObjectMarking::start_offset)
      .def_rw("stop_offset", &roadmaker::ObjectMarking::stop_offset)
      .def_rw("side", &roadmaker::ObjectMarking::side, "e_sideType (bounding-volume marking).")
      .def_rw("weight", &roadmaker::ObjectMarking::weight)
      .def_rw("width", &roadmaker::ObjectMarking::width)
      .def_rw("z_offset", &roadmaker::ObjectMarking::z_offset)
      .def_rw("corner_refs",
              &roadmaker::ObjectMarking::corner_refs,
              "<cornerReference @id> values in outline order (empty for @side markings).")
      .def_ro("preserved", &roadmaker::ObjectMarking::preserved);

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
      .def_rw("markings",
              &roadmaker::ObjectOutline::markings,
              "<markings> referencing this outline's corner points (§13.8).")
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

  nb::class_<roadmaker::RepeatInstance>(m, "RepeatInstance")
      .def(nb::init<>())
      .def_rw("s", &roadmaker::RepeatInstance::s, "Absolute s of the instance origin [m].")
      .def_rw("t", &roadmaker::RepeatInstance::t, "Lateral offset of the instance origin [m].")
      .def_rw("z_offset",
              &roadmaker::RepeatInstance::z_offset,
              "Height above the reference-line elevation [m].");

  m.def("expand_repeat",
        &roadmaker::expand_repeat,
        "repeat"_a,
        "Expands one <repeat> section (§13.4) into its discrete object "
        "instances. distance <= 0 (a continuous object) yields an empty list; "
        "otherwise floor(length / distance) + 1 instances are placed at ds = 0, "
        "distance, ... rounded down (no incomplete trailing instance). t is a "
        "cubic in ds when any of bT/cT/dT is set (1.9.0), else a linear "
        "tStart->tEnd lerp (1.8.1); z_offset is always a linear lerp.");

  nb::class_<roadmaker::CrosswalkData>(m, "CrosswalkData")
      .def(nb::init<>())
      .def_rw("asset", &roadmaker::CrosswalkData::asset, "Library asset key this instance follows.")
      .def_rw(
          "border_width", &roadmaker::CrosswalkData::border_width, "Edge-line width [m]; 0 = none.")
      .def_rw("dash_length",
              &roadmaker::CrosswalkData::dash_length,
              "Stripe length along the crossing [m]; 0 = solid.")
      .def_rw("dash_gap", &roadmaker::CrosswalkData::dash_gap, "Gap between stripes [m].")
      .def_rw("material",
              &roadmaker::CrosswalkData::material,
              "Material code (e.g. 'material.paint_white').")
      .def_rw("material_override",
              &roadmaker::CrosswalkData::material_override,
              "True keeps `material` when the asset's Default Material changes.")
      .def_rw("category", &roadmaker::CrosswalkData::category, "Segmentation category tag.")
      .def("__eq__", [](const roadmaker::CrosswalkData& a, nb::object b) {
        return nb::isinstance<roadmaker::CrosswalkData>(b) &&
               a == nb::cast<roadmaker::CrosswalkData>(b);
      });

  nb::class_<roadmaker::MarkingCurveData>(m, "MarkingCurveData")
      .def(nb::init<>())
      .def_rw(
          "asset", &roadmaker::MarkingCurveData::asset, "Library asset key this instance follows.")
      .def_rw("width", &roadmaker::MarkingCurveData::width, "Band width across the curve [m].")
      .def_rw("dash_length",
              &roadmaker::MarkingCurveData::dash_length,
              "Visible run along the curve [m]; 0 = solid.")
      .def_rw("dash_gap", &roadmaker::MarkingCurveData::dash_gap, "Gap between runs [m].")
      .def_rw("material",
              &roadmaker::MarkingCurveData::material,
              "Material code (e.g. 'material.paint_white').")
      .def_rw("material_override",
              &roadmaker::MarkingCurveData::material_override,
              "True keeps `material` when the asset's Default Material changes.")
      .def_rw("category", &roadmaker::MarkingCurveData::category, "Segmentation category tag.")
      .def_rw("striped",
              &roadmaker::MarkingCurveData::striped,
              "True paints a striped crosswalk band; False a plain line marking.")
      .def_rw("samples",
              &roadmaker::MarkingCurveData::samples,
              "Centreline as road-frame (s,t) sample pairs, in draw order.")
      .def("__eq__", [](const roadmaker::MarkingCurveData& a, nb::object b) {
        return nb::isinstance<roadmaker::MarkingCurveData>(b) &&
               a == nb::cast<roadmaker::MarkingCurveData>(b);
      });

  nb::class_<roadmaker::StencilData>(m, "StencilData")
      .def(nb::init<>())
      .def_rw("asset", &roadmaker::StencilData::asset, "Library asset key this instance follows.")
      .def_rw("material",
              &roadmaker::StencilData::material,
              "Material code (e.g. 'material.paint_white').")
      .def_rw("material_override",
              &roadmaker::StencilData::material_override,
              "True keeps `material` when the asset's Default Material changes.")
      .def_rw("category", &roadmaker::StencilData::category, "Segmentation category tag.")
      .def("__eq__", [](const roadmaker::StencilData& a, nb::object b) {
        return nb::isinstance<roadmaker::StencilData>(b) &&
               a == nb::cast<roadmaker::StencilData>(b);
      });

  nb::class_<roadmaker::Object>(m, "Object")
      .def(nb::init<>())
      .def(nb::init<const roadmaker::Object&>(),
           "Copy an object (keeps the read-only `road`), e.g. to build the "
           "modified value edit.update_objects expects.")
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
      .def_rw("markings",
              &roadmaker::Object::markings,
              "Object-level <markings> (the 1.8.1 @side bounding-volume form).")
      .def_rw("crosswalk",
              &roadmaker::Object::crosswalk,
              "Parametric-crosswalk authoring data (rm:crosswalk userData); None if absent.")
      .def_rw("marking_curve",
              &roadmaker::Object::marking_curve,
              "Free-form marking-curve authoring data (rm:markingCurve userData); None if absent.")
      .def_rw("stencil",
              &roadmaker::Object::stencil,
              "Point-stencil authoring data (rm:stencil userData); None if absent.")
      .def_ro("preserved", &roadmaker::Object::preserved)
      .def("__repr__", [](const roadmaker::Object& object) {
        return "Object(odr_id='" + object.odr_id + "', s=" + std::to_string(object.s) +
               ", t=" + std::to_string(object.t) + ")";
      });

  nb::class_<roadmaker::SurfaceNode>(m, "SurfaceNode")
      .def(nb::init<>())
      .def_rw("x", &roadmaker::SurfaceNode::x)
      .def_rw("y", &roadmaker::SurfaceNode::y)
      .def_rw("tangent_in_x", &roadmaker::SurfaceNode::tangent_in_x)
      .def_rw("tangent_in_y", &roadmaker::SurfaceNode::tangent_in_y)
      .def_rw("tangent_out_x", &roadmaker::SurfaceNode::tangent_out_x)
      .def_rw("tangent_out_y", &roadmaker::SurfaceNode::tangent_out_y)
      .def("__eq__",
           [](const roadmaker::SurfaceNode& a, nb::object b) {
             return nb::isinstance<roadmaker::SurfaceNode>(b) &&
                    a == nb::cast<roadmaker::SurfaceNode>(b);
           })
      .def("__repr__", [](const roadmaker::SurfaceNode& node) {
        return "SurfaceNode(x=" + std::to_string(node.x) + ", y=" + std::to_string(node.y) + ")";
      });

  nb::class_<roadmaker::Surface>(m, "Surface")
      .def(nb::init<>())
      .def_rw("source",
              &roadmaker::Surface::source,
              "DERIVED (boundary follows bounding_roads) or AUTHORED (boundary "
              "is `nodes`). Editing a derived boundary detaches it to AUTHORED; "
              "edit.revert_surface_to_derived puts it back.")
      .def_rw("bounding_roads",
              &roadmaker::Surface::bounding_roads,
              "Ordered ring of RoadIds enclosing the area; deterministic. On an "
              "AUTHORED surface this is PROVENANCE: it no longer shapes the "
              "boundary but still supplies the elevation samples.")
      .def_rw("nodes",
              &roadmaker::Surface::nodes,
              "The authored boundary loop (closed cubic Hermite), in ring order. "
              "Always empty on a DERIVED surface. Edit it with "
              "edit.set_surface_boundary, and read the EFFECTIVE nodes of either "
              "kind with surface_boundary_nodes.")
      .def_rw("material",
              &roadmaker::Surface::material,
              "Ground material name (\"\" = default grass; e.g. \"asphalt\", \"concrete\").")
      .def("__eq__",
           [](const roadmaker::Surface& a, nb::object b) {
             return nb::isinstance<roadmaker::Surface>(b) && a == nb::cast<roadmaker::Surface>(b);
           })
      .def("__repr__", [](const roadmaker::Surface& surface) {
        return "Surface(bounding_roads=" + std::to_string(surface.bounding_roads.size()) +
               ", nodes=" + std::to_string(surface.nodes.size()) + ")";
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

  nb::class_<roadmaker::Control>(m, "Control")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::Control* self, std::string signal_odr_id, std::string type) {
            new (self) roadmaker::Control{.signal_odr_id = std::move(signal_odr_id),
                                          .type = std::move(type)};
          },
          "signal_odr_id"_a,
          "type"_a = std::string{})
      .def_rw("signal_odr_id",
              &roadmaker::Control::signal_odr_id,
              "@signalId — the STRING id of the controlled signal, which is "
              "what OpenDRIVE stores. Resolving it to a live SignalId is a "
              "query's job (junction_signals), never the model's, so a dangling "
              "reference from a foreign file survives a round trip and is "
              "reported by validate_network instead of being dropped.")
      .def_rw("type", &roadmaker::Control::type, "@type — optional, free text.")
      .def("__repr__", [](const roadmaker::Control& control) {
        return "Control(signal_odr_id='" + control.signal_odr_id + "')";
      });

  nb::class_<roadmaker::Controller>(m, "Controller")
      .def(nb::init<>())
      .def_rw("odr_id", &roadmaker::Controller::odr_id, "@id — required, unique in the database.")
      .def_rw("name", &roadmaker::Controller::name)
      .def_rw("sequence", &roadmaker::Controller::sequence, "@sequence, or None.")
      .def_rw("controls",
              &roadmaker::Controller::controls,
              "<control> children (1..*). A controller with none is a "
              "validate_network finding "
              "(asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals).")
      .def_ro("preserved", &roadmaker::Controller::preserved)
      .def("__repr__", [](const roadmaker::Controller& controller) {
        return "Controller(odr_id='" + controller.odr_id +
               "', controls=" + std::to_string(controller.controls.size()) + ")";
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
      .def("add_controller",
           &roadmaker::RoadNetwork::add_controller,
           "value"_a,
           "Adds an OpenDRIVE <controller> (§14.6). Takes NO owner: a "
           "controller is a child of <OpenDRIVE>, not of a road or a junction "
           "— a junction only references it by string id (§12.14).")
      .def("create_surface",
           &roadmaker::RoadNetwork::create_surface,
           "value"_a,
           "Creates a ground surface. derive_surfaces owns the surface arena "
           "and reconciles it against enclosed areas; prefer that over hand "
           "authoring.")
      .def("erase_road", &roadmaker::RoadNetwork::erase_road, "road"_a)
      .def("erase_junction", &roadmaker::RoadNetwork::erase_junction, "junction"_a)
      .def("erase_object", &roadmaker::RoadNetwork::erase_object, "object"_a)
      .def("erase_signal", &roadmaker::RoadNetwork::erase_signal, "signal"_a)
      .def("erase_controller",
           &roadmaker::RoadNetwork::erase_controller,
           "controller"_a,
           "Controllers are leaves — nothing in the arenas points at one, so "
           "erasing cascades nothing. The converse holds too: erase_signal does "
           "NOT cascade into controllers, and the resulting dangling <control> "
           "is an expected state validate_network reports.")
      .def("erase_surface", &roadmaker::RoadNetwork::erase_surface, "surface"_a)
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
      .def("controller",
           nb::overload_cast<roadmaker::ControllerId>(&roadmaker::RoadNetwork::controller),
           "id"_a,
           nb::rv_policy::reference_internal,
           "Controller for id, or None if the id is stale. The reference is "
           "valid only until the network is mutated.")
      .def("surface",
           nb::overload_cast<roadmaker::SurfaceId>(&roadmaker::RoadNetwork::surface),
           "id"_a,
           nb::rv_policy::reference_internal,
           "Surface for id, or None if the id is stale. The reference is valid "
           "only until the network is mutated.")
      .def(
          "surfaces_touching",
          [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road) {
            return roadmaker::surfaces_touching(network, road);
          },
          "road"_a,
          "SurfaceIds whose bounding-road ring contains `road`, in arena order.")
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
      .def(
          "section_at",
          [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, double s) {
            return roadmaker::section_at(network, road, s);
          },
          "road"_a,
          "s"_a,
          "The LaneSectionId governing global station s: the last section "
          "starting at or before it. Invalid id if the road is stale or has "
          "no sections.")
      .def(
          "section_end",
          [](const roadmaker::RoadNetwork& network, roadmaker::LaneSectionId section) {
            return unwrap(roadmaker::section_end(network, section));
          },
          "section"_a,
          "End station of the section: the next section's s0, or the road "
          "length for the last one. Raises ValueError on a stale id.")
      .def(
          "lane_boundary_offsets",
          [](const roadmaker::RoadNetwork& network, roadmaker::RoadId road, double s) {
            return roadmaker::lane_boundary_offsets(network, road, s);
          },
          "road"_a,
          "s"_a,
          "Lateral t of every lane boundary at global station s, leftmost first "
          "(the centre boundary at laneOffset(s)). The same routine the mesher "
          "uses. Empty if the road is stale or has no sections.")
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
      .def_prop_ro("controller_ids",
                   [](const roadmaker::RoadNetwork& network) {
                     std::vector<roadmaker::ControllerId> ids;
                     network.for_each_controller(
                         [&](roadmaker::ControllerId id, const roadmaker::Controller&) {
                           ids.push_back(id);
                         });
                     return ids;
                   })
      .def_prop_ro(
          "surface_ids",
          [](const roadmaker::RoadNetwork& network) {
            std::vector<roadmaker::SurfaceId> ids;
            network.for_each_surface(
                [&](roadmaker::SurfaceId id, const roadmaker::Surface&) { ids.push_back(id); });
            return ids;
          })
      .def_prop_ro("road_count", &roadmaker::RoadNetwork::road_count)
      .def_prop_ro("lane_count", &roadmaker::RoadNetwork::lane_count)
      .def_prop_ro("junction_count", &roadmaker::RoadNetwork::junction_count)
      .def_prop_ro("object_count", &roadmaker::RoadNetwork::object_count)
      .def_prop_ro("signal_count", &roadmaker::RoadNetwork::signal_count)
      .def_prop_ro("controller_count", &roadmaker::RoadNetwork::controller_count)
      .def_prop_ro("surface_count", &roadmaker::RoadNetwork::surface_count)
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

  m.def("derive_surfaces",
        &roadmaker::derive_surfaces,
        "network"_a,
        "Reconciles the surface arena so that, after the call, the set of "
        "surfaces exactly matches the areas enclosed by roads. Id-stable across "
        "calls and idempotent when the topology is unchanged.");

  // --- junction corners (the solve shared by mesher, tool and panel) -------------

  nb::class_<roadmaker::JunctionCornerInfo>(m, "JunctionCornerInfo")
      .def_ro("arm_a", &roadmaker::JunctionCornerInfo::arm_a, "First arm of the CCW pair.")
      .def_ro("arm_b", &roadmaker::JunctionCornerInfo::arm_b, "Second arm of the CCW pair.")
      .def_prop_ro(
          "corner",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.corner); },
          "(x, y) edge-line intersection — the unfilleted corner point.")
      .def_prop_ro(
          "dir_a",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.dir_a); },
          "Unit direction of arm_a's edge ray, pointing into the junction.")
      .def_prop_ro(
          "dir_b",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.dir_b); },
          "Unit direction of arm_b's edge ray, pointing into the junction.")
      .def_prop_ro(
          "bisector",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.bisector); },
          "Unit inward bisector at `corner`.")
      .def_prop_ro(
          "face_a",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.face_a); },
          "Outer corner of arm_a's face that its edge ray starts from.")
      .def_prop_ro(
          "face_b",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.face_b); },
          "Outer corner of arm_b's face that its edge ray starts from.")
      .def_ro("phi", &roadmaker::JunctionCornerInfo::phi, "Angle [rad] between the two edge rays.")
      .def_ro("extent_a",
              &roadmaker::JunctionCornerInfo::extent_a,
              "Effective tangent-leg setback [m] along arm_a's edge.")
      .def_ro("extent_b",
              &roadmaker::JunctionCornerInfo::extent_b,
              "Effective tangent-leg setback [m] along arm_b's edge.")
      .def_prop_ro(
          "tangent_a",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.tangent_a); },
          "(x, y) tangency point on arm_a's edge.")
      .def_prop_ro(
          "tangent_b",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.tangent_b); },
          "(x, y) tangency point on arm_b's edge.")
      .def_ro("radius",
              &roadmaker::JunctionCornerInfo::radius,
              "Effective fillet radius [m] — what the Corner Radius attribute shows.")
      .def_ro("max_radius",
              &roadmaker::JunctionCornerInfo::max_radius,
              "Largest radius the arm faces leave room for [m].")
      .def_ro("max_extent_a",
              &roadmaker::JunctionCornerInfo::max_extent_a,
              "Free run [m] of arm_a's edge from its face corner to `corner`.")
      .def_ro("max_extent_b",
              &roadmaker::JunctionCornerInfo::max_extent_b,
              "Free run [m] of arm_b's edge from its face corner to `corner`.")
      .def_ro("radius_authored",
              &roadmaker::JunctionCornerInfo::radius_authored,
              "True when a JunctionCorner override supplies the radius.")
      .def_ro("extents_authored",
              &roadmaker::JunctionCornerInfo::extents_authored,
              "True when a JunctionCorner override supplies the extents.")
      .def_ro("radius_from_junction_default",
              &roadmaker::JunctionCornerInfo::radius_from_junction_default,
              "True when `radius` came from Junction.default_corner_radius — no "
              "per-corner override supplied one but the junction-wide default "
              "did. Never True together with `radius_authored`.")
      .def_ro("sidewalk_material",
              &roadmaker::JunctionCornerInfo::sidewalk_material,
              "The corner's authored sidewalk overlay material (bare catalog "
              "name), empty when unset.")
      .def_ro("median_material",
              &roadmaker::JunctionCornerInfo::median_material,
              "The corner's authored median-nose overlay material (bare catalog "
              "name), empty when unset.")
      .def_prop_ro(
          "curve",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.curve); },
          "The corner curve tangent_a -> tangent_b as a list of (x, y) tuples.")
      .def(
          "apex",
          [](const roadmaker::JunctionCornerInfo& info) { return to_xy(info.apex()); },
          "(x, y) midpoint of `curve` — the apex control vertex.")
      .def("__repr__", [](const roadmaker::JunctionCornerInfo& info) {
        return "JunctionCornerInfo(" + road_end_text(info.arm_a) + ", " +
               road_end_text(info.arm_b) + ", radius=" + std::to_string(info.radius) +
               ", max_radius=" + std::to_string(info.max_radius) +
               (info.radius_authored ? ", authored" : "") + ")";
      });

  m.def(
      "junction_corners",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_corners(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Solves every corner of the junction in CCW order, applying any authored "
      "JunctionCorner overrides — the same solve the mesher uses. Returns an "
      "empty list for a stale id or a junction with fewer than two usable arms; "
      "degenerate pairs (parallel or behind a face) are skipped.");

  nb::class_<roadmaker::JunctionStopLineInfo>(m, "JunctionStopLineInfo")
      .def_ro("arm",
              &roadmaker::JunctionStopLineInfo::arm,
              "The junction-facing road end this line belongs to — its identity. "
              "On a span (virtual) junction it is a pseudo road end naming which "
              "FACE of the span the line guards: start = the s_start face, end = "
              "the s_end face.")
      .def_ro("span_face",
              &roadmaker::JunctionStopLineInfo::span_face,
              "True when this line is one of the two faces of a span (virtual) "
              "junction rather than an arm's line, i.e. `arm` is a pseudo road end.")
      .def_ro("distance",
              &roadmaker::JunctionStopLineInfo::distance,
              "Effective setback [m] from the junction mouth, clamped to "
              "[0, max_distance].")
      .def_ro("max_distance",
              &roadmaker::JunctionStopLineInfo::max_distance,
              "Largest setback [m] the arm road leaves room for.")
      .def_ro("flipped",
              &roadmaker::JunctionStopLineInfo::flipped,
              "False: the band spans the approach lanes. True: the outgoing ones.")
      .def_ro("distance_authored",
              &roadmaker::JunctionStopLineInfo::distance_authored,
              "True when `distance` came from a record rather than the default.")
      .def_ro("authored",
              &roadmaker::JunctionStopLineInfo::authored,
              "True when the junction carries a StopLine record for this arm at "
              "all — what edit.reset_stopline needs.")
      .def_ro("crosswalk_odr_id",
              &roadmaker::JunctionStopLineInfo::crosswalk_odr_id,
              "odr id of the crosswalk this line was placed alongside, or empty.")
      .def_ro("s_center",
              &roadmaker::JunctionStopLineInfo::s_center,
              "Station [m] of the band's mid-thickness — the exported @s.")
      .def_ro("t_center",
              &roadmaker::JunctionStopLineInfo::t_center,
              "Mid-span lateral offset [m] — the exported @t.")
      .def_ro("span",
              &roadmaker::JunctionStopLineInfo::span,
              "Span ACROSS the lanes [m] — the exported @width.")
      .def_ro("thickness",
              &roadmaker::JunctionStopLineInfo::thickness,
              "Extent ALONG the road [m] — the exported @length.")
      .def_prop_ro(
          "left",
          [](const roadmaker::JunctionStopLineInfo& info) { return to_xy(info.left); },
          "(x, y) endpoint left of the arm's reference line.")
      .def_prop_ro(
          "right",
          [](const roadmaker::JunctionStopLineInfo& info) { return to_xy(info.right); },
          "(x, y) endpoint right of the arm's reference line.")
      .def("__repr__", [](const roadmaker::JunctionStopLineInfo& info) {
        return "JunctionStopLineInfo(arm=" + road_end_text(info.arm) +
               ", distance=" + std::to_string(info.distance) + (info.flipped ? ", flipped" : "") +
               (info.authored ? ", authored" : "") + ")";
      });

  m.def(
      "junction_stoplines",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_stoplines(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Solves every stop line of the junction — one per arm whose junction-facing "
      "end has driving lanes in the line's direction, in connection order. Stop "
      "lines are DERIVED: an arm has one without anything being authored, and any "
      "StopLine record is merged over that derivation. This is the same solve the "
      "mesher and the writer use. Empty for a stale id; an arm already carrying a "
      "plain signalLines object is suppressed so foreign files do not double-draw.");

  nb::class_<roadmaker::JunctionSurfaceSpanInfo>(m, "JunctionSurfaceSpanInfo")
      .def_ro("road",
              &roadmaker::JunctionSurfaceSpanInfo::road,
              "The connecting road this span belongs to — stable across "
              "regeneration, so it is the record key.")
      .def_ro("road_odr_id", &roadmaker::JunctionSurfaceSpanInfo::road_odr_id)
      .def_ro("included",
              &roadmaker::JunctionSurfaceSpanInfo::included,
              "Effective Include Samples: False means the span's samples leave "
              "the floor's elevation and triangulation inputs. Its footprint "
              "stays in the union regardless.")
      .def_ro("sort_index",
              &roadmaker::JunctionSurfaceSpanInfo::sort_index,
              "Effective sort index — higher wins where footprints overlap.")
      .def_ro("authored",
              &roadmaker::JunctionSurfaceSpanInfo::authored,
              "True when the junction carries a SurfaceSpan record for this road.")
      .def_prop_ro(
          "footprint",
          [](const roadmaker::JunctionSurfaceSpanInfo& info) { return to_xy(info.footprint); },
          "The raw plan-view footprint ring (CCW) as (x, y) pairs — the polygon "
          "whose overlaps the sort index arbitrates.")
      .def_ro("border",
              &roadmaker::JunctionSurfaceSpanInfo::border,
              "Exact border-ring samples (x, y, z): the Dirichlet sources and "
              "the road mesh vertices the floor stitches to.")
      .def_ro("centerline",
              &roadmaker::JunctionSurfaceSpanInfo::centerline,
              "Reference-line samples (x, y, z) — the soft interior constraints.")
      .def("__repr__", [](const roadmaker::JunctionSurfaceSpanInfo& info) {
        return "JunctionSurfaceSpanInfo(road='" + info.road_odr_id +
               "', included=" + std::string(info.included ? "True" : "False") +
               ", sort_index=" + std::to_string(info.sort_index) + ")";
      });

  m.def(
      "junction_surface_spans",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_surface_spans(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Every surface span of the junction — one per connecting road, in "
      "connection order — with the exact footprint, border and centerline "
      "samples its floor was built from. The SAME gather the mesher runs, so a "
      "tool never re-implements the sampling. Empty for a stale id and for a "
      "junction with no floor to control (a span/virtual junction has no "
      "connections at all).");

  m.def(
      "surface_boundary_nodes",
      [](const roadmaker::RoadNetwork& network, roadmaker::SurfaceId surface) {
        return roadmaker::surface_boundary_nodes(network, surface);
      },
      "network"_a,
      "surface"_a,
      "The EFFECTIVE editable boundary of a ground surface. For an AUTHORED "
      "surface these are its stored nodes; for a DERIVED one they are a SEED "
      "set computed from the same footprint union the mesher fills, decimated "
      "to a handful of nodes with Catmull-Rom tangents and stored nowhere. "
      "Seed edit.set_surface_boundary from this, so the first edit starts from "
      "the shape already on screen. Empty for a stale id and for a ring that "
      "encloses no area.");

  m.def(
      "sample_surface_boundary",
      [](const std::vector<roadmaker::SurfaceNode>& nodes, double step) {
        return roadmaker::sample_surface_boundary(nodes, step);
      },
      "nodes"_a,
      "step"_a = roadmaker::kBoundarySampleStep,
      "Tessellates a closed Hermite boundary loop into a plan-view polygon of "
      "(x, y) pairs, passing exactly through every node. The single definition "
      "of what an authored boundary means geometrically. Fewer than 3 nodes "
      "yields an empty polygon.");

  m.def(
      "surface_boundary_self_intersects",
      [](const std::vector<roadmaker::SurfaceNode>& nodes) {
        return roadmaker::surface_boundary_self_intersects(nodes);
      },
      "nodes"_a,
      "True when the tessellated boundary loop crosses itself — the shape "
      "edit.set_surface_boundary refuses.");

  nb::class_<roadmaker::ManeuverSlide>(m, "ManeuverSlide")
      .def_prop_ro(
          "anchor",
          [](const roadmaker::ManeuverSlide& slide) {
            return std::pair<double, double>{slide.anchor[0], slide.anchor[1]};
          },
          "(x, y) of the anchor lane's INNER boundary — offset 0, exactly where "
          "an unauthored maneuver's endpoint sits.")
      .def_prop_ro(
          "min_point",
          [](const roadmaker::ManeuverSlide& slide) {
            return std::pair<double, double>{slide.min_point[0], slide.min_point[1]};
          },
          "(x, y) of the min_offset end of the constraint segment.")
      .def_prop_ro(
          "max_point",
          [](const roadmaker::ManeuverSlide& slide) {
            return std::pair<double, double>{slide.max_point[0], slide.max_point[1]};
          },
          "(x, y) of the max_offset end of the constraint segment.")
      .def_ro("min_offset",
              &roadmaker::ManeuverSlide::min_offset,
              "Smallest slide [m] along the arm's +t axis the endpoint may take.")
      .def_ro("max_offset",
              &roadmaker::ManeuverSlide::max_offset,
              "Largest slide [m] along the arm's +t axis the endpoint may take.")
      .def("__repr__", [](const roadmaker::ManeuverSlide& slide) {
        return "ManeuverSlide(" + std::to_string(slide.min_offset) + " .. " +
               std::to_string(slide.max_offset) + ")";
      });

  nb::class_<roadmaker::JunctionManeuverInfo>(m, "JunctionManeuverInfo")
      .def_ro("road",
              &roadmaker::JunctionManeuverInfo::road,
              "The connecting road — stable across regeneration, so it is the "
              "record key every maneuver command takes.")
      .def_ro("road_odr_id", &roadmaker::JunctionManeuverInfo::road_odr_id)
      .def_ro("from_",
              &roadmaker::JunctionManeuverInfo::from,
              "The arm face the maneuver leaves ('from' is a Python keyword).")
      .def_ro("to", &roadmaker::JunctionManeuverInfo::to, "The arm face it enters.")
      .def_ro("from_lane",
              &roadmaker::JunctionManeuverInfo::from_lane,
              "odr id of the linked lane on the incoming face.")
      .def_ro("to_lane",
              &roadmaker::JunctionManeuverInfo::to_lane,
              "odr id of the linked lane on the outgoing face.")
      .def_ro("computed",
              &roadmaker::JunctionManeuverInfo::computed,
              "TurnType derived from the arm-face headings.")
      .def_ro("effective",
              &roadmaker::JunctionManeuverInfo::effective,
              "What to show and act on: the override when there is one, else "
              "`computed`.")
      .def_ro("overridden",
              &roadmaker::JunctionManeuverInfo::overridden,
              "True when a record supplies `effective` rather than the derivation.")
      .def_ro("locked",
              &roadmaker::JunctionManeuverInfo::locked,
              "True when the record locks this road's geometry against "
              "regeneration.")
      .def_ro("authored",
              &roadmaker::JunctionManeuverInfo::authored,
              "True when the junction carries a Maneuver record for this road "
              "at all — what edit.reset_maneuver needs.")
      .def_ro("is_uturn_explicit",
              &roadmaker::JunctionManeuverInfo::is_uturn_explicit,
              "True when the maneuver returns to the arm it came from. The "
              "planner never emits such a turn, so it exists only because it "
              "was authored (edit.add_uturn_maneuver) and has no derived "
              "geometry to fall back on: it cannot be reset, only deleted.")
      .def_ro("start_heading",
              &roadmaker::JunctionManeuverInfo::start_heading,
              "Tangent [rad] leaving the incoming face — locked through a refit.")
      .def_ro("end_heading",
              &roadmaker::JunctionManeuverInfo::end_heading,
              "Tangent [rad] entering the outgoing face — locked through a refit.")
      .def_ro("start_offset",
              &roadmaker::JunctionManeuverInfo::start_offset,
              "Effective slide [m] on the incoming face, 0 when unauthored.")
      .def_ro("end_offset",
              &roadmaker::JunctionManeuverInfo::end_offset,
              "Effective slide [m] on the outgoing face, 0 when unauthored.")
      .def_ro("start_slide",
              &roadmaker::JunctionManeuverInfo::start_slide,
              "The segment the start endpoint may be dragged along.")
      .def_ro("end_slide",
              &roadmaker::JunctionManeuverInfo::end_slide,
              "The segment the end endpoint may be dragged along.")
      .def_ro("control_points",
              &roadmaker::JunctionManeuverInfo::control_points,
              "The authored INTERIOR waypoints; empty for a derived path.")
      .def_ro("path",
              &roadmaker::JunctionManeuverInfo::path,
              "The connecting road's sampled centerline (x, y, z) — the render "
              "polyline and the tool's pick polyline.")
      .def("__repr__", [](const roadmaker::JunctionManeuverInfo& info) {
        return "JunctionManeuverInfo(road='" + info.road_odr_id +
               "', effective=" + std::to_string(static_cast<int>(info.effective)) +
               (info.overridden ? ", overridden" : "") + (info.locked ? ", locked" : "") +
               (info.authored ? ", authored" : "") + ")";
      });

  m.def(
      "junction_maneuvers",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_maneuvers(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Every maneuver of the junction — one per connecting road, in connection "
      "order — with the derived turn type merged over whatever is authored, the "
      "sampled path, and the two endpoint slide constraints. The SAME solve the "
      "editor's Maneuver tool, the properties rows and the command layer's "
      "validate-first checks read, so none of them can disagree about what a "
      "maneuver is. A FOREIGN junction (no arm list) still reports its "
      "maneuvers, inspectable but not authorable. Empty for a stale id and for "
      "a SPAN (virtual) junction, which has no connections at all.");

  m.attr("SIGNAL_APPROACH_WINDOW") = roadmaker::kSignalApproachWindow;

  nb::class_<roadmaker::JunctionApproachInfo>(m, "JunctionApproachInfo")
      .def_ro("arm",
              &roadmaker::JunctionApproachInfo::arm,
              "The junction-facing end of the incoming arm — the approach's "
              "identity, matching JunctionManeuverInfo.from_ and "
              "JunctionStopLineInfo.arm.")
      .def_ro("heading",
              &roadmaker::JunctionApproachInfo::heading,
              "Travel heading [rad] of traffic reaching the junction on this "
              "arm. Opposite arms of one axis differ by ~pi, which is what the "
              "signalization templates cluster on.")
      .def_ro("s_stop",
              &roadmaker::JunctionApproachInfo::s_stop,
              "Placement anchor: the station [m] of the approach's stop line, "
              "taken from junction_stoplines so a head is never placed at a "
              "different station than the line drivers stop at.")
      .def_ro("t_center",
              &roadmaker::JunctionApproachInfo::t_center,
              "Mid-span lateral offset [m] of that stop line.")
      .def_ro("gated",
              &roadmaker::JunctionApproachInfo::gated,
              "The connecting roads this approach feeds — every "
              "junction_maneuvers entry whose from_ is this arm, in connection "
              "order. DERIVED, never stored: a head gates whatever movements "
              "leave its arm.")
      .def_ro("signal_ids",
              &roadmaker::JunctionApproachInfo::signal_ids,
              "SignalIds resolved onto this approach: signals on arm.road "
              "within SIGNAL_APPROACH_WINDOW of the mouth whose @orientation "
              "admits traffic travelling toward the junction (the matching "
              "direction, or NONE, which is valid in both). In arena creation "
              "order. Named for the kernel member, which cannot be called "
              "`signals` — Qt makes that a macro.")
      .def_ro("controller_odr_ids",
              &roadmaker::JunctionApproachInfo::controller_odr_ids,
              "odr ids of the top-level <controller>s naming at least one of "
              "`signal_ids` in a <control>, deduplicated. A <control> naming no "
              "live signal is simply never matched — the query is "
              "dormant-tolerant and never mutates.")
      .def_ro("dynamic",
              &roadmaker::JunctionApproachInfo::dynamic,
              "True when any resolved signal is @dynamic='yes' — the approach "
              "is light-controlled rather than sign-controlled.")
      .def("__repr__", [](const roadmaker::JunctionApproachInfo& info) {
        return "JunctionApproachInfo(gated=" + std::to_string(info.gated.size()) +
               ", signals=" + std::to_string(info.signal_ids.size()) +
               (info.dynamic ? ", dynamic" : "") + ")";
      });

  m.def(
      "junction_signals",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_signals(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Every APPROACH of the junction — one per incoming arm, in connection "
      "order — with the movements it gates, the placement anchor its stop line "
      "already solved, the signals resolved onto it and the controller groups "
      "those signals belong to. The SAME solve the editor's Signal tool, the "
      "Signalization rows, the signalization commands' validate-first path and "
      "(later) the phase editor read, so none of them can disagree about what "
      "an approach is. A FOREIGN junction (no arm list) still reports its "
      "approaches, inspectable but not authorable. Empty for a stale id and for "
      "a SPAN (virtual) junction, which has no connections at all.");

  // --- signal phases (p4-s8, #229) -----------------------------------------------
  // The signal CYCLE — Layer 1 (§14.6 excludes timing; ADR-0008). The derived
  // and authored cycles are reported identically; only JunctionPhasePlan.authored
  // tells them apart. mesh.junction_phases is the ONE solve the editor, Python
  // and the validator all read.
  m.attr("MAX_SIGNAL_PHASES") = roadmaker::kMaxSignalPhases;
  m.attr("MAX_SIGNAL_PHASE_STATES") = roadmaker::kMaxSignalPhaseStates;
  m.attr("MAX_SIGNAL_PHASE_DURATION") = roadmaker::kMaxSignalPhaseDuration;
  m.attr("DEFAULT_PHASE_GREEN_SECONDS") = roadmaker::kDefaultPhaseGreenSeconds;
  m.attr("DEFAULT_PHASE_YELLOW_SECONDS") = roadmaker::kDefaultPhaseYellowSeconds;
  m.attr("DEFAULT_ADDED_PHASE_SECONDS") = roadmaker::kDefaultAddedPhaseSeconds;

  nb::class_<roadmaker::PhaseControllerState>(m, "PhaseControllerState")
      .def_ro("controller_odr_id", &roadmaker::PhaseControllerState::controller_odr_id)
      .def_ro("state", &roadmaker::PhaseControllerState::state)
      .def("__repr__", [](const roadmaker::PhaseControllerState& state) {
        return "PhaseControllerState(controller_odr_id='" + state.controller_odr_id + "')";
      });

  nb::class_<roadmaker::PhaseSignalState>(m, "PhaseSignalState")
      .def_ro("signal",
              &roadmaker::PhaseSignalState::signal,
              "The live signal HEAD this state lights. Named `signal`, not "
              "`signals` — Qt makes that a macro (mirrors "
              "JunctionApproachInfo.signal_ids).")
      .def_ro("state", &roadmaker::PhaseSignalState::state, "The color it shows this phase.")
      .def("__repr__", [](const roadmaker::PhaseSignalState& state) {
        return "PhaseSignalState(signal=" + std::to_string(state.signal.index) + ")";
      });

  nb::class_<roadmaker::JunctionPhaseInfo>(m, "JunctionPhaseInfo")
      .def_ro("name", &roadmaker::JunctionPhaseInfo::name, "The phase label (may be empty).")
      .def_ro("duration", &roadmaker::JunctionPhaseInfo::duration, "Duration [s] of this phase.")
      .def_ro("start",
              &roadmaker::JunctionPhaseInfo::start,
              "Cumulative offset [s] of this phase's start within the cycle "
              "(phase 0 = 0).")
      .def_ro("states",
              &roadmaker::JunctionPhaseInfo::states,
              "EVERY member controller and the state it shows this phase, in "
              "timeline row order (Red-filled — the sparse SignalPhase.states "
              "expanded for the consumer).")
      .def_ro("signal_states",
              &roadmaker::JunctionPhaseInfo::signal_states,
              "The live signal heads and their state this phase, resolved "
              "through each controller's <control> children. COMPLETE — "
              "scrubbing needs no time call, only "
              "plan.phases[phase_index_at(plan, t)].signal_states.")
      .def_ro("moving",
              &roadmaker::JunctionPhaseInfo::moving,
              "The connecting roads whose traffic MAY proceed this phase (GW-4 "
              "step 6): a gated movement whose controlling group is Green.")
      .def("__repr__", [](const roadmaker::JunctionPhaseInfo& info) {
        return "JunctionPhaseInfo(name='" + info.name +
               "', duration=" + std::to_string(info.duration) +
               ", moving=" + std::to_string(info.moving.size()) + ")";
      });

  nb::class_<roadmaker::JunctionPhasePlan>(m, "JunctionPhasePlan")
      .def_ro("authored",
              &roadmaker::JunctionPhasePlan::authored,
              "False ⇒ the cycle is DERIVED and the junction stores no rm:phases "
              "bytes; True ⇒ read from Junction.phases. The first edit flips it.")
      .def_ro("cycle_duration",
              &roadmaker::JunctionPhasePlan::cycle_duration,
              "Sum [s] of every phase's duration — the length phase_index_at "
              "wraps over.")
      .def_ro("controller_odr_ids",
              &roadmaker::JunctionPhasePlan::controller_odr_ids,
              "The live member controllers in TIMELINE ROW order (sync-group / "
              "junction_controllers order); one row per entry.")
      .def_ro("dormant_controller_odr_ids",
              &roadmaker::JunctionPhasePlan::dormant_controller_odr_ids,
              "Controller ids named by AUTHORED states that are NOT live members "
              "— dormant references the writer prunes and the validator advises "
              "on. Always empty on a derived plan.")
      .def_ro("phases",
              &roadmaker::JunctionPhasePlan::phases,
              "The effective phases in cycle order. Empty ⇒ nothing to time "
              "(a static, unsignalized or span junction: 'signalize first').")
      .def("__repr__", [](const roadmaker::JunctionPhasePlan& plan) {
        return "JunctionPhasePlan(authored=" + std::string(plan.authored ? "True" : "False") +
               ", phases=" + std::to_string(plan.phases.size()) +
               ", cycle=" + std::to_string(plan.cycle_duration) + ")";
      });

  m.def(
      "junction_phases",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::junction_phases(network, junction);
      },
      "network"_a,
      "junction"_a,
      "The effective signal CYCLE of a junction — DERIVED from its sync group "
      "when the junction stores no phases (green+yellow per axis), resolved from "
      "Junction.phases when it does. The SAME solve the phase editor, the "
      "viewport overlay and the validator read, so none can disagree about the "
      "cycle. Returns an empty plan (authored=False, no phases) for a stale id, "
      "a SPAN (virtual) junction, an unsignalized junction and a STATIC-template "
      "junction (no controllers) — none has a cycle to time. A FOREIGN dynamic "
      "junction still resolves, off the sync-group references, not the arms.");
  m.def(
      "phase_index_at",
      [](const roadmaker::JunctionPhasePlan& plan, double t) {
        return roadmaker::phase_index_at(plan, t);
      },
      "plan"_a,
      "t"_a,
      "The index of the phase active at cycle time `t` [s], wrapping over "
      "plan.cycle_duration (negative t and t >= cycle handled by modulo). This "
      "is all scrubbing needs — state is piecewise-constant, so "
      "plan.phases[phase_index_at(plan, t)].signal_states is complete. Returns "
      "SIZE_MAX when the plan has no phases.");

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

  nb::class_<roadmaker::StyleLane>(m, "StyleLane")
      .def(nb::init<>())
      .def(
          "__init__",
          [](roadmaker::StyleLane* self,
             roadmaker::LaneType type,
             roadmaker::Poly3 width,
             std::optional<roadmaker::RoadMark> outer_mark) {
            new (self) roadmaker::StyleLane{
                .type = type, .width = width, .outer_mark = std::move(outer_mark)};
          },
          "type"_a = roadmaker::LaneType::Driving,
          "width"_a = roadmaker::Poly3{.a = 3.5},
          "outer_mark"_a = std::nullopt)
      .def_rw("type", &roadmaker::StyleLane::type)
      .def_rw("width", &roadmaker::StyleLane::width)
      .def_rw("outer_mark",
              &roadmaker::StyleLane::outer_mark,
              "RoadMark on this lane's OUTER boundary; None paints nothing.");

  nb::class_<roadmaker::RoadStyle>(m, "RoadStyle")
      .def(nb::init<>())
      .def_rw("left", &roadmaker::RoadStyle::left)
      .def_rw("right", &roadmaker::RoadStyle::right)
      .def_rw("center_mark", &roadmaker::RoadStyle::center_mark)
      .def_static("urban_two_lane",
                  &roadmaker::RoadStyle::urban_two_lane,
                  "Two driving lanes each way, dashed white same-direction lines, solid "
                  "white edges, solid yellow center.")
      .def_static("two_lane_rural",
                  &roadmaker::RoadStyle::two_lane_rural,
                  "One driving lane each way, solid white edges, right-hand shoulder.")
      .def_static("highway",
                  &roadmaker::RoadStyle::highway,
                  "Two driving lanes each way, dashed lane lines, wide shoulders, no "
                  "center mark.");

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
      "fit_forward_clothoid",
      [](std::pair<double, double> start,
         double heading,
         double curvature,
         std::pair<double, double> to) {
        return unwrap(roadmaker::fit_forward_clothoid(
            roadmaker::Waypoint{.x = start.first, .y = start.second},
            heading,
            curvature,
            roadmaker::Waypoint{.x = to.first, .y = to.second}));
      },
      "start"_a,
      "heading"_a,
      "curvature"_a,
      "to"_a,
      "Fits a single clothoid leaving `start` at a fixed heading [rad] AND "
      "curvature [1/m] and passing through `to` (the forward problem). The result "
      "is curvature-continuous at the start — the connector edit.extend_road "
      "appends. Returns a ReferenceLine; raises ValueError when `to` cannot be "
      "reached (behind the start pose).");

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
      .def_ro("topology", &roadmaker::edit::DirtySet::topology)
      .def_ro("junctions_are_current", &roadmaker::edit::DirtySet::junctions_are_current);

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
  nb::class_<roadmaker::edit::CrosswalkParams>(edit, "CrosswalkParams")
      .def(nb::init<>())
      .def_rw("depth_m", &roadmaker::edit::CrosswalkParams::depth_m)
      .def_rw("setback_m", &roadmaker::edit::CrosswalkParams::setback_m)
      .def_rw("border_width_m", &roadmaker::edit::CrosswalkParams::border_width_m)
      .def_rw("dash_length_m",
              &roadmaker::edit::CrosswalkParams::dash_length_m,
              "Stripe length [m]; 0 = solid.")
      .def_rw("dash_gap_m", &roadmaker::edit::CrosswalkParams::dash_gap_m)
      .def_rw("material", &roadmaker::edit::CrosswalkParams::material)
      .def_rw("color", &roadmaker::edit::CrosswalkParams::color)
      .def_rw("asset", &roadmaker::edit::CrosswalkParams::asset)
      .def_rw("category", &roadmaker::edit::CrosswalkParams::category);
  edit.def(
      "junction_crosswalks",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::edit::CrosswalkParams& params) {
        return roadmaker::edit::junction_crosswalks(network, junction, params);
      },
      "network"_a,
      "junction"_a,
      "params"_a = roadmaker::edit::CrosswalkParams{},
      "One zebra crosswalk Object per arm of the junction, spanning its driving "
      "lanes just inside it. `params` carries the parametric-asset fields "
      "(stripe geometry, material, asset key) materialized into the object's "
      "outline + <markings> + rm:crosswalk userData. Returns a list of "
      "(RoadId, Object); add each with edit.add_object (one undo step).");
  edit.def(
      "apply_crosswalk_asset",
      [](roadmaker::Object object, const roadmaker::edit::CrosswalkParams& params) {
        roadmaker::edit::apply_crosswalk_asset(object, params);
        return object;
      },
      "object"_a,
      "params"_a,
      "Rebuilds a copy of `object`'s crosswalk outline + <markings> + "
      "rm:crosswalk userData from `params` (reads s/t/length placement, writes "
      "@width = depth) and returns it — the authoring path shared by "
      "junction_crosswalks and the editor's asset re-materialization.");
  nb::class_<roadmaker::edit::MarkingCurveParams>(edit, "MarkingCurveParams")
      .def(nb::init<>())
      .def_rw("width_m", &roadmaker::edit::MarkingCurveParams::width_m)
      .def_rw("dash_length_m",
              &roadmaker::edit::MarkingCurveParams::dash_length_m,
              "Visible run [m]; 0 = solid.")
      .def_rw("dash_gap_m", &roadmaker::edit::MarkingCurveParams::dash_gap_m)
      .def_rw("material", &roadmaker::edit::MarkingCurveParams::material)
      .def_rw("color", &roadmaker::edit::MarkingCurveParams::color)
      .def_rw("asset", &roadmaker::edit::MarkingCurveParams::asset)
      .def_rw("category", &roadmaker::edit::MarkingCurveParams::category)
      .def_rw("striped",
              &roadmaker::edit::MarkingCurveParams::striped,
              "True paints a striped crosswalk band; False a plain line marking.");
  edit.def(
      "apply_marking_curve_asset",
      [](roadmaker::Object object,
         std::vector<std::array<double, 2>> centerline,
         const roadmaker::edit::MarkingCurveParams& params) {
        unwrap(roadmaker::edit::apply_marking_curve_asset(object, centerline, params));
        return object;
      },
      "object"_a,
      "centerline"_a,
      "params"_a,
      "Authors a copy of `object` as a free-form marking curve from a road-frame "
      "(s,t) `centerline` polyline (outline + <markings> + rm:markingCurve "
      "userData) and returns it. Raises ValueError for fewer than two samples or "
      "a bend tighter than half the width.");
  nb::class_<roadmaker::edit::StencilParams>(edit, "StencilParams")
      .def(nb::init<>())
      .def_rw(
          "subtype", &roadmaker::edit::StencilParams::subtype, "One of the 6 core arrow subtypes.")
      .def_rw("length_m", &roadmaker::edit::StencilParams::length_m)
      .def_rw("width_m", &roadmaker::edit::StencilParams::width_m)
      .def_rw("material", &roadmaker::edit::StencilParams::material)
      .def_rw("color", &roadmaker::edit::StencilParams::color)
      .def_rw("asset", &roadmaker::edit::StencilParams::asset)
      .def_rw("category", &roadmaker::edit::StencilParams::category);
  edit.def(
      "apply_stencil_asset",
      [](roadmaker::Object object, const roadmaker::edit::StencilParams& params) {
        unwrap(roadmaker::edit::apply_stencil_asset(object, params));
        return object;
      },
      "object"_a,
      "params"_a,
      "Authors a copy of `object` as a point stencil (one closed cornerLocal arrow "
      "outline + <material> + rm:stencil userData) and returns it. Raises "
      "ValueError for a subtype outside the 6-arrow core set.");
  edit.def(
      "arrow_glyph_outline",
      [](std::string_view subtype, double length_m, double width_m) {
        return roadmaker::edit::arrow_glyph_outline(subtype, length_m, width_m);
      },
      "subtype"_a,
      "length_m"_a,
      "width_m"_a,
      "The closed cornerLocal outline of one arrow glyph in the object's local "
      "(u,v) frame. Empty for a subtype outside the 6-arrow core set.");
  edit.def(
      "junction_lane_arrows",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::optional<std::function<std::string(roadmaker::RoadId, int)>> glyph) {
        roadmaker::edit::LaneArrowParams params;
        if (glyph.has_value()) {
          // Hand Python the lane's odr id rather than the ContactLane: the id is
          // what a turn decision keys on, and it keeps ContactLane unbound.
          params.glyph = [choose = std::move(*glyph)](
                             roadmaker::RoadId arm,
                             const roadmaker::edit::ContactLane& lane) -> std::string {
            return choose(arm, lane.odr_id);
          };
        }
        return roadmaker::edit::junction_lane_arrows(network, junction, params);
      },
      "network"_a,
      "junction"_a,
      "glyph"_a = nb::none(),
      "One lane-arrow Object on each approach lane of every arm, pointing into "
      "the junction. `glyph` is an optional callable (road, lane_odr_id) -> str "
      "choosing the arrow subtype per approach lane ('arrowLeft', "
      "'arrowStraight', 'arrowRight'); return '' or omit it for arrowStraight "
      "everywhere. Returns a list of (RoadId, Object); add each with "
      "edit.add_object.");
  edit.def(
      "junction_center_marks",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadMarkType type,
         roadmaker::RoadMarkColor color,
         double width) {
        return roadmaker::edit::junction_center_marks(
            network, junction, roadmaker::edit::CenterMarkParams{type, color, width});
      },
      "network"_a,
      "junction"_a,
      "type"_a = roadmaker::RoadMarkType::SolidSolid,
      "color"_a = roadmaker::RoadMarkColor::Yellow,
      "width"_a = 0.12,
      "The centre-line RoadMark for lane 0 of every arm of the junction — a "
      "double-yellow centre by default. Returns a list of (LaneId, RoadMark); "
      "apply each with edit.set_road_mark.");
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
  edit.def(
      "extend_road",
      [](const roadmaker::RoadNetwork& network,
         const roadmaker::RoadEnd& end,
         std::pair<double, double> to) {
        return roadmaker::edit::extend_road(
            network, end, roadmaker::Waypoint{.x = to.first, .y = to.second});
      },
      "network"_a,
      "end"_a,
      "to"_a,
      "Extends a road past `end` with a curvature-continuous forward clothoid "
      "through `to` (elevation continued with matching z and grade). An END "
      "extension appends the fit; a START extension prepends the reversed fit "
      "and re-bases every s-indexed thing (sections, elevation, superelevation, "
      "lane_offset, objects, signals, waypoints) by the extension length.");
  edit.def(
      "create_teed_road",
      [](const roadmaker::RoadNetwork& network,
         const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         roadmaker::RoadId target,
         double s,
         roadmaker::ContactPoint teed_end,
         std::optional<double> start_heading,
         std::optional<double> end_heading) {
        return roadmaker::edit::create_teed_road(network,
                                                 to_waypoints(waypoints),
                                                 profile,
                                                 std::move(name),
                                                 target,
                                                 s,
                                                 teed_end,
                                                 {.start = start_heading, .end = end_heading});
      },
      "network"_a,
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "target"_a,
      "s"_a,
      "teed_end"_a,
      "start_heading"_a = nb::none(),
      "end_heading"_a = nb::none(),
      "Authors a clothoid road AND tees its `teed_end` into the side of `target` "
      "at station s in one undoable command (create + attach_t_junction).");
  edit.def(
      "create_crossing_road",
      [](const roadmaker::RoadNetwork& network,
         const std::vector<std::pair<double, double>>& waypoints,
         const roadmaker::LaneProfile& profile,
         std::string name,
         roadmaker::RoadId target,
         std::optional<double> start_heading,
         std::optional<double> end_heading) {
        return roadmaker::edit::create_crossing_road(network,
                                                     to_waypoints(waypoints),
                                                     profile,
                                                     std::move(name),
                                                     target,
                                                     {.start = start_heading, .end = end_heading});
      },
      "network"_a,
      "waypoints"_a,
      "profile"_a,
      "name"_a = "",
      "target"_a,
      "start_heading"_a = nb::none(),
      "end_heading"_a = nb::none(),
      "Authors a clothoid road that crosses `target`, then forms a 4-way junction "
      "at the crossing in one undoable command (create + assembly.cross_roads).");
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
  edit.def("rotate_road",
           &roadmaker::edit::rotate_road,
           "network"_a,
           "road"_a,
           "angle"_a,
           "pivot_x"_a,
           "pivot_y"_a,
           "Rotates a whole road about the world pivot (pivot_x, pivot_y) by angle "
           "[rad] CCW; rigid (elevation and shape coefficients unchanged), breaks "
           "links to non-rotating roads and refuses junction roads.");
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
  nb::enum_<roadmaker::edit::TurnSetPolicy>(edit, "TurnSetPolicy")
      .value("ALLOW_CHANGE", roadmaker::edit::TurnSetPolicy::AllowChange)
      .value("IN_PLACE_ONLY", roadmaker::edit::TurnSetPolicy::InPlaceOnly);
  edit.def(
      "regenerate_junction",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::edit::JunctionGenOptions& options,
         roadmaker::edit::TurnSetPolicy policy) {
        return roadmaker::edit::regenerate_junction(network, junction, options, policy);
      },
      "network"_a,
      "junction"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "policy"_a = roadmaker::edit::TurnSetPolicy::AllowChange,
      "Re-runs the generator from the junction's recorded arms. Under "
      "ALLOW_CHANGE (default) a lane added, removed, or retyped on an incoming "
      "road grows or shrinks the turn set; the turns that survive keep their "
      "connecting-road ids. IN_PLACE_ONLY refuses any turn-set change.");
  edit.def(
      "set_corner_radius",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::RoadEnd& arm_a,
         const roadmaker::RoadEnd& arm_b,
         double radius) {
        return roadmaker::edit::set_corner_radius(network, junction, arm_a, arm_b, radius);
      },
      "network"_a,
      "junction"_a,
      "arm_a"_a,
      "arm_b"_a,
      "radius"_a,
      "Authors the fillet radius [m] of ONE junction corner, named by its "
      "adjacent arm pair. radius <= 0 clears the override (and any extents on "
      "that corner) so the derived radius applies again. Pushing raises "
      "ValueError for a stale junction, a non-adjacent arm pair, or a clear "
      "with no override to remove.");
  edit.def(
      "set_corner_extents",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::RoadEnd& arm_a,
         const roadmaker::RoadEnd& arm_b,
         double extent_a,
         double extent_b) {
        return roadmaker::edit::set_corner_extents(
            network, junction, arm_a, arm_b, extent_a, extent_b);
      },
      "network"_a,
      "junction"_a,
      "arm_a"_a,
      "arm_b"_a,
      "extent_a"_a,
      "extent_b"_a,
      "Authors the two tangent-leg setbacks [m] of ONE junction corner "
      "independently; the curve stays G1-tangent to both edges. Pushing raises "
      "ValueError for a non-positive extent, a stale junction, or a "
      "non-adjacent arm pair.");
  edit.def(
      "set_corner_sidewalk_material",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::RoadEnd& arm_a,
         const roadmaker::RoadEnd& arm_b,
         std::string material) {
        return roadmaker::edit::set_corner_sidewalk_material(
            network, junction, arm_a, arm_b, std::move(material));
      },
      "network"_a,
      "junction"_a,
      "arm_a"_a,
      "arm_b"_a,
      "material"_a,
      "Authors the sidewalk overlay material of ONE junction corner — a bare "
      "catalog name such as 'concrete'. An empty material clears the slot. The "
      "corner geometry is untouched; the mesher emits the sidewalk overlay only "
      "while a material is authored. Pushing raises ValueError for a stale "
      "junction, a non-adjacent arm pair, a clear with nothing authored, or a "
      "name outside [A-Za-z0-9_.-]+ (':', ';' and spaces are rejected — the "
      "persistence grammar joins on them and does not escape).");
  edit.def(
      "set_corner_median_material",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::RoadEnd& arm_a,
         const roadmaker::RoadEnd& arm_b,
         std::string material) {
        return roadmaker::edit::set_corner_median_material(
            network, junction, arm_a, arm_b, std::move(material));
      },
      "network"_a,
      "junction"_a,
      "arm_a"_a,
      "arm_b"_a,
      "material"_a,
      "The median-nose counterpart of set_corner_sidewalk_material: an arm's "
      "nose takes the material of the corner where that arm is arm_a, falling "
      "back to the corner where it is arm_b. Empty clears; same token rule and "
      "same ValueError cases.");
  edit.def(
      "set_junction_default_corner_radius",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction, double radius) {
        return roadmaker::edit::set_junction_default_corner_radius(network, junction, radius);
      },
      "network"_a,
      "junction"_a,
      "radius"_a,
      "Authors the junction-wide fillet radius [m] — the fallback every corner "
      "without its own radius uses. Resolution order is per-corner override > "
      "this default > derived. radius <= 0 clears the default. The value is "
      "stored uncapped and clamped to each corner's geometry only at mesh time. "
      "Pushing raises ValueError for a stale junction or a clear with no "
      "default set.");
  edit.def(
      "set_junction_material",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::string material) {
        return roadmaker::edit::set_junction_material(network, junction, std::move(material));
      },
      "network"_a,
      "junction"_a,
      "material"_a,
      "Authors the junction carriageway (floor) material — a bare catalog name, "
      "empty to clear. Pushing raises ValueError for a stale junction, a clear "
      "when the material is already empty, or a name outside [A-Za-z0-9_.-]+.");
  edit.def(
      "set_stopline_distance",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadEnd arm,
         double distance,
         std::optional<std::string> crosswalk_link) {
        return roadmaker::edit::set_stopline_distance(
            network, junction, arm, distance, std::move(crosswalk_link));
      },
      "network"_a,
      "junction"_a,
      "arm"_a,
      "distance"_a,
      "crosswalk_link"_a = nb::none(),
      "Authors the setback [m] of ONE arm's stop line, creating the record when "
      "the arm is still fully derived. The value is stored UNCLAMPED and clamped "
      "to the road only when solved. `crosswalk_link` records the odr id of a "
      "crosswalk placed alongside; None leaves any existing link alone. Pushing "
      "raises ValueError for a stale junction, a road end that is not a stop line "
      "of it, or a negative/non-finite distance.");
  edit.def(
      "flip_stopline",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadEnd arm) { return roadmaker::edit::flip_stopline(network, junction, arm); },
      "network"_a,
      "junction"_a,
      "arm"_a,
      "Toggles which travel direction ONE arm's stop line spans (approach lanes "
      "by default, outgoing when flipped). A record left back at its defaults is "
      "erased, so flipping twice is byte-identical to never having flipped. "
      "Pushing raises ValueError for a stale junction, a road end that is not a "
      "stop line of it, or a direction with no driving lanes to span.");
  edit.def(
      "reset_stopline",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadEnd arm) {
        return roadmaker::edit::reset_stopline(network, junction, arm);
      },
      "network"_a,
      "junction"_a,
      "arm"_a,
      "Drops ONE arm's authored stop-line record, returning it to the derived "
      "default. Pushing raises ValueError for a stale junction, a road end that "
      "is not a stop line of it, or an arm with nothing authored to reset.");
  edit.def(
      "set_surface_span_included",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         bool included) {
        return roadmaker::edit::set_surface_span_included(network, junction, road, included);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "included"_a,
      "Authors whether ONE connecting road's samples take part in the junction "
      "floor's elevation and triangulation. Excluding is SAMPLES-ONLY: the "
      "footprint stays in the union, so coverage and the exported <boundary> "
      "never change. A record left back at its defaults is erased, so toggling "
      "twice is byte-identical to never having touched the span. Pushing raises "
      "ValueError for a stale junction, a road that is not a surface span of it "
      "(a span/virtual junction has none), or a no-op against the effective "
      "value.");
  edit.def(
      "set_surface_span_sort_index",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         int sort_index) {
        return roadmaker::edit::set_surface_span_sort_index(network, junction, road, sort_index);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "sort_index"_a,
      "Authors ONE connecting road's precedence where span footprints OVERLAP — "
      "higher wins. A free integer bounded by +/-1000; the editor's Raise/Lower "
      "are just this with current +/- 1, so an index survives regeneration "
      "without renumbering. Same validation and erase-at-default rule as "
      "edit.set_surface_span_included.");
  // --- maneuvers (p4-s6, #227) --------------------------------------------
  // A maneuver is ONE connecting road's path through a junction. Every factory
  // validates through the same junction_maneuvers solve the query exposes, and
  // every one erases a record left authoring nothing, so an edit-and-undo pair
  // writes the original bytes.
  edit.def(
      "set_maneuver_locked",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         bool locked) {
        return roadmaker::edit::set_maneuver_locked(network, junction, road, locked);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "locked"_a,
      "Locks or unlocks ONE maneuver's geometry against regeneration — the "
      "finer-grained sibling of edit.set_junction_locked, and the 'convert to "
      "explicit' verb on a derived maneuver. A locked maneuver keeps its plan "
      "view, length, elevation and lane width through an explicit "
      "regenerate_junction, and its connecting road is kept even when the plan "
      "no longer contains its turn. Pushing raises ValueError for a stale "
      "junction, a road that is not a maneuver of it, or the maneuver already "
      "being in that lock state.");
  edit.def(
      "set_maneuver_turn_type",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         std::optional<roadmaker::TurnType> turn_type) {
        return roadmaker::edit::set_maneuver_turn_type(network, junction, road, turn_type);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "turn_type"_a,
      "Overrides ONE maneuver's turn type, or clears the override with None. "
      "The type is otherwise DERIVED from the arm-face headings (ASAM "
      "OpenDRIVE has no carrier for it), so the override is purely semantic and "
      "never moves geometry — which is why edit.rebuild_maneuvers keeps it. "
      "Storing the COMPUTED value clears the override rather than pinning it. "
      "Pushing raises ValueError for a stale junction, a road that is not a "
      "maneuver of it, clearing an override that does not exist, or setting the "
      "type the maneuver already reports.");
  edit.def(
      "set_maneuver_path",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         const std::vector<std::pair<double, double>>& control_points,
         std::optional<double> start_offset,
         std::optional<double> end_offset) {
        const std::vector<roadmaker::Waypoint> points = to_waypoints(control_points);
        return roadmaker::edit::set_maneuver_path(
            network, junction, road, points, start_offset, end_offset);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "control_points"_a,
      "start_offset"_a = nb::none(),
      "end_offset"_a = nb::none(),
      "Reshapes ONE maneuver: its INTERIOR (x, y) control points plus the "
      "endpoint slides along the two arm faces (None leaves a slide alone). THE "
      "geometry command — add point, move point, insert point and endpoint drag "
      "all compose into it. The path is refitted as a G1 clothoid chain with the "
      "END HEADINGS LOCKED to the arm faces, so a reshaped maneuver still meets "
      "its arms tangentially (§12.4.2), and the road's length, elevation and "
      "blended lane width are rewritten together. Applying it LOCKS the maneuver "
      "in the SAME undo step. Pushing raises ValueError for a stale junction, a "
      "road that is not a maneuver of it, more than 64 control points, a "
      "non-finite coordinate or offset, an offset outside the anchor lane's "
      "span (see ManeuverSlide), a missing anchor lane, a failed refit, or a "
      "request that changes nothing.");
  edit.def(
      "reset_maneuver",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadId road,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::reset_maneuver(network, junction, road, options);
      },
      "network"_a,
      "junction"_a,
      "road"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Drops ONE maneuver's authored record and replans its connecting road "
      "from the junction's arms — the per-maneuver 'back to derived'. Pushing "
      "raises ValueError for a stale junction, a road that is not a maneuver of "
      "it, a maneuver with nothing authored to reset, a FOREIGN junction (no "
      "arm list to replan from), or an EXPLICIT U-turn, which the planner never "
      "emits and so has no derived geometry to fall back on — delete its "
      "connecting road instead.");
  edit.def(
      "rebuild_maneuvers",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::rebuild_maneuvers(network, junction, options);
      },
      "network"_a,
      "junction"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Replans the WHOLE junction ignoring every maneuver lock: hand-shaped "
      "geometry is replaced by the derivation, connecting roads the plan no "
      "longer contains are dropped (explicit U-turns included), and the "
      "records' geometric fields are cleared. turn_type overrides SURVIVE — "
      "they are semantic, not geometric. Pushing raises ValueError for a stale "
      "junction, a FOREIGN or SPAN junction, or a junction with no locked or "
      "hand-shaped maneuver to rebuild.");
  edit.def(
      "add_uturn_maneuver",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::RoadEnd& arm,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::add_uturn_maneuver(network, junction, arm, options);
      },
      "network"_a,
      "junction"_a,
      "arm"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Adds the one maneuver the planner never emits: a U-turn on `arm`, from "
      "its innermost incoming driving lane back to its innermost outgoing one. "
      "The connecting road, the connection-table entry and a LOCKED Maneuver "
      "record are created together — the lock is what keeps the next "
      "regeneration from dropping a turn no plan contains. Pushing raises "
      "ValueError for a stale junction, a FOREIGN or SPAN junction, a road end "
      "that is not an arm of it, no driving lane in one of the two directions, "
      "an existing same-arm connection, or a failed connector fit (a U-turn "
      "between two adjacent lanes can legitimately be too tight).");

  // --- signalization (p4-s7, #228) ----------------------------------------
  // Two commands, both ONE undo step, both validating through the same
  // junction_signals solve the query exposes. Signalizing REPLACES: anything a
  // previous signalization authored on the junction is removed first, so
  // switching templates never leaves two generations of heads behind — and
  // re-applying the identical template is refused, because the command layer's
  // round-trip oracle forbids a command that changes nothing.
  nb::enum_<roadmaker::edit::SignalizeTemplate>(edit, "SignalizeTemplate")
      .value("FOUR_WAY_PROTECTED_LEFT",
             roadmaker::edit::SignalizeTemplate::FourWayProtectedLeft,
             "Dynamic. One head per approach plus a protected-left head on "
             "every approach that has a left turn; per axis, one controller for "
             "the through/right heads and one for the protected-left heads.")
      .value("TWO_PHASE",
             roadmaker::edit::SignalizeTemplate::TwoPhase,
             "Dynamic. One head per approach and ONE controller per axis — "
             "permissive lefts, no separate left group.")
      .value("ALL_WAY_STOP",
             roadmaker::edit::SignalizeTemplate::AllWayStop,
             "Static. A stop sign on every approach and NO controllers at all: "
             "an all-way stop has no phases, so no phase data is created.")
      .value("TWO_WAY_STOP",
             roadmaker::edit::SignalizeTemplate::TwoWayStop,
             "Static. Stop signs on the MINOR axis only, and no controllers. "
             "Needs at least two axes.");

  nb::class_<roadmaker::edit::SignalizeOptions>(edit, "SignalizeOptions")
      .def(nb::init<>())
      .def_rw("tmpl", &roadmaker::edit::SignalizeOptions::tmpl)
      .def_rw("mount_model",
              &roadmaker::edit::SignalizeOptions::mount_model,
              "Optional prop model id placed as an <object> alongside each "
              "authored signal and recorded in the junction's signal_mounts; "
              "empty places nothing. An unknown id is refused before anything "
              "is placed.")
      .def_rw("lateral_offset",
              &roadmaker::edit::SignalizeOptions::lateral_offset,
              "Lateral clearance [m] between the outboard edge of the "
              "approach's stop band and the head, on the driver's right. NOT "
              "persisted, so re-applying the same template with a different "
              "offset is still a no-op and still refused.");

  edit.def(
      "signalize_junction",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         const roadmaker::edit::SignalizeOptions& options) {
        return roadmaker::edit::signalize_junction(network, junction, options);
      },
      "network"_a,
      "junction"_a,
      "options"_a = roadmaker::edit::SignalizeOptions{},
      "Auto-signalizes a junction from a template: places the <signal>s, groups "
      "the dynamic ones into top-level <controller>s (§14.6), references those "
      "controllers from the junction's synchronization group (§12.14), and "
      "records the applied template plus any signal->prop mounts. Static "
      "templates create ZERO controllers — a stop-controlled junction has no "
      "phases. Pushing raises ValueError for a stale junction, a FOREIGN one "
      "(no arms — recreate it to edit), a SPAN/virtual one (which shall not "
      "have controllers, asam.net:xodr:1.9.0:junctions.virtual.no_controllers), "
      "a junction with no solvable approach, an unknown mount_model, "
      "TWO_WAY_STOP on fewer than two axes, and the junction already carrying "
      "exactly this template and mount model.");
  edit.def(
      "clear_signalization",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::edit::clear_signalization(network, junction);
      },
      "network"_a,
      "junction"_a,
      "The exact inverse: erases the signals, controllers and mount props a "
      "signalization authored on the junction and clears its records. What "
      "counts as 'authored here' is DERIVED, never a hidden flag — the "
      "synchronization group's controllers and the signals they name, "
      "everything in signal_mounts, and, for a template junction, the signals "
      "within SIGNAL_APPROACH_WINDOW of its approaches carrying that template's "
      "catalog code. A hand-placed sign of another type is left alone. Pushing "
      "raises ValueError for a stale junction and for one with nothing "
      "signalized.");

  // --- signal phases (p4-s8, #229) ----------------------------------------
  // Six pure junction-value edits over Junction.phases — no geometry moves, so
  // the turn set is untouched. All share phase_edit_context: they reject a
  // stale id, a SPAN junction and an EMPTY plan ('signalize first'), and on the
  // FIRST edit they MATERIALIZE the derived cycle sparsely into the junction so
  // junction_phases().authored flips true while the shape is preserved. The
  // round-trip oracle forbids no-op commands, so every factory rejects an edit
  // that would change nothing.
  edit.def(
      "set_phase_duration",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::size_t phase_index,
         double duration) {
        return roadmaker::edit::set_phase_duration(network, junction, phase_index, duration);
      },
      "network"_a,
      "junction"_a,
      "phase_index"_a,
      "duration"_a,
      "Sets phase `phase_index`'s duration [s]. Raises ValueError for a "
      "non-finite, <= 0 or > MAX_SIGNAL_PHASE_DURATION value, an out-of-range "
      "index, and a value equal to the phase's effective (possibly derived) "
      "duration (the no-op the oracle forbids).");
  edit.def(
      "set_phase_state",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::size_t phase_index,
         std::string controller_odr_id,
         roadmaker::SignalState state) {
        return roadmaker::edit::set_phase_state(
            network, junction, phase_index, std::move(controller_odr_id), state);
      },
      "network"_a,
      "junction"_a,
      "phase_index"_a,
      "controller_odr_id"_a,
      "state"_a,
      "Sets one controller's state within phase `phase_index`. Setting it to RED "
      "ERASES the controller's sparse pair (Red is the omitted default). Raises "
      "ValueError for an out-of-range index, a controller_odr_id that is not a "
      "live member of the junction's sync group or fails the [A-Za-z0-9_.-]+ "
      "token alphabet, and a state equal to the controller's effective state.");
  edit.def(
      "add_signal_phase",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::size_t phase_index) {
        return roadmaker::edit::add_signal_phase(network, junction, phase_index);
      },
      "network"_a,
      "junction"_a,
      "phase_index"_a,
      "Inserts an all-red phase (DEFAULT_ADDED_PHASE_SECONDS, empty state list) "
      "at `phase_index` in 0..count (count appends). Raises ValueError for an "
      "out-of-range index and a resulting count exceeding MAX_SIGNAL_PHASES.");
  edit.def(
      "duplicate_signal_phase",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::size_t phase_index) {
        return roadmaker::edit::duplicate_signal_phase(network, junction, phase_index);
      },
      "network"_a,
      "junction"_a,
      "phase_index"_a,
      "Deep-copies phase `phase_index` to phase_index + 1. Raises ValueError for "
      "an out-of-range index and a resulting count exceeding MAX_SIGNAL_PHASES.");
  edit.def(
      "remove_signal_phase",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         std::size_t phase_index) {
        return roadmaker::edit::remove_signal_phase(network, junction, phase_index);
      },
      "network"_a,
      "junction"_a,
      "phase_index"_a,
      "Removes phase `phase_index`. Raises ValueError for an out-of-range index "
      "and — because a zero-phase authored cycle is unrepresentable — for "
      "removing the LAST remaining phase (use clear_signal_phases to return to "
      "the derived cycle instead).");
  edit.def(
      "clear_signal_phases",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction) {
        return roadmaker::edit::clear_signal_phases(network, junction);
      },
      "network"_a,
      "junction"_a,
      "Clears the authored cycle, returning the junction to its DERIVED cycle "
      "(AUTHORS-NOTHING ⇒ ERASE). Unlike the other five this BYPASSES the "
      "empty-plan rejection — a de-signalized junction carrying only dormant "
      "phases must stay clearable — and raises ValueError only when "
      "Junction.phases is already empty (nothing to clear).");

  edit.def(
      "set_junction_locked",
      [](const roadmaker::RoadNetwork& network, roadmaker::JunctionId junction, bool locked) {
        return roadmaker::edit::set_junction_locked(network, junction, locked);
      },
      "network"_a,
      "junction"_a,
      "locked"_a,
      "Locks or unlocks a junction against the AUTOMATIC regeneration loops, so "
      "hand-tuned connections, corners and stop lines survive edits to its "
      "arms; regenerate_junction still re-derives it on demand. Unlocking a "
      "junction whose arms no longer plan REMOVES it together with its "
      "connecting roads (the delete_junction closure), since there is no "
      "automatic state left to hand back to. Pushing raises ValueError for a "
      "stale junction, a state that is already what was asked for, a foreign "
      "junction (no arms and no spans), or unlocking a span junction, whose "
      "lock is structural.");
  edit.def(
      "add_junction_arm",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadEnd end,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::add_junction_arm(network, junction, end, options);
      },
      "network"_a,
      "junction"_a,
      "end"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Adds a road end to a LOCKED junction's arm list and retargets it: the "
      "turns it opens get fresh connecting roads and every surviving turn keeps "
      "its connecting-road id. The lock is a precondition — an automatic "
      "junction re-derives its arms, so the edit would not survive. Pushing "
      "raises ValueError for a stale junction, a span or foreign junction, an "
      "unlocked junction, an end that is already an arm, an end owned by "
      "another junction, an occupied link slot, or arms the generator refuses "
      "(notably ends farther apart than options.max_end_distance_m).");
  edit.def(
      "remove_junction_arm",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId junction,
         roadmaker::RoadEnd end,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::remove_junction_arm(network, junction, end, options);
      },
      "network"_a,
      "junction"_a,
      "end"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Removes a road end from a LOCKED junction's arm list, freeing its link "
      "slot and erasing the connecting roads that served turns through it. "
      "Authored corners and stop lines naming that arm STAY on the junction: "
      "they go dormant and reactivate if the arm is added back. Pushing raises "
      "ValueError for a stale junction, a span or foreign junction, an unlocked "
      "junction, an end that is not an arm, fewer than 2 arms left afterwards, "
      "or arms the generator refuses.");
  edit.def(
      "merge_junctions",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::JunctionId survivor,
         roadmaker::JunctionId absorbed,
         const roadmaker::edit::JunctionGenOptions& options) {
        return roadmaker::edit::merge_junctions(network, survivor, absorbed, options);
      },
      "network"_a,
      "survivor"_a,
      "absorbed"_a,
      "options"_a = roadmaker::edit::JunctionGenOptions{},
      "Folds `absorbed` into `survivor` — one junction over the union of both "
      "arm lists. The survivor keeps its odr id, name, default corner radius "
      "and material and inherits the absorbed junction's authored corners and "
      "stop lines; connecting roads whose turn still plans keep their ids. The "
      "result is LOCKED. Pushing raises ValueError for a stale id on either "
      "side, the same junction twice, a span or foreign junction, either side "
      "with fewer than 2 arms, or a union the generator refuses (notably ends "
      "farther apart than options.max_end_distance_m — what 'neighbouring' "
      "means here).");
  edit.def(
      "create_span_junction",
      [](const roadmaker::RoadNetwork& network, const std::vector<roadmaker::SpanArm>& spans) {
        return roadmaker::edit::create_span_junction(network, spans);
      },
      "network"_a,
      "spans"_a,
      "Creates a SPAN (virtual) junction over one span (a mid-road crosswalk) "
      "or two (the same crossing over two parallel roads). ASAM OpenDRIVE "
      "1.9.0 §12.7: the main road is UNINTERRUPTED, so nothing is created but "
      "the junction record — no arms, no connecting roads, no road links — and "
      "the result is always locked. Pushing raises ValueError for no spans or "
      "more than two, a stale road id, the same road in both spans, a "
      "connecting road, or a span that is not a real interval inside its road "
      "(s_start < 0, s_end > length, or zero length).");
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
  assembly.def(
      "cross_roads",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::RoadId a,
         roadmaker::RoadId b,
         roadmaker::edit::assembly::IntersectionParams params) {
        return roadmaker::edit::assembly::cross_roads(network, a, b, params);
      },
      "network"_a,
      "a"_a,
      "b"_a,
      "params"_a = roadmaker::edit::assembly::IntersectionParams{},
      "Forms a 4-way junction where two EXISTING roads `a` and `b` cross (split "
      "each at the crossing ± gap, remove the middle stubs, generate the "
      "junction). One undoable command.");

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
  edit.def("insert_lane",
           &roadmaker::edit::insert_lane,
           "network"_a,
           "section"_a,
           "at_odr_id"_a,
           "type"_a,
           "Inserts a lane at at_odr_id, renumbering every lane already at or "
           "outside it one step further out (add_lane only appends the "
           "outermost). The new lane does not link to the neighbouring sections; "
           "adjacent-section links and junction lane_links that named a shifted "
           "lane are remapped to the new numbering.");
  edit.def("set_lane_type", &roadmaker::edit::set_lane_type, "network"_a, "lane"_a, "type"_a);
  edit.def("set_lane_direction",
           &roadmaker::edit::set_lane_direction,
           "network"_a,
           "lane"_a,
           "direction"_a,
           "Sets a lane's travel direction (e_lane_direction). Refuses the center lane.");
  edit.def("set_lane_width",
           &roadmaker::edit::set_lane_width,
           "network"_a,
           "lane"_a,
           "width_m"_a,
           "Sets a CONSTANT width. Refuses a lane whose width already varies along s "
           "(flattening an authored taper is data loss) — use set_lane_width_profile.");
  edit.def("set_lane_width_profile",
           &roadmaker::edit::set_lane_width_profile,
           "network"_a,
           "lane"_a,
           "widths"_a,
           "Replaces the lane's width profile: a list of Poly3 with SECTION-LOCAL "
           "sOffsets, w(ds) = a + b*ds + c*ds^2 + d*ds^3. Needs a record at sOffset 0, "
           "ascending sOffsets inside the section, and width >= 0 — zero is legal, and "
           "is how a turn lane tapers up from nothing.");
  edit.def("set_lane_material",
           &roadmaker::edit::set_lane_material,
           "network"_a,
           "lane"_a,
           "records"_a,
           "Replaces the lane's <material> records (§11.8.2): a list of LaneMaterial "
           "with SECTION-LOCAL sOffsets; an empty list clears them. Refuses the center "
           "lane, non-ascending sOffsets, negative friction/roughness, and records "
           "outside the section. RoadMaker writes surface='rm:<id>'.");
  edit.def("split_lane_section",
           &roadmaker::edit::split_lane_section,
           "network"_a,
           "road"_a,
           "s"_a,
           "Splits the lane section covering s in two, duplicating the cross section "
           "and partitioning each lane's width profile and road marks at the cut. "
           "Idempotent where a section already starts.");
  edit.def("add_lane_span",
           &roadmaker::edit::add_lane_span,
           "network"_a,
           "road"_a,
           "side"_a,
           "s0"_a,
           "s1"_a,
           "type"_a,
           "Lane Add: a self-contained pocket lane on `side` (+1 left, -1 right) that "
           "tapers 0 -> full -> 0 within [s0, s1]. The span is clamped inside the road so "
           "the pocket stays interior and needs no cross-section links; one undo step.");
  edit.def("form_lane",
           &roadmaker::edit::form_lane,
           "network"_a,
           "road"_a,
           "side"_a,
           "s_start"_a,
           "at_odr_id"_a,
           "type"_a,
           "Lane Form: an interior lane that starts at zero width at s_start and holds full "
           "width to the road end, taking numbering position at_odr_id (sign must match side). "
           "Backward-unlinked; carried across every downstream lane-section seam (inserted or "
           "appended per section, seams joined with matched predecessor/successor links) so it "
           "runs to the road end as a properly linked carriageway.");
  edit.def("link_lane_across_seam",
           &roadmaker::edit::link_lane_across_seam,
           "network"_a,
           "upstream_section"_a,
           "upstream_odr"_a,
           "downstream_odr"_a,
           "Joins one lane across a single lane-section seam by setting the matched pair "
           "(asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections, §11.6): the upstream "
           "lane's successor becomes downstream_odr and the downstream lane's predecessor becomes "
           "upstream_odr. The downstream section is the one after upstream_section in road order. "
           "Refuses a center-lane odr, a missing lane, or an upstream section with no follower.");
  edit.def("carve_lane",
           &roadmaker::edit::carve_lane,
           "network"_a,
           "road"_a,
           "side"_a,
           "s_start"_a,
           "s_end"_a,
           "at_odr_id"_a,
           "type"_a,
           "Lane Carve: a turn lane whose width ramps 0 -> full over the dragged span "
           "[s_start, s_end] and then holds full to the road terminus, where junction "
           "regeneration absorbs it. Takes numbering position at_odr_id (sign must match "
           "side); refuses if s_start is not in the road's final lane section.");
  edit.def("set_road_mark",
           &roadmaker::edit::set_road_mark,
           "network"_a,
           "lane"_a,
           "mark"_a,
           "Edits the FIRST road-mark record; later records survive untouched.");
  edit.def(
      "set_surface_material",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::SurfaceId surface,
         std::string material) {
        return roadmaker::edit::set_surface_material(network, surface, std::move(material));
      },
      "network"_a,
      "surface"_a,
      "material"_a,
      "Sets a derived ground surface's material name (\"\" clears to default grass). "
      "Round-trips as a `material` attribute on the surface's rm:surface userData. "
      "A stale SurfaceId yields an invalid command.");
  edit.def(
      "set_surface_boundary",
      [](const roadmaker::RoadNetwork& network,
         roadmaker::SurfaceId surface,
         std::vector<roadmaker::SurfaceNode> nodes) {
        return roadmaker::edit::set_surface_boundary(network, surface, std::move(nodes));
      },
      "network"_a,
      "surface"_a,
      "nodes"_a,
      "Replaces a ground surface's boundary node graph wholesale — one command "
      "per gesture. Applied to a DERIVED surface it also flips the source to "
      "AUTHORED (the boundary is no longer a function of the roads, so the "
      "surface detaches and stops being re-derived); bounding_roads stays as "
      "provenance. Seed the nodes from surface_boundary_nodes. Rejects a stale "
      "id, fewer than 3 nodes, a self-intersecting loop, and a no-op.");
  edit.def(
      "revert_surface_to_derived",
      [](const roadmaker::RoadNetwork& network, roadmaker::SurfaceId surface) {
        return roadmaker::edit::revert_surface_to_derived(network, surface);
      },
      "network"_a,
      "surface"_a,
      "Reattaches an authored surface to its roads: clears the node graph and "
      "flips the source back to DERIVED. The next derive_surfaces pass reclaims "
      "it if its provenance ring still encloses a face, and erases it "
      "otherwise. Rejects a stale id and an already-derived surface.");
  edit.def("apply_road_style",
           &roadmaker::edit::apply_road_style,
           "network"_a,
           "road"_a,
           "style"_a,
           "Replaces road's lane profile and boundary marks with style, flattening it to "
           "a single lane section. Preserves reference-line geometry, elevation, "
           "superelevation, name, links, and placed objects/signals. Refuses a connecting "
           "road or a style with no lanes.");
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
      "set_object_model",
      [](const roadmaker::RoadNetwork& network, roadmaker::ObjectId object, std::string model_id) {
        return roadmaker::edit::set_object_model(network, object, std::move(model_id));
      },
      "network"_a,
      "object"_a,
      "model_id"_a,
      "Re-points a placed object at another bundled prop model (the id also set "
      "as Object.name, e.g. 'tree_pine'), refreshing its radius/height from that "
      "model. Fails for an unknown id.");
  edit.def(
      "update_objects",
      [](const roadmaker::RoadNetwork& network,
         std::vector<std::pair<roadmaker::ObjectId, roadmaker::Object>> updates,
         std::string name) {
        return roadmaker::edit::update_objects(network, std::move(updates), std::move(name));
      },
      "network"_a,
      "updates"_a,
      "name"_a = std::string{},
      "Replaces each listed object with its new value in one undoable command "
      "(the batch behind an asset edit). `updates` is a list of (ObjectId, "
      "Object); ids keep their generation and undo is byte-identical. Refuses a "
      "stale id or an update that changes an object's owning road; an empty list "
      "is a no-op.");
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
      "set_signal_text",
      [](const roadmaker::RoadNetwork& network, roadmaker::SignalId signal, std::string text) {
        return roadmaker::edit::set_signal_text(network, signal, std::move(text));
      },
      "network"_a,
      "signal"_a,
      "text"_a,
      "Sets a signal's @text (§14 Table 122 — editable sign-face text; multi-line "
      "uses literal \\n). Rejects a no-op; one undo step.");
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
      .def_prop_ro("surface_count",
                   [](const roadmaker::NetworkMesh& mesh) { return mesh.surfaces.size(); })
      .def_prop_ro("surface_vertex_count",
                   [](const roadmaker::NetworkMesh& mesh) {
                     std::size_t count = 0;
                     for (const auto& surface : mesh.surfaces) {
                       count += surface.mesh.positions.size() / 3;
                     }
                     return count;
                   })
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
