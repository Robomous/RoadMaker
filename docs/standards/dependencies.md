# Dependencies and licensing

*The license and dependency policy for this Apache-2.0-licensed project. Read this before adding, upgrading, or vendoring any third-party code — including snippets copied from other projects.*

RoadMaker ships under Apache-2.0. Everything it links, embeds, or redistributes must
be compatible with that story. When you are unsure about a license, **stop and
ask the maintainer** — do not add the dependency and sort it out later.

## Allowed licenses

MIT, BSD-2/BSD-3, Apache-2.0, MPL-2.0 (its file-level copyleft is fine),
zlib, BSL-1.0, Unlicense/CC0.

## Forbidden

- **GPL** (any version) — includes GPL-licensed *subsets* of otherwise-fine
  libraries: a permissive top-level license does not make a `copyleft/`
  subdirectory usable, and pinning such a library means auditing which
  headers we include.
- **LGPL** — static-link risk; see the single sanctioned exception below.
- **AGPL**, **SSPL**.
- **Proprietary SDKs** (e.g., the Autodesk FBX SDK).
- **Unlicensed code**, including Stack Overflow snippets — reimplement
  instead of copying.

## The sanctioned exception: Qt

Qt 6 is the **only** LGPL dependency, used under LGPLv3 for the editor, under
hard conditions (see [ADR 0003](../decisions/0003-qt-widgets-editor.md)):

- **Dynamic linking only.** Never build or link static Qt — static linking
  would drag the application into LGPL obligations incompatible with the Apache-2.0
  story.
- **Editor targets only.** `core/` and `python/` never include a Qt header or
  link a Qt library; the kernel and the Python wheels stay pure Apache-2.0.
- **Never vendored, never modified, never FetchContent.** Qt is provisioned by
  `scripts/setup_qt.py` (aqtinstall) into the gitignored `./.qt/` directory;
  the version pin lives in `cmake/QtVersion.cmake` and nowhere else.
- **Relink obligation in distributions:** every bundle ships the LGPLv3/GPLv3
  texts (checked in under `licenses/`), plus a notice in the About dialog and
  in `THIRD_PARTY_LICENSES.md` that users may replace the Qt libraries. The
  deploy tools keep Qt as separate shared libraries, which satisfies the
  provision.

Any *other* LGPL candidate still requires explicit maintainer approval before
it enters the tree.

## Explicit substitutions

Some obvious-looking libraries are forbidden and have designated
replacements. Do not "helpfully" add the forbidden one:

| Need | Use | Not |
|---|---|---|
| Robust booleans / solids | Manifold (Apache-2.0) | CGAL (GPL) |
| 2D triangulation / plan-view ops | CDT (MPL-2.0) + Clipper2 (BSL-1.0) | CGAL; Triangle (non-commercial-only!) |
| Scene export | glTF via tinygltf, later OpenUSD | FBX SDK (proprietary; FBX may become an out-of-tree plugin built against the user's own SDK) |

## npm dependencies (`docs-site/` only)

The documentation site is the one part of this repository with an npm tree
([ADR-0009](../decisions/0009-documentation-site-tiered-docs.md)). The same
licence rules apply, with three additions:

- **It is scoped to `docs-site/`.** Nothing else in the repository has a
  `package.json`, and **CMake never invokes npm** — a developer build of the
  kernel or the editor needs no Node at all.
- **`package-lock.json` is committed and CI runs `npm ci`**, so what CI resolves
  is exactly what a contributor resolved.
- **`npm run licenses` is a gate.** It reads every *installed* package's own
  `package.json` — the lockfile does not record licences — and fails on anything
  outside the permitted set. A package that ships a LICENSE file and declares
  nothing in `package.json` is read from the file rather than failed, because
  the file is still the grant. **Extend that fallback for a similar case; never
  weaken the gate.** If a dependency resolves non-permissive and no permissive
  alternative exists, stop and ask the maintainer.

`sharp` is the standing example of the policy biting: the site's framework
lists it as an optional dependency for image processing, and its prebuilt
libvips binaries are **LGPL-3.0-or-later**. Qt is this project's only sanctioned
LGPL dependency, so `package.json` overrides `sharp` to a local no-op stub that
throws if anything imports it, and the site uses a passthrough image service.
`--omit=optional` was not usable — it would also drop a required native binary.

### Update cadence: monthly

Once a month, and otherwise only when something needs it:

```sh
cd docs-site
npm outdated                 # what has moved
npm update                   # within the declared ranges
npm install <pkg>@<version>  # for a major, deliberately
npm run licenses             # the gate, before anything else
npm test && npm run build:web -- --base=/dev/ && npm run build:local
```

Commit the updated `package-lock.json` with the change. Record any new direct
dependency in `THIRD_PARTY_LICENSES.md`.

**No npm automation may open pull requests against this repository or fail a job
for the C++ side.** Dependency bots and advisory scanners are deliberately not
enabled here: a docs-site advisory that cannot affect the kernel must never
appear as a red check on a kernel change. That is why the cadence is a documented
human task rather than a robot.

Judge an advisory by whether it can reach *this* deployment. The site is
statically prerendered with no adapter and no server, so advisories confined to a
dev server, SSR, middleware, server islands, or an image endpoint do not apply to
what is published — note the assessment in the update commit rather than
silently ignoring it.

## Adding a dependency (checklist — all steps, one commit)

1. **Verify the license file in the upstream repository**, not just the
   README badge.
2. Pin it in `cmake/deps.cmake` with `FetchContent_Declare` using a
   **release-archive URL + `URL_HASH` SHA256**. No live git branches, ever.
   (Why FetchContent at all: [ADR 0002](../decisions/0002-fetchcontent.md).)
3. Set the dependency's build options to the minimum — no tests, examples, or
   tools.
4. Add a row to `THIRD_PARTY_LICENSES.md`: name, version, license, URL,
   usage.
5. Audit transitive dependencies it drags in — they must pass the same
   policy (and get noted; e.g., tinygltf's row records its bundled
   nlohmann/json and stb).

A real entry from `cmake/deps.cmake`:

```cmake
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz
  URL_HASH SHA256=8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0
)
set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_DOC OFF)
```

## General rules

- Prefer header-only or small static libraries. Question anything that adds
  more than ~30 seconds to a clean build; discuss heavyweights before adding
  them (OpenUSD is pre-approved for M2 behind an OFF-by-default option until
  wheel packaging is solved).
- **Vendoring:** never commit vendored source unless the project is
  unmaintained *and* tiny. If vendoring, keep the upstream license header
  intact and record the exact upstream commit.
- **System packages** are only for CI tooling (ninja, ccache) — never for
  libraries the kernel links. Builds must be reproducible from CMake alone.
- **External smoke tools** are a distinct category from dependencies: a pinned
  third-party *binary* that CI runs as a subprocess to check our output, never
  links, never redistributes, and that no build or runtime path depends on.
  Because nothing is linked or shipped, the licence question is "may we run it",
  which is far weaker than the linking test the allowed-licence list exists for
  — but the tool still gets pinned to an exact version and its licence recorded
  at the call site.
  - **esmini** ([#51](https://github.com/Robomous/RoadMaker/issues/51),
    [#249](https://github.com/Robomous/RoadMaker/issues/249)) — the OpenDRIVE
    and OpenSCENARIO round-trip gate, **and** the editor's scenario preview.
    **MPL-2.0** (allowed above; verified against the upstream repository at
    close-out, 2026-07-15). Pinned `v3.5.0` in the `esmini-roundtrip` job,
    fetched as `esmini-bin_Linux.zip` from the GitHub release like a test
    fixture, cached, run `--headless`. It is never linked into any RoadMaker
    target and never redistributed. If it ever needed to be linked or shipped,
    MPL-2.0's file-level copyleft would apply and this entry would not cover it.
    - **The editor's `File ▸ Preview Scenario in esmini…` stays inside this
      entry, and the boundary is exact** (p8-s5, #249): RoadMaker exports the
      pair to a throwaway folder and starts a **detached subprocess** on a
      binary the user already has — resolved from a settings path, then
      `$ESMINI_PATH`, then `PATH`. There is no esmini header in this tree, no
      esmini target in any `CMakeLists.txt`, and nothing esmini-shaped in any
      installer or wheel. Bundling the binary with a release, or linking
      `esminiLib`, would be a *different* question with a different answer, and
      would need this entry rewritten first.
    - **It is also not a build or runtime dependency.** Every RoadMaker target
      builds, tests and runs without esmini present; the preview action reports
      that it could not find one and offers a file dialog, and the CI job that
      uses it is the only thing that fetches it.
- Approved per-case exceptions are recorded in `THIRD_PARTY_LICENSES.md`
  and/or an ADR — e.g., tinyusdz's vendored ISC/Unlicense components are
  covered by [ADR 0005](../decisions/0005-tinyusdz-usda.md), and libtiff's
  BSD-style `libtiff` license by
  [ADR 0010](../decisions/0010-gis-ingest-bounded-crs.md).
- **Near-list licenses** — a license that is permissive in substance but is not
  literally one of the names above is not automatically fine and not
  automatically out. It needs the maintainer's explicit approval, recorded in an
  ADR *and* in `THIRD_PARTY_LICENSES.md`, quoting what the upstream `LICENSE`
  file actually says. Two things must be read rather than skimmed: whether any
  **component** of the library carries different terms from the whole (libtiff's
  LZW code adds a UC Berkeley acknowledgement clause the rest of the library does
  not have), and whether the terms impose an **active obligation** on us —
  attribution in documentation, a notice in distributed materials — as opposed
  to merely disclaiming warranty. Active obligations are discharged by name in
  `THIRD_PARTY_LICENSES.md`, and that entry is then load-bearing: deleting it is
  a license violation, not a tidy-up.

Asset (icon/texture/model) licensing has its own, stricter page:
[assets](./assets.md).
