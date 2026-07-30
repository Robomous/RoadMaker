# P8 discovery — Scenarios

*What the code actually looks like against the P8 scope, and the sprint cut
that follows. Written 2026-07-29, before any P8 sprint starts. Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-6](../golden_workflows/gw6_scenarios.md), which §7 recommended drafting at
this planning step and which the maintainer approved on 2026-07-30 ·
Persistence layering:
[ADR-0008](../../decisions/0008-persistence-layers-asam-first.md).*

## Why this document exists

P8 is the last pillar, and the only one with **no code at all** in its
subject area. That makes this read different from its predecessors: P7's
discovery moved work *out* of its first sprint by finding the hard part
already built, and P2's inverted its plan. Here the surface is genuinely
greenfield, so the useful findings are not about what exists but about
**what the pillar will collide with, what it cannot validate, and one
architectural constraint that decides whether GW-6 can have headless
evidence at all.**

Three findings lead.

**The traffic-signal half is already modelled, and it was built for this.**
`SignalPhase` was deliberately shaped to mirror OpenSCENARIO 1.4.0's
`Phase{duration,name,trafficSignalStates}`, and the kernel says so in its own
source (`core/include/roadmaker/road/junction.hpp:320-322`): *"so the P8
export to traffic-signal actions is near-mechanical."* That claim holds up —
with one decomposition caveat named in §2 that the comment does not mention.

**p8-s6's acceptance criterion cannot be met in CI as the repository stands,
and the reason is licensing rather than effort.** The
[realignment](../updates/2026-07-realignment.md) requires that the exported
1.x file *"validates against the schema"*. There is no XSD in the tree, none
is fetched, and — verified below — the ASAM reference texts are **entirely
untracked**, so CI has neither the schema nor the prose. This is not a
to-do; it needs a maintainer decision between three substitutes (§4).

**Whether GW-6 can have a headless replay is decided by where P8 puts its
model, and the decision is made in the first sprint, not the fifth.** Both
existing replays drive `rm.edit.*` factories through `EditStack`, and the
Python module links `roadmaker::core` alone. If the scenario model lands in
`editor/src/document/`, GW-6 loses its pre-flight evidence permanently and
the release gate narrows to three hand-runs with nothing behind them (§6).

## 1. What exists

- **The signal cycle, complete and end-to-end.** `SignalPhase` /
  `PhaseState` / `SignalState` (`core/include/roadmaker/road/junction.hpp:298-340`),
  persisted as `<userData code="rm:phases">`
  (`core/include/roadmaker/xodr/rm_codes.hpp:68`), six undoable edit
  factories (`core/include/roadmaker/edit/operations.hpp:1615-1672`), a
  timeline editor (`editor/src/panels/phase_panel.hpp`), a viewport overlay,
  Python bindings and tests on both sides. See §2.
- **A resolved query that is the right export input.** `junction_phases()`
  (`core/include/roadmaker/mesh/junction_phases.hpp:138`) returns a
  `JunctionPhasePlan` with **Red-filled** states, cumulative `start` offsets,
  resolved signal heads, and the connecting roads that may proceed. It is
  already the one solve shared by editor, viewport, Python and validator.
- **Signals and controllers, with a stable file-level identity.** `Signal`
  (`core/include/roadmaker/road/signal.hpp:40-70`), `Controller` / `Control`
  (`core/include/roadmaker/road/controller.hpp:36-67`), and the junction-side
  *reference* `JunctionController` (`junction.hpp:255-264`). Every
  cross-reference in the kernel uses the **`odr_id` string**, never the arena
  handle — see §3.
- **`signalize_junction`**, which mints signals, controllers and mounts from
  four templates (`operations.hpp:1537-1611`), so a scenario has something to
  point at without hand-authoring.
- **esmini, pinned and already fed a `.xosc`.** The `esmini-roundtrip` job
  (`.github/workflows/ci.yml:677-713`) runs esmini v3.5.0 headless over every
  committed sample. `scripts/esmini_smoke.py:49-102` synthesises a minimal
  OpenSCENARIO **1.2** wrapper to do it, because esmini has no bare
  road-network mode. See §5.
- **A reserved, hidden toolbar tab.** `ToolbarTab::kScenario`
  (`editor/src/app/shortcut_registry.hpp:136`) with an empty group; a tab
  appears only once a group holds an action, and a test pins that it stays
  hidden (`editor/tests/test_toolbar_registry.cpp:194-210`).
- **The persistence seams a scenario needs.** `Project` is a *directory* with
  a merge-on-write `project.json`
  (`editor/src/document/project.hpp:20,66-75`); `SceneState` is a per-scene
  sidecar with a forward-compatible `raw` merge base and an established
  relative-path idiom (`editor/src/document/scene_sidecar.hpp:94-146`).
  ADR-0008 already reserves *"(P8) scenario-editor state"* as a Layer-2
  resident (`:188`).
- **`Editor2DPage`** (`editor/src/panels/editor2d_host.hpp:43-64`) — a
  plug-in seam whose interface is `title()` / `widget()` /
  `relevant(SelectionModel&)`. The storyboard editor is a fourth page beside
  Profile, Width and Signal Phase.
- **pugixml 1.16**, already the kernel's XML engine for the `.xodr` reader,
  writer and OSM ingest. A `.xosc` writer needs no new dependency.

## 2. What does not exist (confirmed by exhaustive search)

**No scenario code of any kind.** There is no `core/include/roadmaker/scenario/`
directory; `ls core/include/roadmaker` is `assets edit geometry gis io lidar
mesh osm road xodr`. No `Storyboard`, `Story`, `Act`, `Actor`,
`ScenarioObject`, `Entity`, `Trajectory`, `Condition`, `Trigger`, `Init`,
`TrafficSignalController` or `TrafficSignalState` type, binding or enum
anywhere. No `.xosc` file. No writer, reader or validator.

**No routing or pathfinding.** `route` appears nine times tree-wide, all
prose. There is no lane graph, no traversal, no Dijkstra/A\*. This matters
for **#247 (lane-anchored routes)**, which the sprint list places third but
which has the largest greenfield surface of the six —
`core/include/roadmaker/osm/graph.hpp` is an *import* graph and is not
reusable as a driving graph.

**No `Entity`-like selection kind.** `SelectionEntry`
(`editor/src/document/selection_model.hpp:36-45`) is a flat struct of six
typed ids with `operator==`. Adding a seventh is additive, but every consumer
switch — picking, properties panel, context menu, scene tree, gizmo — must
learn it. That is #246's widest blast radius in the editor.

**No GW-6, and no P8 discovery report until this one.**
`docs/roadmap/golden_workflows/README.md:29` carries the only unlinked row in
its table.

### The one adjacency that is genuinely near-ready, and its caveat

`SignalPhase` mirrors OSC `Phase` faithfully, and `junction_phases()` already
does the Red-filling that OSC's *dense* `TrafficSignalState` list requires
against RoadMaker's *sparse* Red-by-omission storage
(`junction.hpp:324-326`). Two things still need modelling, and neither is in
the "near-mechanical" comment:

1. **The cycle is per-junction; OSC's is per-controller.** RoadMaker stores
   one timeline across N controllers of a junction. OSC's
   `TrafficSignalController` has one `@name` and its own `Phase` list. The
   export must **decompose** a junction cycle into one
   `TrafficSignalController` per OpenDRIVE `<controller>`, each carrying the
   same durations with its own row of states sliced out. This is the only
   genuinely new modelling in the traffic-signal half.
2. **Identity must be resolved back to strings.**
   `JunctionPhaseInfo::signal_states` carries `SignalId` — an arena handle
   (`junction_phases.hpp:59`). OSC's `TrafficSignalState/@trafficSignalId`
   needs the `<signal>` `@id`, so the exporter resolves each through
   `network.signal(id)->odr_id`. See §3.

## 3. Two identities, and only one of them may cross the file boundary

Stated once because it is the single easiest way to write a subtly broken
exporter:

| | What it is | Lifetime |
|---|---|---|
| `SignalId` / `ControllerId` | generational arena handles (`core/include/roadmaker/road/id.hpp:53-54`) | **runtime only, one `RoadNetwork` instance**; never persisted, never valid across a load |
| `odr_id` (`std::string`) | the `@id` written to the file | the file's own identity |

Every cross-reference in the kernel already uses the string —
`Control::signal_odr_id`, `JunctionController::controller_odr_id`,
`PhaseState::controller_odr_id`, `SignalMount::signal_odr_id` — and the
rationale is recorded three times (`controller.hpp:31-35`,
`junction.hpp:278-279`, `junction.hpp:302-306`): it is what the standard
stores, it stays faithful to dangling third-party references, and it survives
erase/restore in the command layer.

**Every OSC reference — `TrafficSignalController/@name`,
`TrafficSignalState/@trafficSignalId`, and any entity anchored to a road or
lane — must be built from `odr_id`, never from an arena handle.**

## 4. The XSD is missing, and it is a licensing problem rather than a chore

p8-s6's acceptance
([realignment](../updates/2026-07-realignment.md)) requires the exported 1.x
file to validate **against the schema**. Verified state:

- `find . -name "*.xsd"` over the whole tree returns **nothing**.
- `scripts/fetch_asam_specs.py:432-444` does not download it; it only *checks*
  and writes `PRESENT`/`MISSING` into the generated index. Current status:
  `MISSING`.
- The XSD is distributed only in ASAM's gated bundle, so obtaining it is a
  manual maintainer step.

And it is worse than a local-file gap. **The ASAM reference texts are
entirely untracked**, and `.gitignore:49-56` says why in its own words: the
`INDEX.md` whitelist is **inert**, because git cannot re-include files under
an excluded parent directory. `git ls-files .claude/references/` returns
empty. So CI has neither the schema nor the prose, and a schema-validation
step cannot simply be added.

> **Amendment, 2026-07-30.** The untracked-texts half of this finding has been
> acted on, and it splits by standard. **OpenDRIVE is now committed** under
> `third_party/asam/` — its specification replaces ASAM's regular terms with an
> unrestricted grant, so it can live in the repository, and the inert
> `.gitignore` whitelist described above is gone.
> **OpenSCENARIO remains untracked and must stay that way**: it carries no
> comparable grant. So this section's conclusion is unchanged where it matters —
> CI still has neither the OSC schema nor the OSC prose, which is why the
> maintainer ruled for **esmini as the de-facto validator** (option (b)) on
> [#257](https://github.com/Robomous/RoadMaker/issues/257). See
> `third_party/asam/README.md` for the quoted terms.

**Three substitutes, and a recommendation:**

| Option | Works in CI today? | Cost |
|---|---|---|
| (a) Hand-transcribe OSC Annex C checker rules into tests | yes | real work, partial coverage, but mirrors what `rules.hpp` already does for OpenDRIVE |
| (b) **Treat esmini's parser as the de-facto validator** | **yes** | zero new dependency; esmini is pinned, MPL-2.0-cleared as a run-only tool, and already in CI |
| (c) Maintainer supplies the XSD out-of-band (secret / manual step) | no, not without new infrastructure | licensing review, and CI can still not carry the file |

**(b) is the only one that works today with no new dependency**, and it
sharpens the case for making the esmini job consume a real `.xosc` (§5) —
that step stops being a nice-to-have and becomes the validation story.
Whichever is chosen, **p8-s6's acceptance wording needs amending to match**,
because as written it promises something the repository cannot do.

## 5. esmini is already handed a `.xosc` — a synthetic one

The integration is more advanced than "loads a map", and less advanced than
it looks. `scripts/esmini_smoke.py:49-102` holds `XOSC_TEMPLATE`, a hardcoded
OpenSCENARIO **1.2** wrapper written to a temp file so `esmini --osc` will
load a `.xodr` — because, as the docstring says, *"esmini has no bare
road-network mode — the scenario is the entry point"*. Its only variable part
is `<LogicFile filepath="{xodr}"/>`.

So the wrapper has one `ScenarioObject` (Ego), an `Init` with a single
`TeleportAction` to the world origin, and a `StopTrigger` at 0.5 s. **No
`Story`, no `Act`, no `ManeuverGroup`, no `Event`.** What the gate asserts is
that esmini parses and builds the *road network*; nothing scenario-behavioural
is exercised.

Four concrete changes turn it into a scenario gate, and they belong to #249:

1. A second mode taking a `.xosc` directly — the CLI positional is named
   `xodr` and the wrapper generation is unconditional
   (`esmini_smoke.py:117-120,143`).
2. **`ERROR_MARKERS` are all OpenDRIVE-worded** (`:106-112`). A scenario-level
   failure — unresolvable entity reference, bad catalog reference, malformed
   condition — would slip through as a pass. `"[error]"` is the only generic
   marker, and `--disable_log` (`:123`) suppresses esmini's log file, so
   whether scenario errors still reach stdout needs re-checking.
3. A deliberately-broken `.xosc` guard fixture beside
   `tests/esmini/broken.xodr`, or the `--expect-fail` guard protects only half
   the surface.
4. A `.xosc` glob beside `assets/samples/*.xodr` in the job step.

> **Latent defect found while reading, unrelated to P8 but in its path.**
> The esmini version is pinned twice: `ESMINI_VERSION: v3.5.0`
> (`ci.yml:690`) **and** the literal `v3.5.0` in the cache key (`:698`).
> `docs/contributing/ci.md:145` instructs bumping only the env var — which
> would leave the cache keyed to the old version and silently reuse the old
> binary, so the bump would no-op. Worth fixing whenever P8 needs a newer
> esmini for scenario features.

## 6. Where the model lives decides whether GW-6 can have headless evidence

Both replays — `scripts/gw1_replay.py`, `scripts/gw2_replay.py` — drive
`rm.edit.*` factories through `rm.edit.EditStack`, *"the same command layer
the editor's undo/redo uses"* (`gw1_replay.py:17-29`). Both fingerprint state
with byte-identical `rm.write_xodr` output across undo ×10 / redo ×10.

The boundary is enforced at link level, not by convention:
`python/CMakeLists.txt:36` links `roadmaker::core` **only**, `:12` forces
`RM_BUILD_EDITOR OFF` for a wheel build, and `python/src/bindings.cpp`
contains **zero** `#include "editor/..."` lines. The editor's `Document` and
its `QUndoStack` are unreachable from Python, by construction.

**Therefore a GW-6 replay is possible if and only if scenario mutations are
kernel `edit::Command`s exposed through `rm.edit.*`.** Two preconditions
follow, and both are decided in **#245**, not #249:

1. The scenario model is kernel-side and EditStack-driven. If it lands in
   `editor/src/document/`, no GW-6 replay can exist.
2. A **deterministic** `write_xosc` returning a string, so the undo/redo
   fingerprint has something to compare — the same property `write_xodr` has
   had since M2 and which ~30 tests rely on.

This is the report's most consequential recommendation, because it is cheap
to honour at the start of #245 and impossible to retrofit at #249 without
moving the model.

## 7. GW-6 does not exist, and it is formally blocking three gate issues

`docs/roadmap/golden_workflows/README.md:29` is the only row in its table
without a link. The convention a GW-6 must follow is normative at
`README.md:31-46`: **Purpose · Preconditions · numbered checkbox Steps ·
Pass criteria · Results table**, macOS shortcut with Linux/Windows in
parentheses, RoadMaker/ASAM vocabulary only, and a new doc lands with a
`no runs yet` Results row.

The migration inventory records that GW-6's three hand-run issues (one per
platform) are **"Blocked by #249"**
(`docs/roadmap/_migration/00-inventory.md:110,144,186`).

**Open question the roadmap does not resolve**, already flagged on epic
[#257](https://github.com/Robomous/RoadMaker/issues/257) and unresolved here:
the roadmap says GW-6 is *"drafted during P8 planning"*, but the only issue
owning its authoring is **#249, fifth of six**, and **#248's** headline
acceptance (*"traffic-light scenario authorable end-to-end"*) therefore ships
**before** the workflow that would verify it. Either the GW-6 draft moves to
this discovery/planning step, or the roadmap prose changes to say "drafted in
p8-s5 (#249)". **Maintainer decision.** This read recommends the former —
drafting GW-6 now costs a document and gives #245–#248 an acceptance target
to build against, which is exactly the role GW-1…GW-5 play for their pillars.

## 8. Naming collisions to settle before the first header is written

| Name | Already means | Consequence |
|---|---|---|
| **`Maneuver`** | `roadmaker::Maneuver` — an authored override for ONE connecting road's path through a junction (`junction.hpp:207-235`): turn type, endpoint slides, interior control points. Public type, Python binding, editor tool, `rm:maneuver` userData code. | OSC's `Maneuver`/`ManeuverGroup` is a container of `Event`s with triggers and actions. **They share nothing.** Namespace the OSC model (`roadmaker::osc::`) or rename at the door. |
| **`signals`** | Qt's `#define signals public` | **A member named `signals` makes any header unparseable from an editor TU.** Already documented as a trap twice (`mesh/junction_signals.hpp:83-89`, `mesh/junction_phases.hpp:52-57`). An OSC struct with a `signals` member breaks the editor build. |
| **`Entity`** | prose throughout for "arena entity"; a rule-id format field | An OSC `Entity` type makes every existing doc comment ambiguous. |
| **`Condition`** | free, but `Diagnostic`/`rules` own the "finding/rule" vocabulary | Prefer OSC's own full names (`ByEntityCondition`) over bare `Condition`. |
| **`Route`, `Trajectory`, `Story`, `Act`** | free | no collision |

One further note: `JunctionManeuverInfo::control_points` is **not** a
trajectory, and its sampled polyline (`mesh/junction_maneuvers.hpp:129-133`)
is a render/pick sample, not a driving path. But `JunctionApproachInfo::gated`
and `JunctionPhaseInfo::moving` already express *which connecting roads may
proceed* — genuinely useful raw material for #247's routes through junctions.

## 9. Sprint cut

The cut and order stand as the roadmap sets them — **#245 → #246 → #247 →
#248 → #249 → #327** — with no resequencing implied. What changes is scope
detail and one recommended addition:

- **#245 (p8-s1, data model)** — carries two decisions that are cheap now and
  expensive later: the model is **kernel-side and EditStack-driven** (§6), and
  `write_xosc` is **deterministic and returns a string** (§6). Its "validate"
  half needs the §4 ruling before it can be scoped. The traffic-signal
  decomposition (§2) belongs here, not in #248.
- **#246 (p8-s2, mode + actor placement)** — the editor work is wider than it
  reads: `SelectionEntry` gains a kind and every consumer switch follows (§2).
  The reserved `kScenario` toolbar tab and `Editor2DPage` seam mean the
  *hosting* is free; the *selection* is not. Actor placement is a sibling of
  the existing `signal_placement` / `prop_placement` / `crosswalk_placement`
  resolvers.
- **#247 (p8-s3, lane-anchored routes)** — **the largest greenfield surface of
  the six**, because there is no lane graph or traversal anywhere (§2). Worth
  re-checking its size against the others before scheduling; the sprint list's
  order does not reflect its weight.
- **#248 (p8-s4, storyboard/conditions)** — a fourth `Editor2DPage`. Its
  traffic-signal half is largely discharged by #245 if the decomposition lands
  there.
- **#249 (p8-s5, esmini hooks + GW-6)** — the esmini work is the four concrete
  changes in §5, and the MPL-2.0 boundary matters: launching esmini as a
  **subprocess** stays inside the existing licence entry
  (`docs/standards/dependencies.md:104-112`); linking or bundling it does not.
  **Recommend moving GW-6 authoring out of this sprint** (§7).
- **#327 (p8-s6, OSC 2.x subset)** — **blocked on the §4 ruling.** Its
  acceptance as written cannot be met.

**Recommended addition, not a new sprint:** draft `gw6_scenarios.md` now, as
part of this planning step, so #245–#248 have an acceptance target. It costs
a document and unblocks three gate issues from the fifth sprint.

## 10. What this changes in the tracking

- Epic [#257](https://github.com/Robomous/RoadMaker/issues/257) links this
  report; the GW-6 ownership question (§7) is restated for a decision.
- **#327** gets the §4 blocker note and an amended acceptance criterion once
  the validation substitute is chosen.
- **#245** gets the two architectural constraints from §6 and the
  traffic-signal decomposition from §2.
- **#247** gets a size note: no routing code exists at all.
- **#249** gets the four esmini changes from §5 and the MPL-2.0 boundary note.
- `docs/roadmap/README.md` gains this report in the discovery list.
- The esmini cache-key defect (§5) is worth its own small issue — it is not
  P8's, but P8 will trip it.

## Appendix — file:line map

| Concern | Where |
|---|---|
| Signal phase model | `core/include/roadmaker/road/junction.hpp:298-349` |
| OSC-mirror statement | `core/include/roadmaker/road/junction.hpp:316-322` |
| Resolved phase plan (Red-filled) | `core/include/roadmaker/mesh/junction_phases.hpp:66-144` |
| `rm:phases` grammar / parser / writer | `core/src/xodr/reader.cpp:2512-2640`, `core/src/xodr/writer.cpp:1848-1913` |
| Phase edit factories | `core/include/roadmaker/edit/operations.hpp:1615-1672` |
| Phase editor panel / 2D page | `editor/src/panels/phase_panel.hpp:45`, `editor/src/panels/editor2d_host.hpp:108-125` |
| Signal / Controller / Control | `core/include/roadmaker/road/signal.hpp:40-70`, `core/include/roadmaker/road/controller.hpp:36-67` |
| Junction-side controller reference | `core/include/roadmaker/road/junction.hpp:255-264` |
| Arena handles vs `odr_id` | `core/include/roadmaker/road/id.hpp:53-54`; rationale `controller.hpp:31-35` |
| `signalize_junction` templates | `core/include/roadmaker/edit/operations.hpp:1537-1611` |
| Junction `Maneuver` (name collision) | `core/include/roadmaker/road/junction.hpp:207-235` |
| `signals` Qt-macro trap | `core/include/roadmaker/mesh/junction_signals.hpp:83-89` |
| esmini CI job | `.github/workflows/ci.yml:677-713` |
| esmini version pinned twice | `.github/workflows/ci.yml:690`, `:698` |
| Synthetic `.xosc` wrapper | `scripts/esmini_smoke.py:49-102` |
| esmini error markers (OpenDRIVE-only) | `scripts/esmini_smoke.py:106-112` |
| esmini licence boundary | `docs/standards/dependencies.md:104-112` |
| XSD check (never fetched) | `scripts/fetch_asam_specs.py:432-444` |
| References untracked; whitelist inert | `.gitignore:49-56` |
| OSC 1.4.0 spec chapters | `third_party/asam/openscenario-xml-1.4.0/` (fetched on demand — not committed) (07 storyboard, 06 §6.8/6.9 routes+trajectories, 06 §6.11 signals, 12 Annex A actions / Annex C checker rules) |
| Golden-workflow doc convention | `docs/roadmap/golden_workflows/README.md:31-46` |
| GW-6 unlinked row | `docs/roadmap/golden_workflows/README.md:29` |
| GW-6 hand-runs blocked by #249 | `docs/roadmap/_migration/00-inventory.md:110,144,186` |
| Replay pattern (per-step, non-aborting) | `scripts/gw2_replay.py:743-760` |
| Python links kernel only | `python/CMakeLists.txt:12,36` |
| Reserved Scenario toolbar tab | `editor/src/app/shortcut_registry.hpp:136` |
| `SelectionEntry` (blast radius) | `editor/src/document/selection_model.hpp:36-45` |
| Project directory + scene glob | `editor/src/document/project.hpp:20,66-75,83-86` |
| Scene sidecar + `raw` merge base | `editor/src/document/scene_sidecar.hpp:94-146` |
| ADR-0008 OpenSCENARIO section | `docs/decisions/0008-persistence-layers-asam-first.md:201-207` |
