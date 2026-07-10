# C++ style

*The coding standard for all C++ in RoadMaker. Read this before writing or reviewing kernel, editor, or binding code — it defines the error-handling, ownership, and API patterns every contribution must follow.*

RoadMaker is C++20 with no compiler extensions (see
[ADR 0001](../decisions/0001-cpp20-kernel.md) for why). CI builds with
warnings-as-errors on all three platforms, and formatting is enforced with
clang-format:

```sh
git clang-format
```

## Error handling

- Public kernel APIs return `rm::Expected<T>` — an alias of `std::expected`
  where available, otherwise the pinned `tl::expected` (the kernel targets
  C++20; `std::expected` is C++23). The error type is
  `rm::Error { code, message, context }`.
- Exceptions may exist *inside* a function body, but must never cross the
  public kernel API, the C boundary, or the nanobind bindings. Catch and
  convert to `rm::Error` at the boundary.
- Parsers do not throw and do not stop at the first problem: they accumulate
  `std::vector<rm::Diagnostic>` (severity, XPath-like location, message) and
  keep going. They fail only on structural errors that make further parsing
  meaningless.

## Ownership and identity

- Domain objects live in arenas owned by `RoadNetwork`. Cross-references
  between domain objects are **generational strong IDs**, never raw pointers:
  the `rm::Id<Tag>` template (`core/include/roadmaker/road/id.hpp`) carries an
  `{index, generation}` pair, with `RoadId`, `LaneId`, `JunctionId`, etc. as
  tagged instantiations.
- Lookup goes through the network: `network.road(id)` returns `Road*`, or
  `nullptr` if the ID is stale (the slot was freed or reused). Callers must
  handle the null case.
- Never store references or pointers to arena objects across mutations — any
  mutation of the network may invalidate them. Re-look-up by ID instead.
- Prefer free functions over member functions when they don't need private
  state. This keeps classes small and APIs composable.
- Take non-owning parameters as `std::span` / `std::string_view`. Never sink
  references to temporaries.

## Layout and naming

- Everything lives in `namespace roadmaker`, with the project-wide alias
  `namespace rm = roadmaker`.
- Files: `snake_case.hpp` / `snake_case.cpp`. One class per header where
  reasonable. `#pragma once` in every header.
- Types and template parameters: `PascalCase`. Functions and variables:
  `snake_case`. Constants: `kPascalCase`. Data members: trailing
  `underscore_`.
- Include order: the matching header first, then project headers, then
  third-party, then the standard library.

## Modern C++ usage

Prefer:

- Designated initializers for options/config structs.
- Ranges where they genuinely simplify the code.
- `constexpr` wherever the computation allows it.
- `[[nodiscard]]` on every `Expected`-returning API.
- `enum class`, always — never unscoped enums.

Avoid:

- Raw `new` / `delete` (use containers and smart pointers; arena objects are
  owned by their arena).
- Out-parameters — return a struct instead.
- Inheritance for code reuse — use composition; inheritance is for genuine
  interfaces (e.g., the renderer abstraction).
- `iostream` anywhere in `core/` — use fmt for formatting and spdlog for
  logging.
- Macros and singletons.

Concurrency stance: the kernel is single-threaded per `RoadNetwork`. Internal
parallelism (e.g., per-road meshing via `std::for_each` with a parallel
execution policy) is fine as long as the API stays externally single-threaded.

## Numerics

- `double` everywhere in geometry. `float` appears only in render-facing mesh
  buffers, at the last conversion step.
- Tolerances are **named constants** in `rm::tol`
  (`core/include/roadmaker/tol.hpp`) — e.g., `tol::kLength = 1e-6` m for
  length comparisons, `tol::kAngle = 1e-9` rad for heading continuity. Never
  inline a magic epsilon; if no existing constant fits, add one to `tol.hpp`
  with a doc comment stating its unit and purpose.
- Every public geometric API documents its **units and reference frame** in
  its doc comment. The kernel frame is right-handed, Z-up, meters, radians
  (the OpenDRIVE convention) — see the
  [architecture overview](../architecture/overview.md) for where conversions
  are allowed.

## Testing expectations

Style and tests are inseparable: tests are written with the code, not after.
Geometry code ships golden analytic tests plus property-style checks (e.g.,
arc-length monotonicity, curvature continuity at G1 joints), and round-trip
invariants (author → write → parse → compare) are first-class tests. The full
testing doctrine — frameworks, structure, and what CI runs — lives in
[contributing/testing](../contributing/testing.md).
