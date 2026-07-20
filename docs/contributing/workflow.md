# Contributing workflow

*How day-to-day development works: branches, commit conventions, formatting, and the rules that ride along with every change.*

## Branches

Branch names use a `type/short-description` scheme matching the commit types
below, e.g. `feat/qt-editor`, `feat/m2-command-layer`, `fix/appimage-icon`,
`docs/restructure`. Work happens on a branch and lands on `main` via a pull
request — see [Pull requests](pull-requests.md).

## Commits

We use [Conventional Commits](https://www.conventionalcommits.org/):
`type(scope): summary`, with `!` after the scope for breaking changes.

Types in use: `feat`, `fix`, `refactor`, `test`, `docs`, `build`, `chore`,
`style`. Scopes match the area you touched — the ones the history actually
uses:

| Scope | Area |
|---|---|
| `core` | Kernel (geometry, road model, meshing, command layer) |
| `xodr` | OpenDRIVE reader/writer/diagnostics |
| `mesh`, `geometry` | Meshing and geometry specifically |
| `editor` | Qt editor |
| `python` | Bindings and Python package |
| `build`, `ci`, `release` | CMake, CI workflows, packaging |
| `assets`, `m2` | Asset pipeline, M2 design docs |

Examples from the log:

```
feat(core): edit-operation factories for every M2 command
fix(xodr): ...
refactor(editor): resolve diagnostics via kernel entity ids
test(geometry): ...
feat(editor)!: migrate editor from Dear ImGui/GLFW to Qt 6 Widgets
docs: update README for the Qt editor
```

## Keep changes small

PRs should stay at or below ~500 changed lines where feasible. Split
mechanical churn (renames, formatting) from behavior changes. Larger efforts
are organized as one epic per roadmap pillar — see
[Pull requests](pull-requests.md) for how pillars, milestones, and labels
are used.

## Format before committing

CI rejects unformatted C++ ([`format` job](ci.md)). Format only your staged
changes with:

```sh
git clang-format
```

## Rules that ride along with every change

These are not optional extras; reviewers and CI check for them:

- **Tests ship with the code, not after.** The full doctrine — frameworks,
  conventions, what to test — lives in [Testing](testing.md).
- **New OpenDRIVE features extend the fuzz corpus** in
  `core/tests/fuzz/corpus/` in the same PR (details in
  [Testing](testing.md)).
- **Public kernel API changes** must update `python/src/bindings.cpp` and at
  least one example in `python/examples/` in the same PR — the Python surface
  never lags the C++ surface.
- **Cross-platform is non-negotiable.** macOS, Linux, and Windows are all
  first-class: every PR must pass all three OS [CI jobs](ci.md). No "fix
  Windows later" merges. Portability rules live in
  [Cross-platform](../standards/cross-platform.md).
- **New dependency or asset?** Follow the
  [dependency policy](../standards/dependencies.md) and the
  [asset policy](../standards/assets.md) — both require license bookkeeping
  in the same commit.
- **Architecture boundaries** (what may depend on what, where GL and Qt are
  allowed) are defined in the
  [architecture overview](../architecture/overview.md); PRs that cross them
  are rejected regardless of how green CI is.
- **A new tool or panel ships its user-guide page.** Every editor tool and
  dockable panel must have a page under `docs/user-guide/`, linked from
  `index.md`, and mapped in `editor/src/help/help_registry.cpp` so F1 opens it.
  This is enforced: the `HelpRegistry.EveryPageResolvesToACommittedGuidePage`
  gate (`editor/tests/test_help_registry.cpp`) fails the build when a mapped
  slug has no committed, index-linked page, and the `roadmaker_help` build
  compiles every page. Add the row and the page in the same PR.

## Writing a tutorial

Tool pages answer "what does this tool do"; tutorials string several tools into
one end-to-end task ("draw a road, tee in a second, export"). They live under
`docs/user-guide/tutorials/` and the help pipeline auto-nests them under a
synthetic **Tutorials** node — you only need to add a row to the Tutorials table
in `index.md`.

- **User voice, task order.** Write to the reader ("pick the Create Road tool"),
  in the order they act. No checkboxes — numbered steps.
- **Follow the page skeleton.** An H1, a one-line italic summary, then short
  `##` sections. The H1 is required (the TOC and the keyword gate read it).
- **Link, don't duplicate.** Point at the tool pages for the details rather than
  restating them; a tutorial is the glue between tools.
- **Every image must exist.** Capture screenshots with
  `scripts/editor_screenshot.py` into `docs/user-guide/tutorials/img/` and
  reference them relatively (`img/name.png`). The docs link-check gates broken
  image and page links. Do not add `ASSETS_LICENSES.md` rows for doc images —
  the asset checker scans only `assets/` and `editor/resources/`.

## Agent PR discipline

Automated contributors (Claude Code and other agents) follow the same rules as
everyone else, plus a tighter loop that keeps a multi-PR effort from drifting
into rebase-cascade conflicts or leaving green work unmerged:

- **Main-first branching.** Before creating *any* new branch, sync:
  `git checkout main && git pull --ff-only`, then branch from that tip. **No
  stacked branches off unmerged work** — the previous PR must be in `main`
  before the next branch starts. This is the single biggest source of avoidable
  conflicts, so it is not optional.
- **Verify before merge.** After opening a PR, wait for CI:
  `gh pr checks <n> --watch` until every required check passes. Never merge red,
  never bypass a failing gate — fix it on the same branch.
- **Merge when green, don't let it sit.** Once checks pass, merge with
  `gh pr merge <n> --squash --delete-branch`; don't start new work on top of a
  green-but-unmerged PR.
- **Visual changes gate on the maintainer.** Any PR that changes what the editor
  looks like ships before/after screenshots (a GIF for interactions) and waits
  for **maintainer look-approval before merging** — the visual-quality bar in
  [product parity](../standards/product-parity.md) and
  [UI design](../standards/ui-design.md). Non-visual PRs (kernel, headless
  models, docs, tests) merge on green without that gate.
- **Keep the three sources of truth in sync — in the same session as the merge.**
  After a PR merges, the docs, the issues, and the roadmap must agree about what
  is done: tick the relevant epic checklist item, close the feature issue with a
  comment linking the PR, add the issue to the project board with its Status, and
  update any affected design doc, `roadmap.md` status note, or seed. Anyone
  reading any one of the three gets the same truth.

## Typical loop

```sh
git switch -c feat/my-change
# hack, add tests alongside the code
cmake --build --preset dev-macos && ctest --preset dev-macos
git clang-format
git commit   # conventional message
git push -u origin feat/my-change
# open a PR — see pull-requests.md for the checklist
```

Touching geometry or parsing? Also run the
[sanitizer build](../getting-started/building.md#sanitizer-build) before
opening the PR.
