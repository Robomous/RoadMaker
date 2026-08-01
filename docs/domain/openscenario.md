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
- **OpenSCENARIO 2.x import.** Export of a documented 2.x subset is
  [#327](https://github.com/Robomous/RoadMaker/issues/327); there is no parser
  and no plan for one.
