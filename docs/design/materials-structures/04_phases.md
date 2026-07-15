# 04 — Phases

*The order the work lands in, and what each phase must prove before the next
starts.*

## Phase map

| Phase | Issue | Contents | Gate |
|---|---|---|---|
| **WS-1** material engine | [#196] | Kernel `<material>` promotion; renderer normal/roughness sampling + specular lobe; material param blocks; manifest v2 `materials[]`; per-material map sets | Before/after of one road: flat vs PBR-lite. `<material>` round-trips byte-identically **including unmodeled attributes**. A material with no maps renders exactly as today. GS-1 baseline unchanged more than trivially. |
| **WS-2** material library UX | [#197] | Library **Materials** category; drag-onto-surface; properties dropdown; variants; `assign_material` command; persistence per `01` §3 | Assign `rm:asphalt_worn` to a lane → save → reload → still worn. Undo restores. `apply → revert` leaves `write_xodr()` byte-identical. Centre lane / junction floor route through `<userData>`. |
| **WS-3** bridge generator | [#198] | Manifold spike; `<bridge>` kernel model; grade-separation query; deck/abutments/piers/guardrails; auto-offer UX; regeneration | GS-4's overpass generates from a two-road crossing; under-deck clearance visible; `.xodr` validates zero-error and loads in esmini; every degenerate case in `02` §5 has a passing fixture. |
| **WS-4** city props | [#199] | CC0 kit evaluation (or procedural fallback per `03` §1); Buildings category; streetlights | Props place from the Library and round-trip. Every asset has its `ASSETS_LICENSES.md` row. The style-consistency call is made and recorded with a comparison render. |
| **WS-5** GS-4 assembly | [#200] | Scene authoring (`build_gs4.py`), baseline, checklist walk | Same machinery as GS-1: **one** checklist ticked against a committed baseline with PR citations, gaps filed as issues, baseline in the golden-scenes table, esmini clean. Two asphalt variants + concrete structure visibly distinct from the fixed camera. |

[#196]: https://github.com/Robomous/RoadMaker/issues/196
[#197]: https://github.com/Robomous/RoadMaker/issues/197
[#198]: https://github.com/Robomous/RoadMaker/issues/198
[#199]: https://github.com/Robomous/RoadMaker/issues/199
[#200]: https://github.com/Robomous/RoadMaker/issues/200

## Dependencies

```
WS-1 ──┬── WS-2 ──┐
       │          ├── WS-5
       ├── WS-3 ──┤
       │          │
WS-4 ──────────────┘   (parallel-safe with everything)
```

- **WS-1 is the trunk.** Nothing renders a material until the engine does.
- **WS-2 after WS-1** — nothing to assign before the engine and schema exist.
- **WS-3 after WS-1**, parallel-safe with WS-2: the generator can emit solids
  with a hardcoded concrete material while WS-2 builds the picker.
- **WS-4 anytime** — asset work, no engine dependency. Good filler while WS-1 is
  in review.
- **WS-5 last** — it is the acceptance walk and depends on all of it.

An executor picks the lowest unblocked WS; every issue body carries its own deps
and gate.

## CHANGELOG

`[0.7.0] - Unreleased` opens with WS-1. Each WS adds its entries as it merges,
so the section is written continuously rather than reconstructed at close-out
(M3a's 0.5.0/0.6.0 sections were complete because they were written this way —
keep it).

## Acceptance machinery, reused

GS-1's close-out established the pattern; GS-4 inherits it rather than
reinventing it:

- **One checklist**, not two. GS-1 drifted precisely because an aspirational
  table lived beside an as-built one. GS-4's spec gets a single table, ticked
  against the committed baseline, each row citing its PR.
- **`[GAP]` in place, with a filed issue.** A milestone does not need 11/11 to
  ship; it needs every unticked row accounted for.
- **Baseline from CI**, committed under `golden_scenes/img/`, added to the
  README's baseline table with checklist % and notes.
- **Honest render notes.** If the baseline has visible weaknesses, they go in
  the spec at close-out, not in a reviewer's head.
- **esmini** picks GS-4 up automatically once it lands in `assets/samples/` —
  the job globs.

## What "done" means for this milestone

The maintainer can open RoadMaker, drag two rural roads into a crossing at
different elevations, accept the "Generate bridge structure?" offer, assign a
worn asphalt to one road and a new asphalt to the other from the Library, and
export a `.xodr` that validates clean and loads in esmini — and GS-4's baseline
shows all of it from the fixed camera.

If that sentence is true, v0.7.0 ships.
