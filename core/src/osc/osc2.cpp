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

/// The OpenSCENARIO DSL v2.2.0 emitter (p8-s6, #327). See osc/osc2.hpp for the
/// scope and why it is narrow.
///
/// EVERY CONSTRUCT BELOW IS QUOTED FROM THE SPECIFICATION, and the citation is
/// on the line that emits it. That discipline is not decoration here: unlike
/// the `.xosc` writer there is no schema in the tree, no parser, and no
/// simulator in CI to contradict this output — so the specification and these
/// citations are the only things standing behind it.

#include "roadmaker/osc/osc2.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace roadmaker::osc {
namespace {

/// Shortest-precision round-trippable formatting; locale-independent.
///
/// Copied from `osc/writer.cpp` rather than shared, the same call that file
/// makes about `xodr/writer.cpp`: the two formats' scalar policies are
/// independent and either may need to diverge.
std::string num(double value) {
  std::string out = fmt::format("{}", value);
  if (out == "-0") {
    return "0";
  }
  return out;
}

Diagnostic warning(std::string location, std::string message) {
  return Diagnostic{.severity = Severity::Warning,
                    .location = std::move(location),
                    .message = std::move(message),
                    .rule_id = {}, // No checker-rule catalogue exists for the DSL.
                    .road = {},
                    .lane = {}};
}

// --- the documented subset ---------------------------------------------------
//
// Kept adjacent so a change to one is visibly a change to the other. The doc
// mirrors both, and a gtest compares them.

constexpr std::array kSupported{
    Osc2SubsetRow{"import osc.standard.all", "always emitted (§7.7.5.2)"},
    Osc2SubsetRow{"scenario <name>:", "Osc2WriteOptions::scenario_name"},
    Osc2SubsetRow{"map.set_map_file(\"...\")", "<RoadNetwork><LogicFile @filepath>"},
    Osc2SubsetRow{"<actor>: vehicle", "<ScenarioObject> holding a <Vehicle>"},
    Osc2SubsetRow{"<actor>: pedestrian", "<ScenarioObject> holding a <Pedestrian>"},
    Osc2SubsetRow{"keep(it.vehicle_category == ...)", "<Vehicle @vehicleCategory>"},
    Osc2SubsetRow{"do parallel:", "the <Init> actions, which all start together"},
    Osc2SubsetRow{"<actor>.drive()", "an entity with any <Init> action"},
    Osc2SubsetRow{"speed(<v>mps)", "<SpeedAction><AbsoluteTargetSpeed @value>"},
};

constexpr std::array kUnsupported{
    Osc2SubsetRow{"<Story> / <Act> / <Event> / <Action>",
                  "the storyboard's phase structure has no faithful concrete-subset "
                  "spelling here; emitting a guess would be worse than reporting it"},
    Osc2SubsetRow{"<Trigger> / <Condition>",
                  "conditions are modifiers and constraints in the DSL, not triggers; "
                  "the mapping is not one-to-one"},
    Osc2SubsetRow{"<Route> / <Waypoint>",
                  "a DSL route comes from map.create_route(get_odr_points(...)), an "
                  "external method this exporter cannot synthesize"},
    Osc2SubsetRow{"<TrafficSignalController> / <Phase>",
                  "traffic lights are domain-model actors with their own behaviour; "
                  "the 1.x controller decomposition does not carry over"},
    Osc2SubsetRow{"<LanePosition> / <RoadPosition>",
                  "placement is a position()/lane() modifier relative to a route, and "
                  "there is no route to relate it to (see above)"},
    Osc2SubsetRow{"OpenSCENARIO 2.x import",
                  "export-only at v0.1.0; a parser dependency goes through the "
                  "dependency policy's stop-and-ask first"},
    Osc2SubsetRow{"abstract and logical scenarios",
                  "the internal model holds fixed values only, so it cannot express a "
                  "parameter range or a constraint space"},
};

/// A DSL identifier for `name`.
///
/// ★ THE NAMES COME FROM A DIFFERENT LANGUAGE'S RULES. `ScenarioObject/@name`
/// is an XML string and routinely contains spaces or dashes; a DSL identifier
/// cannot. Rewriting is therefore normal rather than exceptional — and it is
/// REPORTED, because an actor that appears in the `.osc` under a different name
/// than in the `.xosc` is exactly the sort of quiet difference that wastes an
/// afternoon.
///
/// Empty or all-invalid input yields an empty string, which the caller turns
/// into a refusal rather than emitting an unnamed actor.
std::string to_identifier(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0 || c == '_') {
      out.push_back(static_cast<char>(std::tolower(uc)));
    } else if (!out.empty() && out.back() != '_') {
      out.push_back('_');
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  // An identifier may not start with a digit; prefixing beats dropping, which
  // would silently merge "1" and "2" into nothing.
  if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front())) != 0) {
    out.insert(out.begin(), '_');
  }
  return out;
}

/// The `vehicle_category` value for a `<Vehicle @vehicleCategory>`.
///
/// The DSL enumeration and the XML one are NOT the same list — `car` is in
/// both, and a bicycle is `vru_vehicle` in the DSL (§8.7.7's own example)
/// against `bicycle` in the XML. Only spellings the specification shows are
/// mapped; anything else yields an empty string and the `keep` is omitted with
/// a finding, rather than inventing a category.
std::string_view vehicle_category(std::string_view xml_category) {
  if (xml_category == "car") {
    return "car";
  }
  if (xml_category == "truck" || xml_category == "trailer" || xml_category == "semitrailer") {
    return "truck";
  }
  if (xml_category == "van") {
    return "van";
  }
  if (xml_category == "bus") {
    return "bus";
  }
  if (xml_category == "motorbike" || xml_category == "bicycle") {
    return "vru_vehicle";
  }
  return {};
}

/// The entity's initial speed [m/s], when its `<Init>` actions set one.
std::optional<double> init_speed(const Scenario& scenario, const std::string& entity_ref) {
  for (const Private& entry : scenario.storyboard.init.actions.privates) {
    if (entry.entity_ref != entity_ref) {
      continue;
    }
    for (const PrivateAction& action : entry.actions) {
      if (action.longitudinal.has_value() && action.longitudinal->speed.has_value() &&
          action.longitudinal->speed->absolute_target.has_value()) {
        return action.longitudinal->speed->absolute_target->value;
      }
    }
  }
  return std::nullopt;
}

bool has_any_init_action(const Scenario& scenario, const std::string& entity_ref) {
  for (const Private& entry : scenario.storyboard.init.actions.privates) {
    if (entry.entity_ref == entity_ref && !entry.actions.empty()) {
      return true;
    }
  }
  return false;
}

} // namespace

std::span<const Osc2SubsetRow> osc2_supported() {
  return kSupported;
}

std::span<const Osc2SubsetRow> osc2_unsupported() {
  return kUnsupported;
}

std::vector<Diagnostic> validate_osc2_subset(const Scenario& scenario) {
  std::vector<Diagnostic> findings;

  if (!scenario.storyboard.stories.empty()) {
    findings.push_back(
        warning("Storyboard/Story",
                fmt::format("{} <Story> element(s) are not part of the OpenSCENARIO {} "
                            "concrete-scenario subset and are not exported; the 1.x file "
                            "carries them",
                            scenario.storyboard.stories.size(),
                            kOsc2Version)));
  }
  if (!scenario.storyboard.stop_trigger.condition_groups.empty()) {
    findings.push_back(warning("Storyboard/StopTrigger",
                               "the stop trigger's conditions are not part of the "
                               "concrete-scenario subset and are not exported"));
  }
  if (!scenario.road_network.traffic_signal_controllers.empty()) {
    findings.push_back(
        warning("RoadNetwork/TrafficSignals",
                fmt::format("{} <TrafficSignalController>(s) are not exported: traffic "
                            "lights are domain-model actors in the DSL and the 1.x "
                            "controller decomposition does not carry over",
                            scenario.road_network.traffic_signal_controllers.size())));
  }
  if (!scenario.road_network.logic_file.has_value()) {
    findings.push_back(warning("RoadNetwork/LogicFile",
                               "the scenario links no road network, so no map.set_map_file "
                               "is emitted and the result is not a concrete scenario"));
  }

  const auto& privates = scenario.storyboard.init.actions.privates;
  for (std::size_t index = 0; index < privates.size(); ++index) {
    const Private& entry = privates[index];
    const std::string location = fmt::format("Storyboard/Init/Actions/Private[{}]", index);
    for (const PrivateAction& action : entry.actions) {
      if (action.teleport.has_value() &&
          !std::holds_alternative<WorldPosition>(action.teleport->position)) {
        findings.push_back(
            warning(location + "/TeleportAction",
                    "a road- or lane-relative placement is not exported: DSL placement is "
                    "a position()/lane() modifier relative to a route, and routes are "
                    "outside this subset"));
      }
      if (action.routing.has_value()) {
        findings.push_back(warning(location + "/RoutingAction",
                                   "a route is not exported: a DSL route comes from "
                                   "map.create_route(get_odr_points(...)), an external "
                                   "method this exporter cannot synthesize"));
      }
      if (action.lateral.has_value()) {
        findings.push_back(warning(location + "/LateralAction",
                                   "a lane change is not exported: it belongs to the "
                                   "storyboard, which is outside this subset"));
      }
    }
  }
  return findings;
}

Expected<std::string> write_osc2(const Scenario& scenario, const Osc2WriteOptions& options) {
  if (scenario.entities.scenario_objects.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a concrete scenario with no actor describes nothing",
                      "Entities");
  }

  // Names first, so a collision is refused before any output is built.
  std::vector<std::string> identifiers;
  std::set<std::string> taken;
  identifiers.reserve(scenario.entities.scenario_objects.size());
  for (const ScenarioObject& object : scenario.entities.scenario_objects) {
    std::string identifier = to_identifier(object.name);
    if (identifier.empty()) {
      return make_error(
          ErrorCode::InvalidArgument,
          fmt::format("entity '{}' has no name that can be written as an OpenSCENARIO DSL "
                      "identifier",
                      object.name),
          "Entities/ScenarioObject/@name");
    }
    if (!taken.insert(identifier).second) {
      // Refused rather than de-duplicated: two actors silently merged into one
      // is a scenario that describes something else entirely.
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entities '{}' and an earlier one both become the DSL "
                                    "identifier '{}'",
                                    object.name,
                                    identifier),
                        "Entities/ScenarioObject/@name");
    }
    identifiers.push_back(std::move(identifier));
  }

  std::ostringstream out;

  // A header comment (`#` to end of line, §6.1.1.1.3). It names the source and
  // the edition, because a `.osc` has no FileHeader to carry either.
  out << "# Generated by RoadMaker — ASAM OpenSCENARIO DSL " << kOsc2Version << "\n";
  out << "# Concrete-scenario subset; see docs/domain/openscenario.md for what is and\n";
  out << "# is not exported. Export-only: RoadMaker does not read .osc files.\n";
  out << "\n";

  // §7.7.5.2.1: the complete standard library.
  out << "import osc.standard.all\n";
  out << "\n";

  out << "scenario " << options.scenario_name << ":\n";

  // §6.3.1.3.1's concrete-scenario example opens with exactly this line.
  if (scenario.road_network.logic_file.has_value() &&
      !scenario.road_network.logic_file->filepath.empty()) {
    out << "    map.set_map_file(\"" << scenario.road_network.logic_file->filepath << "\")\n";
    out << "\n";
  }

  for (std::size_t index = 0; index < scenario.entities.scenario_objects.size(); ++index) {
    const ScenarioObject& object = scenario.entities.scenario_objects[index];
    const std::string& identifier = identifiers[index];

    if (const auto* vehicle = std::get_if<Vehicle>(&object.entity_object)) {
      const std::string_view category = vehicle_category(vehicle->category);
      if (category.empty()) {
        // The actor is still declared — losing it would change the scenario —
        // but its category is not invented.
        out << "    " << identifier << ": vehicle\n";
      } else {
        out << "    " << identifier << ": vehicle with:\n";
        out << "        keep(it.vehicle_category == " << category << ")\n";
      }
    } else if (std::holds_alternative<Pedestrian>(object.entity_object)) {
      out << "    " << identifier << ": pedestrian\n";
    } else {
      // A catalog reference or a MiscObject: declared as the most general
      // traffic participant the domain model has, rather than guessed at.
      out << "    " << identifier << ": traffic_participant\n";
    }
  }

  // §6.1.1.1.4: the `do` block. PARALLEL, because everything an `<Init>` block
  // holds happens at once by definition — "it is not possible to specify
  // conditional behavior in this section" (OpenSCENARIO XML §7.2.1). A `serial`
  // here would assert an ordering the source document does not have.
  bool wrote_do = false;
  for (std::size_t index = 0; index < scenario.entities.scenario_objects.size(); ++index) {
    const ScenarioObject& object = scenario.entities.scenario_objects[index];
    if (!has_any_init_action(scenario, object.name)) {
      continue;
    }
    if (!wrote_do) {
      out << "\n    do parallel:\n";
      wrote_do = true;
    }

    const std::optional<double> speed = init_speed(scenario, object.name);
    if (speed.has_value()) {
      out << "        " << identifiers[index] << ".drive() with:\n";
      // `mps` is a normative speed unit, so the kernel's m/s needs no
      // conversion — and a conversion is exactly where a rounding error would
      // enter a file nothing in CI can check.
      out << "            speed(" << num(*speed) << "mps)\n";
    } else {
      out << "        " << identifiers[index] << ".drive()\n";
    }
  }
  if (!wrote_do) {
    // A scenario whose actors do nothing still needs a body: an empty
    // `scenario` block is not valid in an indentation-structured language.
    out << "\n    do parallel:\n";
    out << "        " << identifiers.front() << ".drive()\n";
  }

  return std::move(out).str();
}

Expected<void> save_osc2(const Scenario& scenario,
                         const std::filesystem::path& path,
                         const Osc2WriteOptions& options) {
  const Expected<std::string> text = write_osc2(scenario, options);
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
