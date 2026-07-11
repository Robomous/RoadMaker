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
| [GS-3 "Ambulance run"](gs3_ambulance_run.md) | M4 (extended in M5) | GS-1 plus an emergency-vehicle actor on a lane-anchored route with offset |

## Anatomy of a spec

Every spec contains:

1. **Scene definition** — pure OpenDRIVE/OpenSCENARIO vocabulary. Never a
   reference to, or imitation of, any vendor's sample content.
2. **Element checklist** — one row per scene element, each citing the
   kernel feature and the asset it depends on. A milestone is visually done
   when every row is checked against a current render.
3. **Fixed camera** — position, target, and vertical FOV in the kernel
   frame (right-handed, Z-up, meters), so screenshots are reproducible and
   comparable across releases.
4. **Acceptance criteria beyond the image** — round-trip/validation
   requirements on the underlying data.

## The golden-screenshot process

- **Release PRs carry the render.** The release PR of a milestone with a
  golden scene attaches the current render of that scene from the spec's
  fixed camera. Reviewers compare against the previous release's render
  side by side.
- **Scene sources are versioned.** Each scene's `.xodr` (and `.xosc` from
  GS-3 on) lives under `assets/golden_scenes/` once its milestone starts;
  the spec is authoritative until then.
- **CI regenerates on demand.** A manually triggered workflow
  (`workflow_dispatch`) loads each golden scene headless, renders it from
  the fixed camera, and uploads the images as artifacts — so drift is a
  diff, not a memory. The workflow is seeded when M3a lands GS-1; earlier
  milestones have nothing to render.
- **Checklists are tracked in the milestone.** Each element row maps to the
  GitHub issue(s) implementing its kernel feature/asset; the scene spec is
  updated with checkmarks as rows land.
- **esmini round-trip is part of acceptance (from M3a on).** Every golden
  scene's exported `.xodr` must load in esmini without errors; a headless
  smoke job in CI enforces this (the roadmap's
  [simulator round-trip gate](../roadmap.md#cross-cutting-quality-gates)).
  CARLA ingestion validation remains a manual release-checklist item until
  it is CI-feasible.

## Rules for writing or changing a spec

- Specs follow the [product-parity rules](../../standards/product-parity.md):
  OpenDRIVE/OpenSCENARIO concepts only, no vendor screenshots, no imitation
  of sample scenes.
- Changing a camera definition after a milestone has shipped renders breaks
  comparability — it needs a maintainer sign-off and a note in the spec's
  history section.
- New scenes get a `gsN_short_slug.md` file and a row in the table above.
