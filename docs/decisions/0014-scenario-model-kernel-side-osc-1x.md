# ADR-0014: the scenario model lives in the kernel, and OpenSCENARIO 1.x is its Layer-0 format

*Why a data model with no user-visible surface has to be decided before its
first header is written; why the file RoadMaker emits declares itself an older
revision than the specification it was written from; and why "validate" turned
out to mean two different things that a single answer cannot cover.*

- **Status:** accepted
- **Date:** 2026-07-30
- **Deciders:** Armando Anaya

## Context

P8 (Scenarios) is the last pillar with sprints outstanding, and
[#245](https://github.com/Robomous/RoadMaker/issues/245) (`p8-s1`) is its first.
Unlike every sprint since P5 it extends nothing: the
[P8 discovery report](../roadmap/pillars/p8_discovery.md) confirmed by exhaustive
search that there is no `core/include/roadmaker/scenario/`, and no `Storyboard`,
`Entity`, `Trajectory`, `Condition`, `TrafficSignalController` or
`TrafficSignalState` type, binding or enum anywhere in the tree — no `.xosc`
file, no writer, no reader, no validator. What makes it an ADR is that three of
the decisions below stop being available the moment the first header exists.

### The Python boundary is enforced at link level, not by convention

`python/CMakeLists.txt:12` forces `RM_BUILD_EDITOR OFF` for a wheel build and
`:36` links `roadmaker::core` **alone**; `python/src/bindings.cpp` contains zero
`#include "editor/…"`. The editor's `Document` and its `QUndoStack` are therefore
unreachable from Python *by construction*, not by discipline — and the
golden-workflow replays that stand behind the release gate drive `rm.edit.*`
factories through `rm.edit.EditStack`, fingerprinting state with byte-identical
`rm.write_xodr` output across undo ×10 / redo ×10.
[GW-6](../roadmap/golden_workflows/gw6_scenarios.md) is written to accept this
pillar the same way.

### "Validate" had no answer until 2026-07-30, and now has two

There is no XSD in the tree and there cannot be one: the OpenSCENARIO schema
ships only in ASAM's gated bundle, and unlike OpenDRIVE it carries no
redistribution grant, so committing it would hand it to readers who have no
licence to receive it (`.gitignore:49-57`,
[third_party/asam/README.md](../../third_party/asam/README.md)). By
[maintainer ruling](https://github.com/Robomous/RoadMaker/issues/257) esmini's
parser **is** the validator. But reading the specification for this sprint turned
up something the ruling did not anticipate: OpenSCENARIO **does** publish
checker-rule ids, in exactly the form the OpenDRIVE validator already cites —
`asam.net:xosc:<version>:<rule_set>.<rule_name>`, 99 distinct ids in the 1.4.0
text, 76 references in the annex chapter alone.

### The signal half is already half-built, deliberately

`core/include/roadmaker/road/junction.hpp:316-322` says so in the code's own
words: OpenDRIVE §14.6 excludes the signal cycle from its own scope and points at
OpenSCENARIO, so RoadMaker's phases ride a Layer-1 `rm:phases` userData element
**shaped to mirror OpenSCENARIO `Phase{duration,name,trafficSignalStates}`** to
make the P8 export near-mechanical, and `junction_phases()`
(`core/include/roadmaker/mesh/junction_phases.hpp:138`) already resolves the
sparse storage into the dense, Red-filled timeline OSC requires. Half-built is
not built, though, and §8 names the three gaps.

## Decision

### 1. The scenario model is kernel-side and `EditStack`-driven

It lives in `core/include/roadmaker/osc/`, its mutations are `edit::Command`
factories in the existing `roadmaker::edit` namespace, and it is reachable from
`rm.edit.*` like every other editable thing in the kernel.

The argument is the retrofit cost, not elegance. A model in
`editor/src/document/` cannot be replayed headlessly *by construction* — see the
link-level boundary above — so GW-6 would lose its pre-flight evidence
permanently and the release gate would narrow to hand-runs with nothing behind
them. Moving the model at `p8-s5` would mean rewriting every consumer the four
intervening sprints had built on it. The editor still owns the scenario **UI**
(`p8-s2` onward), under the rule [ADR-0003](./0003-qt-widgets-editor.md) already
sets for every other domain concept: state and mutation in the kernel, widgets
thin.

### 2. The namespace is `roadmaker::osc`, and two names can never be used

`roadmaker::Maneuver` is taken. It means an authored override for **one
connecting road's path through a junction** — a public type with a Python
binding, an editor tool and an `rm:maneuver` userData code. OSC's `Maneuver` is a
container of `Event`s with triggers and actions. They share nothing but a word,
so the OSC model is namespaced rather than renamed at the door.

`signals` cannot be a member name **anywhere in a kernel header**. Qt's
`<QObject>` defines it as a macro (`#define signals public`), so a member of that
name makes the header impossible to include from any editor translation unit —
the struct declaration itself fails to parse. This is already documented as a
trap twice (`core/include/roadmaker/mesh/junction_phases.hpp:54-57`,
`core/include/roadmaker/mesh/junction_signals.hpp:84-88`), and an OSC
`TrafficSignals` container is precisely where it would be reached for a third
time. The kernel never includes Qt; its headers are consumed by code that does.

### 3. Layer 0 is OpenSCENARIO 1.x XML, revision-targetable, defaulting to 1.2

`write_xosc` takes an options struct with a target-revision enum and **defaults
to the older revision**, exactly as the OpenDRIVE writer does:
`WriterOptions::target_version` offers `XodrVersion{v1_8_1, v1_9_0}` and defaults
to `v1_8_1` (`core/include/roadmaker/xodr/writer.hpp:39-64`).

The default is 1.2 rather than the 1.4.0 specification this sprint was written
from, because **validation means "esmini accepts the file"** and CI pins esmini
`v3.5.0`. Emitting a newer revision by default would mean `p8-s1`'s acceptance
gate could only be met by first bumping the pinned validator — which is
[#506](https://github.com/Robomous/RoadMaker/issues/506)'s work, not this
sprint's. 1.2 is also what already circulates: `scripts/esmini_smoke.py:44,51`
synthesizes a wrapper declaring `revMajor="1" revMinor="2"` for every tracked
`.xodr`.

**The choice costs nothing in citable rules**: every checker-rule id in the 1.4.0
text is stamped `1.0.0`, `1.1.0` or `1.2.0` and none `1.3.0` or `1.4.0`, so
targeting 1.2 forfeits no rule §7 could otherwise cite. What it does forfeit is
exactly one attribute — `Phase/@semantics`, created in 1.4.0. Through 1.3.0 that
meaning rode in `Phase/@name`, the specification's own words, and since `@name`
is required and must be synthesized anyway (§8), naming phases
`stop`/`go`/`attention_stop`/`fallback` is both revisions' answer, with
`@semantics` additionally emitted when 1.4.0 is targeted — opt-in from the first
release.

### 4. `write_xosc` is deterministic and returns a string

Not a path, not a stream: `Expected<std::string>`, with no timestamps and no
locale-dependent formatting — the property `write_xodr` has had since M2
(`core/include/roadmaker/xodr/writer.hpp:87-91`) and that ~30 tests, the
export-preview tools and both replays' fingerprints depend on. A path-taking
`save_xosc` wraps it, in the writer's existing shape.

### 5. Only `odr_id` may cross the file boundary

`SignalId` and `ControllerId` are generational arena handles
(`core/include/roadmaker/road/id.hpp:25-55`): runtime-only, invalidated by an
erase, never valid across a load. Every cross-reference the kernel persists
already uses the OpenDRIVE `odr_id` string instead. `JunctionPhaseInfo::signal_states`
is the exception that matters: it carries a `SignalId`
(`core/include/roadmaker/mesh/junction_phases.hpp:58-61`), so the exporter
resolves each through `network.signal(id)->odr_id` before writing
`TrafficSignalState/@trafficSignalId`, and a stale handle is a `Diagnostic`
rather than a silently empty attribute. Writing the handle would produce a file
that looks entirely right and references nothing.

### 6. The reader never silently drops input

The OpenSCENARIO reader mirrors the OpenDRIVE one
(`core/include/roadmaker/xodr/reader.hpp:30-34`): a parse fails outright only on
structural problems, and everything skipped, coerced or guessed at is a
`Diagnostic`. Unmodeled attributes and child elements are captured in the
`RawXml` shape (`core/include/roadmaker/xodr/raw_xml.hpp:30-46`) and re-emitted
after the modeled content.

Round-trip stability is claimed at two strengths, and the difference is stated
rather than blurred: **what RoadMaker writes, it rewrites byte-identically**;
what a foreign tool wrote is **preserved verbatim and warned about**. A flat
byte-stability promise over arbitrary third-party `.xosc` is not achievable on
the OpenDRIVE writer's terms, and `fmt-s2`'s re-canonicalization caveat applies
unchanged.

### 7. Validation is esmini *and* rule ids, because they answer different questions

**esmini** answers whether a shipping simulator accepts the file. **`osc/rules.hpp`**
mirrors `core/include/roadmaker/xodr/rules.hpp` — a flat list of
`inline constexpr std::string_view` UIDs — and answers which normative rule broke.
Neither substitutes for the other, so the sprint owes both. The version component
follows the convention the OpenDRIVE rules already state: *the revision the rule
first appeared in*, which the 1.4.0 text corroborates by citing `1.0.0`, `1.1.0`
and `1.2.0` ids side by side.

The constants live in **`roadmaker::osc::rules`**, not the existing
`roadmaker::rules`: mixing `asam.net:xodr:` and `asam.net:xosc:` UIDs in one
namespace leaves no call-site spelling that says which standard a rule belongs to.

Findings ride the **existing** `Diagnostic`
(`core/include/roadmaker/xodr/diagnostic.hpp:35-53`), which is already
format-neutral — it lives in namespace `roadmaker` and includes only
`road/id.hpp`. Two costs, both accepted knowingly: its `rule_id` comment names
only the OpenDRIVE UID form and has to widen, and its *header* still lives under
`xodr/`, so an `osc/` header includes an `xodr/` one. Relocating the type touches
every `xodr` translation unit and belongs to its own change.

### 8. One junction timeline decomposes into one controller per `<controller>`

RoadMaker stores a cycle **per junction, across N controllers**. OpenSCENARIO's
`TrafficSignalController` has one `@name` and its own `Phase` list, and the
specification builds its signal groups from the ASAM OpenDRIVE `<controller>`
element in its own words. So one junction timeline exports as **one
`TrafficSignalController` per `<controller>`**, each carrying the same phase
durations with only its own row of states.

The export reads the `JunctionPhasePlan` returned by `junction_phases()`, never
`Junction::phases` directly. The stored form is **sparse and Red-by-omission**
(`core/include/roadmaker/road/junction.hpp:324-326`): a phase lists only its
non-Red controllers, so an all-red clearance phase carries an empty list. Exported
from the raw storage, a signal that RoadMaker shows as red would appear in the
file as a signal that is never red.

Two smaller gaps the "near-mechanical" comment does not mention, both settled
here rather than at [#248](https://github.com/Robomous/RoadMaker/issues/248).
**`Phase/@name` is required and unique among a controller's phases**, while
`SignalPhase::name` may legally be empty (`…/junction.hpp:328-330`), so the writer
synthesizes a semantic token and de-duplicates on collision. And **`@state` is a
free string the specification leaves to the simulation engine**, so its spelling
is chosen for esmini and recorded as an engine-directed choice, not a normative
one; `SignalState{Red, Yellow, Green, Off}` maps cleanly only *because* the
decomposition leaves each controller one colour per phase. Finally, always emit
`TrafficSignalState` and never `TrafficSignalGroupState`: the group form carries
no ids at all, discarding exactly the identity fact §5 protects.

### 9. A `.xosc` is a top-level project file, stem-matched to its scene

`town.xodr` pairs with `town.xosc`, beside it at the top level of the project
directory, alongside the existing `town.rmscene.json` sidecar — the placement
[persistence](../architecture/persistence.md) already describes. A scenario is a
**second Layer-0 file**, not session state, so it is never folded into the
Layer-2 container and stays standalone-openable in any tool. `Project::scenes()`
globs `*.xodr` only today (`editor/src/document/project.hpp:83-86`); teaching the
project model about scenario files is `p8-s2`'s.

### 10. What this record does not decide

**OpenSCENARIO 2.x stays out**, deferred to `p8-s6`
([#327](https://github.com/Robomous/RoadMaker/issues/327)) as an export-only
subset with no parser dependency; nothing here forecloses it. **The specification
text stays untracked**, for the licensing reason above, and is fetched locally via
`scripts/fetch_asam_specs.py`. And **no new dependency** is taken: pugixml 1.16
already serves the OpenDRIVE writer (`core/src/xodr/writer.cpp:29`,
`cmake/deps.cmake:48-50`) and serves this one, so `cmake/deps.cmake` does not
change — the fourth consecutive record for which that is true.

## Consequences

**Easier.** Everything downstream gets the kernel's existing machinery for free:
undo/redo through `EditStack`, headless replay from Python, `Diagnostic`-shaped
findings the Diagnostics dock already renders, and the `RawXml` preservation
tier. The traffic-signal export reduces to a decomposition over a plan structure
that is already the single source the phase editor, the viewport overlay and the
bindings share. And bumping the pinned esmini later is a default change.

**Harder.** The default output is a revision older than the specification the
implementation is written from, so the code carries a version conditional from
its first release and every reader needs the esmini reasoning to make sense of
it. The `@state` spelling is engine-directed, so a file esmini loves may need a
different one elsewhere, and RoadMaker cannot know without a second engine in CI.
Scenario editing UI cannot hold state in a Qt model — the shortest path in the
editor and permanently the wrong one here. And an `osc/` header including an
`xodr/` header is layering debt taken on deliberately.

**Follow-ups this creates**

- [#506](https://github.com/Robomous/RoadMaker/issues/506) — **esmini is pinned
  twice**: the `esmini-roundtrip` job sets an `ESMINI_VERSION` env var but keys
  its cache on a literal, while [the CI guide](../contributing/ci.md) documents
  bumping only the env var, so a bump silently reuses the old binary. It sits
  directly in this sprint's path — the reader/validator PR adds the first real
  `.xosc` that job has ever seen — and it is what would have to be fixed before
  1.4.0 could become the default.
- **The esmini gate is OpenDRIVE-worded.** `scripts/esmini_smoke.py`'s error
  markers all name OpenDRIVE, so a scenario-level failure would pass today;
  teaching it scenarios, with a deliberately broken `.xosc` fixture, is `p8-s5`'s.
- **`Project::scenes()` is `.xodr`-only**, so a scenario file is invisible to the
  project model until `p8-s2`; until then a `.xosc` is written and read by path.
- **Relocating `Diagnostic`** out from under `xodr/`, once there is a second
  producer to justify the churn.

**Reversal cost, stated plainly.** Low for the revision default — one enum value,
with the 1.4.0 path written and tested from the start — and low for the file
placement. Moderate for the preserved tier, since adding it later means
re-authoring the reader. **High for the kernel-side ruling, which is why it is
first**: reversing it after `p8-s2`–`p8-s4` have built on it means moving the
model, its commands and its bindings, and re-authoring GW-6's replay against
something that cannot be replayed.

**Superseded wording elsewhere.** ADR-0008's OpenSCENARIO paragraph and the
roadmap's P8 row both say "OpenSCENARIO 1.x" without naming a revision, or where
the model lives — accurate when written, under-specified now — so both are updated
in the same change as this record rather than left to contradict it. And the P8
discovery report's finding that the validate half is "blocked pending a maintainer
ruling" is discharged twice over: by the esmini ruling, and by §7's rule ids.

## References

- [#245](https://github.com/Robomous/RoadMaker/issues/245) — `p8-s1`, the
  OpenSCENARIO data model this record precedes;
  [#257](https://github.com/Robomous/RoadMaker/issues/257) — the P8 epic and the
  esmini-as-validator ruling;
  [#327](https://github.com/Robomous/RoadMaker/issues/327) — the OSC 2.x subset
  held out of scope; [#506](https://github.com/Robomous/RoadMaker/issues/506) —
  the double esmini pin that gates raising the default revision
- [ADR-0008](./0008-persistence-layers-asam-first.md) — the persistence layering
  this record extends to a second ASAM format; amended by it
- [ADR-0003](./0003-qt-widgets-editor.md) — why `core/` never links Qt, which is
  why `signals` is unusable and why the model cannot live in the editor
- [ADR-0011](./0011-lidar-ingest-in-house-las.md),
  [ADR-0012](./0012-osm-ingest-xml-in-house.md) — the transitive-drag test, passed
  here trivially by taking nothing
- [P8 discovery report](../roadmap/pillars/p8_discovery.md) — the exhaustive
  search behind "nothing exists", and the naming-collision table
- [GW-6 — Scenarios end-to-end](../roadmap/golden_workflows/gw6_scenarios.md) —
  the acceptance target this record is shaped to keep replayable
- [Persistence architecture](../architecture/persistence.md) — the layer model
  and the sidecar naming rule; [dependency & licensing
  policy](../standards/dependencies.md) — the esmini entry, and why running it as
  a subprocess is not linking it
- ASAM OpenSCENARIO XML 1.4.0 — §10.10, *Traffic signal*; §6.11, *Traffic
  signals*; §5, *Backward compatibility*; the checker-rule annex. ASAM OpenDRIVE
  1.8.1/1.9.0 — §14.6, *Signal controllers*
