# Product parity and IP rules

*The clean-room rules for building a commercial-grade road/scenario editor
without copying anyone — and the naming rule that keeps vendor products out
of this repository entirely.*

RoadMaker aims to be comparable, as a product, to the strongest commercial
road/scenario editors. Getting there must never create IP, trademark, or
trade-dress risk. These rules bind every contribution — code, assets, docs,
and roadmap specs alike.

## The naming rule

**No vendor's product name appears anywhere in this repository** — not in
docs, code, code comments, issues, PR descriptions, commit messages, or
release notes. Commercial editors may inform planning **as internal
inspiration only**; behavioral notes and comparisons live exclusively in
the maintainer's local, untracked notes. Public specs describe *what a road
editor must do* in ASAM OpenDRIVE / OpenSCENARIO vocabulary and in
RoadMaker's own tool names — never *what product X does* in that product's
vocabulary.

## What we may follow

**Functional layout conventions are fair game.** A central 3D viewport, a
bottom asset library, a right-hand attributes panel, a mode switch between
map editing and scenario editing — these are standard DCC/editor patterns
(Blender, Unreal, QGIS, and many others share them). RoadMaker follows them
freely.

Behavior may be informed **only by publicly available documentation**, and
is always re-expressed in our own terms, against ASAM OpenDRIVE /
OpenSCENARIO concepts. That is the clean-room discipline.

## What we must never copy

- **Iconography, color themes, splash screens, branding** of any
  commercial editor.
- **Their 3D assets** — vehicles, trees, props, textures, materials — or
  derivatives of them.
- **Their sample scenes**, or scene specs written by imitating one.
- **Text from their UI** — labels, tooltips, error messages.
- **Screenshots of any commercial editor** anywhere in this repository,
  its docs, its workflow specs, or its marketing material.
- **Vendor product names** — see the naming rule above; there are no
  exceptions, comparative or otherwise.

Our assets come exclusively from license-verified sources under the
[asset policy](assets.md); our acceptance scripts are specified from
OpenDRIVE / OpenSCENARIO concepts in the
[golden workflows](../roadmap/golden_workflows/README.md).

## Visual experience is a first-class product goal

*Maintainer decision, 2026-07-12 (supersedes the earlier "sober UI /
default theme only" stance): users judge in the first 30 seconds, with
their eyes.*

**RoadMaker's editor must look like a modern professional DCC tool. Visual
quality is a first-class acceptance criterion, gated by maintainer
screenshot approval.** The concrete design system — palette tokens, spacing,
icon sizes, typography — is the [UI design standard](ui-design.md).

Three binding rules follow:

- **Visual-quality rule.** "Works but looks like a dated generic Qt app" is
  a defect, not a shipped feature. Correctness of the kernel remains the
  moat; the editor's look and feel carries equal priority.
- **Iteration rule.** UI work lands in small slices where **every PR ships
  a visible improvement**, evidenced by screenshots or a GIF in the PR
  description (the screenshot mode and CI visual artifacts make this
  cheap). Multi-week invisible refactors of editor chrome are not accepted.
- **Discoverability rule.** No feature may exist only behind an
  undocumented shortcut or a status-bar message. Every capability needs at
  least one visible, labeled entry point — toolbar button, library item,
  context menu, or panel control — plus a tooltip. Shortcuts remain as
  accelerators, never as the only path.

The IP guardrail above is unchanged by this: commercial products inform
*functional* choices (labeled toolbar, library browser, attributes panel,
drag-to-place); their icons, colors, and artwork stay copied-from-nothing.

## The decomposition rule: kernel first, then assets, then render

Parity work is decomposed in a fixed order, and lands in that order:

1. **Kernel / standards data model** — the feature exists as OpenDRIVE or
   OpenSCENARIO data (e.g. a crosswalk is an OpenDRIVE object, a lane arrow
   is a road-mark type) with parser, writer, and validation support.
2. **Assets** — any textures/models/icons it needs enter through the
   [asset pipeline](assets.md) with verified licenses.
3. **Render / editor UX** — the viewport and panels present it.

A visual feature that skips step 1 is cosmetic debt: it looks right and
exports wrong. Roadmap items and golden-workflow scripts must always cite
the kernel feature they depend on, not just the visual outcome.

## The golden-workflow process

A **golden workflow** is a step-by-step script a human executes in
RoadMaker — numbered actions, each with an explicit expected result — with
a zero-crash requirement. The set GW-1 … GW-5
([specs and process](../roadmap/golden_workflows/README.md)) is the **only
acceptance mechanism** of the
[Road to Parity roadmap](../roadmap/README.md): the release gate requires
every workflow to pass, executed by the maintainer by hand, on macOS,
Linux, and Windows. Automated soak/regression replays are complementary
evidence, never a substitute for the manual run.

The pre-reset golden scenes and workflows are preserved in the
[2026-07 archive](../roadmap/archive/2026-07-pre-reset/README.md); if
visual scene benchmarks are wanted again later, they will be re-derived
from the golden workflows.

## The workflow-walkthrough rule for specs

Element coverage is not enough. The v0.3.0-era dogfooding lesson: tool
specs written only from the standard's vocabulary ("which OpenDRIVE
elements exist") made T-intersections impossible while every element
checklist was green.

Therefore every tool or sprint spec must include **workflow
walkthroughs**: what a user tries to draw in their first minutes, step by
step, in tool/UI vocabulary — alongside the element coverage. A spec
reviewer asks "can someone actually draw the second thing they'll try?"
before asking "are all the elements representable?".

## Visual acceptance: metrics are a floor, not a ceiling

The hardening-sprint tee lesson: every measured invariant (watertightness,
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
- Golden-workflow visual checks build on the same capture path; the
  artifacts are for eyes, not diffs.

## If in doubt

Treat anything that would make a reasonable observer say "that came from a
specific commercial product" as forbidden, and ask the maintainer before
proceeding — the same stop-and-ask rule as the
[dependency policy](dependencies.md).
