# 2026-07 realism batch — North American defaults, sign pack, prop proportions, orientation & editing robustness

Tracking issue: [#411](https://github.com/Robomous/RoadMaker/issues/411).
Seven maintainer directives from hands-on use, all aimed at one product
gap: scenes assembled from today's defaults do not *read* like real North
American streets. The batch establishes a canonical defaults spec —
[realism defaults](../../domain/realism_defaults.md) — and implements it:
road-class cross sections, a US sign pack with baked text/symbol faces,
prop proportion corrections, sign/signal auto-orientation, a prop
Z-rotation ring, a properties-panel completeness audit, and a
prop-deletion reliability bug. Grounded against `main` @ `e18592b`
(three-way code inventory recorded in the spec doc's changelog); no
production code changed during documentation.

Precedent: the [field-triage cycles](2026-07-field-triage.md)
([batch 2](2026-07-field-triage-2.md)). Like those, per-issue
scope/acceptance lives in the issues; this document records the decisions,
the issue set, the overlap audit, and the gate change.

## Maintainer decisions (2026-07-24, pre-made)

1. **The metric/imperial display toggle lands now, in this batch**
   ([#412](https://github.com/Robomous/RoadMaker/issues/412), metric
   default). Kernel and persistence stay SI meters unconditionally;
   imperial is a display/input layer only. Unit policy is spelled out in
   the [spec doc](../../domain/realism_defaults.md#unit-policy).
2. **The batch is release-blocking in full.** The
   [release gate](../README.md#release-gate) is extended to name the batch:
   every issue listed by #411 — including follow-ups its audits spawn —
   blocks v0.1.0.
3. **Roadmap consistency guard (standing rule).** The
   [realism defaults spec](../../domain/realism_defaults.md) is the only
   place default dimensions, proportions, unit policy, sign-pack content,
   and orientation rules are defined. Issues and code comments reference
   it and never restate its numbers; changes go through the spec doc
   first. Enforcement is structural: one machine-readable code table +
   a CI divergence test against the doc (the `shortcuts.md` /
   `shortcut_registry` precedent), landing with
   [#413](https://github.com/Robomous/RoadMaker/issues/413) and extended
   by #414/#415. The rule enters the roadmap README's
   [conventions](../README.md#sprints-and-issues).

## Work items

| WI | Issue | Slug | Kind | Pillar |
|---|---|---|---|---|
| WI-1 display units toggle | [#412](https://github.com/Robomous/RoadMaker/issues/412) | `p1-f9` | enhancement | P1 |
| WI-2 road-class cross-section & marking defaults + CI guard | [#413](https://github.com/Robomous/RoadMaker/issues/413) | `p2-s11` | sprint | P2 |
| WI-3 US sign pack | [#414](https://github.com/Robomous/RoadMaker/issues/414) | `p6-s12` | sprint | P6 |
| WI-4 prop dimensions audit | [#415](https://github.com/Robomous/RoadMaker/issues/415) | `p6-s13` | sprint | P6 |
| WI-5 sign/signal auto-orientation | [#416](https://github.com/Robomous/RoadMaker/issues/416) | `p6-s14` | sprint | P6 |
| WI-6 prop Z-rotation ring polish | [#417](https://github.com/Robomous/RoadMaker/issues/417) | `p6-s15` | sprint | P6 |
| WI-7 properties-panel completeness audit | [#418](https://github.com/Robomous/RoadMaker/issues/418) | `p6-s16` | sprint | P6 |
| WI-8 prop deletion mis-targets | [#419](https://github.com/Robomous/RoadMaker/issues/419) | — | bug | P6 |

Follow-ups spawned by WI-7's audit (release-blocking by decision 2, which binds
every issue this table lists):

| Follow-up | Issue | Kind | Pillar |
|---|---|---|---|
| signal identity + face dimensions are fixed after placement | [#429](https://github.com/Robomous/RoadMaker/issues/429) | enhancement | P6 |
| lane material friction/roughness not editable | [#430](https://github.com/Robomous/RoadMaker/issues/430) | enhancement | P6 |
| road predecessor/successor links not reported | [#431](https://github.com/Robomous/RoadMaker/issues/431) | enhancement | P2 |

**Pillar placement.** **P2 reopens a second time** (#413, after the
batch-2 reopen for #398/#403) and **P1's reopen extends** (#412 — the
Attributes pane and status-bar readouts are P1's universal-editor scope);
both discovery docs carry dated notes. **P6 does not reopen** — its sprint
stream is still in flight (#321–#323), so the new sprints append to the
open epic; stating this here prevents a misapplied reopen flag. WI-6 sits
under P6 per the #337 precedent (prop-editing affordances belong to the
prop pillar even when they extend the shared gizmo); its plumbing
coordinates with #401/#188.

**Execution order** (one PR each; the maintainer merges each personally
before the next lands): **docs PR → #419 → #412 → #413 → #415 → #414 →
#416 → #417 → #418.** WI-8 first because unreliable deletion endangers
every scene later items touch; WI-1 next so subsequently touched fields
are unit-aware from the start; WI-2 before the prop/sign items so the
divergence-test infrastructure exists to extend; WI-7 last, when every
parameter it audits exists. Golden-workflow amendments ship inside the
PRs that change behavior (#413, #416, #417) — never ahead of them.
WI-7's completed element×parameter matrix is committed into this document
by #418's PR.

## Grounding highlights (full inventory: spec-doc changelog)

- Cross-section defaults are **duplicated** across the create-road
  `LaneProfile` templates and the p2-s8 `RoadStyle` Library assets — #413's
  single table ends the duplication. No defaults registry existed.
- Sign/signal identities are **German StVO** today (`country="DE"`); the
  sign-face text pipeline (plate model → rasterizer → glTF PNG) is
  model-agnostic and ready for US faces. No unit-conversion or mph
  infrastructure exists anywhere (~30 ad-hoc formatting sites).
- The starter-library trees already sit near the spec proportions (the
  p6-s11 ×2); streetlights and signal-head dimensions are the main gaps.
- A **prop yaw ring already exists** in the transform gizmo, so #417 is
  scoped verify-and-extend (snapping, suppression, sign/signal coverage,
  tests), not greenfield.

## Overlap audit (2026-07-24; cross-ref comments posted where noted)

| Open work | Touchpoint | Disposition |
|---|---|---|
| [#338](https://github.com/Robomous/RoadMaker/issues/338) free-canvas placement | sign lateral offset vs edge soft-snap; #419 reworks the same picking files | commented; land order awareness |
| [#337](https://github.com/Robomous/RoadMaker/issues/337) prop gizmo backlog | #417 takes the rotation ring; #337 keeps scale/jitter/random-yaw; values defer to the spec | commented |
| [#401](https://github.com/Robomous/RoadMaker/issues/401) gizmo silent refusal | #417 shares the refusal/feedback plumbing | commented |
| [#406](https://github.com/Robomous/RoadMaker/issues/406) cascade epic | sprint (e) must never silently re-auto orientation/rotation overrides | commented; constraint added when sprints are cut |
| [#402](https://github.com/Robomous/RoadMaker/issues/402) sidewalk bands | band width inherits the sidewalk lane width — #413's change shifts fixtures | commented; regeneration under #413's blast-radius protocol |
| [#364](https://github.com/Robomous/RoadMaker/issues/364) USD sign faces | the flat-plate decision now covers the whole pack | commented |
| [#326](https://github.com/Robomous/RoadMaker/issues/326) `rm:` registry | any new carrier from #414 enters the conformance net in the same PR | commented |
| [#307](https://github.com/Robomous/RoadMaker/issues/307) span relocation | #418's matrix covers span parameters only | commented |
| #403, fix queue (#399/#400/#356/#360), #404/#405, docs-site, #390–#393, P7/P8 | no scope contact | none |

## Release-gate change (decision 2)

The [release gate](../README.md#release-gate) item 2 now additionally
requires every issue belonging to this batch — tracking issue #411 and all
issues it lists — closed via merged PRs. Because #411's table absorbs any
follow-ups the audits spawn, the gate stays checkable from the issue
tracker alone.

## Consistency-guard convention (decision 3)

Added to the roadmap README conventions: *issues that touch shared
defaults, dimensions, or interaction conventions must link the governing
spec doc and must not redefine its values; changes go through the spec doc
first.* This batch's governing doc is the
[realism defaults spec](../../domain/realism_defaults.md); the
[#403 connection contract](https://github.com/Robomous/RoadMaker/issues/403)
will be the same pattern for continuity guarantees.

## WI-7 element × parameter matrix

Committed by [#418](https://github.com/Robomous/RoadMaker/issues/418)'s PR, the
batch's last item. Audited against `main` @ `6ed10f8` by walking every
selectable entity's model struct against `PropertiesPanel`'s branch for it and
against the command factories in `core/include/roadmaker/edit/operations.hpp`.

**Disposition key** — `exposed`: already editable in the pane before #418 ·
`fixed here`: #418 added the row · `tool`: authored elsewhere in the UI, by
design · `read-only`: shown but deliberately not editable · `follow-up #N`: a
real gap, filed and added to the work-item table above.

**The rule #418 applied:** fix every gap whose kernel command already existed;
anything needing new kernel API becomes a follow-up — with one exception, the
signal mounting height, because the WI's acceptance names z-offset explicitly.

### Road

| Parameter | In model | Disposition |
|---|---|---|
| `name` | `Road::name` | exposed (Name row → `rename_road`) |
| `odr_id`, `length`, geometry-record and lane-section counts | — | read-only rows |
| Cross-section style | — | exposed (Road style slot → `apply_road_style`) |
| `elevation` | `Road::elevation` | tool (Elevation tool + the pane's Elevation section) |
| `plan_view`, `authoring_waypoints` | — | tool (Edit Nodes) |
| `predecessor` / `successor` | `Road::predecessor`, `::successor` | **follow-up [#431](https://github.com/Robomous/RoadMaker/issues/431)** — authorable by Link Ends, but reported nowhere |
| `lane_offset`, `superelevation` | — | tool (2D Editor) |
| `bridges` | `Road::bridges` | tool (Edit ▸ Bridge); interactive span handle is [#396](https://github.com/Robomous/RoadMaker/issues/396) |

### Lane and road mark

| Parameter | In model | Disposition |
|---|---|---|
| `type` | `Lane::type` | exposed (`set_lane_type`) |
| `direction` | `Lane::direction` | **fixed here** — `set_lane_direction` existed since P2 with no pane row (the Lane Profile tool cycled it in the viewport); disabled on the centre lane, which the command refuses |
| constant `widths` | `Lane::widths` | exposed (`set_lane_width`); a tapered profile is the 2D Editor's Width tab by design |
| road mark type, width | `RoadMark::type`, `::width` | exposed (`set_road_mark`) |
| road mark **colour** | `RoadMark::color` | **fixed here** — road styles author it (yellow centre, white lane lines) and nothing could change it afterwards |
| road mark `material` | `RoadMark::material` | exposed (Marking slot) |
| road mark `lines` (multi-stripe) | `RoadMark::lines` | read-only — authored as a unit by the mark type (`solid_solid` etc.) |
| material `@surface` | `LaneMaterial::surface` | exposed (lane Materials slot) |
| material `@friction`, `@roughness` | `LaneMaterial::friction`, `::roughness` | **follow-up [#430](https://github.com/Robomous/RoadMaker/issues/430)** — set once from the catalogue nominal, never adjustable |
| `predecessor` / `successor` links | `Lane::predecessor`, `::successor` | tool (lane-link commands, junction regeneration) |

### Junction

| Parameter | In model | Disposition |
|---|---|---|
| derived state, `locked` | `Junction::locked` | exposed (read-only Type row + Locked check → `set_junction_locked`) |
| `default_corner_radius`, `material` | — | exposed |
| corner `radius` | `JunctionCorner::radius` | exposed (Corner section) |
| corner `extent_a` / `extent_b` | `JunctionCorner::extent_a`, `::extent_b` | tool — authored by the Corner tool's viewport handles (`set_corner_extents`); no numeric row, by design |
| corner sidewalk / median material | — | exposed (two slots) |
| stop lines, surface spans, maneuvers, signalization | — | exposed (their own sections) |

### Prop (an `<object>` rendering a bundled model)

| Parameter | In model | Disposition |
|---|---|---|
| `name` (the model it renders) | `Object::name` | exposed (Model slot → `set_object_model`) |
| `s`, `t` | `Object::s`, `::t` | **fixed here** — editable spins + scrub → `move_object` (was one read-only line) |
| `hdg` | `Object::hdg` | **fixed here** — the gizmo's yaw ring was the only way to author a prop heading ([#417](https://github.com/Robomous/RoadMaker/issues/417)'s handoff); the spin steps by the ring's own registry detent |
| `z_offset` | `Object::z_offset` | **fixed here** — via `update_objects`; had **no** authoring path anywhere |
| world position | mesh `ObjectInstance::position` | **fixed here** — read-only, quoted from the mesh so the pane and viewport cannot disagree |
| `height` (and the siblings that scale with it) | `Object::height` | exposed (Height row, batches across the selection) |
| `type`, `subtype`, `orientation`, `pitch`, `roll`, `valid_length`, `dynamic`, `temporary`, `invalidated` | — | read-only — OpenDRIVE bookkeeping, not product parameters |
| `repeats` (Prop Span) | `Object::repeats` | out of scope — span *relocation* is [#307](https://github.com/Robomous/RoadMaker/issues/307) |

### Marking instance (crosswalk / stencil / marking curve)

| Parameter | In model | Disposition |
|---|---|---|
| per-instance material | `CrosswalkData::material` etc. | exposed (Material slot) |
| asset parameters (width, border, dash, gap) | the Library asset | exposed (asset editor in the same pane) |
| `s`, `t`, `hdg`, `z_offset` | `Object::*` | **read-only by design** — the geometry lives in `outlines` / `samples` in road coordinates, so moving the origin alone would desync the two; relocation is [#307](https://github.com/Robomous/RoadMaker/issues/307) |

### Signal

| Parameter | In model | Disposition |
|---|---|---|
| `s`, `t`, `h_offset` | `Signal::*` | exposed (`move_signal`) |
| `z_offset` (mounting height) | `Signal::z_offset` | **fixed here** — the one new kernel command, `edit::set_signal_z_offset`; the acceptance names z-offset and none existed |
| `orientation` (which traffic it governs) | `Signal::orientation` | **fixed here** — read-only row. GW-4 step 12c already asserted the pane reports it; it did not |
| derived facing | — | exposed (Auto facing button → `auto_orient_signal`) |
| `text` | `Signal::text` | exposed |
| `value` + `unit` (posted speed) | `Signal::value`, `::unit` | exposed |
| world position | mesh `SignalInstance::position` | **fixed here** — read-only |
| `height`, `width`, `length` (face size) | `Signal::height` etc. | **read-only here, editing is follow-up [#429](https://github.com/Robomous/RoadMaker/issues/429)** |
| `type`, `subtype`, `country` (designation) | `Signal::*` | **read-only, retype is follow-up [#429](https://github.com/Robomous/RoadMaker/issues/429)** — today a re-designation means delete-and-replace |

### Ground surface

| Parameter | In model | Disposition |
|---|---|---|
| `material` | `Surface::material` | exposed (Materials slot) |
| `nodes` (authored boundary) | `Surface::nodes` | tool (Surface tool); "Revert to derived" is in the pane |
| `source`, `bounding_roads`, area | — | read-only rows |

### Also fixed here

The pose spins display three decimals, so a value carrying more precision — a
gizmo ring drag's output — was **rounded when seeded**, and a focus-out then
committed that rounding as though the user had typed it. The no-op guard is now
each spin's own display quantum rather than a fixed `1e-9`
([#417](https://github.com/Robomous/RoadMaker/issues/417)'s second handoff item).
