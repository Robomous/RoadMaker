# Continuous integration

*What runs on every PR and push to `main`, what each job gates, and what happens on a release tag.*

All jobs must be green before a PR merges — CI is the objective gate (see
[Pull requests](pull-requests.md)). Workflows live in `.github/workflows/`.

## `ci.yml` — every PR and push to `main`

### Orchestration

Three rules decide *whether* and *when* each job below runs.

**Superseded PR runs are cancelled.** A `concurrency` group keyed on
`github.ref` cancels an in-flight run when a new commit lands on the same PR.
Pushes to `main` are deliberately **never** cancelled — `sanitize` carries the
full 9-minute soak there, and every commit on `main` must stay individually
verified.

**`change detection` classifies the diff.** A PR whose every changed path is
under `docs/` or is a root-level `.md` file is *docs-only*. Docs-only PRs skip
the macOS/Windows matrix and the ancillary jobs, but still run **`build+test
ubuntu-latest`**, `clang-format`, and `docs link check`.

That Linux build is not a formality: seven test files read `docs/` directly and
are real doc↔code divergence gates — `test_defaults_registry.cpp` against
[realism defaults](../domain/realism_defaults.md), plus the shortcut, help,
library, lane-profile, units, and T-junction-quality tests. A Markdown-only
edit genuinely can turn the build red, so it is always compiled and tested on
one platform.

This is a change-detection **job**, not `paths-ignore`. `paths-ignore` leaves
skipped checks *pending* forever, which blocks a PR the moment a check is
marked required; a job whose `if:` evaluates false reports `skipped`, which
counts as a conclusion.

**One caveat if you enable branch protection.** A skipped **matrix** job never
expands its matrix, so on a docs-only PR the per-OS names do not appear at
all — GitHub reports the single unexpanded job (`build+test ${{ matrix.os }}`)
rather than `build+test macos-latest` and `build+test windows-latest`. A
missing check is not a skipped check: requiring those two names would block
every docs-only PR forever. Draw the required set from the jobs that run
unconditionally on every PR:

| Always runs | Safe to require |
|---|---|
| `change detection` | yes |
| `build+test ubuntu-latest` | yes — a plain job, not a matrix leg, precisely so it can be required |
| `clang-format` | yes |
| `docs link check` | yes |
| everything else | **no** — skipped on docs-only PRs, and the matrix legs vanish entirely |

`main` is not branch-protected today (`GET /repos/Robomous/RoadMaker/branches/main/protection`
returns 404), so nothing is formally required yet.

**The matrix is staged.** On PRs *and* on `main`, `build+test ubuntu-latest`
runs first and every other build job `needs:` it. Linux is the cheapest runner
(×1 multiplier, ~1.5 min warm) and catches nearly every compile and test
failure, so a branch that does not build never reaches the Windows (×2) and
macOS (×10) jobs. The cost is that `main` serializes ~1.5 min behind Linux
instead of starting everything at once; the Windows jobs dominate wall clock
either way.

### `build+test ubuntu-latest` and `build-test` (macOS, Windows)

The core gate, split into the Linux gate job and the macOS/Windows matrix that
depends on it. Check names are identical to the three the single matrix
produced before, so nothing downstream needs to know about the split.

For each OS it provisions Qt via `scripts/setup_qt.py`, then runs the matching
`ci-*` preset end to end:

```sh
cmake --preset ci-<os>
cmake --build --preset ci-<os>
ctest --preset ci-<os>
```

The `ci-*` presets build `Release` with warnings-as-errors (`RM_WERROR=ON`)
and the editor + tests enabled. On Windows the provisioned Qt `bin/` is put
on `PATH` first, because `gtest_discover_tests` runs the editor test
executable during the build and it must load the Qt DLLs.

### `shared-core` (matrix: macOS, Linux, Windows)

Builds the kernel as a shared library (`RM_BUILD_SHARED=ON`, editor off) and
runs the tests against it — with hidden symbol visibility, a missed export
macro becomes a link error here. It then installs the package and builds
`tests/consume_installed/` against the installed prefix, proving
`find_package(roadmaker)` works outside the build tree.

### `sanitize` (Linux, Clang)

ASan + UBSan build (`-DRM_SANITIZE=address,undefined`) including the editor
(ASan surfaces signal/slot lifetime bugs), with `detect_leaks=1` and
`halt_on_error=1`. Sanitizers run on Linux/Clang because they are unreliable
on macOS runners and unsupported on MSVC. See
[Testing](testing.md#sanitizers) for the local equivalent.

The job runs the full instrumented suite with `ctest -j 4`, then the seeded
random-op soak (`roadmaker_soak`) — 3 minutes on PRs, the full 9 on every
push to `main` (seeds derive from the run id, so PR pushes accumulate varied
coverage and every merge re-soaks at full length). One documented selection
exception: `SoakSmoke.FixedSeedRunsClean` is skipped in this job only,
because the soak step exercises the same driver under the same sanitizers
for minutes right after; it still runs in the three `build-test` jobs and
locally. Rationale, measurements, and the full optimization decision record:
[test-suite audit](../testing/audit-2026-07.md). The manual 24-hour soak
release gate is separate and unaffected.

### `format`

Runs `clang-format --dry-run --Werror` over every tracked `.cpp`/`.hpp` —
run `git clang-format` before committing (see [workflow](workflow.md)). The
same job runs `scripts/check_asset_licenses.py`, gating the
[asset license policy](../standards/assets.md).

### `python`

Builds the wheel (`pip wheel python/`), installs it, and runs
`pytest python/tests`. It then executes two of the runnable examples
(`load_and_export.py` on `assets/samples/t_junction.xodr`, and
`author_road.py`) and uploads the resulting `.glb`/`.xodr` files as
artifacts for visual inspection.

### `fuzz-smoke`

Builds the libFuzzer target `roadmaker_fuzz_xodr` (Clang) and runs it for
30 seconds over `core/tests/fuzz/corpus/`. This is a smoke run, not a deep
campaign — its job is to catch corpus regressions and obvious parser crashes
quickly. Extending the corpus is part of every parser change
([Testing](testing.md#what-to-test)).

### `esmini-roundtrip`

The simulator round-trip quality gate (a permanent cross-cutting gate;
introduced by the
[pre-reset roadmap](../roadmap/archive/2026-07-pre-reset/roadmap.md#cross-cutting-quality-gates)):
every tracked `.xodr` must **load headless in esmini without errors**. The job fetches a pinned esmini release binary (cached; MPL-2.0,
used strictly as an external smoke tool — never linked, never redistributed)
and runs `scripts/esmini_smoke.py`, which wraps each `.xodr` in a minimal
OpenSCENARIO scenario and fails on any parse/load error. A second step runs
the deliberately-broken fixture `tests/esmini/broken.xodr` with
`--expect-fail`, guarding the gate itself: if esmini ever accepts it, the
job fails.

Since [p8-s1](https://github.com/Robomous/RoadMaker/issues/245) two further
steps feed esmini a **real, tracked `.xosc`** — `tests/esmini/signalized.xosc`,
generated by `scripts/gen_xosc_fixtures.py` — plus its own `--expect-fail`
counter-example, `tests/esmini/broken.xosc`. Everything else in the job smokes a
road network through a wrapper the *script* synthesizes; these are the only
steps that test a scenario the product itself writes.

**What the esmini gate does not catch.** Measured against v3.5.0 on 2026-07-30:
it rejects a truncated document, a duplicated element, an unresolvable
`<LogicFile>` and a dangling `entityRef`, but it accepts a dangling
`trafficSignalId`, a garbage `@state`, a dangling `trafficSignalControllerRef`
and a nonexistent phase name **in silence**, with a byte-identical log. For the
traffic-signal half, the checker-rule UIDs in
`core/include/roadmaker/osc/rules.hpp` are therefore not additive to esmini —
they are the only check there is.

Bump the pin by editing `ESMINI_VERSION` in the job; the cache key derives from
it ([#506](https://github.com/Robomous/RoadMaker/issues/506) — it used to
repeat the literal, so a bump hit the stale cache and the job silently kept
running the old binary).

### `docs`

Markdown link check (lychee) over `docs/**` and the root `.md` files —
broken relative links in this documentation tree fail the PR.

## Caching

| Cache | Path | Key |
|---|---|---|
| Qt | `./.qt/` (minus `venv`) | `qt-<os>-hash(cmake/QtVersion.cmake, scripts/setup_qt.py)` |
| Compiler | ccache/sccache dir | `<os>-<job>`, managed by `hendrikmuhs/ccache-action` |
| FetchContent downloads | `…/_deps/*-subbuild/*-populate-prefix/src/*.tar.gz` | `deps-src-<job>-<os>-hash(cmake/deps.cmake)` |
| esmini binary | `.esmini/` | `esmini-<os>-${{ env.ESMINI_VERSION }}` |

- **Qt** — the key hashes `cmake/QtVersion.cmake` **and**
  `scripts/setup_qt.py`, so bumping the Qt pin or changing the provisioning
  script invalidates it automatically; on a hit `setup_qt.py` is a no-op.
- **Compiler** — ccache on Linux and macOS, **sccache on Windows/MSVC**, each
  bounded at 500M. Ten caches at that ceiling stay well inside the 10 GB
  per-repo quota, and `ccache-action` maintains its own restore-key fallback
  chain so a partial hit still helps.

  The launcher is passed **from the workflow**
  (`-DCMAKE_C/CXX_COMPILER_LAUNCHER=…`), never hardcoded in `CMakeLists.txt`.
  This matters: `CMakeLists.txt` previously auto-detected a program literally
  named `ccache`, which Windows never has — the workflow installs `sccache` —
  so MSVC silently ran uncached for the entire life of the workflow. If you add
  a build job, pass the launcher explicitly and match it to the `variant:` the
  cache step installed. `CMakeLists.txt` still auto-detects ccache when the
  caller specifies nothing, so local developer builds are unaffected.

  A caveat for MSVC: sccache cannot cache compilations that use `/Zi`
  (separate PDB). The `ci-*` presets build `Release`, which emits no debug
  info, so this does not bite today. A CI preset that wants MSVC debug info
  must use `/Z7` (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`) or it will
  quietly stop caching.
- **FetchContent** — the downloaded dependency **tarballs**, keyed on the hash
  of `cmake/deps.cmake` (where every dependency is pinned — see
  [dependency policy](../standards/dependencies.md)). Jobs use different
  `binaryDir`s (`build/` vs `build/<preset>/`), so each keys its own entry
  rather than racing another job for one.

  Compiled dependency objects are deliberately **not** cached as a separate
  `_deps` archive. The compiler cache already covers them: once sccache was
  wired up, a warm `build+test windows-latest` — checkout, Qt provisioning,
  configure, full dependency and first-party build, and the whole test suite —
  came down to 3.3 minutes from 19.2. A second cache over the same objects
  would buy little and would introduce a stale-artifact failure mode, since a
  restored `_deps` tree carries generated build files that must agree with the
  current CMake and compiler. If dependency build time ever does become
  visible again, measure first: the compiler cache is the cheaper lever.

### Forcing a clean build

Caches are keyed on content, so the normal way to invalidate one is to change
the file it hashes. To force a full cold build without touching pins:

- **All caches** — delete them: `gh cache list` then
  `gh cache delete <id>` (or `gh cache delete --all`). The next run repopulates
  from scratch.
- **Compiler cache only** — bump the `key:` on the relevant
  `hendrikmuhs/ccache-action` step, or delete just those entries.
- **Locally** — `ccache --clear`, or configure with
  `-DCMAKE_CXX_COMPILER_LAUNCHER=` (empty) to bypass the cache entirely.

If you ever suspect the compiler cache of producing a wrong result, clear it
and re-run before investigating anything else — a cold run and a warm run must
produce identical test outcomes.

## `release.yml` — on `v*` tags

Pushing a tag like `v0.2.0` builds and publishes self-contained artifacts:

- **`package`** (matrix: macOS, Linux, Windows) — builds the `release-*`
  presets (`Release`, shared kernel), runs the full test suite, then packages
  via CPack: a **DMG** on macOS, an **NSIS installer + portable ZIP** on
  Windows, and a **TGZ** plus an **AppImage** (via linuxdeploy and its Qt
  plugin) on Linux. Qt is bundled — users install nothing else.
- **Deployed-artifact smoke test** — each job then mounts/extracts its own
  installer and runs the *deployed* binary with `--version` (offscreen), so
  a broken bundle fails the release, not the user.
- **`wheels`** — builds the Python wheel, installs it, runs
  `pytest python/tests`, uploads the wheel.
- **`publish`** — collects all artifacts into a **draft GitHub release** with
  generated notes; a maintainer reviews and publishes it.

## Reproducing CI locally

Anything CI does, you can run locally with the same presets — see
[Building](../getting-started/building.md). The fastest pre-PR check is:

```sh
cmake --preset ci-<os> && cmake --build --preset ci-<os> && ctest --preset ci-<os>
git clang-format
```
