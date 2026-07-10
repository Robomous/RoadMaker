# Testing

*The project's testing doctrine: frameworks, conventions, and the invariants every change must protect. This page is the single source of truth for testing rules.*

## Frameworks — non-negotiable

- **C++ tests use GoogleTest. Only GoogleTest.** This covers the kernel *and*
  the editor. Catch2, doctest, and hand-rolled frameworks are forbidden by
  project decision — see [ADR 0004](../decisions/0004-gtest.md). Do not
  reintroduce Catch2 even though early project material mentioned it.
- **Python tests use pytest**, leaning into pytest idioms — no
  unittest-style classes.
- googletest is pinned (URL + SHA256, BSD-3-Clause) in `cmake/deps.cmake`;
  test targets build only under `RM_BUILD_TESTS` (on by default and in every
  preset).

## Ground rules

- **Write tests with the code, not after.** A PR that adds behavior without
  tests is incomplete — see the [PR checklist](pull-requests.md).
- Test locations: `core/tests/` (kernel), `editor/tests/` (editor, headless),
  `python/tests/` (bindings), `tests/consume_installed/` (installed-package
  smoke, exercised by [CI](ci.md)).
- Run everything with `ctest --preset dev-<os>` and `pytest python/tests`
  (see [Building](../getting-started/building.md)).

## GoogleTest conventions

- One `TEST(SuiteName, TestName)` per behavior. Suites are PascalCase nouns
  matching the unit under test (`Arena`, `XodrReader`, `RoundTrip`); test
  names are PascalCase sentences (`SlotReuseBumpsGeneration`).
- `ASSERT_*` when continuing makes no sense — null checks, container sizes
  before indexing, `Expected::has_value()` before dereferencing.
  `EXPECT_*` everywhere else, so a single run reports every divergence.
- Geometry comparisons use `EXPECT_NEAR(value, expected, tol)` with **named
  tolerances from `rm::tol`**. Never `EXPECT_DOUBLE_EQ` for computed
  geometry; never magic epsilons.
- Compile-time facts are checked with `static_assert` inside the test body
  (GoogleTest has no STATIC_REQUIRE).
- Test helpers that *return a value* throw on setup failure (GoogleTest
  reports uncaught exceptions as test failures); void helpers may use
  `ASSERT_*`/`EXPECT_*` directly.
- Use `SCOPED_TRACE` inside sampling loops so a failure names the station
  (s-coordinate) that diverged.
- CMake wiring: link `GTest::gtest_main` and register with
  `gtest_discover_tests` (the root `CMakeLists.txt` already does
  `include(GoogleTest)`).
- gmock is off — prefer small hand-written fakes over mocks.

## Editor tests (headless Qt)

Editor logic lives in testable model/document classes and is tested with
GoogleTest like everything else, headless:

- Tests run with `QT_QPA_PLATFORM=offscreen`, set in two places:
  `editor/tests/qt_gtest_main.cpp` (so the binary works standalone) and as a
  ctest `ENVIRONMENT` property (so `ctest` works regardless of shell).
- The offscreen platform provides **no real OpenGL 3.3 context** — never
  unit-test `paintGL`/the GL renderer; the packaged-binary smoke tests in the
  [release workflow](ci.md) cover that path.
- `Qt6::Test` is linked **only** for helper classes — `QSignalSpy` and
  `QAbstractItemModelTester`. It is never the test runner; GoogleTest is.
- **Every new `QAbstractItemModel` ships its `QAbstractItemModelTester`
  GoogleTest in the same commit** — the tester catches most model-index and
  signal-contract bugs for free.

## pytest conventions

- Plain test functions plus fixtures; no `unittest.TestCase`.
- `tmp_path` for any file I/O; `pytest.raises` for error contracts;
  `pytest.approx(..., abs=...)` for float comparisons, mirroring the
  `rm::tol` values used on the C++ side;
  `@pytest.mark.parametrize` when one behavior spans many inputs.
- Setup:

```sh
pip install -e python/
pytest python/tests
```

## What to test

- **Round-trip invariants are first-class tests.** Author → write → parse →
  compare within `rm::tol::kRoundTrip*` tolerances. Any change to the
  OpenDRIVE writer or reader must keep them green.
- **Geometry gets golden analytic cases plus property-style checks** —
  known closed-form results on one hand; invariants like arc-length
  monotonicity and G1 continuity at joints sampled along the curve on the
  other.
- **Parser changes extend the fuzz corpus.** Every new OpenDRIVE feature adds
  representative inputs to `core/tests/fuzz/corpus/`; CI runs a fuzz smoke
  over it (see [CI](ci.md)). Build the fuzzers locally with
  `-DRM_BUILD_FUZZERS=ON` (Clang only).

## Sanitizers

Run an ASan+UBSan build before merging **anything touching geometry or
parsing**:

```sh
cmake -B build-asan -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DRM_BUILD_TESTS=ON -DRM_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

CI runs the same configuration on Linux/Clang (with the editor enabled — ASan
surfaces signal/slot lifetime bugs) as a required job.
