# OpenSCENARIO in RoadMaker

*What RoadMaker's scenario model covers, what it deliberately does not, and the
rules that govern the difference. The normative reference is ASAM OpenSCENARIO
XML 1.4.0 — see [Working with the ASAM references](references.md) for how to
obtain it (it is **not** tracked in this repository).*

A RoadMaker scenario is a second Layer-0 file: a `.xosc` sitting beside its
`.xodr`, stem-matched, at the top level of the project directory, and openable
standalone in any tool. That split, and why the model is kernel-side rather than
editor-side, is
[ADR-0014](../decisions/0014-scenario-model-kernel-side-osc-1x.md).

## The revision RoadMaker writes

The writer targets **OpenSCENARIO 1.2 by default**, with 1.4 available through
`WriteOptions::target_version`. The reason is concrete rather than conservative
habit: validation for this project means *"the pinned esmini accepts the file"*,
and CI pins esmini v3.5.0 — emitting a newer revision by default would put the
acceptance gate out of reach. The choice costs nothing in citable rules: every
checker-rule UID in the 1.4.0 catalogue is stamped 1.0.0, 1.1.0 or 1.2.0.

What 1.2 forfeits is exactly one attribute this model holds, `Phase/@semantics`,
created in 1.4.0.

## Two rules that govern the whole model

**Every cross-reference is an OpenDRIVE `@id` STRING, never a runtime handle.**
A `RoadId`/`LaneId`/`SignalId` is a generational arena handle and is not valid
across a load, so a scenario that stored one would name nothing the next time
the project opened. `RoadPosition::road_id`, `LanePosition::lane_id`,
`TrafficSignalState::traffic_signal_id` and `TrafficSignalController::name` all
carry the `.xodr`'s own string ids.

**Nothing a foreign file carries is dropped.** Every struct has a `RawXml
preserved` tier holding the attributes and child elements this version does not
model, verbatim and in document order, re-emitted in its own schema slot. A
`.xosc` RoadMaker cannot fully model still survives a round trip, and the
diagnostics say what was preserved and why. The round trip is a *fixed point*
rather than a byte-for-byte echo: a foreign file re-canonicalizes on its first
write, and from there `write == write ∘ parse ∘ write`, byte for byte.

## What is modeled

### Document structure

`<FileHeader>`, `<ParameterDeclarations>`, `<CatalogLocations>` (preserved-only),
`<RoadNetwork>` with `<LogicFile>`, `<SceneGraphFile>` and `<TrafficSignals>`,
`<Entities>`, and `<Storyboard>`.

`<ParameterDeclarations>`, `<CatalogLocations>`, `<Properties>` and
`<StopTrigger>` are emitted **unconditionally**, empty when they hold nothing.
Each was required in the 1.0/1.2 schema and relaxed only later, so emitting them
conditionally would turn one version conditional into five.

### Entities

`<ScenarioObject>` holding a `<Vehicle>` or a `<Pedestrian>` — bounding box,
performance, axles and properties. Any other entity object (a `<MiscObject>`, an
`<ExternalObjectReference>`, a `<CatalogReference>`) rides the preserved tier
whole.

### Positions

`<WorldPosition>`, `<RoadPosition>` and `<LanePosition>`. The other eight
position types keep riding the preserved tier — as a *whole preserved action*
rather than as a defaulted position, because substituting the origin for a
position this version cannot read would move an actor silently.

RoadMaker itself authors `<LanePosition>`: it names the lane, so the actor
follows its road through every later edit rather than staying behind in world
space.

### Init actions

`<TeleportAction>` (the actor's placement), `<LongitudinalAction><SpeedAction>`
(its initial speed) and `<RoutingAction><AssignRouteAction><Route>` (its path).

`<PrivateAction>` is a **per-element choice**, so the writer emits one element
per set arm — an action carrying both a teleport and a speed becomes two
elements, which is the model the reader would have produced from the same file.

### The storyboard

`Story ▸ Act ▸ ManeuverGroup ▸ Maneuver ▸ Event ▸ Action`, in full.

| Element | Modeled | Notes |
|---|---|---|
| `<Story>` | ✔ | with its optional `<ParameterDeclarations>` |
| `<Act>` | ✔ | with optional `<StartTrigger>` / `<StopTrigger>` |
| `<ManeuverGroup>` | ✔ | `<Actors>` may legally be empty; `<CatalogReference>` is preserved **in its schema slot** |
| `<Maneuver>` | ✔ | the struct is `StoryManeuver` — `roadmaker::Maneuver` is the junction turn-path type |
| `<Event>` | ✔ | `@priority`, `@maximumExecutionCount`, optional `<StartTrigger>` |
| `<Action>` | ✔ | wraps exactly one choice arm |

#### Actions

| Action | Modeled |
|---|---|
| `<PrivateAction><LateralAction><LaneChangeAction>` | ✔ (absolute and relative target lane) |
| `<PrivateAction><LongitudinalAction><SpeedAction>` | ✔ |
| `<PrivateAction><TeleportAction>` | ✔ |
| `<PrivateAction><RoutingAction><AssignRouteAction>` | ✔ |
| `<GlobalAction><InfrastructureAction><TrafficSignalAction>` | ✔ (both arms) |
| `<UserDefinedAction>` | preserved — its content is by definition tool-specific |
| every other `<GlobalAction>` and `<PrivateAction>` arm | preserved |

#### Conditions

| Condition | Modeled |
|---|---|
| `<SimulationTimeCondition>` | ✔ |
| `<StoryboardElementStateCondition>` | ✔ |
| `<TrafficSignalCondition>` | ✔ |
| `<TrafficSignalControllerCondition>` | ✔ |
| `<RelativeDistanceCondition>` | ✔ |
| `<SpeedCondition>` | ✔ |
| every other `<ByValueCondition>` and `<EntityCondition>` arm | preserved, inside its own wrapper |

`<Condition>` is a schema **choice**: at most one arm may be set, and the
validator refuses a condition carrying more.

### Traffic signals

`<TrafficSignalController>` with its `<Phase>` list and `<TrafficSignalState>`
rows. `TrafficSignalController::name` carries the **OpenDRIVE `<controller>
@id`**, not a human-readable label — *"the ASAM OpenDRIVE controller ID is used
as the name of the `TrafficSignalController` to reference it"* (§10.10).

Every member controller's head appears in **every** phase — the dense,
red-filled list, never RoadMaker's sparse red-by-omission storage, or a signal
the editor shows as red would export as a signal that is never red.

## ★ The phase-name trap

`Phase::name` may legally be **empty** in the model, because
`roadmaker::SignalPhase::name` is. The writer therefore *synthesizes* a name —
the semantic token, else the literal `"phase"` — and de-duplicates it per
controller, **into the output only**. It never writes the result back into the
model, so that two writes of one `Scenario` stay byte-identical.

The consequence: a `TrafficSignalControllerCondition/@phase` or
`TrafficSignalControllerAction/@phase` authored from `Phase::name` **references
nothing in the file it is written into**. Nothing downstream will tell you —
esmini v3.5.0 was measured to load a dangling `@phase`, and a dangling
`@trafficSignalControllerRef`, with exit 0 and no error at all.

So there is exactly one way to author a `@phase`:

```cpp
const std::vector<std::string> names = osc::phase_names(controller);
action.phase = names.front();
```

```python
action.phase = rm.osc.phase_names(controller)[0]
```

`osc::phase_names()` **is** the writer's synthesis, exposed — one
implementation, so the file and the reference to it cannot drift. The editor's
phase combo, the validator and `python/examples/scenario_storyboard.py` all
resolve through it, and `validate_scenario` refuses a `@phase` that names no
phase the writer will emit.

## What validation does and does not cover

Two checkers see different halves of a scenario, and neither replaces the other.

| Checker | Sees | Catches |
|---|---|---|
| `osc::validate_scenario` | one `Scenario` | schema shape, names, and every reference whose **both ends** are inside the document — entity refs, controller refs, `@phase` against the synthesized names |
| `osc::validate_routes` | a `Scenario` **and** a `RoadNetwork` | whether a route's waypoints resolve to lanes that exist and can be driven between |
| esmini (external, CI) | the file, loaded | document structure, and **lane anchors resolved against the `.xodr`** |

**Measured against the pinned esmini v3.5.0**, one mutation at a time on the
tracked fixtures:

*Fails the load* — a dangling `roadId`/`laneId`, an `s` past the road's end (in
a teleport *or* a route waypoint), a dangling `entityRef` anywhere, an invalid
`Event/@priority`, an invalid `@dynamicsShape`.

*Loads in complete silence* — a dangling `trafficSignalId`, a garbage
`TrafficSignalState/@state`, a dangling `trafficSignalControllerRef`, a
`@phase` that names no phase.

That split is why the traffic-signal half of a scenario is gated by RoadMaker's
own validator rather than by the simulator, and why
`osc::validate_scenario` treats a dangling reference as an **error** while
treating a schema-shape problem (an `<Act>` with no `<ManeuverGroup>`) as a
**warning** — the shape came from a file RoadMaker could already read, and
refusing to write it back would mean a document just loaded can no longer be
saved.

## OpenSCENARIO 2.x — the export-only concrete-scenario subset

RoadMaker also emits **ASAM OpenSCENARIO DSL v2.2.0** (`.osc`) from the same
internal model that writes 1.x. Two things about it are load-bearing and easy to
miss:

- **It is a different language, not a different serialization.** OpenSCENARIO
  XML 1.x is XML. OpenSCENARIO DSL 2.x is a Python-style textual DSL with
  significant indentation, `#` comments and the `.osc` extension. Nothing about
  the `.xosc` writer carries over except the model it reads.
- **It is export-only, and it always will be at v0.1.0.** There is no `.osc`
  reader, no grammar and no parser dependency. The round trip is via the
  internal model and the project container
  ([ADR-0008](../decisions/0008-persistence-layers-asam-first.md)) — never via
  re-importing 2.x. A future parser dependency goes through the
  [dependency policy](../standards/dependencies.md)'s stop-and-ask *before* it
  is prototyped.

**Layer 0, like the `.xodr` and the `.xosc`.** No RoadMaker-specific enrichment
may leak into the `.osc`: anything the standard cannot express belongs in the
project's own Layer-2 container, not in a vendor extension to this file.

### Why "concrete"

The standard defines a concrete scenario as *"a scenario for which the exact
evaluation of any of its parameters are completely determined to a fixed value
for any point in time"* (DSL v2.2.0 §6.3.1.2.1). RoadMaker's model holds exactly
that — a named map, named actors, fixed speeds — and holds nothing that could
express a parameter range or a constraint space, so the level is a consequence
of the model rather than a choice between levels.

### What is exported

★ **This table is generated-adjacent, not decorative.** It mirrors
`osc::osc2_supported()` in `roadmaker/osc/osc2.hpp`, and
`Osc2Subset.TheCommittedDocMatchesTheRegistry` fails CI if the two drift. There
is no schema, no parser and no simulator in CI to check this output — "a
documented subset" is the only promise made about it, so the promise is gated.

| DSL construct | Produced by |
|---|---|
| `import osc.standard.all` | always emitted (§7.7.5.2) |
| `scenario <name>:` | `Osc2WriteOptions::scenario_name` |
| `map.set_map_file("...")` | `<RoadNetwork><LogicFile @filepath>` |
| `<actor>: vehicle` | `<ScenarioObject>` holding a `<Vehicle>` |
| `<actor>: pedestrian` | `<ScenarioObject>` holding a `<Pedestrian>` |
| `keep(it.vehicle_category == ...)` | `<Vehicle @vehicleCategory>` |
| `do parallel:` | the `<Init>` actions, which all start together |
| `<actor>.drive()` | an entity with any `<Init>` action |
| `speed(<v>mps)` | `<SpeedAction><AbsoluteTargetSpeed @value>` |

Three details worth knowing:

- **`mps` is a normative speed unit**, so the kernel's m/s needs no conversion.
  A `kph` conversion would put a rounding error into a file nothing in CI can
  check.
- **`vehicle_category` is the domain model's name** (§8.7.7), not `category` —
  which appears only in a loose conceptual example. The DSL and XML
  enumerations are also *not* the same list: a bicycle is `vru_vehicle` in the
  DSL. A category with no DSL spelling omits the `keep` rather than inventing
  one.
- **Entity names are rewritten to DSL identifiers.** `ScenarioObject/@name` is
  an XML string and routinely holds spaces or dashes; `Lead Vehicle-2` becomes
  `lead_vehicle_2`. Two names that would collapse to one identifier are
  **refused**, because two actors merged into one is a scenario that describes
  something else.

### What is not exported at v0.1.0

Everything here is **reported** by `osc::validate_osc2_subset` rather than
silently omitted — as a warning, since a 2.x file is a lossy export-only view by
definition and refusing to write one because it is lossy would make the feature
unusable.

| Not exported | Why |
|---|---|
| `<Story> / <Act> / <Event> / <Action>` | the storyboard's phase structure has no faithful concrete-subset spelling here; emitting a guess would be worse than reporting it |
| `<Trigger> / <Condition>` | conditions are modifiers and constraints in the DSL, not triggers; the mapping is not one-to-one |
| `<Route> / <Waypoint>` | a DSL route comes from `map.create_route(get_odr_points(...))`, an external method this exporter cannot synthesize |
| `<TrafficSignalController> / <Phase>` | traffic lights are domain-model actors with their own behaviour; the 1.x controller decomposition does not carry over |
| `<LanePosition> / <RoadPosition>` | placement is a `position()`/`lane()` modifier relative to a route, and there is no route to relate it to (see above) |
| `OpenSCENARIO 2.x import` | export-only at v0.1.0; a parser dependency goes through the dependency policy's stop-and-ask first |
| `abstract and logical scenarios` | the internal model holds fixed values only, so it cannot express a parameter range or a constraint space |

**The subset is narrow on purpose.** Every construct emitted appears in the
specification's own normative text. A wider emitter that guessed at modifier
signatures would be worse than a small correct one, precisely because nothing
downstream would contradict it.

### How it is checked

Not by a schema — there is none in this repository and none can be tracked. The
2.x file is **reviewed against the table above**, and that review is made
possible by the doc↔code gate rather than by memory. The 1.x file emitted from
the same scenario is checked by esmini, as described above; the two exports have
different acceptance criteria and neither substitutes for the other.

## Not supported

- **OpenSCENARIO catalogs.** A `<Catalog>` document is refused by name at parse
  time rather than returning an empty `Scenario` that looks like a successful
  load of nothing. A `<CatalogReference>` *inside* a scenario is preserved.
- **`$parameter` expression evaluation.** A numeric attribute holding one fails
  the strict scalar parse and takes the preserve-the-spelling path, which is the
  correct outcome for a value whose meaning is only known at runtime.
- **Simulation.** Previewing a scenario is
  [#249](https://github.com/Robomous/RoadMaker/issues/249)'s, through esmini as
  an external process.
- **OpenSCENARIO 2.x import.** The 2.x *export* is described above; there is no
  parser and no plan for one.
