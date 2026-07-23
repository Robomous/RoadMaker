# Continuous integration

*What runs on every PR and push to `main`, what each job gates, and what happens on a release tag.*

All jobs must be green before a PR merges — CI is the objective gate (see
[Pull requests](pull-requests.md)). Workflows live in `.github/workflows/`.

## `ci.yml` — every PR and push to `main`

### `build-test` (matrix: macOS, Linux, Windows)

The core gate. For each OS it provisions Qt via `scripts/setup_qt.py`, then
runs the matching `ci-*` preset end to end:

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
job fails. Bump the pin by editing `ESMINI_VERSION` in the job.

### `docs`

Markdown link check (lychee) over `docs/**` and the root `.md` files —
broken relative links in this documentation tree fail the PR.

## Caching

- **Qt**: `./.qt/` is cached with a key that hashes `cmake/QtVersion.cmake`
  **and** `scripts/setup_qt.py` — bumping the Qt pin or changing the
  provisioning script automatically invalidates the cache; otherwise
  `setup_qt.py` is a no-op on a cache hit.
- **Compiler**: ccache (sccache on Windows), keyed per OS and job.
- **FetchContent**: dependency download tarballs cached, keyed on the hash of
  `cmake/deps.cmake` (where every dependency is pinned — see
  [dependency policy](../standards/dependencies.md)).

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
