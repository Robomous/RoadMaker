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

/// OpenSCENARIO XML writer (p8-s1, issue #245) — the twin of
/// core/src/xodr/writer.cpp, holding the same three disciplines:
///
///   1. ORDER IS THE SCHEMA. The OpenSCENARIO XSD sequences are ordered, so
///      the emission order below is correctness and not taste. Within an
///      element: modeled attributes, preserved attributes, modeled children,
///      preserved children — appended in that order so output stays canonical
///      and idempotent.
///   2. DETERMINISM. No clock, no locale-dependent formatting, no associative
///      container reaching the output. Every collection reached from
///      `Scenario` is a vector emitted in vector order, so THERE IS NO SORT IN
///      THIS FILE. If a future change introduces an arena or map traversal,
///      the sort belongs at the point of construction, not here.
///   3. NEVER SILENTLY DROP INPUT. Preserved fragments are re-emitted
///      verbatim, and one that is not well-formed is refused rather than
///      quietly discarded.

#include "roadmaker/osc/writer.hpp"

#include "roadmaker/osc/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace roadmaker::osc {
namespace {

// --- formatting -------------------------------------------------------------

/// Shortest-precision round-trippable formatting; locale-independent.
///
/// Copied deliberately from core/src/xodr/writer.cpp:52-60 rather than shared:
/// the two formats' number policies are independent and either may need to
/// diverge. The "-0" normalization is load-bearing — without it a negative
/// zero reaches the file and no round trip normalizes it away — and it has its
/// own test here, because the OpenDRIVE suite does not cover this copy.
std::string num(double value) {
  std::string text = fmt::format("{}", value);
  return text == "-0" ? "0" : text;
}

void set_num(pugi::xml_node node, const char* name, double value) {
  node.append_attribute(name).set_value(num(value).c_str());
}

void set_optional_num(pugi::xml_node node, const char* name, const std::optional<double>& value) {
  if (value.has_value()) {
    set_num(node, name, *value);
  }
}

/// Sets an attribute only when the string is non-empty. Optional OpenSCENARIO
/// attributes are omitted, never written empty: `reference=""` names a
/// controller called "" rather than saying "no reference".
void set_optional_text(pugi::xml_node node, const char* name, const std::string& value) {
  if (!value.empty()) {
    node.append_attribute(name).set_value(value.c_str());
  }
}

const char* semantics_name(PhaseSemantics semantics) {
  switch (semantics) {
  case PhaseSemantics::AttentionGo:
    return "attention_go";
  case PhaseSemantics::AttentionStop:
    return "attention_stop";
  case PhaseSemantics::Caution:
    return "caution";
  case PhaseSemantics::Fallback:
    return "fallback";
  case PhaseSemantics::Go:
    return "go";
  case PhaseSemantics::Stop:
    return "stop";
  }
  return "fallback";
}

// --- the preserved tier -----------------------------------------------------

/// Parses a preserved fragment to check it is well-formed.
///
/// `pugi::parse_fragment` is required: a stored fragment is a self-contained
/// node, not a document, and the default parser rejects some legal ones.
bool fragment_is_well_formed(const std::string& fragment) {
  pugi::xml_document scratch;
  const pugi::xml_parse_result result = scratch.load_buffer(
      fragment.data(), fragment.size(), pugi::parse_default | pugi::parse_fragment);
  return result.status == pugi::status_ok;
}

/// How many preserved fragments of `preserved` are `<element>` elements.
///
/// ★ EXISTS BECAUSE A CARDINALITY RULE COUNTS WHAT IS EMITTED, NOT WHAT IS
/// MODELED (p8-s3, issue #247). A `<Waypoint>` whose position is one of the
/// eight types this version does not model rides the preserved tier whole, and
/// the writer emits it verbatim — so the document really does carry it. Counting
/// only the modeled ones made `write_xosc` REFUSE a file `parse_xosc` had just
/// accepted, which is the round trip failing in the one direction the never-drop
/// contract exists to protect (ADR-0014 §6).
///
/// Parses each fragment rather than sniffing its prefix: `<WaypointGroup>` also
/// starts with `<Waypoint`.
std::size_t preserved_element_count(const RawXml& preserved, std::string_view element) {
  std::size_t count = 0;
  for (const std::string& fragment : preserved.children) {
    pugi::xml_document scratch;
    const pugi::xml_parse_result result = scratch.load_buffer(
        fragment.data(), fragment.size(), pugi::parse_default | pugi::parse_fragment);
    if (result.status != pugi::status_ok) {
      continue; // reported separately by check_preserved_fragments
    }
    for (const pugi::xml_node child : scratch.children()) {
      if (std::string_view(child.name()) == element) {
        ++count;
      }
    }
  }
  return count;
}

void append_fragment(pugi::xml_node parent, const std::string& fragment) {
  parent.append_buffer(
      fragment.data(), fragment.size(), pugi::parse_default | pugi::parse_fragment);
}

void write_preserved_attributes(pugi::xml_node node, const RawXml& preserved) {
  for (const auto& [name, value] : preserved.attributes) {
    node.append_attribute(name.c_str()).set_value(value.c_str());
  }
}

void write_preserved_children(pugi::xml_node node, const RawXml& preserved) {
  for (const std::string& child : preserved.children) {
    append_fragment(node, child);
  }
}

// --- traffic signals --------------------------------------------------------

/// Emits one controller's phases, synthesizing and de-duplicating `@name`.
///
/// `@name` is required and must be unique among ONE controller's phases, while
/// `roadmaker::SignalPhase::name` is legally empty
/// (`road/junction.hpp:328-330`) — so a whole cycle must not become unwritable
/// over a label. The rule:
///   - a non-empty authored name wins, and still joins the de-dup pool, so an
///     author who writes "go" on phase 0 pushes a synthesized "go" on phase 3
///     to "go_1" and never the reverse;
///   - otherwise the semantic token, when one is set;
///   - otherwise the literal "phase".
/// Collisions take the first free `_1`, `_2`, ... suffix.
///
/// ★ `used` IS DECLARED PER CONTROLLER, and that placement is the whole
/// correctness of the function. Uniqueness is required within a controller;
/// hoisting the set out of the caller's loop would rename the second
/// controller's phases to "go_1"/"stop_1", breaking the invariant that every
/// controller decomposed from ONE junction timeline carries the SAME phase
/// names — which is exactly what a traffic-signal condition references. The
/// output stays schema-valid and a simulator loads it happily, so nothing but
/// a dedicated test catches it.
///
/// Terminating: `used` grows by one per phase, so the probe runs at most
/// `phases.size()` times. `std::set` rather than `std::unordered_set` because
/// a later refactor that iterates it must not become nondeterministic.
///
/// ★ THE BODY MOVED TO THE PUBLIC `osc::phase_names()` (p8-s4, issue #248) and
/// this function now CALLS it rather than reimplementing it. A second copy is
/// exactly how the file and a `@phase` reference authored against it drift —
/// the trap #248 was filed with — and a copy stays green under every test that
/// only reads one of the two.
void write_phases(pugi::xml_node controller_node,
                  const TrafficSignalController& controller,
                  const WriteOptions& options) {
  const std::vector<std::string> names = phase_names(controller);

  for (std::size_t index = 0; index < controller.phases.size(); ++index) {
    const Phase& phase = controller.phases[index];

    pugi::xml_node node = controller_node.append_child("Phase");
    node.append_attribute("name").set_value(names[index].c_str());
    set_num(node, "duration", phase.duration);

    // The ONLY content-level version conditional in this writer: @semantics
    // was created in 1.4.0, and a 1.2-declared file carrying it is exactly
    // what a pinned parser rejects.
    if (options.target_version == OscVersion::v1_4 && phase.semantics.has_value()) {
      node.append_attribute("semantics").set_value(semantics_name(*phase.semantics));
    }
    write_preserved_attributes(node, phase.preserved);

    for (const TrafficSignalState& state : phase.signal_states) {
      pugi::xml_node state_node = node.append_child("TrafficSignalState");
      state_node.append_attribute("trafficSignalId").set_value(state.traffic_signal_id.c_str());
      state_node.append_attribute("state").set_value(state.state.c_str());
      write_preserved_attributes(state_node, state.preserved);
      write_preserved_children(state_node, state.preserved);
    }

    write_preserved_children(node, phase.preserved);
  }
}

/// `<TrafficSignals>` — §6.11. Omitted entirely when there are no controllers:
/// an empty wrapper in every unsignalized scenario is noise, and the element
/// is optional.
///
/// Always `<TrafficSignalState>`, never `<TrafficSignalGroupState>`, at every
/// revision (ADR-0014 §8): the group form carries no ids at all and would
/// discard the identity fact the odr_id rule exists to protect. There is
/// deliberately nothing here to "optimize" a uniform phase into.
void write_traffic_signals(pugi::xml_node road_network_node,
                           const RoadNetworkRef& road_network,
                           const WriteOptions& options) {
  if (road_network.traffic_signal_controllers.empty()) {
    return;
  }

  pugi::xml_node signals_node = road_network_node.append_child("TrafficSignals");
  for (const TrafficSignalController& controller : road_network.traffic_signal_controllers) {
    pugi::xml_node node = signals_node.append_child("TrafficSignalController");
    node.append_attribute("name").set_value(controller.name.c_str());
    set_optional_num(node, "delay", controller.delay);
    set_optional_text(node, "reference", controller.reference);
    write_preserved_attributes(node, controller.preserved);

    write_phases(node, controller, options);

    write_preserved_children(node, controller.preserved);
  }
}

// --- entities ---------------------------------------------------------------

void write_bounding_box(pugi::xml_node parent, const BoundingBox& box) {
  pugi::xml_node node = parent.append_child("BoundingBox");
  write_preserved_attributes(node, box.preserved);

  pugi::xml_node center = node.append_child("Center");
  set_num(center, "x", box.center_x);
  set_num(center, "y", box.center_y);
  set_num(center, "z", box.center_z);

  pugi::xml_node dimensions = node.append_child("Dimensions");
  set_num(dimensions, "width", box.width);
  set_num(dimensions, "length", box.length);
  set_num(dimensions, "height", box.height);

  write_preserved_children(node, box.preserved);
}

void write_axle(pugi::xml_node parent, const char* element, const Axle& axle) {
  pugi::xml_node node = parent.append_child(element);
  set_num(node, "maxSteering", axle.max_steering);
  set_num(node, "wheelDiameter", axle.wheel_diameter);
  set_num(node, "trackWidth", axle.track_width);
  set_num(node, "positionX", axle.position_x);
  set_num(node, "positionZ", axle.position_z);
  write_preserved_attributes(node, axle.preserved);
  write_preserved_children(node, axle.preserved);
}

/// `<Properties>` — emitted even when empty; see the always-present skeleton
/// note on `osc::Scenario`.
///
/// `wrapper` is the `<Properties>` element's OWN preserved tier, which is why
/// it is a separate parameter from the entity's: a `<File>` read from inside
/// `<Properties>` has to be re-emitted inside it, not beside it.
void write_properties(pugi::xml_node parent,
                      const std::vector<Property>& properties,
                      const RawXml& wrapper) {
  pugi::xml_node node = parent.append_child("Properties");
  write_preserved_attributes(node, wrapper);
  for (const Property& property : properties) {
    pugi::xml_node property_node = node.append_child("Property");
    property_node.append_attribute("name").set_value(property.name.c_str());
    property_node.append_attribute("value").set_value(property.value.c_str());
    write_preserved_attributes(property_node, property.preserved);
    write_preserved_children(property_node, property.preserved);
  }
  write_preserved_children(node, wrapper);
}

void write_vehicle(pugi::xml_node parent, const Vehicle& vehicle) {
  pugi::xml_node node = parent.append_child("Vehicle");
  node.append_attribute("name").set_value(vehicle.name.c_str());
  node.append_attribute("vehicleCategory").set_value(vehicle.category.c_str());
  set_optional_num(node, "mass", vehicle.mass);
  set_optional_text(node, "model3d", vehicle.model3d);
  write_preserved_attributes(node, vehicle.preserved);

  write_bounding_box(node, vehicle.bounding_box);

  pugi::xml_node performance = node.append_child("Performance");
  set_num(performance, "maxSpeed", vehicle.performance.max_speed);
  set_num(performance, "maxAcceleration", vehicle.performance.max_acceleration);
  set_num(performance, "maxDeceleration", vehicle.performance.max_deceleration);
  write_preserved_attributes(performance, vehicle.performance.preserved);
  write_preserved_children(performance, vehicle.performance.preserved);

  pugi::xml_node axles = node.append_child("Axles");
  write_preserved_attributes(axles, vehicle.axles.preserved);
  write_axle(axles, "FrontAxle", vehicle.axles.front);
  write_axle(axles, "RearAxle", vehicle.axles.rear);
  for (const Axle& axle : vehicle.axles.additional) {
    write_axle(axles, "AdditionalAxle", axle);
  }
  write_preserved_children(axles, vehicle.axles.preserved);

  write_properties(node, vehicle.properties, vehicle.properties_preserved);
  write_preserved_children(node, vehicle.preserved);
}

void write_pedestrian(pugi::xml_node parent, const Pedestrian& pedestrian) {
  pugi::xml_node node = parent.append_child("Pedestrian");
  node.append_attribute("name").set_value(pedestrian.name.c_str());
  set_num(node, "mass", pedestrian.mass);
  node.append_attribute("pedestrianCategory").set_value(pedestrian.category.c_str());
  set_optional_text(node, "model3d", pedestrian.model3d);
  write_preserved_attributes(node, pedestrian.preserved);

  write_bounding_box(node, pedestrian.bounding_box);
  write_properties(node, pedestrian.properties, pedestrian.properties_preserved);
  write_preserved_children(node, pedestrian.preserved);
}

void write_entities(pugi::xml_node root, const Entities& entities) {
  pugi::xml_node node = root.append_child("Entities");
  write_preserved_attributes(node, entities.preserved);

  for (const ScenarioObject& object : entities.scenario_objects) {
    pugi::xml_node object_node = node.append_child("ScenarioObject");
    object_node.append_attribute("name").set_value(object.name.c_str());
    write_preserved_attributes(object_node, object.preserved);

    // std::monostate means the entity object rode in entirely on the preserved
    // tier — a MiscObject, an ExternalObjectReference or a CatalogReference
    // this version does not model. Emitting nothing here is correct; the
    // preserved children below carry it.
    if (const auto* vehicle = std::get_if<Vehicle>(&object.entity_object)) {
      write_vehicle(object_node, *vehicle);
    } else if (const auto* pedestrian = std::get_if<Pedestrian>(&object.entity_object)) {
      write_pedestrian(object_node, *pedestrian);
    }

    write_preserved_children(object_node, object.preserved);
  }

  write_preserved_children(node, entities.preserved);
}

// --- storyboard -------------------------------------------------------------

/// `<Orientation>` — omitted entirely when unset, since "missing Orientation
/// property is interpreted as the relative reference context with
/// Heading=Pitch=Roll=0" (§7.6), which is exactly what an actor placed on a
/// lane centre means.
void write_orientation(pugi::xml_node parent, const std::optional<Orientation>& orientation) {
  if (!orientation.has_value()) {
    return;
  }
  pugi::xml_node node = parent.append_child("Orientation");
  set_optional_num(node, "h", orientation->h);
  set_optional_num(node, "p", orientation->p);
  set_optional_num(node, "r", orientation->r);
  set_optional_text(node, "type", orientation->type);
  write_preserved_attributes(node, orientation->preserved);
  write_preserved_children(node, orientation->preserved);
}

/// The `<Position>` choice — one `std::visit`, so adding a fourth alternative
/// to `osc::Position` is a compile error here rather than a silent omission.
///
/// `@offset` on `<LanePosition>` is emitted unconditionally even at its 0
/// default, unlike `@t` on `<RoadPosition>` which is required anyway: the value
/// is what a reader round-trips, and omitting a modeled double because it
/// happens to equal its default is how a write→read→write stops being
/// idempotent for anyone who set it explicitly to 0.
void write_position(pugi::xml_node parent, const Position& position) {
  pugi::xml_node node = parent.append_child("Position");
  std::visit(
      [&node](const auto& pose) {
        using T = std::decay_t<decltype(pose)>;
        if constexpr (std::is_same_v<T, WorldPosition>) {
          pugi::xml_node world = node.append_child("WorldPosition");
          set_num(world, "x", pose.x);
          set_num(world, "y", pose.y);
          set_num(world, "z", pose.z);
          set_optional_num(world, "h", pose.h);
          set_optional_num(world, "p", pose.p);
          set_optional_num(world, "r", pose.r);
          write_preserved_attributes(world, pose.preserved);
          write_preserved_children(world, pose.preserved);
        } else if constexpr (std::is_same_v<T, RoadPosition>) {
          pugi::xml_node road = node.append_child("RoadPosition");
          road.append_attribute("roadId").set_value(pose.road_id.c_str());
          set_num(road, "s", pose.s);
          set_num(road, "t", pose.t);
          write_preserved_attributes(road, pose.preserved);
          write_orientation(road, pose.orientation);
          write_preserved_children(road, pose.preserved);
        } else {
          pugi::xml_node lane = node.append_child("LanePosition");
          lane.append_attribute("roadId").set_value(pose.road_id.c_str());
          lane.append_attribute("laneId").set_value(pose.lane_id.c_str());
          set_num(lane, "s", pose.s);
          set_num(lane, "offset", pose.offset);
          write_preserved_attributes(lane, pose.preserved);
          write_orientation(lane, pose.orientation);
          write_preserved_children(lane, pose.preserved);
        }
      },
      position);
}

void write_teleport(pugi::xml_node action_node, const TeleportAction& teleport) {
  pugi::xml_node node = action_node.append_child("TeleportAction");
  write_preserved_attributes(node, teleport.preserved);
  write_position(node, teleport.position);
  write_preserved_children(node, teleport.preserved);
}

void write_longitudinal(pugi::xml_node action_node, const LongitudinalAction& longitudinal) {
  pugi::xml_node node = action_node.append_child("LongitudinalAction");
  write_preserved_attributes(node, longitudinal.preserved);

  if (longitudinal.speed.has_value()) {
    const SpeedAction& speed = *longitudinal.speed;
    pugi::xml_node speed_node = node.append_child("SpeedAction");
    write_preserved_attributes(speed_node, speed.preserved);

    pugi::xml_node dynamics = speed_node.append_child("SpeedActionDynamics");
    dynamics.append_attribute("dynamicsShape").set_value(speed.dynamics.dynamics_shape.c_str());
    set_num(dynamics, "value", speed.dynamics.value);
    dynamics.append_attribute("dynamicsDimension")
        .set_value(speed.dynamics.dynamics_dimension.c_str());
    set_optional_text(dynamics, "followingMode", speed.dynamics.following_mode);
    write_preserved_attributes(dynamics, speed.dynamics.preserved);
    write_preserved_children(dynamics, speed.dynamics.preserved);

    // <SpeedActionTarget> is flattened into SpeedAction, so its own preserved
    // tier is written here rather than on the action — a <RelativeTargetSpeed>
    // has to be re-emitted INSIDE the target, not beside it.
    pugi::xml_node target = speed_node.append_child("SpeedActionTarget");
    write_preserved_attributes(target, speed.target_preserved);
    if (speed.absolute_target.has_value()) {
      pugi::xml_node absolute = target.append_child("AbsoluteTargetSpeed");
      set_num(absolute, "value", speed.absolute_target->value);
      write_preserved_attributes(absolute, speed.absolute_target->preserved);
      write_preserved_children(absolute, speed.absolute_target->preserved);
    }
    write_preserved_children(target, speed.target_preserved);

    write_preserved_children(speed_node, speed.preserved);
  }

  write_preserved_children(node, longitudinal.preserved);
}

/// `<RoutingAction>` — p8-s3, issue #247.
///
/// The `<Route>` child order is the XSD sequence exactly:
/// `<ParameterDeclarations>` (0..1) then `<Waypoint>` (2..*). Unlike the
/// scenario-level `<ParameterDeclarations>`, which is part of the always-present
/// skeleton, a route's is OPTIONAL — so it is emitted only when it has content.
/// An empty one would be a child a foreign input did not have, and this writer's
/// round trip is a fixed point measured in bytes.
void write_routing(pugi::xml_node action_node, const RoutingAction& routing) {
  pugi::xml_node node = action_node.append_child("RoutingAction");
  write_preserved_attributes(node, routing.preserved);

  if (routing.assign_route.has_value()) {
    const AssignRouteAction& assign = *routing.assign_route;
    pugi::xml_node assign_node = node.append_child("AssignRouteAction");
    write_preserved_attributes(assign_node, assign.preserved);
    // Unset `route` means the action's content is a <CatalogReference> riding
    // the preserved tier. Emitting an empty <Route> here would replace a legal
    // catalog reference with an invalid element.
    if (assign.route.has_value()) {
      const Route& route = *assign.route;
      pugi::xml_node route_node = assign_node.append_child("Route");
      route_node.append_attribute("name").set_value(route.name.c_str());
      route_node.append_attribute("closed").set_value(route.closed);
      write_preserved_attributes(route_node, route.preserved);
      if (!route.parameter_declarations.empty()) {
        pugi::xml_node parameters = route_node.append_child("ParameterDeclarations");
        for (const ParameterDeclaration& parameter : route.parameter_declarations) {
          pugi::xml_node parameter_node = parameters.append_child("ParameterDeclaration");
          parameter_node.append_attribute("name").set_value(parameter.name.c_str());
          parameter_node.append_attribute("parameterType")
              .set_value(parameter.parameter_type.c_str());
          parameter_node.append_attribute("value").set_value(parameter.value.c_str());
          write_preserved_attributes(parameter_node, parameter.preserved);
          write_preserved_children(parameter_node, parameter.preserved);
        }
      }
      for (const RouteWaypoint& waypoint : route.waypoints) {
        pugi::xml_node waypoint_node = route_node.append_child("Waypoint");
        waypoint_node.append_attribute("routeStrategy").set_value(waypoint.route_strategy.c_str());
        write_preserved_attributes(waypoint_node, waypoint.preserved);
        write_position(waypoint_node, waypoint.position);
        write_preserved_children(waypoint_node, waypoint.preserved);
      }
      write_preserved_children(route_node, route.preserved);
    }
    write_preserved_children(assign_node, assign.preserved);
  }

  write_preserved_children(node, routing.preserved);
}

/// `<LateralAction>` — p8-s4, issue #248. The `<LaneChangeAction>` child order
/// is the XSD `all` group in its declared order: `<LaneChangeActionDynamics>`
/// then `<LaneChangeTarget>`.
void write_lateral(pugi::xml_node action_node, const LateralAction& lateral) {
  pugi::xml_node node = action_node.append_child("LateralAction");
  write_preserved_attributes(node, lateral.preserved);

  if (lateral.lane_change.has_value()) {
    const LaneChangeAction& change = *lateral.lane_change;
    pugi::xml_node change_node = node.append_child("LaneChangeAction");
    set_optional_num(change_node, "targetLaneOffset", change.target_lane_offset);
    write_preserved_attributes(change_node, change.preserved);

    pugi::xml_node dynamics = change_node.append_child("LaneChangeActionDynamics");
    dynamics.append_attribute("dynamicsShape").set_value(change.dynamics.dynamics_shape.c_str());
    set_num(dynamics, "value", change.dynamics.value);
    dynamics.append_attribute("dynamicsDimension")
        .set_value(change.dynamics.dynamics_dimension.c_str());
    set_optional_text(dynamics, "followingMode", change.dynamics.following_mode);
    write_preserved_attributes(dynamics, change.dynamics.preserved);
    write_preserved_children(dynamics, change.dynamics.preserved);

    // <LaneChangeTarget> is flattened into LaneChangeAction, so its own
    // preserved tier is written here — the SpeedActionTarget rationale exactly.
    pugi::xml_node target = change_node.append_child("LaneChangeTarget");
    write_preserved_attributes(target, change.target_preserved);
    std::visit(
        [&target](const auto& arm) {
          using T = std::decay_t<decltype(arm)>;
          if constexpr (std::is_same_v<T, AbsoluteTargetLane>) {
            pugi::xml_node absolute = target.append_child("AbsoluteTargetLane");
            absolute.append_attribute("value").set_value(arm.value.c_str());
            write_preserved_attributes(absolute, arm.preserved);
            write_preserved_children(absolute, arm.preserved);
          } else if constexpr (std::is_same_v<T, RelativeTargetLane>) {
            pugi::xml_node relative = target.append_child("RelativeTargetLane");
            relative.append_attribute("entityRef").set_value(arm.entity_ref.c_str());
            relative.append_attribute("value").set_value(arm.value);
            write_preserved_attributes(relative, arm.preserved);
            write_preserved_children(relative, arm.preserved);
          }
          // std::monostate: the arm rode target_preserved whole.
        },
        change.target);
    write_preserved_children(target, change.target_preserved);

    write_preserved_children(change_node, change.preserved);
  }

  write_preserved_children(node, lateral.preserved);
}

/// One `PrivateAction` — ONE `<PrivateAction>` ELEMENT PER SET ARM.
///
/// The schema's choice is per-element, so an action carrying both a teleport
/// and a longitudinal action becomes two elements rather than one invalid one.
/// `preserved` rides the first, so nothing is duplicated. An action with
/// NEITHER arm is still emitted: that is how a whole preserved action (a
/// `<RelativeLanePosition>` teleport the reader would not model) survives.
void write_private_action(pugi::xml_node private_node, const PrivateAction& action) {
  bool preserved_written = false;
  const auto open = [&](pugi::xml_node& node) {
    node = private_node.append_child("PrivateAction");
    if (!preserved_written) {
      write_preserved_attributes(node, action.preserved);
    }
  };
  const auto close = [&](pugi::xml_node node) {
    if (!preserved_written) {
      write_preserved_children(node, action.preserved);
      preserved_written = true;
    }
  };

  if (action.teleport.has_value()) {
    pugi::xml_node node;
    open(node);
    write_teleport(node, *action.teleport);
    close(node);
  }
  if (action.longitudinal.has_value()) {
    pugi::xml_node node;
    open(node);
    write_longitudinal(node, *action.longitudinal);
    close(node);
  }
  if (action.routing.has_value()) {
    pugi::xml_node node;
    open(node);
    write_routing(node, *action.routing);
    close(node);
  }
  if (action.lateral.has_value()) {
    pugi::xml_node node;
    open(node);
    write_lateral(node, *action.lateral);
    close(node);
  }
  if (!preserved_written) {
    pugi::xml_node node;
    open(node);
    close(node);
  }
}

/// `<EntityRef>`* — the shape `<Actors>` and `<TriggeringEntities>` share.
void write_entity_refs(pugi::xml_node parent, const std::vector<EntityRef>& refs) {
  for (const EntityRef& ref : refs) {
    pugi::xml_node node = parent.append_child("EntityRef");
    node.append_attribute("entityRef").set_value(ref.entity_ref.c_str());
    write_preserved_attributes(node, ref.preserved);
    write_preserved_children(node, ref.preserved);
  }
}

/// The `<ByEntityCondition>` arm — p8-s4, issue #248.
void write_by_entity_condition(pugi::xml_node condition_node, const ByEntityCondition& by_entity) {
  pugi::xml_node node = condition_node.append_child("ByEntityCondition");
  write_preserved_attributes(node, by_entity.preserved);

  pugi::xml_node triggering = node.append_child("TriggeringEntities");
  triggering.append_attribute("triggeringEntitiesRule")
      .set_value(by_entity.triggering_entities.rule.c_str());
  write_preserved_attributes(triggering, by_entity.triggering_entities.preserved);
  write_entity_refs(triggering, by_entity.triggering_entities.entity_refs);
  write_preserved_children(triggering, by_entity.triggering_entities.preserved);

  pugi::xml_node entity = node.append_child("EntityCondition");
  write_preserved_attributes(entity, by_entity.entity_condition_preserved);
  std::visit(
      [&entity](const auto& arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, SpeedCondition>) {
          pugi::xml_node speed = entity.append_child("SpeedCondition");
          speed.append_attribute("rule").set_value(arm.rule.c_str());
          set_num(speed, "value", arm.value);
          write_preserved_attributes(speed, arm.preserved);
          write_preserved_children(speed, arm.preserved);
        } else if constexpr (std::is_same_v<T, RelativeDistanceCondition>) {
          pugi::xml_node distance = entity.append_child("RelativeDistanceCondition");
          distance.append_attribute("entityRef").set_value(arm.entity_ref.c_str());
          distance.append_attribute("freespace").set_value(arm.freespace);
          distance.append_attribute("relativeDistanceType")
              .set_value(arm.relative_distance_type.c_str());
          distance.append_attribute("rule").set_value(arm.rule.c_str());
          set_num(distance, "value", arm.value);
          write_preserved_attributes(distance, arm.preserved);
          write_preserved_children(distance, arm.preserved);
        }
        // std::monostate: the arm rode entity_condition_preserved whole.
      },
      by_entity.entity_condition);
  write_preserved_children(entity, by_entity.entity_condition_preserved);

  write_preserved_children(node, by_entity.preserved);
}

/// One `<Condition>`.
///
/// ★ AT MOST ONE ARM IS EMITTED, and the model can hold more than one — the
/// arms are optionals rather than a variant for source-compatibility reasons
/// (`osc/scenario.hpp`, `Condition`). `validate_scenario` refuses a
/// multiply-armed condition BEFORE the writer runs, so the order the checks sit
/// in here is unreachable in a written document; it is written most-specific
/// first anyway, so that a model built by hand in a test still produces a
/// schema-valid element rather than two sibling choice arms.
void write_condition(pugi::xml_node group_node, const Condition& condition) {
  pugi::xml_node node = group_node.append_child("Condition");
  node.append_attribute("name").set_value(condition.name.c_str());
  set_num(node, "delay", condition.delay);
  node.append_attribute("conditionEdge").set_value(condition.condition_edge.c_str());
  write_preserved_attributes(node, condition.preserved);

  if (condition.by_entity.has_value()) {
    write_by_entity_condition(node, *condition.by_entity);
  } else if (condition.simulation_time.has_value()) {
    pugi::xml_node by_value = node.append_child("ByValueCondition");
    pugi::xml_node time_node = by_value.append_child("SimulationTimeCondition");
    set_num(time_node, "value", condition.simulation_time->value);
    time_node.append_attribute("rule").set_value(condition.simulation_time->rule.c_str());
    write_preserved_attributes(time_node, condition.simulation_time->preserved);
    write_preserved_children(time_node, condition.simulation_time->preserved);
  } else if (condition.traffic_signal.has_value()) {
    pugi::xml_node by_value = node.append_child("ByValueCondition");
    pugi::xml_node signal = by_value.append_child("TrafficSignalCondition");
    signal.append_attribute("name").set_value(condition.traffic_signal->name.c_str());
    signal.append_attribute("state").set_value(condition.traffic_signal->state.c_str());
    write_preserved_attributes(signal, condition.traffic_signal->preserved);
    write_preserved_children(signal, condition.traffic_signal->preserved);
  } else if (condition.traffic_signal_controller.has_value()) {
    pugi::xml_node by_value = node.append_child("ByValueCondition");
    pugi::xml_node controller = by_value.append_child("TrafficSignalControllerCondition");
    controller.append_attribute("trafficSignalControllerRef")
        .set_value(condition.traffic_signal_controller->traffic_signal_controller_ref.c_str());
    controller.append_attribute("phase").set_value(
        condition.traffic_signal_controller->phase.c_str());
    write_preserved_attributes(controller, condition.traffic_signal_controller->preserved);
    write_preserved_children(controller, condition.traffic_signal_controller->preserved);
  } else if (condition.storyboard_element_state.has_value()) {
    const StoryboardElementStateCondition& state = *condition.storyboard_element_state;
    pugi::xml_node by_value = node.append_child("ByValueCondition");
    pugi::xml_node element = by_value.append_child("StoryboardElementStateCondition");
    element.append_attribute("storyboardElementRef")
        .set_value(state.storyboard_element_ref.c_str());
    element.append_attribute("state").set_value(state.state.c_str());
    element.append_attribute("storyboardElementType")
        .set_value(state.storyboard_element_type.c_str());
    write_preserved_attributes(element, state.preserved);
    write_preserved_children(element, state.preserved);
  }

  write_preserved_children(node, condition.preserved);
}

void write_trigger(pugi::xml_node parent, const char* element, const Trigger& trigger) {
  pugi::xml_node node = parent.append_child(element);
  write_preserved_attributes(node, trigger.preserved);

  for (const ConditionGroup& group : trigger.condition_groups) {
    pugi::xml_node group_node = node.append_child("ConditionGroup");
    write_preserved_attributes(group_node, group.preserved);

    for (const Condition& condition : group.conditions) {
      write_condition(group_node, condition);
    }

    write_preserved_children(group_node, group.preserved);
  }

  write_preserved_children(node, trigger.preserved);
}

/// An OPTIONAL `<StartTrigger>`/`<StopTrigger>` — absent stays absent, which is
/// what keeps a foreign document's byte identity (`Event::start_trigger`).
void write_optional_trigger(pugi::xml_node parent,
                            const char* element,
                            const std::optional<Trigger>& trigger) {
  if (trigger.has_value()) {
    write_trigger(parent, element, *trigger);
  }
}

/// A `<ParameterDeclarations>` wrapper that is OPTIONAL in its parent — emitted
/// only when it has content, unlike the scenario-level one which is part of the
/// always-present skeleton. An empty one is a child a foreign input did not
/// have, and this writer's round trip is a fixed point measured in bytes.
void write_optional_parameter_declarations(pugi::xml_node parent,
                                           const std::vector<ParameterDeclaration>& declarations) {
  if (declarations.empty()) {
    return;
  }
  pugi::xml_node node = parent.append_child("ParameterDeclarations");
  for (const ParameterDeclaration& parameter : declarations) {
    pugi::xml_node parameter_node = node.append_child("ParameterDeclaration");
    parameter_node.append_attribute("name").set_value(parameter.name.c_str());
    parameter_node.append_attribute("parameterType").set_value(parameter.parameter_type.c_str());
    parameter_node.append_attribute("value").set_value(parameter.value.c_str());
    write_preserved_attributes(parameter_node, parameter.preserved);
    write_preserved_children(parameter_node, parameter.preserved);
  }
}

/// `<GlobalAction>` — p8-s4, issue #248. Only the `<InfrastructureAction>` arm
/// is modeled; every other one rides `preserved` whole.
void write_global_action(pugi::xml_node action_node, const GlobalAction& global) {
  pugi::xml_node node = action_node.append_child("GlobalAction");
  write_preserved_attributes(node, global.preserved);

  if (global.infrastructure.has_value()) {
    pugi::xml_node infrastructure = node.append_child("InfrastructureAction");
    write_preserved_attributes(infrastructure, global.infrastructure->preserved);

    const TrafficSignalAction& signal_action = global.infrastructure->traffic_signal;
    pugi::xml_node signal = infrastructure.append_child("TrafficSignalAction");
    write_preserved_attributes(signal, signal_action.preserved);
    std::visit(
        [&signal](const auto& arm) {
          using T = std::decay_t<decltype(arm)>;
          if constexpr (std::is_same_v<T, TrafficSignalStateAction>) {
            pugi::xml_node state = signal.append_child("TrafficSignalStateAction");
            state.append_attribute("name").set_value(arm.name.c_str());
            state.append_attribute("state").set_value(arm.state.c_str());
            write_preserved_attributes(state, arm.preserved);
            write_preserved_children(state, arm.preserved);
          } else if constexpr (std::is_same_v<T, TrafficSignalControllerAction>) {
            pugi::xml_node controller = signal.append_child("TrafficSignalControllerAction");
            controller.append_attribute("trafficSignalControllerRef")
                .set_value(arm.traffic_signal_controller_ref.c_str());
            controller.append_attribute("phase").set_value(arm.phase.c_str());
            write_preserved_attributes(controller, arm.preserved);
            write_preserved_children(controller, arm.preserved);
          }
          // std::monostate: the arm rode `preserved` whole.
        },
        signal_action.action);
    write_preserved_children(signal, signal_action.preserved);

    write_preserved_children(infrastructure, global.infrastructure->preserved);
  }

  write_preserved_children(node, global.preserved);
}

/// One `<Action>` — p8-s4, issue #248.
///
/// ★ A STORY `<Action>` HOLDS EXACTLY ONE `<PrivateAction>`, unlike an `<Init>`
/// one which may emit several elements from a multi-armed `PrivateAction`.
/// `Action` wraps a single choice arm, so the multi-element expansion
/// `write_private_action` performs would produce an invalid `<Action>`; this
/// writes each set arm as its own `<PrivateAction>` INSIDE the action, which is
/// what the schema's `maxOccurs="unbounded"` on `<Action>` means an author
/// should have split into two actions. Refused before it gets here:
/// `validate_scenario` reports a story action carrying more than one arm.
void write_action(pugi::xml_node event_node, const Action& action) {
  pugi::xml_node node = event_node.append_child("Action");
  node.append_attribute("name").set_value(action.name.c_str());
  write_preserved_attributes(node, action.preserved);

  std::visit(
      [&node](const auto& arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, GlobalAction>) {
          write_global_action(node, arm);
        } else if constexpr (std::is_same_v<T, PrivateAction>) {
          write_private_action(node, arm);
        }
        // std::monostate: a <UserDefinedAction>, or an arm this version does
        // not model, riding `preserved` whole.
      },
      action.action);

  write_preserved_children(node, action.preserved);
}

void write_event(pugi::xml_node maneuver_node, const Event& event) {
  pugi::xml_node node = maneuver_node.append_child("Event");
  if (event.maximum_execution_count.has_value()) {
    node.append_attribute("maximumExecutionCount").set_value(*event.maximum_execution_count);
  }
  node.append_attribute("name").set_value(event.name.c_str());
  node.append_attribute("priority").set_value(event.priority.c_str());
  write_preserved_attributes(node, event.preserved);

  for (const Action& action : event.actions) {
    write_action(node, action);
  }
  write_optional_trigger(node, "StartTrigger", event.start_trigger);

  write_preserved_children(node, event.preserved);
}

void write_maneuver(pugi::xml_node group_node, const StoryManeuver& maneuver) {
  pugi::xml_node node = group_node.append_child("Maneuver");
  node.append_attribute("name").set_value(maneuver.name.c_str());
  write_preserved_attributes(node, maneuver.preserved);

  write_optional_parameter_declarations(node, maneuver.parameter_declarations);
  for (const Event& event : maneuver.events) {
    write_event(node, event);
  }

  write_preserved_children(node, maneuver.preserved);
}

void write_maneuver_group(pugi::xml_node act_node, const ManeuverGroup& group) {
  pugi::xml_node node = act_node.append_child("ManeuverGroup");
  node.append_attribute("maximumExecutionCount").set_value(group.maximum_execution_count);
  node.append_attribute("name").set_value(group.name.c_str());
  write_preserved_attributes(node, group.preserved);

  pugi::xml_node actors = node.append_child("Actors");
  actors.append_attribute("selectTriggeringEntities").set_value(group.select_triggering_entities);
  write_preserved_attributes(actors, group.actors_preserved);
  write_entity_refs(actors, group.actors);
  write_preserved_children(actors, group.actors_preserved);

  // <CatalogReference>* sits between <Actors> and <Maneuver>* in the sequence,
  // which is why these are their own vector and not `preserved.children`.
  for (const std::string& fragment : group.preserved_catalog_references) {
    append_fragment(node, fragment);
  }
  for (const StoryManeuver& maneuver : group.maneuvers) {
    write_maneuver(node, maneuver);
  }

  write_preserved_children(node, group.preserved);
}

void write_act(pugi::xml_node story_node, const Act& act) {
  pugi::xml_node node = story_node.append_child("Act");
  node.append_attribute("name").set_value(act.name.c_str());
  write_preserved_attributes(node, act.preserved);

  for (const ManeuverGroup& group : act.maneuver_groups) {
    write_maneuver_group(node, group);
  }
  write_optional_trigger(node, "StartTrigger", act.start_trigger);
  write_optional_trigger(node, "StopTrigger", act.stop_trigger);

  write_preserved_children(node, act.preserved);
}

void write_story(pugi::xml_node storyboard_node, const Story& story) {
  pugi::xml_node node = storyboard_node.append_child("Story");
  node.append_attribute("name").set_value(story.name.c_str());
  write_preserved_attributes(node, story.preserved);

  write_optional_parameter_declarations(node, story.parameter_declarations);
  for (const Act& act : story.acts) {
    write_act(node, act);
  }

  write_preserved_children(node, story.preserved);
}

void write_storyboard(pugi::xml_node root, const Storyboard& storyboard) {
  pugi::xml_node node = root.append_child("Storyboard");
  write_preserved_attributes(node, storyboard.preserved);

  pugi::xml_node init = node.append_child("Init");
  write_preserved_attributes(init, storyboard.init.preserved);

  pugi::xml_node actions = init.append_child("Actions");
  write_preserved_attributes(actions, storyboard.init.actions.preserved);
  for (const Private& entry : storyboard.init.actions.privates) {
    pugi::xml_node private_node = actions.append_child("Private");
    private_node.append_attribute("entityRef").set_value(entry.entity_ref.c_str());
    write_preserved_attributes(private_node, entry.preserved);

    for (const PrivateAction& action : entry.actions) {
      write_private_action(private_node, action);
    }

    write_preserved_children(private_node, entry.preserved);
  }
  write_preserved_children(actions, storyboard.init.actions.preserved);
  write_preserved_children(init, storyboard.init.preserved);

  // <Story>* sits between <Init> and <StopTrigger> in the Storyboard sequence.
  for (const Story& story : storyboard.stories) {
    write_story(node, story);
  }

  // Always emitted, empty if it has no condition groups.
  write_trigger(node, "StopTrigger", storyboard.stop_trigger);

  write_preserved_children(node, storyboard.preserved);
}

// --- validation -------------------------------------------------------------

/// A finding that does NOT block the write.
///
/// ★ THE STORYBOARD'S SCHEMA-SHAPE FINDINGS ARE WARNINGS, and that split is
/// load-bearing rather than cosmetic (p8-s4, issue #248). A foreign `.xosc`
/// carrying an `<Act>` with no `<ManeuverGroup>` was READABLE before this
/// version modeled `<Story>`, and refusing to write it back would mean a
/// document RoadMaker just loaded can no longer be saved — the never-drop
/// contract inverted (ADR-0014 §6). The shape came from the file; re-emitting
/// it loses nothing and reports it in full.
///
/// REFERENCE findings stay errors, because those are the ADR-0014 §5 failure
/// the format exists to prevent — a file that looks entirely right and points
/// at nothing — and because they are what an AUTHOR can create. Authoring is
/// additionally guarded one layer up: `osc::edit::set_story` refuses a story
/// with no act, so the editor cannot produce the shape this tier tolerates.
Diagnostic warning(std::string location, std::string message, std::string_view rule = {}) {
  return Diagnostic{.severity = Severity::Warning,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = std::string(rule),
                    .road = {},
                    .lane = {}};
}

Diagnostic error(std::string location, std::string message, std::string_view rule = {}) {
  return Diagnostic{.severity = Severity::Error,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = std::string(rule),
                    .road = {},
                    .lane = {}};
}

/// Reports a `::` in any modeled name. Separate from the caller's other checks
/// so every name-bearing element is held to the same rule.
void check_no_double_colon(std::vector<Diagnostic>& findings,
                           const std::string& name,
                           const std::string& location) {
  if (name.find("::") != std::string::npos) {
    findings.push_back(error(location,
                             fmt::format("name '{}' contains '::', which is reserved for "
                                         "parameter and catalog references",
                                         name),
                             rules::kNoDoubleColonPrefix));
  }
}

void check_entities(std::vector<Diagnostic>& findings, const Entities& entities) {
  std::set<std::string> seen;
  for (std::size_t index = 0; index < entities.scenario_objects.size(); ++index) {
    const ScenarioObject& object = entities.scenario_objects[index];
    const std::string location = fmt::format("Entities/ScenarioObject[{}]", index);

    if (object.name.empty()) {
      // No rule id: the schema requires @name, but an EMPTY name is a
      // RoadMaker-side refusal because every entityRef resolves through it.
      findings.push_back(error(location, "scenario object has no name"));
      continue;
    }
    check_no_double_colon(findings, object.name, location);

    if (!seen.insert(object.name).second) {
      findings.push_back(error(location,
                               fmt::format("duplicate scenario object name '{}'", object.name),
                               rules::kUniqueElementNames));
    }
  }
}

void check_traffic_signals(std::vector<Diagnostic>& findings, const RoadNetworkRef& road_network) {
  std::set<std::string> controller_names;
  for (const TrafficSignalController& controller : road_network.traffic_signal_controllers) {
    if (!controller.name.empty()) {
      controller_names.insert(controller.name);
    }
  }

  for (std::size_t index = 0; index < road_network.traffic_signal_controllers.size(); ++index) {
    const TrafficSignalController& controller = road_network.traffic_signal_controllers[index];
    const std::string location =
        fmt::format("RoadNetwork/TrafficSignals/TrafficSignalController[{}]", index);

    if (controller.name.empty()) {
      findings.push_back(error(location, "traffic signal controller has no name"));
    } else {
      check_no_double_colon(findings, controller.name, location);
    }

    if (controller.delay.has_value() && controller.reference.empty()) {
      findings.push_back(
          error(location,
                "traffic signal controller declares a delay but references no controller",
                rules::kTrafficSignalControllerReferences));
    }
    if (!controller.reference.empty() && controller_names.count(controller.reference) == 0) {
      findings.push_back(
          error(location,
                fmt::format("traffic signal controller references '{}', which is not a "
                            "controller in this scenario",
                            controller.reference),
                rules::kTrafficSignalControllerReferences));
    }

    for (std::size_t phase_index = 0; phase_index < controller.phases.size(); ++phase_index) {
      const Phase& phase = controller.phases[phase_index];
      const std::string phase_location = fmt::format("{}/Phase[{}]", location, phase_index);

      // NON-negative, per the rule's TEXT rather than its name: a zero-length
      // phase is legal and must not be refused.
      if (phase.duration < 0.0) {
        findings.push_back(error(phase_location,
                                 fmt::format("phase duration {} is negative", num(phase.duration)),
                                 rules::kPhaseDurationNonNegative));
      }
      check_no_double_colon(findings, phase.name, phase_location);

      for (std::size_t state_index = 0; state_index < phase.signal_states.size(); ++state_index) {
        if (phase.signal_states[state_index].traffic_signal_id.empty()) {
          findings.push_back(
              error(fmt::format("{}/TrafficSignalState[{}]", phase_location, state_index),
                    "traffic signal state names no signal",
                    rules::kTrafficSignalStateReferences));
        }
      }
    }
  }
}

/// What a `<Condition>`, a `<ManeuverGroup>` and a story action all need: the
/// entity names this scenario declares, and the controllers it declares with
/// the phase names each will actually be written with.
///
/// Built once per `validate_scenario` and threaded down, rather than rebuilt at
/// every reference — a scenario with a hundred conditions would otherwise walk
/// the entity list a hundred times, and more importantly the PHASE NAMES would
/// be synthesized repeatedly, which is exactly the kind of duplicated synthesis
/// `osc::phase_names` exists to prevent.
struct DocumentIndex {
  std::set<std::string> entity_names;
  /// controller `@name` -> the phase names `write_xosc` will emit for it.
  std::map<std::string, std::set<std::string>> controller_phases;
};

/// One `@entityRef`. Rule-less on purpose — see `check_storyboard`'s note on
/// `general.references_to_scenario_object`, which looks like the fit and is not.
void check_entity_ref(std::vector<Diagnostic>& findings,
                      const DocumentIndex& index,
                      const std::string& entity_ref,
                      const std::string& what,
                      const std::string& location) {
  if (entity_ref.empty()) {
    findings.push_back(error(location, fmt::format("{} names no entity", what)));
    return;
  }
  if (index.entity_names.count(entity_ref) == 0) {
    findings.push_back(error(
        location,
        fmt::format(
            "{} references entity '{}', which this scenario does not declare", what, entity_ref)));
  }
}

/// A `@trafficSignalControllerRef` + `@phase` pair, wherever it appears — a
/// `<TrafficSignalControllerCondition>` or a `<TrafficSignalControllerAction>`.
///
/// ★ THIS IS THE CHECK #248 WAS FILED FOR. The phase name is compared against
/// what `write_xosc` WILL EMIT (`osc::phase_names`), never against
/// `Phase::name` — which may legally be empty, and which the writer
/// deliberately never rewrites. Comparing against the model instead would pass
/// a reference that matches nothing in the file, in silence, which is precisely
/// the failure esmini was measured not to catch either (#257, #533).
void check_controller_phase_ref(std::vector<Diagnostic>& findings,
                                const DocumentIndex& index,
                                const std::string& controller_ref,
                                const std::string& phase,
                                const std::string& location) {
  if (controller_ref.empty()) {
    findings.push_back(error(location,
                             "traffic signal controller reference names no controller",
                             rules::kTrafficSignalControllerReferences));
    return;
  }
  const auto entry = index.controller_phases.find(controller_ref);
  if (entry == index.controller_phases.end()) {
    findings.push_back(
        error(location,
              fmt::format("references traffic signal controller '{}', which is not a controller "
                          "in this scenario",
                          controller_ref),
              rules::kTrafficSignalControllerReferences));
    return;
  }
  if (phase.empty()) {
    findings.push_back(error(location, "traffic signal controller reference names no phase"));
    return;
  }
  if (entry->second.count(phase) == 0) {
    findings.push_back(
        error(location,
              fmt::format("references phase '{}' of traffic signal controller '{}', which has no "
                          "phase of that name; the written names are the ones write_xosc "
                          "synthesizes, which are not necessarily Phase::name",
                          phase,
                          controller_ref),
              rules::kTrafficSignalControllerReferences));
  }
}

/// Conditions inside one trigger.
///
/// The delay check is the gap PR-B left: `osc/scenario.hpp` cited
/// `condition_delay_not_negative` on `Condition::delay` from the first commit,
/// and nothing enforced it — invisible while every `Condition` in the tree was
/// built by test code, reachable the moment a reader could take one from a
/// file.
void check_triggers(std::vector<Diagnostic>& findings,
                    const DocumentIndex& index,
                    const Trigger& trigger,
                    const std::string& location) {
  for (std::size_t group_index = 0; group_index < trigger.condition_groups.size(); ++group_index) {
    const ConditionGroup& group = trigger.condition_groups[group_index];
    for (std::size_t condition_index = 0; condition_index < group.conditions.size();
         ++condition_index) {
      const Condition& condition = group.conditions[condition_index];
      const std::string condition_location = fmt::format(
          "{}/ConditionGroup[{}]/Condition[{}]", location, group_index, condition_index);

      if (condition.delay < 0.0) {
        findings.push_back(
            error(condition_location,
                  fmt::format("condition delay {} is negative", num(condition.delay)),
                  rules::kConditionDelayNonNegative));
      }
      check_no_double_colon(findings, condition.name, condition_location);

      // ★ THE ARMS ARE AN XSD `choice`, so more than one set is not a document
      // this writer can emit — it would produce two sibling arms inside one
      // <Condition>. Refused rather than silently dropped: `write_condition`
      // emits the first arm it finds, and a model that carries two is an
      // authoring slip whose second arm would vanish without a word.
      const int armed = static_cast<int>(condition.by_entity.has_value()) +
                        static_cast<int>(condition.simulation_time.has_value()) +
                        static_cast<int>(condition.traffic_signal.has_value()) +
                        static_cast<int>(condition.traffic_signal_controller.has_value()) +
                        static_cast<int>(condition.storyboard_element_state.has_value());
      if (armed > 1) {
        findings.push_back(error(condition_location,
                                 fmt::format("<Condition> carries {} condition arms; the schema's "
                                             "choice admits exactly one",
                                             armed),
                                 rules::kValidSchema));
      }

      if (condition.traffic_signal.has_value() && condition.traffic_signal->name.empty()) {
        findings.push_back(error(condition_location + "/ByValueCondition/TrafficSignalCondition",
                                 "traffic signal condition names no signal",
                                 rules::kTrafficSignalStateReferences));
      }
      if (condition.traffic_signal_controller.has_value()) {
        check_controller_phase_ref(
            findings,
            index,
            condition.traffic_signal_controller->traffic_signal_controller_ref,
            condition.traffic_signal_controller->phase,
            condition_location + "/ByValueCondition/TrafficSignalControllerCondition");
      }
      if (condition.storyboard_element_state.has_value() &&
          condition.storyboard_element_state->storyboard_element_ref.empty()) {
        findings.push_back(
            error(condition_location + "/ByValueCondition/StoryboardElementStateCondition",
                  "storyboard element state condition names no element"));
      }

      if (condition.by_entity.has_value()) {
        const ByEntityCondition& by_entity = *condition.by_entity;
        const std::string by_entity_location = condition_location + "/ByEntityCondition";
        // `minOccurs` 1 on <EntityRef> inside <TriggeringEntities>: an empty
        // list is an element no schema-aware reader accepts.
        if (by_entity.triggering_entities.entity_refs.empty()) {
          findings.push_back(warning(by_entity_location + "/TriggeringEntities",
                                     "<TriggeringEntities> names no entity; the schema requires at "
                                     "least one <EntityRef>",
                                     rules::kValidSchema));
        }
        for (std::size_t ref_index = 0;
             ref_index < by_entity.triggering_entities.entity_refs.size();
             ++ref_index) {
          check_entity_ref(
              findings,
              index,
              by_entity.triggering_entities.entity_refs[ref_index].entity_ref,
              "triggering entity",
              fmt::format("{}/TriggeringEntities/EntityRef[{}]", by_entity_location, ref_index));
        }
        if (const auto* distance =
                std::get_if<RelativeDistanceCondition>(&by_entity.entity_condition)) {
          check_entity_ref(findings,
                           index,
                           distance->entity_ref,
                           "relative distance condition",
                           by_entity_location + "/EntityCondition/RelativeDistanceCondition");
        }
      }
    }
  }
}

void check_optional_trigger(std::vector<Diagnostic>& findings,
                            const DocumentIndex& index,
                            const std::optional<Trigger>& trigger,
                            const std::string& location) {
  if (trigger.has_value()) {
    check_triggers(findings, index, *trigger, location);
  }
}

/// A name that is required, non-empty and unique among its siblings.
void check_sibling_name(std::vector<Diagnostic>& findings,
                        std::set<std::string>& seen,
                        const std::string& name,
                        const char* element,
                        const std::string& location) {
  if (name.empty()) {
    // No rule id: `use="required"` is a schema constraint, not one of Annex C's
    // checker rules, and citing a UID that does not cover it would be worse
    // than citing none — the `check_route` call.
    findings.push_back(
        warning(location, fmt::format("<{}> has no name, which the schema requires", element)));
    return;
  }
  check_no_double_colon(findings, name, location);
  if (!seen.insert(name).second) {
    findings.push_back(
        warning(location,
                fmt::format("a <{}> named '{}' is already declared at this level", element, name),
                rules::kUniqueElementNames));
  }
}

/// Forward-declared because the storyboard checks below sit ABOVE the position
/// check they share with `<Init>` — the alternative is moving a function the
/// init-action checks read from its own section, which reads worse than four
/// lines here.
void check_road_relative_position(std::vector<Diagnostic>& findings,
                                  const Position& position,
                                  bool has_logic_file,
                                  const std::string& location);

/// One story `<Action>` — p8-s4, issue #248.
void check_action(std::vector<Diagnostic>& findings,
                  const DocumentIndex& index,
                  const Action& action,
                  bool has_logic_file,
                  const std::string& location) {
  if (const auto* global = std::get_if<GlobalAction>(&action.action)) {
    if (!global->infrastructure.has_value()) {
      return; // The arm rode the preserved tier whole.
    }
    const std::string signal_location =
        location + "/GlobalAction/InfrastructureAction/TrafficSignalAction";
    const auto& arm = global->infrastructure->traffic_signal.action;
    if (const auto* state = std::get_if<TrafficSignalStateAction>(&arm)) {
      if (state->name.empty()) {
        findings.push_back(error(signal_location + "/TrafficSignalStateAction",
                                 "traffic signal state action names no signal",
                                 rules::kTrafficSignalStateReferences));
      }
    } else if (const auto* controller = std::get_if<TrafficSignalControllerAction>(&arm)) {
      check_controller_phase_ref(findings,
                                 index,
                                 controller->traffic_signal_controller_ref,
                                 controller->phase,
                                 signal_location + "/TrafficSignalControllerAction");
    }
    return;
  }

  const auto* entry = std::get_if<PrivateAction>(&action.action);
  if (entry == nullptr) {
    return; // <UserDefinedAction> or an unmodeled arm, riding `preserved`.
  }

  // ★ ONE ARM PER STORY ACTION. `<Action>` wraps a single choice, so a
  // `PrivateAction` carrying two arms would expand into two `<PrivateAction>`
  // siblings inside one `<Action>` — invalid. In `<Init>` the same struct
  // legally expands, because `<Private>` admits `<PrivateAction>*`.
  const int armed = static_cast<int>(entry->teleport.has_value()) +
                    static_cast<int>(entry->longitudinal.has_value()) +
                    static_cast<int>(entry->routing.has_value()) +
                    static_cast<int>(entry->lateral.has_value());
  if (armed > 1) {
    findings.push_back(error(location + "/PrivateAction",
                             fmt::format("a story <Action> carries {} private-action arms; each "
                                         "<Action> wraps exactly one, so these belong in "
                                         "separate <Action> elements",
                                         armed),
                             rules::kValidSchema));
  }

  if (entry->teleport.has_value()) {
    check_road_relative_position(findings,
                                 entry->teleport->position,
                                 has_logic_file,
                                 location + "/PrivateAction/TeleportAction/Position");
  }
  if (entry->lateral.has_value() && entry->lateral->lane_change.has_value()) {
    const LaneChangeAction& change = *entry->lateral->lane_change;
    const std::string change_location = location + "/PrivateAction/LateralAction/LaneChangeAction";
    // "If no road network (logicFile) is defined ... the scenario shall not use
    // ... LaneChangeAction". Checkable in full: both ends are in this document.
    if (!has_logic_file) {
      findings.push_back(error(change_location,
                               "<LaneChangeAction> names a lane, but the scenario links no "
                               "<LogicFile> for it to be resolved against",
                               rules::kInvalidElementsIfNoRoadNetwork));
    }
    if (const auto* relative = std::get_if<RelativeTargetLane>(&change.target)) {
      check_entity_ref(findings,
                       index,
                       relative->entity_ref,
                       "relative target lane",
                       change_location + "/LaneChangeTarget/RelativeTargetLane");
    } else if (const auto* absolute = std::get_if<AbsoluteTargetLane>(&change.target)) {
      if (absolute->value.empty()) {
        findings.push_back(error(change_location + "/LaneChangeTarget/AbsoluteTargetLane",
                                 "<AbsoluteTargetLane> names no lane",
                                 rules::kRoadLaneExists));
      }
    }
  }
}

/// The modeled `<Story>` tree — p8-s4, issue #248.
///
/// Every level checks the same two things the schema asks of it: a unique,
/// non-empty `@name` among its siblings, and at least one of whatever its
/// `minOccurs` requires. Refused rather than reported, because each produces a
/// document a schema-aware reader rejects, and the writer's contract is that
/// what it emits loads.
void check_stories(std::vector<Diagnostic>& findings,
                   const DocumentIndex& index,
                   const Storyboard& storyboard,
                   bool has_logic_file) {
  std::set<std::string> story_names;
  for (std::size_t story_index = 0; story_index < storyboard.stories.size(); ++story_index) {
    const Story& story = storyboard.stories[story_index];
    const std::string story_location = fmt::format("Storyboard/Story[{}]", story_index);
    check_sibling_name(findings, story_names, story.name, "Story", story_location);
    if (story.acts.empty()) {
      findings.push_back(warning(story_location,
                                 "<Story> has no <Act>; the schema requires at "
                                 "least one"));
    }

    std::set<std::string> act_names;
    for (std::size_t act_index = 0; act_index < story.acts.size(); ++act_index) {
      const Act& act = story.acts[act_index];
      const std::string act_location = fmt::format("{}/Act[{}]", story_location, act_index);
      check_sibling_name(findings, act_names, act.name, "Act", act_location);
      if (act.maneuver_groups.empty()) {
        findings.push_back(warning(
            act_location, "<Act> has no <ManeuverGroup>; the schema requires at least one"));
      }
      check_optional_trigger(findings, index, act.start_trigger, act_location + "/StartTrigger");
      check_optional_trigger(findings, index, act.stop_trigger, act_location + "/StopTrigger");

      std::set<std::string> group_names;
      for (std::size_t group_index = 0; group_index < act.maneuver_groups.size(); ++group_index) {
        const ManeuverGroup& group = act.maneuver_groups[group_index];
        const std::string group_location =
            fmt::format("{}/ManeuverGroup[{}]", act_location, group_index);
        check_sibling_name(findings, group_names, group.name, "ManeuverGroup", group_location);
        // An EMPTY actor list is legal — "allowed for situations where the
        // maneuvers ... are not related to instances of Entity" (§7.3.1), which
        // is what an infrastructure-only group is. Only the refs themselves are
        // checked.
        for (std::size_t ref_index = 0; ref_index < group.actors.size(); ++ref_index) {
          check_entity_ref(findings,
                           index,
                           group.actors[ref_index].entity_ref,
                           "actor",
                           fmt::format("{}/Actors/EntityRef[{}]", group_location, ref_index));
        }

        std::set<std::string> maneuver_names;
        for (std::size_t maneuver_index = 0; maneuver_index < group.maneuvers.size();
             ++maneuver_index) {
          const StoryManeuver& maneuver = group.maneuvers[maneuver_index];
          const std::string maneuver_location =
              fmt::format("{}/Maneuver[{}]", group_location, maneuver_index);
          check_sibling_name(
              findings, maneuver_names, maneuver.name, "Maneuver", maneuver_location);
          if (maneuver.events.empty()) {
            findings.push_back(warning(
                maneuver_location, "<Maneuver> has no <Event>; the schema requires at least one"));
          }

          std::set<std::string> event_names;
          for (std::size_t event_index = 0; event_index < maneuver.events.size(); ++event_index) {
            const Event& event = maneuver.events[event_index];
            const std::string event_location =
                fmt::format("{}/Event[{}]", maneuver_location, event_index);
            check_sibling_name(findings, event_names, event.name, "Event", event_location);
            if (event.priority.empty()) {
              findings.push_back(
                  warning(event_location, "<Event> has no priority, which the schema requires"));
            }
            if (event.actions.empty()) {
              findings.push_back(warning(
                  event_location, "<Event> has no <Action>; the schema requires at least one"));
            }
            check_optional_trigger(
                findings, index, event.start_trigger, event_location + "/StartTrigger");

            std::set<std::string> action_names;
            for (std::size_t action_index = 0; action_index < event.actions.size();
                 ++action_index) {
              const std::string action_location =
                  fmt::format("{}/Action[{}]", event_location, action_index);
              check_sibling_name(findings,
                                 action_names,
                                 event.actions[action_index].name,
                                 "Action",
                                 action_location);
              check_action(
                  findings, index, event.actions[action_index], has_logic_file, action_location);
            }
          }
        }
      }
    }
  }
}

/// One road- or lane-relative position inside an init action.
///
/// `has_logic_file` is threaded in rather than looked up per call because the
/// no-road-network rule is a property of the WHOLE document, not of the
/// position: a `<LanePosition>` is perfectly legal, right up until the scenario
/// turns out to link no `.xodr` for its `roadId` to be resolved against.
void check_road_relative_position(std::vector<Diagnostic>& findings,
                                  const Position& position,
                                  bool has_logic_file,
                                  const std::string& location) {
  const std::string* road_id = nullptr;
  const std::string* lane_id = nullptr;
  double station = 0.0;
  const char* element = nullptr;

  if (const auto* road = std::get_if<RoadPosition>(&position)) {
    road_id = &road->road_id;
    station = road->s;
    element = "RoadPosition";
  } else if (const auto* lane = std::get_if<LanePosition>(&position)) {
    road_id = &lane->road_id;
    lane_id = &lane->lane_id;
    station = lane->s;
    element = "LanePosition";
  } else {
    return; // A <WorldPosition> refers to no road and none of this applies.
  }

  const std::string element_location = fmt::format("{}/{}", location, element);

  if (!has_logic_file) {
    findings.push_back(
        error(element_location,
              fmt::format("<{}> names a road network, but the scenario links no <LogicFile> "
                          "for its roadId to be resolved against",
                          element),
              rules::kInvalidElementsIfNoRoadNetwork));
  }
  if (road_id->empty()) {
    findings.push_back(error(
        element_location, fmt::format("<{}> names no road", element), rules::kRoadLaneExists));
  }
  if (lane_id != nullptr && lane_id->empty()) {
    findings.push_back(
        error(element_location, "<LanePosition> names no lane", rules::kRoadLaneExists));
  }
  if (station < 0.0) {
    findings.push_back(
        error(element_location,
              fmt::format("s-coordinate {} is negative, which is outside the boundaries of any "
                          "road it could resolve to",
                          num(station)),
              rules::kRoadLaneOffsetInBounds));
  }
}

/// One `<Route>` (p8-s3, issue #247), and the route names already seen in this
/// document — `@name` must be unique among siblings, and every route this
/// version writes is a sibling of every other because they all hang off
/// `<Init>`.
void check_route(std::vector<Diagnostic>& findings,
                 std::set<std::string>& route_names,
                 const Route& route,
                 bool has_logic_file,
                 const std::string& location) {
  // No rule id for the missing name: `use="required"` is a schema constraint,
  // not one of Annex C's checker rules, and citing a UID that does not cover it
  // would be worse than citing none.
  if (route.name.empty()) {
    findings.push_back(error(location, "<Route> has no name, which the schema requires"));
  } else if (!route_names.insert(route.name).second) {
    findings.push_back(error(location,
                             fmt::format("a route named '{}' is already declared", route.name),
                             rules::kUniqueElementNames));
  }
  // minOccurs="2": "at least two waypoints are needed to define a route".
  //
  // ★ COUNTS WHAT IS EMITTED, not what is modeled. A waypoint whose position
  // this version does not model rides the preserved tier and is written out
  // verbatim, so it is a `<Waypoint>` in the document exactly like a modeled
  // one. Counting only `route.waypoints` made the writer refuse a file the
  // reader had just accepted — see preserved_element_count.
  const std::size_t emitted_waypoints =
      route.waypoints.size() + preserved_element_count(route.preserved, "Waypoint");
  if (emitted_waypoints < 2) {
    // Refused rather than padded — a one-waypoint route names a destination and
    // no origin, and inventing the missing end is not this writer's call.
    findings.push_back(
        error(location,
              fmt::format("<Route> has {} waypoint(s); at least two are needed to define one",
                          emitted_waypoints)));
  }
  for (std::size_t index = 0; index < route.waypoints.size(); ++index) {
    const RouteWaypoint& waypoint = route.waypoints[index];
    const std::string waypoint_location = fmt::format("{}/Waypoint[{}]", location, index);
    if (waypoint.route_strategy.empty()) {
      findings.push_back(
          error(waypoint_location, "<Waypoint> has no routeStrategy, which the schema requires"));
    }
    // "When a Waypoint of a Route is ambiguous then the Waypoint can be defined
    // in Road or Lane position types to be unambiguous." A <WorldPosition>
    // waypoint is the ambiguous case the rule names — a WARNING, not an error:
    // it is legal, it is merely how a route stops being reproducible.
    if (std::holds_alternative<WorldPosition>(waypoint.position)) {
      findings.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = waypoint_location + "/Position/WorldPosition",
                     .message = "a route waypoint given as a world position is ambiguous where "
                                "several lanes meet; a <RoadPosition> or <LanePosition> names "
                                "which one",
                     .rule_id = std::string(rules::kAmbiguousRouteWaypoints),
                     .road = {},
                     .lane = {}});
    }
    check_road_relative_position(
        findings, waypoint.position, has_logic_file, waypoint_location + "/Position");
  }
}

void check_storyboard(std::vector<Diagnostic>& findings, const Scenario& scenario) {
  DocumentIndex document;
  for (const ScenarioObject& object : scenario.entities.scenario_objects) {
    document.entity_names.insert(object.name);
  }
  // The phase names as WRITTEN, not as modeled — the #248 trap, resolved once
  // for the whole document. See `check_controller_phase_ref`.
  for (const TrafficSignalController& controller :
       scenario.road_network.traffic_signal_controllers) {
    const std::vector<std::string> names = phase_names(controller);
    document.controller_phases[controller.name].insert(names.begin(), names.end());
  }
  const std::set<std::string>& entity_names = document.entity_names;

  const bool has_logic_file = scenario.road_network.logic_file.has_value();
  // Route names are unique across the whole init block, not per entity: they
  // are siblings once flattened, and a simulator resolves a route by name.
  std::set<std::string> route_names;
  const auto& privates = scenario.storyboard.init.actions.privates;
  for (std::size_t index = 0; index < privates.size(); ++index) {
    const std::string location = fmt::format("Storyboard/Init/Actions/Private[{}]", index);
    const std::string& entity_ref = privates[index].entity_ref;

    for (std::size_t action_index = 0; action_index < privates[index].actions.size();
         ++action_index) {
      const PrivateAction& action = privates[index].actions[action_index];
      const std::string action_location =
          fmt::format("{}/PrivateAction[{}]", location, action_index);
      if (action.teleport.has_value()) {
        check_road_relative_position(findings,
                                     action.teleport->position,
                                     has_logic_file,
                                     action_location + "/TeleportAction/Position");
      }
      // A negative target speed is not a rule violation the catalogue names —
      // it is a RoadMaker-side refusal, because a scenario that starts an actor
      // reversing at -30 m/s is a data-entry slip and never an intention worth
      // silently exporting.
      if (action.longitudinal.has_value() && action.longitudinal->speed.has_value() &&
          action.longitudinal->speed->absolute_target.has_value() &&
          action.longitudinal->speed->absolute_target->value < 0.0) {
        findings.push_back(
            error(action_location + "/LongitudinalAction/SpeedAction/SpeedActionTarget/"
                                    "AbsoluteTargetSpeed",
                  fmt::format("target speed {} is negative",
                              num(action.longitudinal->speed->absolute_target->value))));
      }
      if (action.routing.has_value() && action.routing->assign_route.has_value() &&
          action.routing->assign_route->route.has_value()) {
        check_route(findings,
                    route_names,
                    *action.routing->assign_route->route,
                    has_logic_file,
                    action_location + "/RoutingAction/AssignRouteAction/Route");
      }
    }

    if (entity_ref.empty()) {
      findings.push_back(error(location, "init action names no entity"));
    } else if (entity_names.count(entity_ref) == 0) {
      // No rule id on purpose. `general.references_to_scenario_object` looks
      // like the fit and is not: it constrains the referenced object's TYPE to
      // Vehicle or Pedestrian, which says nothing about a reference that
      // resolves to no object at all. See roadmaker/osc/rules.hpp.
      findings.push_back(
          error(location,
                fmt::format("init action references entity '{}', which this scenario does not "
                            "declare",
                            entity_ref)));
    }
  }

  check_triggers(findings, document, scenario.storyboard.stop_trigger, "Storyboard/StopTrigger");
  check_stories(findings, document, scenario.storyboard, has_logic_file);
}

/// Walks every preserved fragment in the document and reports any that is not
/// well-formed XML.
///
/// A deliberate strengthening of the OpenDRIVE writer, which ignores
/// `append_buffer`'s parse status and can therefore drop a corrupt fragment in
/// silence — the opposite of the never-drop contract (ADR-0014 §6).
void check_preserved_fragments(std::vector<Diagnostic>& findings, const Scenario& scenario) {
  const auto check = [&findings](const RawXml& preserved, const std::string& location) {
    for (std::size_t index = 0; index < preserved.children.size(); ++index) {
      if (!fragment_is_well_formed(preserved.children[index])) {
        findings.push_back(error(fmt::format("{}/preserved[{}]", location, index),
                                 "preserved fragment is not well-formed XML and would be "
                                 "dropped by re-emission"));
      }
    }
  };

  check(scenario.preserved, "OpenSCENARIO");
  check(scenario.header.preserved, "FileHeader");
  check(scenario.catalog_locations, "CatalogLocations");
  check(scenario.road_network.preserved, "RoadNetwork");
  check(scenario.entities.preserved, "Entities");
  check(scenario.storyboard.preserved, "Storyboard");
  check(scenario.storyboard.init.preserved, "Storyboard/Init");
  check(scenario.storyboard.init.actions.preserved, "Storyboard/Init/Actions");

  for (std::size_t index = 0; index < scenario.entities.scenario_objects.size(); ++index) {
    const ScenarioObject& object = scenario.entities.scenario_objects[index];
    const std::string location = fmt::format("Entities/ScenarioObject[{}]", index);
    check(object.preserved, location);
    // The <Properties> wrapper tier is a second fragment store on the entity,
    // so it needs its own walk or a corrupt <File> read from a foreign catalog
    // would be dropped by re-emission in exactly the silence this function
    // exists to prevent.
    if (const auto* vehicle = std::get_if<Vehicle>(&object.entity_object)) {
      check(vehicle->properties_preserved, location + "/Vehicle/Properties");
    } else if (const auto* pedestrian = std::get_if<Pedestrian>(&object.entity_object)) {
      check(pedestrian->properties_preserved, location + "/Pedestrian/Properties");
    }
  }
  for (std::size_t index = 0; index < scenario.road_network.traffic_signal_controllers.size();
       ++index) {
    const TrafficSignalController& controller =
        scenario.road_network.traffic_signal_controllers[index];
    const std::string location =
        fmt::format("RoadNetwork/TrafficSignals/TrafficSignalController[{}]", index);
    check(controller.preserved, location);
    for (std::size_t phase_index = 0; phase_index < controller.phases.size(); ++phase_index) {
      check(controller.phases[phase_index].preserved,
            fmt::format("{}/Phase[{}]", location, phase_index));
    }
  }
  // The modeled story tree (p8-s4, issue #248). Every level carries a preserved
  // tier, and `<ManeuverGroup>` carries a SECOND fragment store for the catalog
  // references that must be re-emitted before its maneuvers — both are walked,
  // or a corrupt fragment is dropped in exactly the silence this function
  // exists to prevent.
  for (std::size_t story_index = 0; story_index < scenario.storyboard.stories.size();
       ++story_index) {
    const Story& story = scenario.storyboard.stories[story_index];
    const std::string story_location = fmt::format("Storyboard/Story[{}]", story_index);
    check(story.preserved, story_location);
    for (std::size_t act_index = 0; act_index < story.acts.size(); ++act_index) {
      const Act& act = story.acts[act_index];
      const std::string act_location = fmt::format("{}/Act[{}]", story_location, act_index);
      check(act.preserved, act_location);
      for (std::size_t group_index = 0; group_index < act.maneuver_groups.size(); ++group_index) {
        const ManeuverGroup& group = act.maneuver_groups[group_index];
        const std::string group_location =
            fmt::format("{}/ManeuverGroup[{}]", act_location, group_index);
        check(group.preserved, group_location);
        check(group.actors_preserved, group_location + "/Actors");
        for (std::size_t catalog_index = 0;
             catalog_index < group.preserved_catalog_references.size();
             ++catalog_index) {
          if (!fragment_is_well_formed(group.preserved_catalog_references[catalog_index])) {
            findings.push_back(
                error(fmt::format("{}/CatalogReference[{}]", group_location, catalog_index),
                      "preserved catalog reference is not well-formed XML and would be dropped "
                      "by re-emission"));
          }
        }
        for (std::size_t maneuver_index = 0; maneuver_index < group.maneuvers.size();
             ++maneuver_index) {
          const StoryManeuver& maneuver = group.maneuvers[maneuver_index];
          const std::string maneuver_location =
              fmt::format("{}/Maneuver[{}]", group_location, maneuver_index);
          check(maneuver.preserved, maneuver_location);
          for (std::size_t event_index = 0; event_index < maneuver.events.size(); ++event_index) {
            const Event& event = maneuver.events[event_index];
            const std::string event_location =
                fmt::format("{}/Event[{}]", maneuver_location, event_index);
            check(event.preserved, event_location);
            for (std::size_t action_index = 0; action_index < event.actions.size();
                 ++action_index) {
              check(event.actions[action_index].preserved,
                    fmt::format("{}/Action[{}]", event_location, action_index));
            }
          }
        }
      }
    }
  }
}

} // namespace

std::vector<std::string> phase_names(const TrafficSignalController& controller) {
  // ★ `used` IS PER CONTROLLER. Uniqueness is required within a controller, and
  // hoisting the set across controllers would rename the second one's phases to
  // "go_1"/"stop_1" — breaking the invariant that every controller decomposed
  // from ONE junction timeline carries the SAME phase names, which is exactly
  // what a traffic-signal condition references. The output stays schema-valid
  // and a simulator loads it happily, so nothing but a dedicated test catches
  // it. `std::set` rather than `std::unordered_set` because a later refactor
  // that iterates it must not become nondeterministic.
  std::set<std::string> used;
  std::vector<std::string> names;
  names.reserve(controller.phases.size());

  for (const Phase& phase : controller.phases) {
    // A non-empty authored name wins, and still joins the de-dup pool, so an
    // author who writes "go" on phase 0 pushes a synthesized "go" on phase 3 to
    // "go_1" and never the reverse.
    std::string base = phase.name;
    if (base.empty()) {
      base = phase.semantics.has_value() ? semantics_name(*phase.semantics) : "phase";
    }

    // Terminating: `used` grows by one per phase, so the probe runs at most
    // `phases.size()` times.
    std::string candidate = base;
    for (unsigned suffix = 1; used.count(candidate) != 0; ++suffix) {
      candidate = fmt::format("{}_{}", base, suffix);
    }
    used.insert(candidate);
    names.push_back(std::move(candidate));
  }
  return names;
}

std::vector<Diagnostic> validate_scenario(const Scenario& scenario, const WriteOptions& options) {
  (void)options; // No revision-specific rule exists: every citable UID is <= 1.2.0.

  std::vector<Diagnostic> findings;
  check_entities(findings, scenario.entities);
  check_traffic_signals(findings, scenario.road_network);
  check_storyboard(findings, scenario);
  check_preserved_fragments(findings, scenario);
  return findings;
}

Expected<std::string> write_xosc(const Scenario& scenario, const WriteOptions& options) {
  // Refuse before writing, demoting the first blocking finding — the shape
  // core/src/xodr/writer.cpp:2764-2766 established. The rule id is lost in the
  // demotion, which is why validate_scenario is public: it is where a caller
  // reads which normative rule broke.
  const std::vector<Diagnostic> findings = validate_scenario(scenario, options);
  for (const Diagnostic& finding : findings) {
    if (finding.severity == Severity::Error) {
      return tl::unexpected<Error>(Error{.code = ErrorCode::InvalidArgument,
                                         .message = finding.message,
                                         .context = finding.location});
    }
  }

  pugi::xml_document doc;
  pugi::xml_node decl = doc.append_child(pugi::node_declaration);
  decl.append_attribute("version").set_value("1.0");
  decl.append_attribute("encoding").set_value("UTF-8");

  pugi::xml_node root = doc.append_child("OpenSCENARIO");

  pugi::xml_node header = root.append_child("FileHeader");
  header.append_attribute("revMajor").set_value(1);
  // The ONE structural version conditional. It stays alone only because the
  // always-present skeleton below is emitted unconditionally: every one of
  // those elements was required at 1.0/1.2 and relaxed later, so making any of
  // them conditional would add a second version branch here.
  header.append_attribute("revMinor").set_value(options.target_version == OscVersion::v1_4 ? 4 : 2);
  header.append_attribute("date").set_value(scenario.header.date.c_str());
  header.append_attribute("description").set_value(scenario.header.description.c_str());
  header.append_attribute("author").set_value(scenario.header.author.c_str());
  write_preserved_attributes(header, scenario.header.preserved);
  write_preserved_children(header, scenario.header.preserved);

  pugi::xml_node parameters = root.append_child("ParameterDeclarations");
  for (const ParameterDeclaration& parameter : scenario.parameter_declarations) {
    pugi::xml_node node = parameters.append_child("ParameterDeclaration");
    node.append_attribute("name").set_value(parameter.name.c_str());
    node.append_attribute("parameterType").set_value(parameter.parameter_type.c_str());
    node.append_attribute("value").set_value(parameter.value.c_str());
    write_preserved_attributes(node, parameter.preserved);
    write_preserved_children(node, parameter.preserved);
  }

  pugi::xml_node catalogs = root.append_child("CatalogLocations");
  write_preserved_attributes(catalogs, scenario.catalog_locations);
  write_preserved_children(catalogs, scenario.catalog_locations);

  pugi::xml_node road_network = root.append_child("RoadNetwork");
  write_preserved_attributes(road_network, scenario.road_network.preserved);
  if (scenario.road_network.logic_file.has_value()) {
    pugi::xml_node node = road_network.append_child("LogicFile");
    node.append_attribute("filepath").set_value(scenario.road_network.logic_file->filepath.c_str());
    write_preserved_attributes(node, scenario.road_network.logic_file->preserved);
    write_preserved_children(node, scenario.road_network.logic_file->preserved);
  }
  if (scenario.road_network.scene_graph_file.has_value()) {
    pugi::xml_node node = road_network.append_child("SceneGraphFile");
    node.append_attribute("filepath")
        .set_value(scenario.road_network.scene_graph_file->filepath.c_str());
    write_preserved_attributes(node, scenario.road_network.scene_graph_file->preserved);
    write_preserved_children(node, scenario.road_network.scene_graph_file->preserved);
  }
  // <TrafficSignals> follows the two file references and precedes <UsedArea>,
  // per the RoadNetwork sequence.
  write_traffic_signals(road_network, scenario.road_network, options);
  write_preserved_children(road_network, scenario.road_network.preserved);

  write_entities(root, scenario.entities);
  write_storyboard(root, scenario.storyboard);
  write_preserved_children(root, scenario.preserved);

  std::ostringstream out;
  doc.save(out, "  ", pugi::format_default, pugi::encoding_utf8);
  return std::move(out).str();
}

Expected<void> save_xosc(const Scenario& scenario,
                         const std::filesystem::path& path,
                         const WriteOptions& options) {
  const Expected<std::string> text = write_xosc(scenario, options);
  if (!text) {
    return tl::unexpected<Error>(text.error());
  }

  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open file for writing", path.string());
  }
  stream.write(text->data(), static_cast<std::streamsize>(text->size()));
  if (!stream.good()) {
    return make_error(ErrorCode::IoFailure, "write failed", path.string());
  }
  return {};
}

} // namespace roadmaker::osc
