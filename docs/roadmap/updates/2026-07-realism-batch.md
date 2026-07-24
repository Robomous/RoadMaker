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

*(committed by #418's PR — placeholder until then; the audit may spawn
follow-ups, which are added to #411's table and become release-blocking.)*
