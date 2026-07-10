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
are organized as one epic per milestone phase — see
[Pull requests](pull-requests.md) for how milestones and labels are used.

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
