# Golden scenes

*What golden scenes are, and the screenshot process that keeps milestone
progress measurable and visible.*

A **golden scene** is a concrete target scene that serves as the visual and
functional acceptance criterion of a milestone. Instead of "the viewport
should look like a real editor", each milestone commits to rendering and
round-tripping one specific scene, element by element. The idea and the
governing IP rules live in the
[product-parity standard](../../standards/product-parity.md); this folder
holds the specs.

## The scene set

| Scene | Milestone | One-liner |
|---|---|---|
| [GS-1 "Urban intersection"](gs1_urban_intersection.md) | M3a | 4-arm signalized junction with crosswalks, arrows, stop lines, signs, vegetation, textured surfaces |
| [GS-2 "Imported district"](gs2_imported_district.md) | M3b | A ~500 m × 500 m real-world OSM extract imported, meshed, and textured |
| [GS-4 "Rural overpass"](gs4_rural_overpass.md) | Materials & Structures | Two rural roads crossing via a built bridge (deck, abutments, guardrails), two asphalt materials + concrete structure |
| [GS-3 "Ambulance run"](gs3_ambulance_run.md) | M4 (extended in M5) | GS-1 plus an emergency-vehicle actor on a lane-anchored route with offset |

## Anatomy of a spec

Every spec contains:

1. **Scene definition** — pure OpenDRIVE/OpenSCENARIO vocabulary. Never a
   reference to, or imitation of, any vendor's sample content.
2. **Element checklist** — one row per scene element, ticked against the
   committed baseline and citing the PR that delivered it. **One table, not
   two**: a row that did not land is marked `[GAP]` in place, with a reason and
   a filed follow-up issue. A milestone does not need 14/14 to ship, but it
   does need every unticked row to be accounted for — an aspirational checklist
   kept separately from an as-built one always drifts (GS-1's did).
3. **Fixed camera** — position, target, and vertical FOV in the kernel
   frame (right-handed, Z-up, meters), so screenshots are reproducible and
   comparable across releases.
4. **Acceptance criteria beyond the image** — round-trip/validation
   requirements on the underlying data.

## The golden-screenshot process

- **Release PRs carry the render.** The release PR of a milestone with a
  golden scene attaches the current render of that scene from the spec's
  fixed camera. Reviewers compare against the previous release's baseline
  side by side.
- **Scene sources are versioned.** Each scene's `.xodr` (and `.xosc` from
  GS-3 on) lives under `assets/samples/` once its milestone starts; the spec
  is authoritative until then.
- **CI renders every push.** The `visual-artifacts` job in `ci.yml` loads each
  golden scene headless under `xvfb-run`, renders it from the fixed camera,
  and uploads the images as the `editor-screenshots` artifact — so drift is a
  diff, not a memory. It is **skip-not-fail**: a runner with no GL context
  exits 3 and the job absorbs it rather than blocking the merge.
- **Comparison is by eye, not by pixel.** There is deliberately no automated
  pixel-diff gate: the artifact is for human review against the committed
  baseline. A render that changes for a legitimate reason (an asset upgrade, a
  scene edit) refreshes the baseline in the same PR, with a note saying why.
- **Baselines are committed from CI, never from a dev machine.** macOS has no
  offscreen GL context, so the baseline PNG is downloaded from the branch's own
  `editor-screenshots` artifact (`gh run download <run> -n editor-screenshots`)
  and committed under `img/`.
- **Checklists are tracked in the milestone.** Each element row maps to the
  GitHub issue(s) implementing its kernel feature/asset; the scene spec is
  updated with checkmarks as rows land.
- **esmini round-trip is part of acceptance (from M3a on).** Every golden
  scene's exported `.xodr` must load in esmini without errors; a headless
  smoke job in CI enforces this (the roadmap's
  [simulator round-trip gate](../roadmap.md#cross-cutting-quality-gates)).
  CARLA ingestion validation remains a manual release-checklist item until
  it is CI-feasible.

## Baselines

The committed render of each scene, release over release. A new row lands with
the milestone that changes the scene; the previous row's image stays in `img/`
so any two releases can be put side by side.

### GS-1 "Urban intersection"

| Release | Baseline | Checklist | Notes |
|---|---|---|---|
| v0.6.0 | [`img/gs1_baseline_v0.6.0.png`](img/gs1_baseline_v0.6.0.png) | **12 / 14** (86%) | First baseline. M3a close-out. Gaps: centre double-yellow ([#193]), dashed lane lines ([#194], needs a multi-lane profile). Trees raised 16 → 24. Junction floor untextured and sky dark — both are v0.7.0 material work; see the spec's [honest notes](gs1_urban_intersection.md#honest-notes-on-the-v060-baseline). |

### GS-2 / GS-3 / GS-4

No baselines yet — their milestones have not started. GS-4 lands its first with
Materials & Structures (v0.7.0).

[#193]: https://github.com/Robomous/RoadMaker/issues/193
[#194]: https://github.com/Robomous/RoadMaker/issues/194

## Rules for writing or changing a spec

- Specs follow the [product-parity rules](../../standards/product-parity.md):
  OpenDRIVE/OpenSCENARIO concepts only, no vendor screenshots, no imitation
  of sample scenes.
- Changing a camera definition after a milestone has shipped renders breaks
  comparability — it needs a maintainer sign-off and a note in the spec's
  history section.
- New scenes get a `gsN_short_slug.md` file and a row in the table above.
