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

/// The ASAM OpenSCENARIO XML scenario document model (p8-s1, issue #245).
///
/// Layer 0, the second one (ADR-0014, ADR-0008): a `.xosc` sits beside its
/// `.xodr` at the top level of the project directory, stem-matched, and stays
/// standalone-openable in any tool.
///
/// THIS MODELS THE FILE, NOT THE NETWORK. Nothing here includes anything from
/// `road/`, and nothing here holds a `RoadId`/`SignalId`/`ControllerId` — a
/// generational arena handle is runtime-only and never valid across a load
/// (`road/arena.hpp:29-36`), so every cross-reference is the OpenDRIVE
/// `odr_id` STRING, exactly as `Control::signal_odr_id` and
/// `PhaseState::controller_odr_id` already are. Building this model from a
/// live `RoadNetwork` is the decomposition's job (p8-s1 PR-D), not the
/// model's.
///
/// SCOPE. p8-s1 models the spine a RoadMaker-authored scenario needs plus the
/// whole traffic-signal half; routes, trajectories and the condition/trigger
/// vocabulary stay in the preserved tier until p8-s3/p8-s4 model them. That
/// is not a gap: every struct below carries a `RawXml preserved`, so an
/// element this version does not understand survives verbatim rather than
/// being dropped (ADR-0014 §6).
///
/// NAMES THAT CAN NEVER BE USED HERE, both already trapped twice in this tree
/// (`mesh/junction_phases.hpp:54-57`, `mesh/junction_signals.hpp:84-88`):
///   - `signals` as a member — Qt's <QObject> does `#define signals public`,
///     so the struct declaration itself stops parsing in any editor TU. The
///     `<TrafficSignals>` wrapper therefore has no struct at all; controllers
///     hang directly off `RoadNetworkRef`, and `Phase`'s member is
///     `signal_states`.
///   - `RoadNetwork` as a type — `roadmaker::RoadNetwork` is the kernel's most
///     central type, and an unqualified `RoadNetwork` inside this namespace
///     would silently shadow it in exactly the translation unit (PR-D's
///     decomposition) that needs both. The element is `<RoadNetwork>`; the
///     struct is `RoadNetworkRef`, which is also the more honest name for a
///     record that is mostly a filepath.
/// `slots`, `emit`, `foreach` and `forever` are Qt macros too — none is used
/// today, and a storyboard action verb is where one would first be reached for.
///
/// Reference: ASAM OpenSCENARIO XML 1.4.0 §6.11 (traffic signals), §7
/// (scenario components), §10.10 (traffic signal). The specification text is
/// NOT tracked in this repository — it carries no redistribution grant, unlike
/// OpenDRIVE (third_party/asam/README.md); fetch it locally with
/// `scripts/fetch_asam_specs.py --std openscenario`.

#pragma once

#include "roadmaker/xodr/raw_xml.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace roadmaker::osc {

/// The `FileHeader/@date` this writer stamps when the caller names none.
///
/// A FIXED value, never a clock: `write_xosc` is deterministic (ADR-0014 §4),
/// the property `write_xodr` has had since M2 and that the golden-workflow
/// replays fingerprint state with. A `std::chrono::system_clock::now()` here
/// is the one-line change that breaks it, and two writes a second apart still
/// compare equal — so the test that guards this asserts the literal.
///
/// Spelled in the XSD's `xsd:dateTime` (ISO 8601 extended) form, which is what
/// `scripts/esmini_smoke.py:51` already proves esmini accepts. Note that
/// `asam.net:xosc:1.0.0:data_type.time_format` says "Basic Notation" while the
/// schema type it constrains is `xsd:dateTime`, which admits only the extended
/// form; the schema wins, since a Basic-notation string would not validate.
inline constexpr std::string_view kDefaultFileHeaderDate = "1970-01-01T00:00:00";

/// `<FileHeader>` — §7.2.2.
///
/// `revMajor`/`revMinor` are deliberately NOT modeled. They are computed from
/// `WriteOptions::target_version`, so the revision a file declares and the
/// content it carries can never disagree. A reader records the revision it
/// parsed as a `Diagnostic`, not as model state.
struct FileHeader {
  std::string author = "RoadMaker";
  std::string date{kDefaultFileHeaderDate};
  std::string description = "RoadMaker scenario";
  RawXml preserved;
};

/// `<ParameterDeclaration>` — §9.2.
struct ParameterDeclaration {
  std::string name;
  std::string parameter_type;
  std::string value;
  RawXml preserved;
};

/// `<LogicFile>` / `<SceneGraphFile>` — a filepath and nothing else.
struct FileRef {
  std::string filepath;
  RawXml preserved;
};

/// `<TrafficSignalState>` — one signal head's state during one phase (§6.11).
struct TrafficSignalState {
  /// `@trafficSignalId` — the OpenDRIVE `<signal @id>` STRING (ADR-0014 §5).
  ///
  /// "The attribute `trafficSignalId` in `TrafficSignalState` shall reference
  /// a valid element (element exists and of correct type) within the
  /// referenced road network file"
  /// (asam.net:xosc:1.0.0:reference_control.traffic_signal_state_references).
  /// The writer refuses an empty one: writing a runtime handle, or nothing at
  /// all, produces a file that looks entirely right and references nothing.
  std::string traffic_signal_id;

  /// `@state` — a FREE STRING, deliberately not `roadmaker::SignalState`.
  ///
  /// The specification leaves the spelling to the simulation engine, and
  /// §10.10 makes the range wider than a colour enum can hold: a signal
  /// modelled as a whole box (which is what a RoadMaker `Signal` head is)
  /// carries a COMPOSITE state such as "on;off;off", while one modelled per
  /// light bulb carries "on" or "off" — and the OpenDRIVE signal's `@type` is
  /// what decides which. The specification's own worked example writes
  /// "on"/"off", not colour words at all.
  ///
  /// So an enum here could not represent a legal foreign value, and routing
  /// the unrepresentable spelling into `preserved.attributes` (the documented
  /// escape, `xodr/raw_xml.hpp:33-36`) would make the writer emit `state=`
  /// TWICE. Holding the string keeps the round trip exact and confines the
  /// engine-directed `SignalState` -> token choice to the decomposition
  /// (PR-D), which is the only place a network enum is in hand.
  std::string state;

  RawXml preserved;
};

/// `Phase/@semantics` — §6.11, created in OpenSCENARIO 1.4.0.
///
/// A closed schema enumeration (`TrafficSignalSemantics`), unlike the free
/// `TrafficSignalState::state` beside it — hence an enum here and a string
/// there. `Phase::semantics` is `std::optional` so a value this version does
/// not know stays unset and rides `Phase::preserved.attributes` instead,
/// which is what stops the writer emitting `semantics=` twice.
enum class PhaseSemantics {
  AttentionGo,
  AttentionStop,
  Caution,
  Fallback,
  Go,
  Stop,
};

/// `<Phase>` — one labelled interval of a controller's cycle (§6.11).
struct Phase {
  /// `@name` — required, and unique among ONE controller's phases:
  /// "Element names at each level shall be unique at that level"
  /// (asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level).
  ///
  /// MAY BE EMPTY HERE. `roadmaker::SignalPhase::name` is legally empty
  /// (`road/junction.hpp:328-330`), so the writer synthesizes a name and
  /// de-duplicates it per controller rather than refusing. The synthesized
  /// name exists only in the output; this field is never rewritten, so two
  /// writes of one `Scenario` produce identical bytes.
  ///
  /// `@name` and `@semantics` are INDEPENDENT: §10.10's example writes
  /// `name="stop_short" semantics="stop"` beside `name="stop_long"
  /// semantics="stop"` — the name separates two phases that share a semantic.
  std::string name;

  /// `@duration` [s]. "The attribute `duration` in the complex type `Phase`
  /// shall contain NON-NEGATIVE values"
  /// (asam.net:xosc:1.0.0:data_type.phase_duration_positive) — so zero is
  /// legal despite the rule's name, and only a negative duration is refused.
  double duration = 0.0;

  /// `@semantics` — emitted only when targeting 1.4.0 (the attribute does not
  /// exist before it). Through 1.3.0 the meaning rode in `@name`.
  std::optional<PhaseSemantics> semantics;

  /// NOT `signals` — see this file's header comment. Every member controller's
  /// head appears in EVERY phase: the dense, Red-filled list, never
  /// RoadMaker's sparse Red-by-omission storage, or a signal the editor shows
  /// as red exports as a signal that is never red (ADR-0014 §8).
  std::vector<TrafficSignalState> signal_states;

  RawXml preserved;
};

/// `<TrafficSignalController>` — §6.11, §10.10.
struct TrafficSignalController {
  /// `@name` — THE OPENDRIVE CONTROLLER `@id`, not a human-readable label.
  ///
  /// "The ASAM OpenDRIVE controller ID is used as the name of the
  /// `TrafficSignalController` to reference it" (§10.10), whose example writes
  /// `name="42"`; the checker rule agrees that `@name` "can reference a valid
  /// element ... within the referenced road network file". GW-6 step 11
  /// independently requires the controller be selectable by its OpenDRIVE
  /// `@id`. RoadMaker's `Controller::name` is the `j1_axis0_through` form —
  /// emitting that would produce a file that references nothing.
  std::string name;

  /// `@delay` [s] relative to the controller named by `reference`. Meaningless
  /// without it, so the writer refuses `delay` set while `reference` is empty.
  std::optional<double> delay;

  /// `@reference` — "shall reference an existing `TrafficSignalController`
  /// with the given `name` within the scenario"
  /// (asam.net:xosc:1.0.0:reference_control.traffic_signal_controller_references).
  std::string reference;

  /// Cycle order. Every controller decomposed from one junction timeline
  /// carries the SAME durations in the SAME order, differing only in the row
  /// of states each phase holds (ADR-0014 §8).
  std::vector<Phase> phases;

  RawXml preserved;
};

/// `<RoadNetwork>` — the scenario's link to the road network it plays on.
///
/// NOT named `RoadNetwork`; see this file's header comment.
///
/// ORDERING CONTRACT FOR PR-D. Everything here is a vector emitted in vector
/// order, so `write_xosc` needs no sort anywhere. A decomposition that fills
/// `traffic_signal_controllers` from a live network MUST sort by `odr_id` at
/// the point of construction: `RoadNetwork::for_each_controller` walks arena
/// SLOTS ascending and a freed slot is reused, so it is not creation order
/// after any erase.
struct RoadNetworkRef {
  std::optional<FileRef> logic_file;
  std::optional<FileRef> scene_graph_file;
  std::vector<TrafficSignalController> traffic_signal_controllers;
  RawXml preserved;
};

/// `<BoundingBox>` — §7.4.2. `<Center>` and `<Dimensions>` are flattened in:
/// both are pure containers with no attributes of their own and no siblings,
/// so there is nothing for either to preserve.
struct BoundingBox {
  double center_x = 0.0;
  double center_y = 0.0;
  double center_z = 0.0;
  double width = 0.0;
  double length = 0.0;
  double height = 0.0;
  RawXml preserved;
};

/// `<Performance>` — required child of `<Vehicle>` in every revision, so the
/// defaults must produce a writable `Vehicle{}`. These are the values
/// `scripts/esmini_smoke.py:62` already proves esmini accepts.
struct Performance {
  double max_speed = 70.0;
  double max_acceleration = 5.0;
  double max_deceleration = 10.0;
  RawXml preserved;
};

/// `<FrontAxle>` / `<RearAxle>` / `<AdditionalAxle>` — §7.4.
struct Axle {
  double max_steering = 0.0;
  double wheel_diameter = 0.6;
  double track_width = 1.8;
  double position_x = 0.0;
  double position_z = 0.3;
  RawXml preserved;
};

/// `<Axles>` — required child of `<Vehicle>`, like `<Performance>`.
struct Axles {
  Axle front{.max_steering = 0.5, .position_x = 2.98};
  Axle rear;
  std::vector<Axle> additional;
  RawXml preserved;
};

/// `<Property>` — a free name/value pair inside `<Properties>`.
struct Property {
  std::string name;
  std::string value;
  RawXml preserved;
};

/// `<Vehicle>` — §7.4.1. `@vehicleCategory` is a free string here rather than
/// an enum for the same reason `TrafficSignalState::state` is: an unmodeled
/// spelling must round-trip, and a modeled enum plus a preserved attribute
/// would emit the attribute twice.
struct Vehicle {
  std::string name;
  std::string category = "car";
  std::optional<double> mass;
  std::string model3d;
  BoundingBox bounding_box;
  Performance performance;
  Axles axles;
  std::vector<Property> properties;

  /// The `<Properties>` WRAPPER's own preserved tier, separate from
  /// `preserved` because the two sit at different nesting levels.
  ///
  /// `<Properties>` holds `Property*` but also `File*` and `CustomContent*`,
  /// and an esmini vehicle catalog routinely carries a `<File>` there.
  /// Preserving it in `preserved` would re-emit it as a SIBLING of
  /// `<Properties>` rather than inside it — the `InitActions` rationale below,
  /// met a second time. A bare `RawXml` rather than a `Properties` struct keeps
  /// `properties` a plain vector, which is what every call site already indexes.
  RawXml properties_preserved;

  RawXml preserved;
};

/// `<Pedestrian>` — §7.4.1.
struct Pedestrian {
  std::string name;
  std::string category = "pedestrian";
  double mass = 80.0;
  std::string model3d;
  BoundingBox bounding_box;
  std::vector<Property> properties;

  /// The `<Properties>` wrapper's own preserved tier; see `Vehicle`.
  RawXml properties_preserved;

  RawXml preserved;
};

/// `<ScenarioObject>` — §7.4.
struct ScenarioObject {
  /// `@name` — the key every `entityRef` resolves through, so it is required
  /// and must be unique
  /// (asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level). The
  /// writer refuses an empty or duplicate one.
  std::string name;

  /// The EntityObject choice — a variant, not two optionals, because the
  /// schema group is an XOR: two optionals would make "both" and "neither"
  /// representable and force the writer to refuse states the model should
  /// never have allowed.
  ///
  /// `std::monostate` means the entity object is entirely in
  /// `preserved.children` — which is how a `MiscObject`,
  /// `ExternalObjectReference` or `CatalogReference` survives a round trip
  /// without this version modelling it.
  std::variant<std::monostate, Vehicle, Pedestrian> entity_object;

  RawXml preserved;
};

/// `<Entities>` — §7.4.
struct Entities {
  std::vector<ScenarioObject> scenario_objects;
  RawXml preserved;
};

/// `<WorldPosition>` — §7.6, absolute in the OpenDRIVE inertial frame.
struct WorldPosition {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  std::optional<double> h;
  std::optional<double> p;
  std::optional<double> r;
  RawXml preserved;
};

/// `<TeleportAction>` — the only `<PrivateAction>` p8-s1 models, because it is
/// the one the initial placement of an actor needs. Everything else rides
/// `PrivateAction::preserved.children` until p8-s2/p8-s4.
struct TeleportAction {
  WorldPosition position;
  RawXml preserved;
};

/// `<PrivateAction>` — §7.5.
struct PrivateAction {
  std::optional<TeleportAction> teleport;
  RawXml preserved;
};

/// `<Private>` — §7.5, the init actions belonging to one entity.
struct Private {
  /// `@entityRef` — must name a `ScenarioObject`; the writer refuses a
  /// dangling one.
  std::string entity_ref;
  std::vector<PrivateAction> actions;
  RawXml preserved;
};

/// `<Actions>` inside `<Init>`.
///
/// Its own struct rather than folded into `Init`: the preserved children of
/// `<Init>` and of `<Actions>` sit at different nesting levels, and collapsing
/// them would re-emit a foreign file's fragments one level too high.
struct InitActions {
  std::vector<Private> privates;
  RawXml preserved;
};

/// `<Init>` — §7.3.
struct Init {
  InitActions actions;
  RawXml preserved;
};

/// `<SimulationTimeCondition>` — §7.9, the one condition p8-s1 models.
struct SimulationTimeCondition {
  double value = 0.0;
  /// `@rule` — a free string for the same round-trip reason as elsewhere.
  std::string rule = "greaterThan";
  RawXml preserved;
};

/// `<Condition>` — §7.9.
struct Condition {
  std::string name;
  /// `@delay` [s]. "The condition delay shall be non negative"
  /// (asam.net:xosc:1.0.0:data_type.condition_delay_not_negative).
  double delay = 0.0;
  std::string condition_edge = "rising";
  std::optional<SimulationTimeCondition> simulation_time;
  RawXml preserved;
};

/// `<ConditionGroup>` — §7.9.
struct ConditionGroup {
  std::vector<Condition> conditions;
  RawXml preserved;
};

/// `<Trigger>`, `<StartTrigger>`, `<StopTrigger>` — §7.9.
struct Trigger {
  std::vector<ConditionGroup> condition_groups;
  RawXml preserved;
};

/// `<Storyboard>` — §7.3.
struct Storyboard {
  Init init;

  /// `<Story>` fragments, verbatim. p8-s4 owns the storyboard editor and will
  /// model acts, events, triggers and actions; until then a story authored
  /// elsewhere survives a round trip untouched rather than being dropped.
  std::vector<std::string> preserved_stories;

  /// `<StopTrigger>` is ALWAYS emitted, empty if it has no condition groups —
  /// see `Scenario`'s note on the always-present skeleton.
  Trigger stop_trigger;

  RawXml preserved;
};

/// A whole `.xosc` document.
///
/// THE ALWAYS-PRESENT SKELETON. `<ParameterDeclarations>`, `<CatalogLocations>`,
/// `<Properties>` and `<StopTrigger>` are emitted unconditionally, empty when
/// they hold nothing. This is not padding and must not be "cleaned up": each
/// was required in the 1.0/1.2 schema and relaxed only later, so emitting them
/// conditionally would turn one version conditional (`revMinor`, plus
/// `Phase/@semantics`) into five. The shape mirrors
/// `scripts/esmini_smoke.py:49-101`, which CI already proves a shipping
/// parser accepts at 1.2.
struct Scenario {
  FileHeader header;
  std::vector<ParameterDeclaration> parameter_declarations;

  /// `<CatalogLocations>` is required and always emitted; nothing inside it is
  /// modeled until catalogs exist as a feature, so it is preserved-only.
  RawXml catalog_locations;

  RoadNetworkRef road_network;
  Entities entities;
  Storyboard storyboard;
  RawXml preserved;
};

} // namespace roadmaker::osc
