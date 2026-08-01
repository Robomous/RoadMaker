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
void write_phases(pugi::xml_node controller_node,
                  const TrafficSignalController& controller,
                  const WriteOptions& options) {
  std::set<std::string> used;

  for (const Phase& phase : controller.phases) {
    std::string base = phase.name;
    if (base.empty()) {
      base = phase.semantics.has_value() ? semantics_name(*phase.semantics) : "phase";
    }

    std::string candidate = base;
    for (unsigned suffix = 1; used.count(candidate) != 0; ++suffix) {
      candidate = fmt::format("{}_{}", base, suffix);
    }
    used.insert(candidate);

    pugi::xml_node node = controller_node.append_child("Phase");
    node.append_attribute("name").set_value(candidate.c_str());
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
  if (!preserved_written) {
    pugi::xml_node node;
    open(node);
    close(node);
  }
}

void write_trigger(pugi::xml_node parent, const char* element, const Trigger& trigger) {
  pugi::xml_node node = parent.append_child(element);
  write_preserved_attributes(node, trigger.preserved);

  for (const ConditionGroup& group : trigger.condition_groups) {
    pugi::xml_node group_node = node.append_child("ConditionGroup");
    write_preserved_attributes(group_node, group.preserved);

    for (const Condition& condition : group.conditions) {
      pugi::xml_node condition_node = group_node.append_child("Condition");
      condition_node.append_attribute("name").set_value(condition.name.c_str());
      set_num(condition_node, "delay", condition.delay);
      condition_node.append_attribute("conditionEdge").set_value(condition.condition_edge.c_str());
      write_preserved_attributes(condition_node, condition.preserved);

      if (condition.simulation_time.has_value()) {
        pugi::xml_node by_value = condition_node.append_child("ByValueCondition");
        pugi::xml_node time_node = by_value.append_child("SimulationTimeCondition");
        set_num(time_node, "value", condition.simulation_time->value);
        time_node.append_attribute("rule").set_value(condition.simulation_time->rule.c_str());
        write_preserved_attributes(time_node, condition.simulation_time->preserved);
        write_preserved_children(time_node, condition.simulation_time->preserved);
      }

      write_preserved_children(condition_node, condition.preserved);
    }

    write_preserved_children(group_node, group.preserved);
  }

  write_preserved_children(node, trigger.preserved);
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
  for (const std::string& story : storyboard.preserved_stories) {
    append_fragment(node, story);
  }

  // Always emitted, empty if it has no condition groups.
  write_trigger(node, "StopTrigger", storyboard.stop_trigger);

  write_preserved_children(node, storyboard.preserved);
}

// --- validation -------------------------------------------------------------

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

/// Conditions inside one trigger.
///
/// The delay check is the gap PR-B left: `osc/scenario.hpp` cited
/// `condition_delay_not_negative` on `Condition::delay` from the first commit,
/// and nothing enforced it — invisible while every `Condition` in the tree was
/// built by test code, reachable the moment a reader could take one from a
/// file.
void check_triggers(std::vector<Diagnostic>& findings,
                    const Trigger& trigger,
                    const std::string& location) {
  for (std::size_t group_index = 0; group_index < trigger.condition_groups.size(); ++group_index) {
    const ConditionGroup& group = trigger.condition_groups[group_index];
    for (std::size_t index = 0; index < group.conditions.size(); ++index) {
      const Condition& condition = group.conditions[index];
      const std::string condition_location =
          fmt::format("{}/ConditionGroup[{}]/Condition[{}]", location, group_index, index);

      if (condition.delay < 0.0) {
        findings.push_back(
            error(condition_location,
                  fmt::format("condition delay {} is negative", num(condition.delay)),
                  rules::kConditionDelayNonNegative));
      }
      check_no_double_colon(findings, condition.name, condition_location);
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
  std::set<std::string> entity_names;
  for (const ScenarioObject& object : scenario.entities.scenario_objects) {
    entity_names.insert(object.name);
  }

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

  check_triggers(findings, scenario.storyboard.stop_trigger, "Storyboard/StopTrigger");
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
  for (std::size_t index = 0; index < scenario.storyboard.preserved_stories.size(); ++index) {
    if (!fragment_is_well_formed(scenario.storyboard.preserved_stories[index])) {
      findings.push_back(error(fmt::format("Storyboard/Story[{}]", index),
                               "preserved story fragment is not well-formed XML and would be "
                               "dropped by re-emission"));
    }
  }
}

} // namespace

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
