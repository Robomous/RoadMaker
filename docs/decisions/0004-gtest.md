# ADR-0004: GoogleTest for all C++ tests

*Why GoogleTest is the only C++ test framework in the repository — for the
kernel and the editor — replacing the originally specified Catch2.*

- **Status:** accepted
- **Date:** 2026-06 (recorded retroactively 2026-07-10)
- **Deciders:** Armando Anaya

## Context

The original project bootstrap specified Catch2. Before the test suite grew
large, the maintainer overrode that choice: one framework across kernel and
editor tests, best-in-class ctest integration (`gtest_discover_tests`), and
alignment with the wider C++ ecosystem tooling mattered more than Catch2's
terser macros.

## Decision

**GoogleTest is mandatory for every C++ test** in the repository — kernel
and editor alike. Catch2, doctest, and hand-rolled frameworks are forbidden;
Catch2 specifically must not be reintroduced even though early documents
mention it. Python tests use **pytest** with its native idioms.

For the editor this means GoogleTest remains the runner even for Qt code:
tests run headless (`QT_QPA_PLATFORM=offscreen`), and `Qt6::Test` is linked
only for helpers (`QSignalSpy`, `QAbstractItemModelTester`), never as the
test framework.

## Consequences

- One assertion vocabulary and one discovery mechanism project-wide;
  `ctest --preset ...` runs everything.
- googletest is pinned in `cmake/deps.cmake` (BSD-3-Clause); gmock stays
  off — fakes are preferred over mocks.
- Editor test binaries need the offscreen platform plumbing (gtest main +
  ctest `ENVIRONMENT` property) described in the
  [testing doctrine](../contributing/testing.md).

## References

- [Testing doctrine](../contributing/testing.md) — conventions, what to
  test, sanitizers, fuzzing.
