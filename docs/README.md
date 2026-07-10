# RoadMaker documentation

*The documentation home: what lives where, and the fastest reading path for
what you want to do.*

RoadMaker is an open-source (MIT) road-network authoring toolkit for
autonomous-driving simulation — a C++20 kernel with Python bindings and a
Qt 6 Widgets editor, centered on ASAM OpenDRIVE correctness. This `docs/`
tree is the single source of truth for the project's standards,
architecture, domain conventions, and roadmap.

## I want to build and run it

1. [Building](getting-started/building.md) — Qt provisioning, CMake presets,
   all three platforms, troubleshooting.
2. [Running](getting-started/running.md) — the editor, sample files, the
   Python package.
3. [Repository tour](getting-started/repository-tour.md) — what each folder
   is.

## I want to contribute

1. [Workflow](contributing/workflow.md) — branches, conventional commits,
   what rides along every change.
2. [Pull requests](contributing/pull-requests.md) — the PR checklist and
   review process.
3. [Testing](contributing/testing.md) — GoogleTest/pytest doctrine, headless
   Qt tests, sanitizers, fuzzing.
4. [CI](contributing/ci.md) — what each gate checks.
5. The standards your change must meet:
   [C++ style](standards/cpp-style.md) ·
   [Cross-platform](standards/cross-platform.md) ·
   [Dependencies & licensing](standards/dependencies.md) ·
   [Assets](standards/assets.md) ·
   [Product parity & IP](standards/product-parity.md)

## I want to understand the architecture

1. [Overview](architecture/overview.md) — the three layers and the rules
   between them.
2. [Kernel](architecture/kernel.md) — data model, geometry, I/O, meshing,
   exporters.
3. [Editor](architecture/editor.md) — Document/SelectionModel, undo,
   renderer.
4. [Python bindings](architecture/python-bindings.md) — the nanobind layer.
5. Domain background: [OpenDRIVE conventions](domain/opendrive.md) ·
   [Geometry & meshing](domain/geometry.md) ·
   [ASAM references](domain/references.md)

## Where is the project going?

- [Roadmap](roadmap/roadmap.md) — milestone sequence M2 → M5, themes and
  gates. The README table is a summary of this page.
- [Gap analysis](roadmap/gap_analysis.md) — where RoadMaker stands against a
  RoadRunner-class editing experience.
- [Golden scenes](roadmap/golden_scenes/README.md) — the target scenes that
  serve as visual acceptance criteria per milestone.
- [Milestone seeds](roadmap/seeds/m3a.md) — scope sketches for M3a, M3b, M4,
  M5.

## Design documents and decisions

- [design/](design/m2/00_overview.md) — full design docs per milestone
  (currently M2). These are written when a milestone's planning task runs;
  the [seeds](roadmap/seeds/m3a.md) are their precursors.
- [decisions/](decisions/0001-cpp20-kernel.md) — Architecture Decision
  Records (ADRs): one page per significant, hard-to-reverse decision.
  Start new ones from the [template](decisions/template.md).

## Documentation conventions

Every page in this tree follows these rules — apply them when adding or
editing docs:

- One H1 per file; sentence-case headings.
- Every page opens with a 1–2 line statement of what the page is for.
- Relative links only for in-repo targets; a rule lives on exactly one page
  and everything else links to it.
- Runnable commands go in fenced code blocks and are tested on at least the
  platform you develop on.
- No page over ~300 lines — split instead.
- Diagrams are Mermaid, inline in the page.

The tree is plain Markdown, structured so a static-site generator (e.g.
MkDocs) could be adopted later without moving files.
