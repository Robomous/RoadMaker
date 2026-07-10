# Contributing to RoadMaker

Thanks for your interest! RoadMaker is an MIT-licensed project by Robomous.

## Ground rules

- **Licensing:** every dependency must be MIT / BSD / Apache-2.0 / MPL-2.0 /
  zlib / BSL-1.0. No GPL, no LGPL, no proprietary SDKs. New dependencies are
  pinned by exact release tag + SHA256 in `cmake/deps.cmake` and recorded in
  `THIRD_PARTY_LICENSES.md` in the same commit.
- **Architecture:** `core/` never includes UI, OpenGL, ImGui, or Python
  headers. `python/` and `editor/` depend on `core/` only — never on each
  other. GL calls live exclusively in `editor/src/render/`.
- **Cross-platform:** macOS, Linux, and Windows are all first-class. Every PR
  must pass all three CI jobs — no "fix Windows later" merges.

## Building

```sh
cmake -B build -G Ninja -DRM_BUILD_TESTS=ON -DRM_BUILD_PYTHON=ON -DRM_BUILD_EDITOR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build (run before merging anything touching geometry or parsing):

```sh
cmake -B build-asan -G Ninja -DRM_BUILD_TESTS=ON -DRM_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

## Style

- C++20, no compiler extensions. `snake_case` functions, `PascalCase` types.
- No exceptions across the public kernel API — return `rm::Expected`.
- No iostream in `core/` — use fmt/spdlog.
- Format with `git clang-format` before committing; CI enforces it.
- Write Catch2 tests with the code, not after.

## Commits & PRs

- Conventional commits: `feat(core): ...`, `fix(xodr): ...`,
  `refactor(mesh): ...`, `test(geometry): ...`, `docs: ...`.
- New OpenDRIVE features must extend the fuzz corpus in
  `core/tests/fuzz/corpus/`.
- Public kernel API changes must update `python/src/bindings.cpp` and at
  least one example in `python/examples/` in the same PR.
