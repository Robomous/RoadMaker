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

/// `<Orientation>` — §7.6, the optional child of every road/lane-relative
/// position.
///
/// "Missing h value is interpreted as 0", and likewise `p` and `r` — so each is
/// `std::optional` rather than a defaulted double: absent and explicitly-zero
/// mean the same thing to a simulator but NOT to the byte-identity contract,
/// and writing `h="0"` into a file that omitted it would break a round trip.
///
/// `@type` is `relative` by default per the specification's own wording
/// ("Missing Orientation property is interpreted as the relative reference
/// context"), and is a free string for the round-trip reason that governs every
/// other spelling in this file.
struct Orientation {
  std::optional<double> h;
  std::optional<double> p;
  std::optional<double> r;
  std::string type = "relative";
  RawXml preserved;
};

/// `<RoadPosition>` — §7.6: a station `s` along a road's reference line and a
/// lateral `t` off it.
///
/// `road_id` IS THE OPENDRIVE `<road @id>` STRING, never a `RoadId` — the
/// ADR-0014 §5 rule this whole file is built on. "The ID of the target road
/// taken from the respective road network definition file."
struct RoadPosition {
  std::string road_id;

  /// `@s` — "taken along the road's reference line from the start point of the
  /// target road. Unit: [m]. Range: [0..inf[."
  double s = 0.0;

  /// `@t` — "taken on the axis orthogonal to the reference line of the road."
  double t = 0.0;

  std::optional<Orientation> orientation;
  RawXml preserved;
};

/// `<LanePosition>` — §7.6, and the position RoadMaker AUTHORS when an actor is
/// placed (p8-s2, issue #246).
///
/// WHY THIS AND NOT `<WorldPosition>`. A world position is a snapshot of where a
/// road happened to be when the actor was dropped: move the road afterwards and
/// the actor stays behind, floating. A lane position names the lane, so the
/// actor follows its road through every later edit — which is the same property
/// p8-s3's lane-anchored routes are defined by, and GW-6 step 7 is the step that
/// distinguishes the two.
///
/// `lane_id` is a STRING for the same reason `road_id` is, and additionally
/// because the specification types it `string` rather than `int`: "If a
/// temporary lane layer is present (e.g. in roadworks sections), this shall be
/// interpreted as the ID on the temporary lane layer."
///
/// `@layer` (LaneLayerType) is NOT modeled: it was created in 1.4.0 and this
/// writer defaults to 1.2 (ADR-0014 §3), so modeling it would add a second
/// version conditional to the writer for an attribute nothing here authors. A
/// foreign `@layer` rides `preserved.attributes` and round-trips exactly.
struct LanePosition {
  std::string road_id;

  /// `@laneId` — "The ID of the target lane belonging to the target road."
  /// Negative = right of the reference line, positive = left, "0" = the centre
  /// lane, which carries no traffic; refusing lane 0 is the PLACEMENT layer's
  /// job, not the model's, since a foreign file may legally hold one.
  std::string lane_id;

  double s = 0.0;

  /// `@offset` — "the lateral offset to the center line of the target lane".
  /// "Missing value is interpreted as 0", and 0 is the lane centre, which is
  /// where an actor belongs — hence a plain double rather than an optional.
  double offset = 0.0;

  std::optional<Orientation> orientation;
  RawXml preserved;
};

/// The `<Position>` choice this version models — §7.6's group is an XOR of
/// eleven position types, of which three are here.
///
/// A VARIANT, not three optionals, for exactly the reason
/// `ScenarioObject::entity_object` is one: two optionals make "both" and
/// "neither" representable and force the writer to refuse states the model
/// should never have allowed.
///
/// ★ `WorldPosition` IS DELIBERATELY ALTERNATIVE 0. A default-constructed
/// `TeleportAction` therefore still holds a world position at the origin, which
/// is what every p8-s1 caller and test already means by one. Reordering these
/// would change the meaning of `TeleportAction{}` silently.
///
/// The other eight (`RelativeWorldPosition`, `RelativeObjectPosition`,
/// `RelativeRoadPosition`, `RelativeLanePosition`, `GeoPosition`,
/// `TrajectoryPosition`, `RoutePosition`, `ClothoidPosition`…) keep riding
/// `PrivateAction::preserved.children` as a whole preserved action — see
/// `parse_private_action`, which explains why preserving the ACTION rather than
/// defaulting the position is the only safe reading.
using Position = std::variant<WorldPosition, RoadPosition, LanePosition>;

/// `<TeleportAction>` — the `<PrivateAction>` that places an entity, and the
/// one an actor's initial placement needs (p8-s1 modeled it; p8-s2 gave it the
/// road-relative positions).
struct TeleportAction {
  Position position;
  RawXml preserved;
};

/// `<AbsoluteTargetSpeed>` — §7.5, "Absolute speed defined as a target for a
/// SpeedAction". Unit: [m/s], the kernel frame's unit; the editor's mph/km-h
/// display is a units-layer concern and never reaches this field.
struct AbsoluteTargetSpeed {
  double value = 0.0;
  RawXml preserved;
};

/// `<TransitionDynamics>` — §7.5, a REQUIRED child of `<SpeedAction>`
/// (cardinality 1..1), which is why it is modeled here rather than deferred:
/// a `<SpeedAction>` without it does not validate, so an initial speed could
/// not be authored at all.
///
/// The defaults spell "jump to the target speed immediately", which is what an
/// INITIAL speed means. Per the specification, "Step is an immediate
/// transition... In this case value for time or distance must be 0" — so
/// `value = 0` is not a placeholder, it is required by the shape above it.
///
/// `dynamics_dimension` and `dynamics_shape` are free strings rather than
/// enums, the same call `TrafficSignalState::state` and `Vehicle::category`
/// already make: an unmodeled spelling must round-trip, and a modeled enum plus
/// a preserved attribute would emit the attribute twice.
struct TransitionDynamics {
  std::string dynamics_shape = "step";
  std::string dynamics_dimension = "time";
  double value = 0.0;

  /// `@followingMode` — 0..1, "Default value if omitted: position". Optional so
  /// an omitted one stays omitted.
  std::string following_mode;

  RawXml preserved;
};

/// `<SpeedAction>` — §7.5, inside `<LongitudinalAction>`.
///
/// `<SpeedActionTarget>` is FLATTENED into this struct, unlike `<Center>` and
/// `<Dimensions>`: it is a pure container whose only modeled arm is
/// `<AbsoluteTargetSpeed>`. Its own preserved tier is kept separately
/// (`target_preserved`) for the `Vehicle::properties_preserved` reason — a
/// `<RelativeTargetSpeed>` read from a foreign file must be re-emitted INSIDE
/// `<SpeedActionTarget>`, not beside it.
struct SpeedAction {
  TransitionDynamics dynamics;

  /// Unset when the target was a `<RelativeTargetSpeed>` this version does not
  /// model — it then rides `target_preserved` whole, and the writer emits no
  /// `<AbsoluteTargetSpeed>`.
  std::optional<AbsoluteTargetSpeed> absolute_target;

  RawXml target_preserved;
  RawXml preserved;
};

/// `<LongitudinalAction>` — §7.5, a union whose only modeled arm is
/// `<SpeedAction>`.
struct LongitudinalAction {
  std::optional<SpeedAction> speed;
  RawXml preserved;
};

/// `<Waypoint>` — §7.7, "reference position used to form a route" (p8-s3,
/// issue #247).
///
/// ★ THE STRUCT IS `RouteWaypoint`, NEVER `Waypoint`, and that is forced rather
/// than stylistic. `roadmaker::Waypoint` is the road-authoring node type
/// (`road/authoring.hpp`), and an unqualified `Waypoint` declared inside
/// `roadmaker::osc` would shadow it in exactly the translation unit that needs
/// both — `osc/route.cpp`, which walks a route's waypoints across a network
/// whose roads are shaped by authoring waypoints. This is the third name in
/// this header the surrounding tree has already taken, after `signals` (a Qt
/// macro) and `RoadNetwork` (the kernel's most central type); the rule those
/// two established is that the ELEMENT keeps its schema name and the STRUCT
/// gets an unambiguous one.
struct RouteWaypoint {
  /// `@routeStrategy` — REQUIRED (`use="required"`, no default), so this is a
  /// plain value and not an optional. A free string for the reason
  /// `Vehicle::category` and `TrafficSignalState::state` are: the enumeration is
  /// {fastest, shortest, random, leastIntersections} today, and an unmodeled
  /// spelling must round-trip rather than be rewritten into the nearest one this
  /// build knows — the class of corruption #476 found in the OpenDRIVE writer.
  ///
  /// `kDefaultRouteStrategy` is what RoadMaker authors; nothing here narrows
  /// what can be read.
  std::string route_strategy;

  /// `<Position>` — 1..1. The waypoints RoadMaker authors are
  /// `<LanePosition>`s, which is what makes a route lane-anchored: the roads
  /// beneath it can move and the route follows, because it names the lane rather
  /// than a point in space (GW-6 step 7).
  Position position;

  RawXml preserved;
};

/// The `@routeStrategy` RoadMaker authors.
///
/// "shortest" rather than "fastest": a RoadMaker route names every lane it
/// passes through, so there is nothing for a simulator to optimize between
/// consecutive waypoints and the cheapest reading is the least surprising one.
inline constexpr std::string_view kDefaultRouteStrategy = "shortest";

/// `<Route>` — §7.7, "a continuous path throughout the road network, defined by
/// a series of waypoints".
struct Route {
  /// `@name` — REQUIRED. "Required in catalogs", and required by the schema
  /// everywhere else too (`use="required"`).
  std::string name;

  /// `@closed` — REQUIRED. "In a closed route, the last waypoint is followed by
  /// the first waypoint to create a closed route."
  bool closed = false;

  std::vector<ParameterDeclaration> parameter_declarations;

  /// `minOccurs="2"`: "at least two waypoints are needed to define a route."
  /// The writer refuses a shorter one rather than emitting a `<Route>` no
  /// schema-aware reader will accept.
  std::vector<RouteWaypoint> waypoints;

  RawXml preserved;
};

/// `<AssignRouteAction>` — §7.7, "controls an entity to follow a route using
/// waypoints on the road network".
///
/// A choice of `<Route>` XOR `<CatalogReference>`. Only the inline route is
/// modeled; a catalog reference rides `preserved.children` whole, exactly as an
/// unmodeled entity object does. `route` is therefore an OPTIONAL rather than a
/// value: unset means the action's content is entirely preserved, and the
/// writer emits no `<Route>` for it — emitting an empty one would turn a legal
/// catalog reference into an invalid document.
struct AssignRouteAction {
  std::optional<Route> route;
  RawXml preserved;
};

/// `<RoutingAction>` — §7.7, a five-way choice of which this version models one
/// arm: `<AssignRouteAction>`. `<FollowTrajectoryAction>`,
/// `<AcquirePositionAction>`, `<RandomRouteAction>` and
/// `<PreferredLaneLayerAction>` ride `preserved.children`.
struct RoutingAction {
  std::optional<AssignRouteAction> assign_route;
  RawXml preserved;
};

/// `<AbsoluteTargetLane>` — §7.4.1, a `<LaneChangeTarget>` arm.
///
/// `@value` is a STRING and not an int, exactly as `LanePosition::lane_id` is
/// and for the same reason: the schema types it `string` so a temporary lane
/// layer's id can be spelled.
struct AbsoluteTargetLane {
  std::string value;
  RawXml preserved;
};

/// `<RelativeTargetLane>` — §7.4.1, a `<LaneChangeTarget>` arm, and THE cut-in
/// spelling (p8-s4, issue #248): "target lane defined as difference compared to
/// the reference entity's current lane".
///
/// `@value` IS AN INT here, unlike `AbsoluteTargetLane`'s string — the schema
/// types it `Int`, because a difference has no lane-layer spelling to preserve.
struct RelativeTargetLane {
  /// `@entityRef` — the entity whose current lane the offset is relative to.
  /// Usually the actor itself, which is what "move one lane left" means.
  std::string entity_ref;
  int value = 0;
  RawXml preserved;
};

/// `<LaneChangeAction>` — §7.4.1, inside `<LateralAction>`, and the action the
/// acceptance's cut-in IS (p8-s4, issue #248).
///
/// `<LaneChangeActionDynamics>` is a `TransitionDynamics` — the same type
/// `<SpeedActionDynamics>` uses — but its DEFAULT here is deliberately not the
/// same: a lane change is a manoeuvre with a duration, so `"step"` (jump) would
/// author a teleport sideways. `kDefaultLaneChangeDynamics` spells the honest
/// default and the writer emits whatever the model holds.
struct LaneChangeAction {
  TransitionDynamics dynamics;

  /// The `<LaneChangeTarget>` choice. `std::monostate` means the arm rode
  /// `target_preserved` whole; the writer then emits no modeled arm.
  std::variant<std::monostate, AbsoluteTargetLane, RelativeTargetLane> target;

  /// `@targetLaneOffset` — optional, "the lateral offset to the center line of
  /// the target lane"; omitted stays omitted, per the byte-identity contract.
  std::optional<double> target_lane_offset;

  /// The `<LaneChangeTarget>` WRAPPER's own preserved tier — the
  /// `SpeedAction::target_preserved` rationale exactly.
  RawXml target_preserved;

  RawXml preserved;
};

/// The `<LaneChangeActionDynamics>` RoadMaker authors: a two-second linear
/// lane change. `"step"` — the `TransitionDynamics` default, correct for an
/// INITIAL speed — would author an instantaneous sideways teleport.
inline constexpr std::string_view kDefaultLaneChangeShape = "linear";
inline constexpr double kDefaultLaneChangeDuration = 2.0;

/// `<LateralAction>` — §7.4.1, a three-way choice whose modeled arm is
/// `<LaneChangeAction>`. `<LaneOffsetAction>` and `<LateralDistanceAction>`
/// ride `preserved`.
struct LateralAction {
  std::optional<LaneChangeAction> lane_change;
  RawXml preserved;
};

/// `<PrivateAction>` — §7.5, a ten-way choice of which this version models four
/// arms: the teleport that places an entity, the longitudinal action that gives
/// it an initial speed, the routing action that gives it a route, and the
/// lateral action that changes its lane.
///
/// ★ ONE ELEMENT PER SET ARM. The schema's choice is per-`<PrivateAction>`, so
/// the writer emits one `<PrivateAction>` per arm that is set: teleport-only and
/// longitudinal-only each produce one element, and an action carrying BOTH
/// produces two (teleport first, matching the order an actor is placed then
/// given a speed). `preserved` rides the FIRST element emitted, so a preserved
/// fragment is never duplicated.
///
/// The reader never produces a both-arms action — each `<PrivateAction>` in a
/// file holds exactly one child — so the round trip is one element in, one
/// element out, byte for byte. `set_entity_init_speed` likewise APPENDS a
/// second `PrivateAction` rather than setting `longitudinal` on the teleport's,
/// which keeps the model the reader would have produced from the same file.
struct PrivateAction {
  std::optional<TeleportAction> teleport;
  std::optional<LongitudinalAction> longitudinal;

  /// The routing arm (p8-s3, issue #247). Emitted AFTER the other two, matching
  /// the order an actor is authored: placed, given a speed, then given a route.
  std::optional<RoutingAction> routing;

  /// The lateral arm (p8-s4, issue #248). Emitted LAST, which keeps the element
  /// order of every scenario written before this arm existed unchanged — the
  /// byte-identity contract is measured across versions of this writer too.
  ///
  /// Never authored into `<Init>`: an entity cannot change lane before the
  /// scenario starts. It is a story action, reached through `Action`.
  std::optional<LateralAction> lateral;

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

/// `<TrafficSignalCondition>` — §7.6.5, a `<ByValueCondition>` arm (p8-s4,
/// issue #248): "true if a referenced signal reaches a specific state".
struct TrafficSignalCondition {
  /// `@name` — THE OPENDRIVE `<signal @id>` STRING, the same reference
  /// `TrafficSignalState::traffic_signal_id` carries and named `@name` only
  /// because the schema calls it that. ADR-0014 §5: never a runtime handle.
  std::string name;

  /// `@state` — a free string, for the reason `TrafficSignalState::state`
  /// documents: §10.10 makes the range wider than a colour enum can hold.
  std::string state;

  RawXml preserved;
};

/// `<TrafficSignalControllerCondition>` — §7.6.5, a `<ByValueCondition>` arm
/// (p8-s4, issue #248): true while a controller is in a named phase.
///
/// ★ THE PHASE-NAME TRAP (#248). `@phase` names a phase of the referenced
/// controller AS THE FILE SPELLS IT, and `Phase::name` may legally be empty —
/// the writer synthesizes and de-duplicates names into the OUTPUT only and
/// never writes them back (`Phase::name`'s note). So an author who reads
/// `Phase::name` to fill this field produces a reference that matches nothing
/// in the file it is written into. Everything that authors or checks a
/// `@phase` therefore goes through `osc::phase_names()`
/// (`roadmaker/osc/writer.hpp`), which IS the writer's synthesis — one
/// function, so the two can never drift.
struct TrafficSignalControllerCondition {
  /// `@trafficSignalControllerRef` — a `TrafficSignalController::name` in THIS
  /// document, which is itself the OpenDRIVE `<controller @id>` (§10.10).
  std::string traffic_signal_controller_ref;

  /// `@phase` — a SYNTHESIZED phase name; see this struct's note.
  std::string phase;

  RawXml preserved;
};

/// `<StoryboardElementStateCondition>` — §7.6.5, a `<ByValueCondition>` arm
/// (p8-s4, issue #248): true when a named storyboard element reaches a state.
///
/// This is how a scenario ends on its own content rather than on a wall-clock
/// timeout — the `<Storyboard><StopTrigger>` an authored cut-in wants.
struct StoryboardElementStateCondition {
  /// `@storyboardElementRef` — the `@name` of a `<Story>`, `<Act>`,
  /// `<ManeuverGroup>`, `<Maneuver>`, `<Event>` or `<Action>`.
  std::string storyboard_element_ref;

  /// `@state` (`StoryboardElementState`) and `@storyboardElementType`
  /// (`StoryboardElementType`) — free strings, per this file's rule for closed
  /// enumerations whose unmodeled spellings must still round-trip.
  std::string state = "completeState";
  std::string storyboard_element_type = "event";

  RawXml preserved;
};

/// `<SpeedCondition>` — §7.6.5, an `<EntityCondition>` arm (p8-s4).
struct SpeedCondition {
  /// `@rule` — a free string, as everywhere else in this file.
  std::string rule = "greaterThan";
  /// `@value` [m/s], the kernel frame's unit.
  double value = 0.0;
  RawXml preserved;
};

/// `<RelativeDistanceCondition>` — §7.6.5, an `<EntityCondition>` arm and THE
/// cut-in trigger (p8-s4, issue #248): "the distance to a reference entity".
struct RelativeDistanceCondition {
  /// `@entityRef` — must name a `<ScenarioObject>`; the writer refuses a
  /// dangling one, exactly as it does for `Private::entity_ref`.
  std::string entity_ref;

  /// `@freespace` — required. "true: distance is measured between closest
  /// bounding box points; false: reference points are used." True is the
  /// reading a cut-in means.
  bool freespace = true;

  /// `@relativeDistanceType` and `@rule` — free strings, per this file's rule.
  std::string relative_distance_type = "longitudinal";
  std::string rule = "lessThan";

  /// `@value` [m].
  double value = 0.0;

  RawXml preserved;
};

/// `<EntityRef>` — §7.3.1, "explicitly couples an existing entity to an actor".
///
/// A struct rather than a bare string because the element carries its own
/// preserved tier: a foreign `<EntityRef>` attribute must round-trip, and a
/// `std::vector<std::string>` has nowhere to put one.
struct EntityRef {
  /// `@entityRef` — must name a `<ScenarioObject>`.
  std::string entity_ref;
  RawXml preserved;
};

/// `<TriggeringEntities>` — §7.6.5, the entities a `<ByEntityCondition>` is
/// evaluated over.
struct TriggeringEntities {
  /// `@triggeringEntitiesRule` (`TriggeringEntitiesRule`) — "any" or "all"; a
  /// free string, per this file's rule.
  std::string rule = "any";

  /// `maxOccurs="unbounded"`, `minOccurs` 1 — the writer refuses an empty list
  /// rather than emitting a `<TriggeringEntities>` no schema-aware reader
  /// accepts.
  std::vector<EntityRef> entity_refs;

  RawXml preserved;
};

/// `<ByEntityCondition>` — §7.6.5.
struct ByEntityCondition {
  TriggeringEntities triggering_entities;

  /// The `<EntityCondition>` choice — sixteen arms in 1.4.0, of which this
  /// version models two. `std::monostate` means the arm rode
  /// `entity_condition_preserved` whole, which is how the other fourteen
  /// survive a round trip; the writer then emits no modeled arm.
  ///
  /// A VARIANT rather than optionals, for the reason
  /// `ScenarioObject::entity_object` is one.
  std::variant<std::monostate, SpeedCondition, RelativeDistanceCondition> entity_condition;

  /// The `<EntityCondition>` WRAPPER's own preserved tier, separate from
  /// `preserved` because the two sit at different nesting levels — the
  /// `Vehicle::properties_preserved` rationale, met again. An unmodeled
  /// `<TimeToCollisionCondition>` must be re-emitted INSIDE
  /// `<EntityCondition>`, not beside it.
  RawXml entity_condition_preserved;

  RawXml preserved;
};

/// `<Condition>` — §7.9 / §7.6.
///
/// ★ THE ARMS ARE MUTUALLY EXCLUSIVE and the writer refuses more than one set
/// (`asam.net:xosc:1.0.0:xml.valid_schema`): `<Condition>` is an XSD `choice`
/// of `<ByEntityCondition>` and `<ByValueCondition>`, and `<ByValueCondition>`
/// is itself a choice of eight. They are optionals rather than one variant
/// only because `simulation_time` predates them (p8-s1) and is read by name in
/// the editor, in Python and in ~20 tests; a variant here would be a source
/// break for no behavioural gain the validator does not already provide.
///
/// ALL UNSET is legal and means the condition's arm rode `preserved` — how a
/// `<TimeOfDayCondition>` survives a round trip untouched.
struct Condition {
  std::string name;
  /// `@delay` [s]. "The condition delay shall be non negative"
  /// (asam.net:xosc:1.0.0:data_type.condition_delay_not_negative).
  double delay = 0.0;
  std::string condition_edge = "rising";

  // --- <ByValueCondition> arms ---
  std::optional<SimulationTimeCondition> simulation_time;
  std::optional<TrafficSignalCondition> traffic_signal;
  std::optional<TrafficSignalControllerCondition> traffic_signal_controller;
  std::optional<StoryboardElementStateCondition> storyboard_element_state;

  // --- the <ByEntityCondition> arm ---
  std::optional<ByEntityCondition> by_entity;

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

// --- the storyboard proper (p8-s4, issue #248) --------------------------------
//
// Story ▸ Act ▸ ManeuverGroup ▸ Maneuver ▸ Event ▸ Action, §7.2.1 and §7.3.
// Before this sprint a `<Story>` rode `Storyboard::preserved_stories` as a raw
// fragment, which round-tripped it and made it uneditable. Modelling it is what
// makes the storyboard editable at all; a *foreign* construct inside one still
// rides the same tiered `RawXml preserved` every other element here uses, so
// nothing regressed on the never-drop contract.

/// `<TrafficSignalStateAction>` — §7.4.2, "sets a specific state of a traffic
/// signal".
struct TrafficSignalStateAction {
  /// `@name` — the OpenDRIVE `<signal @id>` STRING (ADR-0014 §5), named
  /// `@name` only because the schema calls it that.
  std::string name;
  /// `@state` — free, per `TrafficSignalState::state`'s note.
  std::string state;
  RawXml preserved;
};

/// `<TrafficSignalControllerAction>` — §7.4.2, "sets a specific phase of a
/// traffic signal controller".
///
/// `@phase` carries the phase-name trap in full; see
/// `TrafficSignalControllerCondition`.
struct TrafficSignalControllerAction {
  std::string traffic_signal_controller_ref;
  std::string phase;
  RawXml preserved;
};

/// `<TrafficSignalAction>` — §7.4.2, a two-way choice.
struct TrafficSignalAction {
  /// `std::monostate` means the arm rode `preserved` whole.
  std::variant<std::monostate, TrafficSignalStateAction, TrafficSignalControllerAction> action;
  RawXml preserved;
};

/// `<InfrastructureAction>` — §7.4.2. An `xsd:all` with one required child, so
/// `traffic_signal` is a VALUE and not an optional: an `<InfrastructureAction>`
/// without it does not validate and could not be authored at all.
struct InfrastructureAction {
  TrafficSignalAction traffic_signal;
  RawXml preserved;
};

/// `<GlobalAction>` — §7.4.2, a seven-way choice whose modeled arm is
/// `<InfrastructureAction>`. `<EnvironmentAction>`, `<EntityAction>`,
/// `<TrafficAction>`, `<VariableAction>` and `<SetMonitorAction>` ride
/// `preserved`.
struct GlobalAction {
  std::optional<InfrastructureAction> infrastructure;
  RawXml preserved;
};

/// `<Action>` — §7.3.2, the wrapper every storyboard action sits in.
///
/// ★ `<UserDefinedAction>` IS THE THIRD ARM and is deliberately not modeled:
/// its content is by definition tool-specific, so `std::monostate` plus the
/// preserved tier is not a shortcut here, it is the correct representation.
struct Action {
  /// `@name` — required and unique among the event's actions
  /// (asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level).
  std::string name;

  /// The `<GlobalAction>` / `<UserDefinedAction>` / `<PrivateAction>` choice.
  /// `std::monostate` means the arm rode `preserved` whole.
  std::variant<std::monostate, GlobalAction, PrivateAction> action;

  RawXml preserved;
};

/// The `Event/@priority` RoadMaker authors — see `Event::priority`.
inline constexpr std::string_view kDefaultEventPriority = "overwrite";

/// `<Event>` — §7.3.2, "a container for actions" plus the trigger that starts
/// them.
struct Event {
  /// `@name` — required and unique among the maneuver's events.
  std::string name;

  /// `@priority` — REQUIRED, and a free string for the round-trip reason every
  /// other closed enumeration in this file is one.
  ///
  /// ★ THE DEFAULT IS THE 1.0 SPELLING, `"overwrite"`, and that is a version
  /// choice rather than an oversight: `WriteOptions::target_version` defaults
  /// to 1.2 (`osc/writer.hpp`), where `"override"` does not exist. The 1.4.0
  /// catalogue deprecates `"overwrite"` but still declares it, and the
  /// specification's own worked examples (§6) write it — so this spelling is
  /// legal in every revision this writer can target and `"override"` is not.
  /// An author targeting 1.4 sets it explicitly.
  std::string priority{kDefaultEventPriority};

  /// `@maximumExecutionCount` — optional ("Default value if omitted: 1"), so an
  /// omitted one stays omitted.
  std::optional<unsigned> maximum_execution_count;

  /// `minOccurs` 1: an event with no action does nothing, and the schema says
  /// so. The writer refuses an empty one.
  std::vector<Action> actions;

  /// `<StartTrigger>` — 0..1. "If no trigger is defined, the event starts when
  /// the act enters runningState"; an ABSENT trigger and an EMPTY one mean the
  /// same thing to a simulator but not to the byte-identity contract, which is
  /// why this is an optional and `Storyboard::stop_trigger` is not.
  std::optional<Trigger> start_trigger;

  RawXml preserved;
};

/// `<Maneuver>` — §7.3.3, "groups events creating a scope where the events can
/// interact with each other using the event priority rules".
///
/// ★ THE STRUCT IS `StoryManeuver`, NEVER `Maneuver`. `roadmaker::Maneuver` is
/// the junction turn-path type (`road/junction.hpp`), and an unqualified
/// `Maneuver` declared inside `roadmaker::osc` would shadow it in exactly the
/// translation units that need both — the network-aware scenario cross-checker
/// most of all. This is the fourth name in this header the surrounding tree has
/// already taken, after `signals` (a Qt macro), `RoadNetwork` and `Waypoint`;
/// the rule those established is that the ELEMENT keeps its schema name and the
/// STRUCT gets an unambiguous one.
struct StoryManeuver {
  std::string name;
  std::vector<ParameterDeclaration> parameter_declarations;

  /// `minOccurs` 1 — the writer refuses a maneuver with no events.
  std::vector<Event> events;

  RawXml preserved;
};

/// `<ManeuverGroup>` — §7.3.1, which "singles out the instances of Entity that
/// may be actuated ... by the maneuvers in that group".
struct ManeuverGroup {
  std::string name;

  /// `@maximumExecutionCount` — REQUIRED (`use="required"`), so a plain value.
  unsigned maximum_execution_count = 1;

  /// `<Actors>` — 1..1. `@selectTriggeringEntities` is required; the entity
  /// list may legally be EMPTY ("this is allowed for situations where the
  /// maneuvers ... are not related to instances of Entity"), which is exactly
  /// the traffic-light half of the acceptance: an infrastructure action has no
  /// actor.
  bool select_triggering_entities = false;
  std::vector<EntityRef> actors;

  /// The `<Actors>` element's own preserved tier, separate from `preserved`
  /// because the two sit at different nesting levels — the
  /// `Vehicle::properties_preserved` rationale.
  RawXml actors_preserved;

  /// `<CatalogReference>`* sits BETWEEN `<Actors>` and `<Maneuver>`* in the
  /// sequence, so it cannot ride `preserved.children` — those are re-emitted
  /// LAST and a catalog reference would land after the maneuvers, out of its
  /// schema slot. Verbatim fragments in their own vector, the mechanism
  /// `Storyboard`'s stories used before they were modeled.
  std::vector<std::string> preserved_catalog_references;

  std::vector<StoryManeuver> maneuvers;

  RawXml preserved;
};

/// `<Act>` — §7.2.1, "a container for maneuver groups" that "focuses on
/// answering the question when".
struct Act {
  std::string name;

  /// `maxOccurs="unbounded"`, `minOccurs` 1 — the writer refuses an act with no
  /// maneuver groups.
  std::vector<ManeuverGroup> maneuver_groups;

  /// Both 0..1, and optional for the reason `Event::start_trigger` is.
  std::optional<Trigger> start_trigger;
  std::optional<Trigger> stop_trigger;

  RawXml preserved;
};

/// `<Story>` — §7.2.1, the top-level grouping "so scenario authors can group
/// different scenario aspects into a higher-level hierarchy".
struct Story {
  std::string name;
  std::vector<ParameterDeclaration> parameter_declarations;

  /// `maxOccurs="unbounded"`, `minOccurs` 1 — the writer refuses a story with
  /// no acts.
  std::vector<Act> acts;

  RawXml preserved;
};

/// `<Storyboard>` — §7.3.
struct Storyboard {
  Init init;

  /// `<Story>`* — modeled since p8-s4 (issue #248). Emitted in vector order,
  /// in the schema slot between `<Init>` and `<StopTrigger>`.
  std::vector<Story> stories;

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
