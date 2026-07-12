# Product parity and IP rules

*The clean-room rules for building a RoadRunner-class editor without
copying RoadRunner — and the golden-scene process that turns "looks like a
real editor" into a checklist.*

RoadMaker aims to be comparable, as a product, to commercial road/scenario
editors such as MathWorks RoadRunner. Getting there must never create IP,
trademark, or trade-dress risk. These rules bind every contribution — code,
assets, docs, and roadmap specs alike.

## What we may follow

**Functional layout conventions are fair game.** A central 3D viewport, a
bottom asset library, a right-hand attributes panel, a mode switch between
map editing and scenario editing — these are standard DCC/editor patterns
(Blender, Unreal, QGIS, and many others share them). RoadMaker follows them
freely.

Behavior may be informed **only by publicly available documentation**, and
is always re-expressed in our own terms, against ASAM OpenDRIVE /
OpenSCENARIO concepts. That is the clean-room discipline: we describe *what
a road editor must do* in the standard's vocabulary, never *what RoadRunner
does* in its vocabulary.

## What we must never copy

- **Iconography, color themes, splash screens, branding** of MathWorks
  products.
- **Their 3D assets** — vehicles, trees, props, textures, materials — or
  derivatives of them.
- **Their sample scenes**, or scene specs written by imitating one.
- **Text from their UI** — labels, tooltips, error messages.
- **Screenshots of RoadRunner** anywhere in this repository, its docs, its
  golden-scene specs, or its marketing material.
- The word "RoadRunner" in feature names, file names, code identifiers, or
  UI strings. It appears only in factual comparative statements (as in the
  README), and the README's trademark disclaimer stays.

Our assets come exclusively from license-verified sources under the
[asset policy](assets.md); our scenes are specified from OpenDRIVE /
OpenSCENARIO concepts in the [golden-scene specs](../roadmap/golden_scenes/README.md).

## The decomposition rule: kernel first, then assets, then render

Parity work is decomposed in a fixed order, and lands in that order:

1. **Kernel / standards data model** — the feature exists as OpenDRIVE or
   OpenSCENARIO data (e.g. a crosswalk is an OpenDRIVE object, a lane arrow
   is a road-mark type) with parser, writer, and validation support.
2. **Assets** — any textures/models/icons it needs enter through the
   [asset pipeline](assets.md) with verified licenses.
3. **Render / editor UX** — the viewport and panels present it.

A visual feature that skips step 1 is cosmetic debt: it looks right and
exports wrong. Roadmap items and golden-scene checklists must always cite
the kernel feature they depend on, not just the visual outcome.

## The golden-scene process

A **golden scene** is a concrete target scene whose element-by-element
checklist is the acceptance criterion for a milestone's visual and
functional completeness. Each spec defines a fixed camera (position, target,
FOV) so renders are reproducible and comparable release over release.

Process (details and the current scene set:
[golden scenes](../roadmap/golden_scenes/README.md)):

- Every milestone with a golden scene attaches the current render of that
  scene, from the defined camera, to its release PR.
- A manually triggered CI workflow regenerates the golden screenshots so
  drift is visible instead of anecdotal.
- Scene specs are reviewed against this page: pure OpenDRIVE/OpenSCENARIO
  vocabulary, no references to any vendor's sample content.

## The golden-workflow process

A **golden workflow** is the path-based complement to golden scenes: a
scripted sequence of user actions with a time budget and a zero-crash
requirement, executed **by the maintainer by hand** at every milestone
gate. Specs and process:
[golden workflows](../roadmap/golden_workflows/README.md).

- **Every milestone from the hardening sprint (v0.4.0) on gates on its
  golden scene AND at least one golden workflow.** Scenes measure the
  result; workflows measure the path. Automated soak/regression replays
  are complementary evidence, never a substitute for the manual run.

## The workflow-walkthrough rule for specs

Element coverage is not enough. The v0.3.0 dogfooding lesson: tool specs
written only from the standard's vocabulary ("which OpenDRIVE elements
exist") made T-intersections impossible while every element checklist was
green — and the golden scene shared the blind spot.

Therefore every tool or milestone spec must include **workflow
walkthroughs**: what a user tries to draw in their first minutes, step by
step, in tool/UI vocabulary — alongside the element coverage. A spec
reviewer asks "can someone actually draw the second thing they'll try?"
before asking "are all the elements representable?".

## Visual acceptance: metrics are a floor, not a ceiling

The hardening sprint's tee lesson: every measured invariant (watertightness,
curvature, seam stitching) was green while the rendered junction was still
visibly wrong — a dark rectangular patch instead of a filleted intersection.
**Metric acceptance is a floor; visual work is accepted with pixels.**

- Geometry, material, normal, and renderer changes ship with
  editor-rendered before/after screenshots in the PR
  ([pull requests — visual output](../contributing/pull-requests.md)); the
  maintainer approves appearance before merge.
- The editor has a headless screenshot mode
  (`roadmaker-editor --screenshot scene.xodr out.png`, wrapped by
  `scripts/editor_screenshot.py`); CI renders the canonical scenes — a
  4-arm crossing, the tee, an overpass — and uploads the PNGs as workflow
  artifacts on every run for human review.
- Golden-scene pixel comparison (GS-1) will build on the same capture
  path; until then the artifacts are for eyes, not diffs.

## If in doubt

Treat anything that would make a reasonable observer say "that came from
RoadRunner" as forbidden, and ask the maintainer before proceeding —
the same stop-and-ask rule as the
[dependency policy](dependencies.md).
