# ADR-0002: FetchContent with pinned release archives for all dependencies

*Why every third-party library is pulled by CMake FetchContent from an exact
release archive with a SHA256 hash — never submodules, vendoring, or system
packages.*

- **Status:** accepted
- **Date:** 2026-05 (recorded retroactively 2026-07-10)
- **Deciders:** Armando Anaya

## Context

The project needs reproducible builds on three platforms from a plain CMake
configure, a clean license audit trail, and a low barrier for new
contributors (no package-manager bootstrap). Candidates were git submodules,
vendored sources, system packages (apt/brew/vcpkg/conan), and CMake
FetchContent.

## Decision

All library dependencies are declared in `cmake/deps.cmake` via
**`FetchContent_Declare` with a release-archive URL and `URL_HASH SHA256`**.
No live git branches, no submodules, no vendored source trees (narrow
exception: tiny unmaintained code, license header kept, commit recorded).
System packages are allowed only for CI tooling (ninja, ccache), never for
libraries the kernel links. Every new pin adds a row to
`THIRD_PARTY_LICENSES.md` in the same commit.

**Qt is the single exception**: it is provisioned outside the build by
`scripts/setup_qt.py` (see [ADR-0003](0003-qt-widgets-editor.md)) because
building Qt from source in FetchContent is impractical and the LGPL
compliance posture requires stock, dynamically linked Qt binaries.

## Consequences

- A clean checkout builds with CMake + a compiler + Python (for Qt
  provisioning); no other bootstrap.
- Hash-pinned archives make supply-chain drift visible: an upstream retag
  fails the build instead of silently changing code.
- CI caches FetchContent keyed on the hash of `cmake/deps.cmake`.
- Upgrades are deliberate, one-commit affairs (new URL + hash + license row),
  which is the intended friction.

## References

- [Dependency & licensing policy](../standards/dependencies.md) — the
  full add-a-dependency checklist.
