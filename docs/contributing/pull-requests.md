# Pull requests

*What a mergeable PR looks like: the checklist, what reviewers expect, and how milestones and issues are organized.*

## Before you open it

Work through the [contributing workflow](workflow.md) first — branch naming,
conventional commits, and the size guideline (≤ ~500 changed lines where
feasible). Draft PRs are welcome early; open one as soon as you want CI
feedback or design discussion.

## Checklist

Every PR:

- [ ] Builds and tests pass on **all three OSes** — the [CI matrix](ci.md)
      (macOS, Linux, Windows) is the gate; no "fix Windows later" merges.
- [ ] C++ is formatted (`git clang-format`; the `format` CI job enforces it).
- [ ] Tests are included **with** the change, following the
      [testing doctrine](testing.md).

When applicable:

- [ ] Touched the OpenDRIVE parser/writer or added an xodr feature → fuzz
      corpus in `core/tests/fuzz/corpus/` extended (see [Testing](testing.md)).
- [ ] Changed the public kernel API → `python/src/bindings.cpp` updated and at
      least one example in `python/examples/` added or updated, same PR.
- [ ] Added/updated a dependency → follow the
      [dependency policy](../standards/dependencies.md), including its
      `THIRD_PARTY_LICENSES.md` row in the same commit.
- [ ] Added/updated a bundled asset → follow the
      [asset policy](../standards/assets.md), including its
      `ASSETS_LICENSES.md` row (CI runs the license check).
- [ ] Touched geometry or parsing → ran the
      [sanitizer build](../getting-started/building.md#sanitizer-build)
      locally.
- [ ] Added a Qt item model → its `QAbstractItemModelTester` test ships in
      the same commit (see [Testing](testing.md)).

## UI and interactive work: show it

PRs that change the editor UI or add interactive behavior (tools, panels,
viewport interactions) must include **screenshots or GIFs in the PR
description**. Reviewers should be able to judge the result without building
the branch. Before/after pairs are ideal for visual changes.

## Review process

- A Robomous maintainer reviews every PR; maintainer approval plus green CI
  are the merge requirements.
- CI is the objective gate — reviewers will not merge around a red job, and
  you should not ask them to.
- Review feedback focuses on correctness (geometric and
  [standards](../domain/opendrive.md) correctness above all), the
  [architecture boundaries](../architecture/overview.md), and test quality.
- Force-pushing to address review is fine; the merge to `main` is what
  matters.

## Milestones, issues, and labels

- Work is tracked in **GitHub issues**, grouped by **GitHub milestones**
  (M1: viewer + kernel — done; M2: interactive editing, junction blending,
  USD export — current; M3: GIS/lidar import, OpenSCENARIO).
- Milestone work carries a matching label (e.g. `m2`) and follows the phase
  plan in the design docs — one epic issue per phase, with PRs referencing
  their issue (`Closes #N`). See the
  [M2 overview](../design/m2/00_overview.md).
- Found a bug or want a feature that isn't tracked? Open an issue first for
  anything non-trivial — it is the cheapest place to agree on an approach.

## After the merge

Squash or merge commits keep the conventional-commit style so the history
stays readable and releasable. Release packaging happens automatically on
`v*` tags — see [CI](ci.md).
